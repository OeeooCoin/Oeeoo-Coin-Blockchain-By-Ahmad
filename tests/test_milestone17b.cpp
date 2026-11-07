// Tests: Milestone 17 (Stream B) — crash-injection tests.
//
// OEEOO ROBUSTNESS WORK, NOT Bitcoin Core parity: no named Core
// crash-injection test could be verified, so nothing here is
// presented as Core parity. The harness spawns the real `oeeoo-node`
// binary (the sibling of this test binary — or $OEEOO_NODE_BIN),
// mines blocks through its JSON-RPC the way the M14 funded E2E did
// (getblocktemplate + submitblock), kills it with SIGKILL at a
// chosen point, and verifies the store loads cleanly afterwards
// (restart on the same data directory, deep verification on).
//
// Ports are derived from the process id and retried on the next pair
// when busy — never fixed (the M16 report notes an environmental
// flake from a fixed-port collision on Listen(46479)). Data
// directories are unique per run (pid-tagged) and removed afterwards.
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
#include "crypto/address.h"
#include "crypto/ecdsa.h"
#include "db/chainstore.h"
#include "mining/miner.h"
#include "rpc/json.h"
#include "rpc/rpcutil.h"
#include "script/script.h"
#include "test_harness.h"
#include "transactions/transaction.h"

using namespace oeeoo;

