// Tests: Milestone 20 Stream B — OOC-002 durable acceptance
// (audit report OOC-SA-001, HIGH) + orphan-pool coinbase guard.
//
// OOC-002: before M20, Node::AcceptBlockDirect / AcceptBlockReorg
// committed a block in memory (chain, UTXO, mempool, undo) and
// returned success BEFORE ChainStore::ApplyBlock ran; a persistence
// failure only set LastStoreError_ (M17 documented 40 submits
// "successful", in-memory height 45, restart recovered at height 6
// — 39 blocks lost). Since M20 the node persists FIRST and reports
// failure when persistence fails: the in-memory height never
// advances past the durable height.
//
// The spawned-node sections reuse the M17 Stream B pattern (fork +
// exec of the sibling oeeoo-node, pid-tagged data dirs, pid-derived
// ports with retry, RLIMIT_FSIZE disk-full simulation). The
// in-process sections use a Node with an attached ChainStore and
// force store failure deterministically by closing the store
// (ApplyBlock on a closed database fails cleanly) — the most
// reliable I/O-failure mechanism available in-process; the spawned
// disk-full sections cover the real RLIMIT_FSIZE path.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "app/cli.h"
#include "app/genesis.h"
#include "blockchain/block.h"
#include "blockchain/verify.h"
#include "consensus/consensus.h"
#include "consensus/subsidy.h"
#include "crypto/address.h"
#include "crypto/ecdsa.h"
#include "db/chainstore.h"
#include "mining/miner.h"
#include "network/node.h"
#include "network/protocol.h"
#include "network/transport.h"
#include "rpc/json.h"
#include "rpc/rpcutil.h"
#include "script/script.h"
#include "test_harness.h"
#include "transactions/sighash.h"
#include "transactions/transaction.h"
#include "transactions/validation.h"

using namespace oeeoo;

namespace {
constexpr uint32_t kTestBits = 0x207fffff;
constexpr uint32_t kTime = 1700000000;
constexpr uint32_t kNow = 1800000000;
constexpr uint32_t kGenesisTime = 1793923200;  // 2026-11-06 00:00 UTC

std::string PidTag() { return std::to_string(static_cast<long>(::getpid())); }

void RmRf(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
}

std::string HashToHex(const Hash256& h) {
  return BytesToHexStr(std::vector<unsigned char>(h.begin(), h.end()));
}

bool HexToHash(const std::string& hex, Hash256& out) {
  std::vector<unsigned char> bytes;
  if (!HexToBytesStr(hex, bytes) || bytes.size() != out.size()) return false;
  std::copy(bytes.begin(), bytes.end(), out.begin());
  return true;
}

// ---- spawned-node helpers (M17 Stream B pattern) ----

std::string NodeBinaryPath() {
  const char* env = std::getenv("OEEOO_NODE_BIN");
  if (env != nullptr && env[0] != '\0') return env;
  char buf[4096];
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    const std::string exe(buf);
    const std::size_t slash = exe.find_last_of('/');
    if (slash != std::string::npos) {
      return exe.substr(0, slash + 1) + "oeeoo-node";
    }
  }
  return "oeeoo-node";
}

// RPC call: true only when the transport worked AND the response has
// no "error". `result` is the response's "result" on success. (Note:
// submitblock rejections are NOT JSON-RPC errors in this codebase —
// like Bitcoin Core, submitblock returns null on acceptance and a
// "rejected: ..." string on rejection — so RpcCall returns true
// with a string result for a rejected submit; the caller must check
// IsString vs IsNull. See section (d).)
bool RpcCall(const std::string& dataDir, uint16_t rpcPort,
             const std::string& method, const Json& params, Json& result,
             std::string& error) {
  Json response;
  if (!CliCall(dataDir, rpcPort, method, params, response, error)) {
    return false;
  }
  const Json* err = response.Get("error");
  if (err != nullptr && !err->IsNull()) {
    error = "RPC error: " + err->Dump();
    return false;
  }
  const Json* res = response.Get("result");
  if (res == nullptr) {
    error = "RPC reply has no result";
    return false;
  }
  result = *res;
  return true;
}

// Raw RPC call: the full response envelope, so the test can assert
// on the result/error shape itself (needed for (d)).
bool RpcRaw(const std::string& dataDir, uint16_t rpcPort,
            const std::string& method, const Json& params, Json& response,
            std::string& error) {
  return CliCall(dataDir, rpcPort, method, params, response, error);
}

bool RpcHeight(const std::string& dataDir, uint16_t rpcPort, int& height) {
  Json result;
  std::string error;
  if (!RpcCall(dataDir, rpcPort, "getblockcount", Json::Array(), result,
               error)) {
    return false;
  }
  height = static_cast<int>(result.AsInt(-1));
  return height >= 0;
}

bool RpcTip(const std::string& dataDir, uint16_t rpcPort,
            std::string& tipHex) {
  Json result;
  std::string error;
  if (!RpcCall(dataDir, rpcPort, "getbestblockhash", Json::Array(), result,
               error)) {
    return false;
  }
  tipHex = result.AsString();
  return tipHex.size() == 64;
}

bool ChildAlive(pid_t pid) { return ::kill(pid, 0) == 0; }

bool WaitExit(pid_t pid, int timeoutSec, int& status) {
  for (int i = 0; i < timeoutSec * 10; ++i) {
    const pid_t r = ::waitpid(pid, &status, WNOHANG);
    if (r == pid) return true;
    if (r < 0) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

bool WaitRpcReady(const std::string& dataDir, uint16_t rpcPort, pid_t pid,
                  int timeoutSec) {
  for (int i = 0; i < timeoutSec * 10; ++i) {
    int height = -1;
    if (RpcHeight(dataDir, rpcPort, height)) return true;
    if (!ChildAlive(pid)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

pid_t SpawnNode(const std::string& nodeBin, const std::string& dataDir,
                const std::string& genesisPath, int p2pPort, int rpcPort,
                const std::string& logPath, long rlimitFsize) {
  const pid_t pid = ::fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    const int fd =
        ::open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
      ::dup2(fd, STDOUT_FILENO);
      ::dup2(fd, STDERR_FILENO);
      if (fd > STDERR_FILENO) ::close(fd);
    }
    if (rlimitFsize >= 0) {
      const rlim_t lim = static_cast<rlim_t>(rlimitFsize);
      const struct rlimit rl = {lim, lim};
      ::setrlimit(RLIMIT_FSIZE, &rl);
      ::signal(SIGXFSZ, SIG_IGN);
    }
    std::vector<std::string> args;
    args.push_back(nodeBin);
    args.push_back("--datadir");
    args.push_back(dataDir);
    args.push_back("--port");
    args.push_back(std::to_string(p2pPort));
    args.push_back("--rpcport");
    args.push_back(std::to_string(rpcPort));
    args.push_back("--pow-limit-bits");
    args.push_back("207fffff");
    if (!genesisPath.empty()) {
      args.push_back("--genesis-file");
      args.push_back(genesisPath);
    }
    args.push_back("--deep-verify");
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    ::execv(nodeBin.c_str(), argv.data());
    ::_exit(127);
  }
  return pid;
}

// Start the node and wait until its RPC answers. Retries on the next
// pid-derived port pair when the child dies (port busy) or does not
// answer in time.
bool StartNodeReady(const std::string& nodeBin, const std::string& dataDir,
                    const std::string& genesisPath,
                    const std::string& logPath, long rlimitFsize,
                    pid_t& pidOut, uint16_t& rpcPortOut) {
  const int pidPart = static_cast<int>(::getpid()) % 400;
  for (int attempt = 0; attempt < 4; ++attempt) {
    const int base = 49000 + ((pidPart + attempt * 37) % 400) * 3;
    const uint16_t rpcPort = static_cast<uint16_t>(base + 1);
    const pid_t pid = SpawnNode(nodeBin, dataDir, genesisPath, base,
                                rpcPort, logPath, rlimitFsize);
    if (pid < 0) return false;
    if (WaitRpcReady(dataDir, rpcPort, pid, 10)) {
      pidOut = pid;
      rpcPortOut = rpcPort;
      return true;
    }
    ::kill(pid, SIGKILL);
    int status = 0;
    WaitExit(pid, 10, status);
  }
  return false;
}

bool StopNode(pid_t pid, int& status) {
  ::kill(pid, SIGTERM);
  if (WaitExit(pid, 10, status)) return true;
  ::kill(pid, SIGKILL);
  return WaitExit(pid, 10, status);
}

// Mine one block on the node via getblocktemplate (the M14 funded-E2E
// pattern) WITHOUT submitting it. Returns the template height, or -1
// + `error`; `blockHex`/`blockHashHex` are the mined block.
// `timeOffsetSeconds` shifts the header time so two blocks mined at
// the same height are distinct (MineBlock otherwise re-finds the same
// nonce from 0 and returns the identical block).
int MineBlockHex(const std::string& dataDir, uint16_t rpcPort,
                 const std::string& minerAddress,
                 const std::vector<unsigned char>& minerScript,
                 std::string& blockHex, std::string& blockHashHex,
                 std::string& error, uint32_t timeOffsetSeconds = 0) {
  Json params = Json::Array();
  params.Push(Json(minerAddress));
  Json templ;
  if (!RpcCall(dataDir, rpcPort, "getblocktemplate", params, templ,
               error)) {
    return -1;
  }
  const Json* heightJ = templ.Get("height");
  const Json* bitsJ = templ.Get("bits");
  const Json* timeJ = templ.Get("curtime");
  const Json* prevJ = templ.Get("previousblockhash");
  const Json* valueJ = templ.Get("coinbasevalue");
  const Json* versionJ = templ.Get("version");
  const Json* txsJ = templ.Get("transactions");
  if (heightJ == nullptr || bitsJ == nullptr || timeJ == nullptr ||
      prevJ == nullptr || valueJ == nullptr || versionJ == nullptr ||
      txsJ == nullptr || !txsJ->IsArray()) {
    error = "getblocktemplate reply is missing fields";
    return -1;
  }
  const int height = static_cast<int>(heightJ->AsInt());
  uint32_t bits = 0;
  try {
    bits = static_cast<uint32_t>(std::stoul(bitsJ->AsString(), nullptr, 16));
  } catch (...) {
    error = "getblocktemplate bits are not hex";
    return -1;
  }
  Hash256 prev;
  if (!HexToHash(prevJ->AsString(), prev)) {
    error = "bad previousblockhash";
    return -1;
  }
  Block block;
  block.header.nVersion = static_cast<int>(versionJ->AsInt());
  block.header.hashPrevBlock = prev;
  block.header.nTime =
      static_cast<uint32_t>(timeJ->AsInt()) + timeOffsetSeconds;
  block.header.nBits = bits;
  block.header.nNonce = 0;
  block.vtx.push_back(CreateCoinbase(height, minerScript, valueJ->AsInt()));
  for (const Json& item : txsJ->AsArray()) {
    const Json* dataJ = item.Get("data");
    if (dataJ == nullptr) {
      error = "template tx has no data";
      return -1;
    }
    std::vector<unsigned char> bytes;
    if (!HexToBytesStr(dataJ->AsString(), bytes)) {
      error = "bad template tx hex";
      return -1;
    }
    Transaction tx;
    if (!Transaction::Deserialize(bytes, tx)) {
      error = "template tx does not deserialize";
      return -1;
    }
    block.vtx.push_back(tx);
  }
  block.header.hashMerkleRoot = block.ComputeMerkleRoot();
  if (!MineBlock(block, height, "")) {
    error = "mining failed at test difficulty";
    return -1;
  }
  blockHex = BytesToHexStr(block.Serialize());
  blockHashHex = HashToHex(block.GetHash());
  return height;
}

// Submit a block hex via RPC and return the RAW response envelope
// (so the test can assert on the result/error shape itself).
bool SubmitBlockRaw(const std::string& dataDir, uint16_t rpcPort,
                    const std::string& blockHex, Json& response,
                    std::string& error) {
  Json params = Json::Array();
  params.Push(Json(blockHex));
  return RpcRaw(dataDir, rpcPort, "submitblock", params, response, error);
}

// True when `response` is a submitblock rejection string
// ("rejected: ..."), i.e. NOT a null acceptance.
bool IsSubmitRejected(const Json& response, std::string& rejection) {
  const Json* res = response.Get("result");
  if (res == nullptr || !res->IsString()) return false;
  rejection = res->AsString();
  return rejection.rfind("rejected:", 0) == 0;
}

// Mine one block on the node via getblocktemplate + submitblock (the
// M14 funded-E2E pattern). Returns the new height, or -1 + `error`
// on failure (including a rejected submit); `blockHashHex` is the
// mined hash.
int MineOneRpc(const std::string& dataDir, uint16_t rpcPort,
               const std::string& minerAddress,
               const std::vector<unsigned char>& minerScript,
               std::string& blockHashHex, std::string& error) {
  std::string blockHex;
  const int height = MineBlockHex(dataDir, rpcPort, minerAddress,
                                  minerScript, blockHex, blockHashHex, error);
  if (height < 0) return -1;
  Json response;
  if (!SubmitBlockRaw(dataDir, rpcPort, blockHex, response, error)) {
    return -1;
  }
  std::string rejection;
  if (IsSubmitRejected(response, rejection)) {
    error = "submitblock rejected: " + rejection;
    return -1;
  }
  const Json* res = response.Get("result");
  if (res == nullptr || !res->IsNull()) {
    error = "submitblock reply is neither null nor a rejection string";
    return -1;
  }
  return height;
}

// Submit blocks from `hexes` (in order) and require each to be
// accepted (null result). Returns false + `error` on the first
// failure.
bool ReplayHexes(const std::string& dataDir, uint16_t rpcPort,
                 const std::vector<std::string>& hexes,
                 std::string& error) {
  for (const std::string& hex : hexes) {
    Json response;
    if (!SubmitBlockRaw(dataDir, rpcPort, hex, response, error)) {
      return false;
    }
    const Json* res = response.Get("result");
    if (res == nullptr || !res->IsNull()) {
      error = "replayed block was not accepted";
      return false;
    }
  }
  return true;
}

// Full re-validation of a stopped node's store. `expectedTipHex`
// must match the verified tip. (Copied from test_milestone17b.cpp —
// the vesting parameters are derived from the stored genesis block
// exactly the way node_main does.)
bool DeepVerifyChain(const std::string& dataDir,
                     const std::string& expectedTipHex, std::string& error) {
  ChainStore store;
  if (!store.Open(dataDir + "/chain.db", error)) return false;
  VestingParams vesting;
  {
    Hash256 tip;
    std::string tipError;
    if (store.LoadTip(tip, tipError)) {
      std::vector<std::tuple<int, BlockHeader, Target>> headers;
      if (store.GetAllHeaders(headers, error)) {
        if (!headers.empty()) {
          Block genesis;
          if (store.GetBlock(std::get<1>(headers[0]).GetHash(), genesis,
                             error)) {
            if (genesis.vtx.size() >= 1 &&
                genesis.vtx[0].vout.size() == 1 &&
                genesis.vtx[0].vout[0].value == FOUNDER_ALLOCATION) {
              vesting.enabled = true;
              vesting.launchTime = genesis.header.nTime;
              vesting.founderScript = genesis.vtx[0].vout[0].scriptPubKey;
            }
          } else {
            store.Close();
            return false;
          }
        }
      } else {
        store.Close();
        return false;
      }
    }
  }
  Hash256 tip;
  const bool ok =
      VerifyChainFromStore(store, vesting, kTestBits, tip, error);
  store.Close();
  if (!ok) {
    std::printf("  [m20b] deep verify failed: %s\n", error.c_str());
    return false;
  }
  return HashToHex(tip) == expectedTipHex;
}

int CountLogMatches(const std::string& logPath, const std::string& needle,
                    std::string& firstMatch) {
  std::ifstream in(logPath);
  if (!in) return -1;
  int count = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (line.find(needle) != std::string::npos) {
      if (count == 0) firstMatch = line;
      ++count;
    }
  }
  return count;
}

bool BuildTestGenesis(const std::string& path, std::string& error) {
  KeyPair k1, k2, k3;
  if (!GenerateKeyPair(k1) || !GenerateKeyPair(k2) || !GenerateKeyPair(k3)) {
    error = "cannot generate founder test keys";
    return false;
  }
  const std::vector<unsigned char> founderScript =
      BuildMultisigScript(2, {k1.pubkey, k2.pubkey, k3.pubkey});
  if (founderScript.empty()) {
    error = "cannot build founder script";
    return false;
  }
  Block genesis = BuildGenesisBlock(founderScript, kGenesisTime, kTestBits);
  std::atomic<uint64_t> hashes{0};
  std::atomic<bool> stop{false};
  if (!MineGenesisParallel(genesis, 1, &hashes, &stop, error)) return false;
  return WriteGenesisFile(path, genesis, founderScript, error);
}

// ---- in-process node helpers (test_reorg.cpp pattern) ----

KeyPair g_key;
bool g_keysReady = false;

void EnsureKeys() {
  if (!g_keysReady) {
    CHECK(GenerateKeyPair(g_key));
    g_keysReady = true;
  }
}

std::vector<unsigned char> Spk() {
  return CreateP2PKHScriptPubKey(PubKeyHash160(g_key.pubkey));
}

Hash256 TxIdWith(unsigned char fill) {
  Hash256 h;
  h.fill(fill);
  return h;
}

Block MineGenesis() {
  Block genesis;
  genesis.header.nVersion = 1;
  genesis.header.nTime = kTime;
  genesis.header.nBits = kTestBits;
  genesis.vtx = {CreateCoinbase(0, Spk(), 0, "THINK ABOUT GOD?")};
  genesis.header.hashMerkleRoot = genesis.ComputeMerkleRoot();
  CHECK(MineBlock(genesis, 0, "THINK ABOUT GOD?"));
  return genesis;
}

// A spend of `prev` (a non-coinbase funding coin) paying `value` to
// our own key, signed with it.
Transaction MakeSpend(const Hash256& prev, Amount value) {
  Transaction tx;
  tx.version = 1;
  TxIn in;
  in.prevTxHash = prev;
  in.prevOutIndex = 0;
  in.sequence = 0xffffffff;
  tx.vin.push_back(in);
  TxOut out;
  out.value = value;
  out.scriptPubKey = Spk();
  tx.vout.push_back(out);
  tx.lockTime = 0;
  Hash256 h;
  CHECK(LegacySighash(tx, 0, Spk(), kSigHashAll, h));
  std::vector<unsigned char> sig = SignHash(h, g_key.privkey);
  sig.push_back(static_cast<unsigned char>(kSigHashAll));
  tx.vin[0].scriptSig = CreateP2PKHScriptSig(sig, g_key.pubkey);
  return tx;
}

void Fund(CoinsView& utxo, const Hash256& txid, Amount value) {
  Coin coin;
  coin.out.value = value;
  coin.out.scriptPubKey = Spk();
  coin.height = 0;
  coin.blockTime = kTime;
  coin.isCoinbase = false;
  CHECK(utxo.AddCoin({txid, 0}, coin));
}

// A hand-built block on `prev` at `height`/`time` carrying `txs`.
// The coinbase pays subsidy + the txs' fees to our key.
Block MakeBlock(const Hash256& prev, int height, uint32_t time,
                const std::vector<Transaction>& txs) {
  Amount fees = 0;
  for (const Transaction& tx : txs) {
    Amount in = 50 * COIN;  // every test spend consumes a 50-coin fund
    Amount out = 0;
    for (const TxOut& o : tx.vout) out += o.value;
    fees += in - out;
  }
  Block block;
  block.header.nVersion = 1;
  block.header.hashPrevBlock = prev;
  block.header.nTime = time;
  block.header.nBits = kTestBits;
  block.vtx.push_back(
      CreateCoinbase(height, Spk(), GetBlockSubsidy(height) + fees, ""));
  for (const Transaction& tx : txs) block.vtx.push_back(tx);
  block.header.hashMerkleRoot = block.ComputeMerkleRoot();
  CHECK(MineBlock(block, height, ""));
  return block;
}

bool UtxoHas(const CoinsView& utxo, const Hash256& txid, uint32_t vout) {
  Coin coin;
  return utxo.GetCoin({txid, vout}, coin);
}

// ---- loopback P2P helpers (test_milestone18b.cpp pattern) ----

template <typename F>
void Pump(const std::vector<Node*>& nodes, F done, int rounds = 30) {
  for (int i = 0; i < rounds; ++i) {
    for (Node* n : nodes) n->Poll();
    if (done()) return;
  }
}

std::unique_ptr<Node> MakeNode(const std::string& loopbackName) {
  auto node = std::make_unique<Node>(kTestBits);
  node->SetTime(kNow);
  node->SetMaxPeers(500);
  node->SetTransport(std::make_shared<LoopbackTransport>(loopbackName));
  return node;
}
}  // namespace

void TestMilestone20B() {
  const std::string nodeBin = NodeBinaryPath();
  CHECK(::access(nodeBin.c_str(), X_OK) == 0);
  if (::access(nodeBin.c_str(), X_OK) != 0) {
    std::printf("  [m20b] oeeoo-node binary not found next to the test "
                "binary (looked for %s); spawned-node tests cannot run\n",
                nodeBin.c_str());
    return;
  }
  KeyPair minerKey;
  CHECK(GenerateKeyPair(minerKey));
  const std::string minerAddress = PubKeyToAddress(minerKey.pubkey);
  const std::vector<unsigned char> minerScript =
      CreateP2PKHScriptPubKey(PubKeyHash160(minerKey.pubkey));

  const std::string genesisPath = "/tmp/oeeoo-m20b-" + PidTag() +
                                  "-genesis.json";
  {
    std::string error;
    CHECK(BuildTestGenesis(genesisPath, error));
  }
  Block genesisBlock;
  {
    std::vector<unsigned char> founderScript;
    std::string error;
    CHECK(ReadGenesisFile(genesisPath, genesisBlock, founderScript, error));
  }
  const std::string genesisHash = HashToHex(genesisBlock.GetHash());

  // (a) Disk-full block submission returns FAILURE and the
  // in-memory height does NOT advance past the durable height.
  // Before M20 every submit under the limit returned success while
  // the store lagged (M17: 40 "successful" submits, restart at
  // height 6). Now the first submit the store cannot persist is
  // rejected, the node keeps reporting the durable height, and a
  // restart recovers exactly that height.
  test::BeginSection(
      "m20b: disk-full submitblock fails, height stays at durable height");
  {
    const std::string tag = "/tmp/oeeoo-m20b-" + PidTag() + "-diskfull-a";
    const std::string dataDir = tag + "-data";
    const std::string logPath = tag + "-node.log";
    RmRf(dataDir);
    pid_t pid = -1;
    uint16_t rpcPort = 0;
    CHECK(StartNodeReady(nodeBin, dataDir, genesisPath, logPath, -1, pid,
                         rpcPort));
    int durableHeight = 0;
    std::string durableTip = genesisHash;
    for (int i = 0; i < 5; ++i) {
      std::string blockHex, blockHash, error;
      const int h = MineBlockHex(dataDir, rpcPort, minerAddress,
                                 minerScript, blockHex, blockHash, error);
      CHECK(h == i + 1);
      Json response;
      CHECK(SubmitBlockRaw(dataDir, rpcPort, blockHex, response, error));
      const Json* res = response.Get("result");
      CHECK(res != nullptr && res->IsNull());  // accepted
      durableHeight = h;
      durableTip = blockHash;
    }
    int height = -1;
    CHECK(RpcHeight(dataDir, rpcPort, height));
    CHECK(height == 5);
    int stopStatus = 0;
    CHECK(StopNode(pid, stopStatus));
    std::error_code ec;
    const std::uintmax_t dbSize =
        std::filesystem::file_size(dataDir + "/chain.db", ec);
    CHECK(!ec);
    const long limit = static_cast<long>(dbSize) + 8192;
    std::printf("  [m20b] disk-full (a): chain.db is %llu bytes after 5 "
                "blocks; file-size limit set to %ld\n",
                static_cast<unsigned long long>(dbSize), limit);
    pid_t pid2 = -1;
    uint16_t rpcPort2 = 0;
    CHECK(StartNodeReady(nodeBin, dataDir, "", logPath, limit, pid2,
                         rpcPort2));
    CHECK(RpcHeight(dataDir, rpcPort2, height));
    CHECK(height == 5);
    // Submit until the store cannot persist one (at most 12 tries;
    // M17 observed the limit biting within ~2 submits). A submit
    // that IS persisted legitimately advances the durable height;
    // the first one that is not must be rejected, not "successful".
    bool sawRejection = false;
    std::string rejection;
    for (int i = 0; i < 12 && !sawRejection; ++i) {
      std::string blockHex, blockHash, error;
      const int h =
          MineBlockHex(dataDir, rpcPort2, minerAddress, minerScript,
                       blockHex, blockHash, error, static_cast<uint32_t>(i * 60));
      CHECK(h == durableHeight + 1);
      Json response;
      CHECK(SubmitBlockRaw(dataDir, rpcPort2, blockHex, response, error));
      const Json* res = response.Get("result");
      CHECK(res != nullptr);
      if (res->IsNull()) {
        durableHeight = h;
        durableTip = blockHash;
      } else {
        CHECK(IsSubmitRejected(response, rejection));
        CHECK(rejection.find("failed to persist") != std::string::npos);
        sawRejection = true;
      }
    }
    CHECK(sawRejection);  // the limit must bite, or nothing was tested
    std::printf("  [m20b] disk-full (a): first unpersistable submit "
                "rejected (%s); durable height %d\n",
                rejection.c_str(), durableHeight);
    // The rejection changed nothing in memory: height and tip are
    // still exactly the durable ones.
    CHECK(RpcHeight(dataDir, rpcPort2, height));
    CHECK(height == durableHeight);
    std::string tip;
    CHECK(RpcTip(dataDir, rpcPort2, tip));
    CHECK(tip == durableTip);
    // A second, distinct block at the same height is rejected too,
    // and the height still does not move.
    {
      std::string blockHex, blockHash, error;
      const int h =
          MineBlockHex(dataDir, rpcPort2, minerAddress, minerScript,
                       blockHex, blockHash, error, 3600);
      CHECK(h == durableHeight + 1);
      Json response;
      CHECK(SubmitBlockRaw(dataDir, rpcPort2, blockHex, response, error));
      CHECK(IsSubmitRejected(response, rejection));
      CHECK(rejection.find("failed to persist") != std::string::npos);
      CHECK(RpcHeight(dataDir, rpcPort2, height));
      CHECK(height == durableHeight);
      CHECK(RpcTip(dataDir, rpcPort2, tip));
      CHECK(tip == durableTip);
    }
    // M17 loudness is preserved: the node_main loop warns to stderr
    // whenever LastStoreError() is set. Count BEFORE the restart
    // (SpawnNode truncates the log file with O_TRUNC).
    // NOTE (M20 integration fix): the warning is printed by the
    // node_main loop (Poll 100ms + sleep 100ms), so it lands
    // asynchronously after the rejection — poll the log briefly
    // instead of asserting on a single instant read, which raced
    // (first run: 0 matches at count time).
    std::string firstStoreError;
    int loudBeforeRestart = 0;
    for (int i = 0; i < 50 && loudBeforeRestart == 0; ++i) {
      loudBeforeRestart =
          CountLogMatches(logPath, "chain store error", firstStoreError);
      if (loudBeforeRestart == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
    std::printf("  [m20b] disk-full (a): %d \"chain store error\" "
                "warning(s) in the limited run's log (first: %s)\n",
                loudBeforeRestart,
                loudBeforeRestart > 0 ? firstStoreError.c_str() : "(none)");
    CHECK(loudBeforeRestart > 0);
    int stopStatus2 = 0;
    CHECK(StopNode(pid2, stopStatus2));
    pid_t pid3 = -1;
    uint16_t rpcPort3 = 0;
    CHECK(StartNodeReady(nodeBin, dataDir, "", logPath, -1, pid3,
                         rpcPort3));
    CHECK(RpcHeight(dataDir, rpcPort3, height));
    CHECK(height == durableHeight);
    CHECK(RpcTip(dataDir, rpcPort3, tip));
    CHECK(tip == durableTip);
    int stopStatus3 = 0;
    CHECK(StopNode(pid3, stopStatus3));
    std::string error;
    CHECK(DeepVerifyChain(dataDir, durableTip, error));
    RmRf(dataDir);
    ::remove(logPath.c_str());
  }

  // (b) I/O-failure submission returns failure — in-process, with
  // the store closed (ApplyBlock on a closed database fails
  // cleanly). The most reliable mechanism available in-process;
  // the spawned disk-full sections cover the real RLIMIT_FSIZE
  // path. The failed submission must leave NO partial in-memory
  // state, and the same block must be acceptable once the store
  // is healthy again.
  test::BeginSection(
      "m20b: closed-store submission fails with no partial state");
  {
    EnsureKeys();
    const std::string path = "/tmp/oeeoo-m20b-" + PidTag() + "-closed.db";
    ::remove(path.c_str());
    ::remove((path + "-wal").c_str());
    ::remove((path + "-shm").c_str());
    ChainStore store;
    std::string error;
    CHECK(store.Open(path, error));
    Node node(kTestBits);
    node.SetTime(kNow);
    node.AttachStore(&store);
    Block genesis = MineGenesis();
    CHECK(node.BroadcastBlock(genesis, error));
    CHECK(node.TipHeight() == 0);
    Block b1 = MakeBlock(genesis.GetHash(), 1, kTime + 600, {});
    CHECK(node.BroadcastBlock(b1, error));
    CHECK(node.TipHeight() == 1);
    Hash256 storeTip;
    CHECK(store.LoadTip(storeTip, error));
    CHECK(storeTip == b1.GetHash());
    // Fail the store: close the underlying database. (The node
    // keeps its non-owning pointer; ApplyBlock now fails.)
    store.Close();
    Block b2 = MakeBlock(b1.GetHash(), 2, kTime + 1200, {});
    CHECK(!node.BroadcastBlock(b2, error));
    CHECK(error.find("failed to persist") != std::string::npos);
    CHECK(node.TipHeight() == 1);           // height did not advance
    CHECK(node.TipHash() == b1.GetHash());  // tip did not move
    CHECK(!node.LastStoreError().empty());
    CHECK(!node.HasBlock(b2.GetHash()));  // never indexed
    // No partial UTXO state: b2's coinbase output is not spendable
    // in the node's UTXO set.
    CHECK(!UtxoHas(node.Utxo(), b2.vtx[0].GetTxId(), 0));
    // The store is untouched too (reopen a fresh handle and check
    // the tip — the failed block never landed).
    {
      ChainStore probe;
      CHECK(probe.Open(path, error));
      Hash256 probeTip;
      CHECK(probe.LoadTip(probeTip, error));
      CHECK(probeTip == b1.GetHash());
      probe.Close();
    }
    // Healthy again: the SAME block is now accepted, proving the
    // failed submission left no poisoned in-memory state.
    CHECK(store.Open(path, error));
    CHECK(node.BroadcastBlock(b2, error));
    CHECK(node.TipHeight() == 2);
    CHECK(node.TipHash() == b2.GetHash());
    CHECK(store.LoadTip(storeTip, error));
    CHECK(storeTip == b2.GetHash());
    store.Close();
    ::remove(path.c_str());
    ::remove((path + "-wal").c_str());
    ::remove((path + "-shm").c_str());
  }

  // (c) Restart after failed submissions recovers at the durable
  // height, with identical tips on two nodes that saw the same
  // history (node B replays exactly the durable blocks).
  test::BeginSection(
      "m20b: restart after failed submissions recovers at durable "
      "height; second node agrees");
  {
    const std::string tag = "/tmp/oeeoo-m20b-" + PidTag() + "-diskfull-c";
    const std::string dataDir = tag + "-data";
    const std::string logPath = tag + "-node.log";
    RmRf(dataDir);
    pid_t pid = -1;
    uint16_t rpcPort = 0;
    CHECK(StartNodeReady(nodeBin, dataDir, genesisPath, logPath, -1, pid,
                         rpcPort));
    int durableHeight = 0;
    std::string durableTip = genesisHash;
    std::vector<std::string> durableHexes;
    for (int i = 0; i < 3; ++i) {
      std::string blockHex, blockHash, error;
      const int h = MineBlockHex(dataDir, rpcPort, minerAddress,
                                 minerScript, blockHex, blockHash, error);
      CHECK(h == i + 1);
      Json response;
      CHECK(SubmitBlockRaw(dataDir, rpcPort, blockHex, response, error));
      const Json* res = response.Get("result");
      CHECK(res != nullptr && res->IsNull());
      durableHeight = h;
      durableTip = blockHash;
      durableHexes.push_back(blockHex);
    }
    int stopStatus = 0;
    CHECK(StopNode(pid, stopStatus));
    std::error_code ec;
    const std::uintmax_t dbSize =
        std::filesystem::file_size(dataDir + "/chain.db", ec);
    CHECK(!ec);
    const long limit = static_cast<long>(dbSize) + 8192;
    pid_t pid2 = -1;
    uint16_t rpcPort2 = 0;
    CHECK(StartNodeReady(nodeBin, dataDir, "", logPath, limit, pid2,
                         rpcPort2));
    int height = -1;
    CHECK(RpcHeight(dataDir, rpcPort2, height));
    CHECK(height == 3);
    bool sawRejection = false;
    std::string rejection;
    for (int i = 0; i < 12 && !sawRejection; ++i) {
      std::string blockHex, blockHash, error;
      const int h =
          MineBlockHex(dataDir, rpcPort2, minerAddress, minerScript,
                       blockHex, blockHash, error, static_cast<uint32_t>(i * 60));
      CHECK(h == durableHeight + 1);
      Json response;
      CHECK(SubmitBlockRaw(dataDir, rpcPort2, blockHex, response, error));
      const Json* res = response.Get("result");
      CHECK(res != nullptr);
      if (res->IsNull()) {
        durableHeight = h;
        durableTip = blockHash;
        durableHexes.push_back(blockHex);
      } else {
        CHECK(IsSubmitRejected(response, rejection));
        CHECK(rejection.find("failed to persist") != std::string::npos);
        sawRejection = true;
      }
    }
    CHECK(sawRejection);
    CHECK(RpcHeight(dataDir, rpcPort2, height));
    CHECK(height == durableHeight);
    int stopStatus2 = 0;
    CHECK(StopNode(pid2, stopStatus2));
    pid_t pid3 = -1;
    uint16_t rpcPort3 = 0;
    CHECK(StartNodeReady(nodeBin, dataDir, "", logPath, -1, pid3,
                         rpcPort3));
    std::string tipA;
    CHECK(RpcHeight(dataDir, rpcPort3, height));
    CHECK(height == durableHeight);
    CHECK(RpcTip(dataDir, rpcPort3, tipA));
    CHECK(tipA == durableTip);
    int stopStatus3 = 0;
    CHECK(StopNode(pid3, stopStatus3));
    std::string error;
    CHECK(DeepVerifyChain(dataDir, durableTip, error));
    // Node B: a fresh data dir, the same genesis, replays exactly
    // the durable blocks — and must end at the same height + tip.
    const std::string tagB = "/tmp/oeeoo-m20b-" + PidTag() + "-diskfull-cb";
    const std::string dataB = tagB + "-data";
    const std::string logB = tagB + "-node.log";
    RmRf(dataB);
    pid_t pidB = -1;
    uint16_t rpcPortB = 0;
    CHECK(StartNodeReady(nodeBin, dataB, genesisPath, logB, -1, pidB,
                         rpcPortB));
    CHECK(ReplayHexes(dataB, rpcPortB, durableHexes, error));
    int heightB = -1;
    CHECK(RpcHeight(dataB, rpcPortB, heightB));
    CHECK(heightB == durableHeight);
    std::string tipB;
    CHECK(RpcTip(dataB, rpcPortB, tipB));
    CHECK(tipB == durableTip);
    CHECK(tipB == tipA);
    std::printf("  [m20b] restart recovery (c): node A height %d tip %s; "
                "node B height %d tip %s — identical\n",
                durableHeight, tipA.c_str(), heightB, tipB.c_str());
    int stopStatusB = 0;
    CHECK(StopNode(pidB, stopStatusB));
    CHECK(DeepVerifyChain(dataB, durableTip, error));
    RmRf(dataDir);
    RmRf(dataB);
    ::remove(logPath.c_str());
    ::remove(logB.c_str());
  }

  // (d) The RPC submitblock response after a persistence failure
  // is a REJECTION, not success. This codebase's submitblock
  // contract is Bitcoin Core's: null result = accepted, a
  // "rejected: ..." string result = not accepted (NOT a JSON-RPC
  // error object — the handler returns Json("rejected: "+error)
  // for every rejection, rpcserver.cpp:1353). The assertion is
  // therefore on the raw envelope: the "error" field is null and
  // the "result" is the rejection string naming the persistence
  // failure. (A JSON-RPC error object would need an rpcserver.cpp
  // change, which is out of this worker's scope.)
  test::BeginSection(
      "m20b: RPC submitblock after persistence failure is a rejection");
  {
    const std::string tag = "/tmp/oeeoo-m20b-" + PidTag() + "-diskfull-d";
    const std::string dataDir = tag + "-data";
    const std::string logPath = tag + "-node.log";
    RmRf(dataDir);
    pid_t pid = -1;
    uint16_t rpcPort = 0;
    CHECK(StartNodeReady(nodeBin, dataDir, genesisPath, logPath, -1, pid,
                         rpcPort));
    for (int i = 0; i < 2; ++i) {
      std::string blockHash, error;
      const int h = MineOneRpc(dataDir, rpcPort, minerAddress, minerScript,
                               blockHash, error);
      CHECK(h == i + 1);
    }
    int height = -1;
    CHECK(RpcHeight(dataDir, rpcPort, height));
    CHECK(height == 2);
    int stopStatus = 0;
    CHECK(StopNode(pid, stopStatus));
    std::error_code ec;
    const std::uintmax_t dbSize =
        std::filesystem::file_size(dataDir + "/chain.db", ec);
    CHECK(!ec);
    const long limit = static_cast<long>(dbSize) + 8192;
    pid_t pid2 = -1;
    uint16_t rpcPort2 = 0;
    CHECK(StartNodeReady(nodeBin, dataDir, "", logPath, limit, pid2,
                         rpcPort2));
    CHECK(RpcHeight(dataDir, rpcPort2, height));
    CHECK(height == 2);
    // Submit until one is rejected (a persisted submit first is
    // fine — it legitimately advances the durable height).
    int durableHeight = 2;
    bool sawRejection = false;
    Json badResponse;
    for (int i = 0; i < 12 && !sawRejection; ++i) {
      std::string blockHex, blockHash, error;
      const int h =
          MineBlockHex(dataDir, rpcPort2, minerAddress, minerScript,
                       blockHex, blockHash, error, static_cast<uint32_t>(i * 60));
      CHECK(h == durableHeight + 1);
      Json response;
      CHECK(SubmitBlockRaw(dataDir, rpcPort2, blockHex, response, error));
      const Json* res = response.Get("result");
      CHECK(res != nullptr);
      if (res->IsNull()) {
        durableHeight = h;
      } else {
        badResponse = response;
        sawRejection = true;
      }
    }
    CHECK(sawRejection);
    // The raw envelope: no JSON-RPC error, result is the rejection
    // string naming the persistence failure — and NOT null
    // (null would be success).
    const Json* errField = badResponse.Get("error");
    CHECK(errField == nullptr || errField->IsNull());
    std::string rejection;
    CHECK(IsSubmitRejected(badResponse, rejection));
    CHECK(rejection.find("failed to persist accepted block:") !=
          std::string::npos);
    std::printf("  [m20b] RPC rejection (d): %s\n", rejection.c_str());
    // And the node's height is still the durable one.
    CHECK(RpcHeight(dataDir, rpcPort2, height));
    CHECK(height == durableHeight);
    int stopStatus2 = 0;
    CHECK(StopNode(pid2, stopStatus2));
    RmRf(dataDir);
    ::remove(logPath.c_str());
  }

  // (e) Happy path: a normal (non-failing) submission still
  // succeeds and persists — restart recovers it.
  test::BeginSection("m20b: normal submission succeeds and persists");
  {
    const std::string tag = "/tmp/oeeoo-m20b-" + PidTag() + "-happy";
    const std::string dataDir = tag + "-data";
    const std::string logPath = tag + "-node.log";
    RmRf(dataDir);
    pid_t pid = -1;
    uint16_t rpcPort = 0;
    CHECK(StartNodeReady(nodeBin, dataDir, genesisPath, logPath, -1, pid,
                         rpcPort));
    std::string tip;
    for (int i = 0; i < 3; ++i) {
      std::string blockHash, error;
      const int h = MineOneRpc(dataDir, rpcPort, minerAddress, minerScript,
                               blockHash, error);
      CHECK(h == i + 1);
      tip = blockHash;
    }
    int height = -1;
    CHECK(RpcHeight(dataDir, rpcPort, height));
    CHECK(height == 3);
    std::string tipNow;
    CHECK(RpcTip(dataDir, rpcPort, tipNow));
    CHECK(tipNow == tip);
    int stopStatus = 0;
    CHECK(StopNode(pid, stopStatus));
    pid_t pid2 = -1;
    uint16_t rpcPort2 = 0;
    CHECK(StartNodeReady(nodeBin, dataDir, "", logPath, -1, pid2,
                         rpcPort2));
    CHECK(RpcHeight(dataDir, rpcPort2, height));
    CHECK(height == 3);
    CHECK(RpcTip(dataDir, rpcPort2, tipNow));
    CHECK(tipNow == tip);
    int stopStatus2 = 0;
    CHECK(StopNode(pid2, stopStatus2));
    std::string error;
    CHECK(DeepVerifyChain(dataDir, tip, error));
    RmRf(dataDir);
    ::remove(logPath.c_str());
  }

  // Reorg under store failure: a reorg whose persist fails is
  // rejected WITHOUT adopting it — tip, UTXO set and mempool stay
  // on the old chain — and the same reorg succeeds once the store
  // is healthy again. (The happy-path reorg with a store is
  // covered by test_reorg.cpp "node follows the heavier fork,
  // store follows".)
  test::BeginSection(
      "m20b: reorg under store failure is rejected without adopting");
  {
    EnsureKeys();
    const std::string path = "/tmp/oeeoo-m20b-" + PidTag() + "-reorg.db";
    ::remove(path.c_str());
    ::remove((path + "-wal").c_str());
    ::remove((path + "-shm").c_str());
    ChainStore store;
    std::string error;
    CHECK(store.Open(path, error));
    Node node(kTestBits);
    node.SetTime(kNow);
    node.AttachStore(&store);
    const Hash256 fund1 = TxIdWith(0x31);
    const Hash256 fund2 = TxIdWith(0x32);
    Fund(node.Utxo(), fund1, 50 * COIN);
    Fund(node.Utxo(), fund2, 50 * COIN);
    Block genesis = MineGenesis();
    CHECK(node.BroadcastBlock(genesis, error));
    Block m1 = MakeBlock(genesis.GetHash(), 1, kTime + 600, {});
    CHECK(node.BroadcastBlock(m1, error));
    Transaction tx1 = MakeSpend(fund1, 49 * COIN);
    Block m2 = MakeBlock(m1.GetHash(), 2, kTime + 1200, {tx1});
    CHECK(node.BroadcastBlock(m2, error));
    Transaction tx3 = MakeSpend(fund2, 49 * COIN);
    Block m3 = MakeBlock(m2.GetHash(), 3, kTime + 1800, {tx3});
    CHECK(node.BroadcastBlock(m3, error));
    CHECK(node.TipHeight() == 3);
    CHECK(node.TipHash() == m3.GetHash());
    // Fork from block 1: 2' spends fund1 differently, 3' is empty.
    // The fork only wins once it has more work (4').
    Transaction tx2 = MakeSpend(fund1, 48 * COIN);
    Block f2 = MakeBlock(m1.GetHash(), 2, kTime + 1500, {tx2});
    CHECK(node.BroadcastBlock(f2, error));
    CHECK(node.TipHeight() == 3);  // side branch: tip does not move
    Block f3 = MakeBlock(f2.GetHash(), 3, kTime + 2100, {});
    CHECK(node.BroadcastBlock(f3, error));
    CHECK(node.TipHeight() == 3);  // equal work: still no reorg
    Hash256 storeTip;
    CHECK(store.LoadTip(storeTip, error));
    CHECK(storeTip == m3.GetHash());
    // Fail the store, then offer the winning fork block.
    store.Close();
    Block f4 = MakeBlock(f3.GetHash(), 4, kTime + 2700, {});
    CHECK(!node.BroadcastBlock(f4, error));
    CHECK(error.find("failed to persist") != std::string::npos);
    CHECK(node.TipHeight() == 3);  // reorg NOT adopted
    CHECK(node.TipHash() == m3.GetHash());
    CHECK(UtxoHas(node.Utxo(), tx1.GetTxId(), 0));   // loser's output kept
    CHECK(!UtxoHas(node.Utxo(), tx2.GetTxId(), 0));  // winner's not here
    CHECK(UtxoHas(node.Utxo(), fund2, 0) == false);  // still spent by tx3
    CHECK(!node.LastStoreError().empty());
    // Healthy again: the SAME fork block now reorgs the chain.
    CHECK(store.Open(path, error));
    CHECK(node.BroadcastBlock(f4, error));
    CHECK(node.TipHeight() == 4);
    CHECK(node.TipHash() == f4.GetHash());
    CHECK(!UtxoHas(node.Utxo(), tx1.GetTxId(), 0));
    CHECK(UtxoHas(node.Utxo(), tx2.GetTxId(), 0));
    CHECK(UtxoHas(node.Utxo(), fund2, 0));  // tx3's input restored
    CHECK(store.LoadTip(storeTip, error));
    CHECK(storeTip == f4.GetHash());
    store.Close();
    ::remove(path.c_str());
    ::remove((path + "-wal").c_str());
    ::remove((path + "-shm").c_str());
  }

  // Orphan-pool coinbase guard (M20, Worker A finding): a
  // coinbase-shaped transaction relayed as a "tx" used to fail
  // ValidateTransaction with "missing input" and land in the
  // orphan pool, where it can never validate. It must now be
  // rejected outright — never orphaned, never in the mempool.
  // Control first: a NON-coinbase tx with a genuinely missing
  // parent IS admitted to the orphan pool, proving the test
  // delivery path works (a broken delivery would false-pass).
  test::BeginSection(
      "m20b: relayed coinbase tx is rejected, never orphaned");
  {
    EnsureKeys();
    const std::string pid = PidTag();
    auto victim = MakeNode("loopback:m20b-victim-" + pid);
    auto attacker = MakeNode("loopback:m20b-attacker-" + pid);
    Block genesis = MineGenesis();
    std::string error;
    CHECK(victim->BroadcastBlock(genesis, error));
    CHECK(attacker->BroadcastBlock(genesis, error));
    const std::string victimAddr = "loopback:m20b-victim-" + pid;
    CHECK(attacker->ConnectTo(victimAddr));
    Pump({victim.get(), attacker.get()},
         [&] {
           return victim->HandshookPeerCount() == 1 &&
                  attacker->HandshookPeerCount() == 1;
         },
         200);
    CHECK(victim->HandshookPeerCount() == 1);
    CHECK(victim->OrphanCount() == 0);
    // Control: a spend of an unknown parent is a genuine orphan.
    const Transaction orphanTx = MakeSpend(TxIdWith(0x77), 49 * COIN);
    CHECK(attacker->SendRawToAddress(victimAddr,
                                     {"tx", orphanTx.Serialize()}));
    Pump({victim.get(), attacker.get()},
         [&] { return victim->OrphanCount() == 1; }, 200);
    CHECK(victim->OrphanCount() == 1);
    CHECK(victim->Orphans().Contains(orphanTx.GetTxId()));
    CHECK(!victim->HasTx(orphanTx.GetTxId()));
    // The coinbase-shaped relay: rejected, never orphaned.
    const Transaction coinbaseTx = CreateCoinbase(1, Spk(), 50 * COIN);
    CHECK(IsCoinbase(coinbaseTx));
    CHECK(attacker->SendRawToAddress(victimAddr,
                                     {"tx", coinbaseTx.Serialize()}));
    Pump({victim.get(), attacker.get()}, [] { return false; }, 30);
    CHECK(victim->OrphanCount() == 1);  // still only the control orphan
    CHECK(!victim->Orphans().Contains(coinbaseTx.GetTxId()));
    CHECK(!victim->HasTx(coinbaseTx.GetTxId()));
    // +10 misbehavior is far below the 100-point ban threshold:
    // the peer is still connected.
    CHECK(victim->HandshookPeerCount() == 1);
  }

  ::remove(genesisPath.c_str());
}