namespace {
constexpr uint32_t kTestBits = 0x207fffff;
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

// The real node binary: the sibling of this test binary (both are
// built into the same directory), or $OEEOO_NODE_BIN when set.
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
// no "error". `result` is the response's "result" on success.
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
    if (r < 0) return false;  // already reaped / no such child
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

bool WaitRpcReady(const std::string& dataDir, uint16_t rpcPort, pid_t pid,
                  int timeoutSec) {
  for (int i = 0; i < timeoutSec * 10; ++i) {
    int height = -1;
    if (RpcHeight(dataDir, rpcPort, height)) return true;
    if (!ChildAlive(pid)) return false;  // died (e.g. port busy)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

// Fork + exec the node. `rlimitFsize >= 0` caps the size of every
// file the child creates (disk-full simulation): SIGXFSZ is ignored
// in the child so writes past the limit fail with EFBIG instead of
// killing the process with a signal — the test wants to observe the
// node's own error handling, not a signal death.
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
// answer in time. Returns false when no attempt came up.
bool StartNodeReady(const std::string& nodeBin, const std::string& dataDir,
                    const std::string& genesisPath,
                    const std::string& logPath, long rlimitFsize,
                    pid_t& pidOut, uint16_t& rpcPortOut) {
  const int pidPart = static_cast<int>(::getpid()) % 500;
  for (int attempt = 0; attempt < 4; ++attempt) {
    const int base = 47000 + ((pidPart + attempt * 37) % 500) * 3;
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

// SIGTERM, then wait; SIGKILL only when the node does not exit.
bool StopNode(pid_t pid, int& status) {
  ::kill(pid, SIGTERM);
  if (WaitExit(pid, 10, status)) return true;
  ::kill(pid, SIGKILL);
  return WaitExit(pid, 10, status);
}

// Mine one block on the node via getblocktemplate + submitblock (the
// M14 funded-E2E pattern). The template carries no coinbase
// transaction, so the coinbase is rebuilt exactly as AssembleBlock
// builds it (BIP34 height push, no extra data) and the header PoW
// is solved in-process at test difficulty. Returns the new height,
// or -1 + `error` on failure; `blockHashHex` is the mined hash.
int MineOneRpc(const std::string& dataDir, uint16_t rpcPort,
               const std::string& minerAddress,
               const std::vector<unsigned char>& minerScript,
               std::string& blockHashHex, std::string& error) {
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
  block.header.nTime = static_cast<uint32_t>(timeJ->AsInt());
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
  Json submitParams = Json::Array();
  submitParams.Push(Json(BytesToHexStr(block.Serialize())));
  Json submitResult;
  if (!RpcCall(dataDir, rpcPort, "submitblock", submitParams, submitResult,
               error)) {
    return -1;
  }
  if (submitResult.IsString()) {
    error = "submitblock rejected: " + submitResult.AsString();
    return -1;
  }
  blockHashHex = HashToHex(block.GetHash());
  return height;
}

// Full re-validation of a stopped node's store. `expectedTipHex`
// must match the verified tip. The vesting parameters are derived
// from the stored genesis block exactly the way node_main does
// (DeriveVestingFromStore): the test genesis pays FOUNDER_ALLOCATION
// to the founder vault, so verifying with vesting disabled would
// reject the genesis coinbase as over the subsidy.
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
    std::printf("  [m17b] deep verify failed: %s\n", error.c_str());
    return false;
  }
  return HashToHex(tip) == expectedTipHex;
}

// Count log lines containing `needle` (and keep the first match).
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
}  // namespace

void TestMilestone17B() {
  const std::string nodeBin = NodeBinaryPath();
  CHECK(::access(nodeBin.c_str(), X_OK) == 0);
  if (::access(nodeBin.c_str(), X_OK) != 0) {
    std::printf("  [m17b] oeeoo-node binary not found next to the test "
                "binary (looked for %s); crash tests cannot run\n",
                nodeBin.c_str());
    return;
  }
  KeyPair minerKey;
  CHECK(GenerateKeyPair(minerKey));
  const std::string minerAddress = PubKeyToAddress(minerKey.pubkey);
  const std::vector<unsigned char> minerScript =
      CreateP2PKHScriptPubKey(PubKeyHash160(minerKey.pubkey));

  // One shared test genesis for all sections (fresh keys per run;
  // the chain is throwaway — test difficulty, test data dirs only).
  const std::string genesisPath = "/tmp/oeeoo-m17b-" + PidTag() +
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

  test::BeginSection("m17b: SIGKILL during a block burst, restart recovers");
  {
    const std::string tag = "/tmp/oeeoo-m17b-" + PidTag() + "-burst";
    const std::string dataDir = tag + "-data";
    const std::string logPath = tag + "-node.log";
    RmRf(dataDir);
    pid_t pid = -1;
    uint16_t rpcPort = 0;
    CHECK(StartNodeReady(nodeBin, dataDir, genesisPath, logPath, -1, pid,
                         rpcPort));
    int height = -1;
    CHECK(RpcHeight(dataDir, rpcPort, height));
    CHECK(height == 0);
    // Baseline: 6 blocks persisted before the burst.
    for (int i = 0; i < 6; ++i) {
      std::string blockHash, error;
      const int h = MineOneRpc(dataDir, rpcPort, minerAddress, minerScript,
                               blockHash, error);
      CHECK(h == i + 1);
    }
    CHECK(RpcHeight(dataDir, rpcPort, height));
    CHECK(height == 6);
    // Burst: a worker thread mines + submits continuously while this
    // thread kills the node mid-burst, so the kill can land
    // mid-block-apply rather than between RPC calls.
    std::atomic<int> lastAcked{-1};
    std::atomic<int> attempts{0};
    std::atomic<bool> stopBurst{false};
    std::string lastAckedHash;
    std::mutex hashMutex;
    std::thread burst([&] {
      while (!stopBurst.load()) {
        std::string blockHash, error;
        const int h = MineOneRpc(dataDir, rpcPort, minerAddress,
                                 minerScript, blockHash, error);
        attempts.fetch_add(1);
        if (h < 0) break;
        lastAcked.store(h);
        std::lock_guard<std::mutex> lock(hashMutex);
        lastAckedHash = blockHash;
      }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    // Make sure the burst really got going before killing: if fewer
    // than 3 submits completed (a loaded machine), give it another
    // second rather than killing an idle node.
    if (attempts.load() < 3) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    CHECK(::kill(pid, SIGKILL) == 0);
    stopBurst.store(true);
    burst.join();
    int status = 0;
    CHECK(WaitExit(pid, 10, status));
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    const int acked = lastAcked.load();
    std::string ackedHash;
    {
      std::lock_guard<std::mutex> lock(hashMutex);
      ackedHash = lastAckedHash;
    }
    std::printf("  [m17b] burst kill: %d submit attempts, last acked "
                "height %d (baseline 6)\n",
                attempts.load(), acked);
    CHECK(acked >= 6);  // the burst got at least the baseline past
    // Restart on the same data directory (deep verification is on by
    // default: a corrupt store refuses to load, loudly).
    pid_t pid2 = -1;
    uint16_t rpcPort2 = 0;
    CHECK(StartNodeReady(nodeBin, dataDir, "", logPath, -1, pid2,
                         rpcPort2));
    int height2 = -1;
    CHECK(RpcHeight(dataDir, rpcPort2, height2));
    std::string tip2;
    CHECK(RpcTip(dataDir, rpcPort2, tip2));
    std::printf("  [m17b] burst kill: restart height %d, tip %s\n", height2,
                tip2.c_str());
    // Every block the node acknowledged before the kill was applied
    // and committed (WAL + synchronous=FULL), so it must survive;
    // blocks submitted but never acknowledged may or may not.
    CHECK(height2 >= acked);
    if (height2 == acked) CHECK(tip2 == ackedHash);
    CHECK(height2 >= 6);
    int stopStatus = 0;
    CHECK(StopNode(pid2, stopStatus));
    std::string error;
    CHECK(DeepVerifyChain(dataDir, tip2, error));
    RmRf(dataDir);
    ::remove(logPath.c_str());
  }

  test::BeginSection("m17b: SIGKILL during shutdown, restart recovers");
  {
    const std::string tag = "/tmp/oeeoo-m17b-" + PidTag() + "-shutdown";
    const std::string dataDir = tag + "-data";
    const std::string logPath = tag + "-node.log";
    RmRf(dataDir);
    pid_t pid = -1;
    uint16_t rpcPort = 0;
    CHECK(StartNodeReady(nodeBin, dataDir, genesisPath, logPath, -1, pid,
                         rpcPort));
    for (int i = 0; i < 3; ++i) {
      std::string blockHash, error;
      const int h = MineOneRpc(dataDir, rpcPort, minerAddress, minerScript,
                               blockHash, error);
      CHECK(h == i + 1);
    }
    int height = -1;
    CHECK(RpcHeight(dataDir, rpcPort, height));
    CHECK(height == 3);
    std::string tip;
    CHECK(RpcTip(dataDir, rpcPort, tip));
    // SIGTERM starts the shutdown (RPC stop, wallet save, store
    // close); SIGKILL 5 ms later aims to land mid-shutdown. The
    // exact point is not observable from outside, so the test
    // records which outcome happened and asserts recovery either
    // way: a clean exit before the SIGKILL is the same store state
    // as a kill mid-close.
    CHECK(::kill(pid, SIGTERM) == 0);
    ::usleep(5000);
    ::kill(pid, SIGKILL);  // may be a no-op if shutdown already done
    int status = 0;
    CHECK(WaitExit(pid, 10, status));
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) {
      std::printf("  [m17b] shutdown kill: died by SIGKILL mid-shutdown\n");
    } else {
      std::printf("  [m17b] shutdown kill: exited %d before the SIGKILL "
                  "landed (clean shutdown won the race)\n",
                  WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
    pid_t pid2 = -1;
    uint16_t rpcPort2 = 0;
    CHECK(StartNodeReady(nodeBin, dataDir, "", logPath, -1, pid2,
                         rpcPort2));
    int height2 = -1;
    CHECK(RpcHeight(dataDir, rpcPort2, height2));
    CHECK(height2 == 3);
    std::string tip2;
    CHECK(RpcTip(dataDir, rpcPort2, tip2));
    CHECK(tip2 == tip);
    int stopStatus = 0;
    CHECK(StopNode(pid2, stopStatus));
    std::string error;
    CHECK(DeepVerifyChain(dataDir, tip2, error));
    RmRf(dataDir);
    ::remove(logPath.c_str());
  }

  test::BeginSection("m17b: disk-full (RLIMIT_FSIZE), restart recovers");
  {
    // Sub-case A: a fresh data directory whose files cannot grow
    // past 4 KiB — the schema itself does not fit, so startup must
    // refuse loudly (or, if it somehow starts, the restart below
    // must still recover to the genesis).
    const std::string tagA = "/tmp/oeeoo-m17b-" + PidTag() + "-diskfull-a";
    const std::string dataA = tagA + "-data";
    const std::string logA = tagA + "-node.log";
    RmRf(dataA);
    pid_t pidA = -1;
    uint16_t rpcA = 0;
    const bool startedA =
        StartNodeReady(nodeBin, dataA, genesisPath, logA, 4096, pidA, rpcA);
    if (!startedA) {
      std::string firstMatch;
      const int matches = CountLogMatches(logA, "oeeoo-node:", firstMatch);
      std::printf("  [m17b] disk-full A: node refused to start under a "
                  "4 KiB file limit (%d startup-error log line(s)): %s\n",
                  matches, firstMatch.c_str());
      CHECK(matches > 0);  // loud failure, not a silent hang
    } else {
      std::printf("  [m17b] disk-full A: node started under a 4 KiB file "
                  "limit (schema fit); stopping it\n");
      int stopStatus = 0;
      CHECK(StopNode(pidA, stopStatus));
    }
    // Restart without the limit: the chain must be the genesis
    // (nothing could have been persisted under the limit, or the
    // store was never created).
    pid_t pidA2 = -1;
    uint16_t rpcA2 = 0;
    CHECK(StartNodeReady(nodeBin, dataA, genesisPath, logA, -1, pidA2,
                         rpcA2));
    int heightA = -1;
    CHECK(RpcHeight(dataA, rpcA2, heightA));
    CHECK(heightA == 0);
    std::string tipA;
    CHECK(RpcTip(dataA, rpcA2, tipA));
    CHECK(tipA == genesisHash);
    int stopStatusA = 0;
    CHECK(StopNode(pidA2, stopStatusA));
    RmRf(dataA);
    ::remove(logA.c_str());

    // Sub-case B: five blocks persisted normally, then the
    // file-size limit (current chain.db size + 8 KiB) bites
    // mid-chain. Observation (not assumption): a store failure is
    // recorded in Node::LastStoreError(), and since M17 the
    // node_main loop also warns about it to stderr as soon as it
    // is set — so the node keeps serving its in-memory chain and
    // reporting success while the store lags, but the failure is
    // LOUD in the log during the run; it becomes visible on disk
    // only as a height rollback after the restart. That is the
    // behaviour this test documents; the M17 fix (the stderr
    // warning) is asserted below via loudBeforeRestart.
    const std::string tagB = "/tmp/oeeoo-m17b-" + PidTag() + "-diskfull-b";
    const std::string dataB = tagB + "-data";
    const std::string logB = tagB + "-node.log";
    RmRf(dataB);
    pid_t pidB = -1;
    uint16_t rpcB = 0;
    CHECK(StartNodeReady(nodeBin, dataB, genesisPath, logB, -1, pidB,
                         rpcB));
    std::string tip5;
    for (int i = 0; i < 5; ++i) {
      std::string blockHash, error;
      const int h = MineOneRpc(dataB, rpcB, minerAddress, minerScript,
                               blockHash, error);
      CHECK(h == i + 1);
      if (h == 5) tip5 = blockHash;
    }
    int stopStatusB = 0;
    CHECK(StopNode(pidB, stopStatusB));
    std::error_code ec;
    const std::uintmax_t dbSize =
        std::filesystem::file_size(dataB + "/chain.db", ec);
    CHECK(!ec);
    const long limit =
        static_cast<long>(dbSize) + 8192;  // chain.db was ~dbSize bytes
    std::printf("  [m17b] disk-full B: chain.db is %llu bytes after 5 "
                "blocks; file-size limit set to %ld\n",
                static_cast<unsigned long long>(dbSize), limit);
    pid_t pidB2 = -1;
    uint16_t rpcB2 = 0;
    CHECK(StartNodeReady(nodeBin, dataB, "", logB, limit, pidB2, rpcB2));
    int heightB = -1;
    CHECK(RpcHeight(dataB, rpcB2, heightB));
    CHECK(heightB == 5);  // the persisted chain still loads under it
    // Mine under the limit until a submit fails or 40 are in.
    int memHeight = 5;
    std::string memTip = tip5;
    int submitsOk = 0;
    bool submitFailed = false;
    std::string submitError;
    for (int i = 0; i < 40; ++i) {
      std::string blockHash, error;
      const int h = MineOneRpc(dataB, rpcB2, minerAddress, minerScript,
                               blockHash, error);
      if (h < 0) {
        submitFailed = true;
        submitError = error;
        break;
      }
      ++submitsOk;
      memHeight = h;
      memTip = blockHash;
    }
    int memHeightSeen = -1;
    CHECK(RpcHeight(dataB, rpcB2, memHeightSeen));
    if (submitFailed) {
      std::printf("  [m17b] disk-full B: %d submits ok under the limit, "
                  "in-memory height %d (node reports %d); then a submit "
                  "failed: %s\n",
                  submitsOk, memHeight, memHeightSeen, submitError.c_str());
    } else {
      std::printf("  [m17b] disk-full B: %d submits ok under the limit, "
                  "in-memory height %d (node reports %d); no submit "
                  "failed\n",
                  submitsOk, memHeight, memHeightSeen);
    }
    CHECK(memHeightSeen == memHeight);
    // Stop (SIGTERM: the close/checkpoint runs under the limit too)
    // and restart without the limit.
    int stopStatusB2 = 0;
    CHECK(StopNode(pidB2, stopStatusB2));
    // M17: the node_main loop now warns to stderr whenever
    // LastStoreError() is set, so the limited run's log must contain
    // the warning. Count BEFORE the restart below — SpawnNode
    // truncates the log file (O_TRUNC), which would wipe the
    // evidence (that is why the original run reported 0 matches).
    std::string firstStoreError;
    const int loudBeforeRestart =
        CountLogMatches(logB, "chain store error", firstStoreError);
    pid_t pidB3 = -1;
    uint16_t rpcB3 = 0;
    CHECK(StartNodeReady(nodeBin, dataB, "", logB, -1, pidB3, rpcB3));
    int heightB3 = -1;
    CHECK(RpcHeight(dataB, rpcB3, heightB3));
    std::string tipB3;
    CHECK(RpcTip(dataB, rpcB3, tipB3));
    std::printf("  [m17b] disk-full B: restart height %d (was %d "
                "in-memory); rolled back %d block(s)\n",
                heightB3, memHeight, memHeight - heightB3);
    CHECK(heightB3 >= 5);        // nothing persisted is lost
    CHECK(heightB3 <= memHeight);  // nothing unpersisted is invented
    // The node's own log: did anything surface the store failure?
    // (The limited run's log was already counted before the restart
    // above — this count covers only the unlimited restart's log.)
    std::string firstMatch;
    const int loud = CountLogMatches(logB, "error", firstMatch);
    std::printf("  [m17b] disk-full B: %d log line(s) contain \"error\" "
                "in the restart log (first: %s); %d \"chain store error\" "
                "warning(s) in the limited run's log (first: %s)\n",
                loud, loud > 0 ? firstMatch.c_str() : "(none)",
                loudBeforeRestart,
                loudBeforeRestart > 0 ? firstStoreError.c_str() : "(none)");
    if (heightB3 < memHeight) {
      // The limit did bite: unpersisted blocks were silently
      // dropped on restart. Deep verification must still pass on
      // what survived — rollback, not corruption. M17: the failure
      // is now LOUD at runtime — the node_main loop warns to stderr
      // as soon as LastStoreError() is set — so the warning must be
      // present in the limited run's log.
      CHECK(loudBeforeRestart > 0);
      std::printf("  [m17b] disk-full B: OBSERVED — submitblock kept "
                  "returning success while persistence failed; the "
                  "failure surfaced as a height rollback after "
                  "restart AND as a \"chain store error\" warning in "
                  "the node's log during the run (M17 fix)\n");
    } else if (submitFailed) {
      // M20: the limit DID bite — a submit was rejected upfront
      // ("failed to persist accepted block") and the height stayed
      // at the durable height, so no rollback was needed. That
      // rejection (not a rollback) is the M20 durable-acceptance
      // behaviour; the failure still surfaced as a "chain store
      // error" warning in the limited run's log.
      CHECK(loudBeforeRestart > 0);
      std::printf("  [m17b] disk-full B: OBSERVED (M20) — the limit "
                  "bit: a submit was rejected on persistence failure "
                  "and the height stayed at the durable height %d; "
                  "no rollback was needed (M20 durable-acceptance "
                  "semantics)\n",
                  memHeight);
    } else {
      std::printf("  [m17b] disk-full B: NOTE — the limit never bit "
                  "(all %d in-memory blocks were persisted); disk-full "
                  "behaviour was NOT observed in this run\n",
                  memHeight);
    }
    int stopStatusB3 = 0;
    CHECK(StopNode(pidB3, stopStatusB3));
    std::string error;
    CHECK(DeepVerifyChain(dataB, tipB3, error));
    RmRf(dataB);
    ::remove(logB.c_str());
  }

  ::remove(genesisPath.c_str());
}
