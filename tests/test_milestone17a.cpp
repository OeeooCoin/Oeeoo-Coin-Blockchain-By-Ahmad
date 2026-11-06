// Tests: Milestone 17 Stream A — mempool persistence (Bitcoin's
// `mempool.dat` model): save/load round-trip, restart E2E through
// a real node + store, `savemempool` / `importmempool` RPCs, the
// `loaded` flag in `getmempoolinfo`, and the `-persistmempool=0`
// opt-out. The coordinator wires TestMilestone17A() into
// test_main.cpp.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

#include "app/config.h"
#include "consensus/amount.h"
#include "consensus/consensus.h"
#include "consensus/subsidy.h"
#include "crypto/ecdsa.h"
#include "db/chainstore.h"
#include "mempool/mempooldump.h"
#include "mining/miner.h"
#include "network/node.h"
#include "network/transport.h"
#include "rpc/json.h"
#include "rpc/rpcserver.h"
#include "rpc/rpcutil.h"
#include "script/script.h"
#include "test_harness.h"
#include "transactions/sighash.h"

using namespace oeeoo;

namespace {
constexpr uint32_t kTestBits = 0x207fffff;
constexpr uint32_t kTime = 1700000000;
constexpr uint32_t kNow = 1800000000;

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

std::string HashHex(const Hash256& h) {
  return BytesToHexStr(std::vector<unsigned char>(h.begin(), h.end()));
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

void Fund(CoinsView& utxo, const Hash256& txid, Amount value) {
  Coin coin;
  coin.out.value = value;
  coin.out.scriptPubKey = Spk();
  coin.height = 0;
  coin.blockTime = kTime;
  coin.isCoinbase = false;
  CHECK(utxo.AddCoin({txid, 0}, coin));
}

// A spend of `prev` (a non-coinbase funding coin, or a mature
// coinbase output) paying `value` to `scriptPubKey`, signed with
// our key. The output script is set BEFORE signing: SIGHASH_ALL
// commits to the outputs, so changing one after signing would
// invalidate the signature.
Transaction MakeSpendTo(const Hash256& prev, Amount value,
                        const std::vector<unsigned char>& scriptPubKey) {
  Transaction tx;
  tx.version = 1;
  TxIn in;
  in.prevTxHash = prev;
  in.prevOutIndex = 0;
  in.sequence = 0xffffffff;
  tx.vin.push_back(in);
  TxOut out;
  out.value = value;
  out.scriptPubKey = scriptPubKey;
  tx.vout.push_back(out);
  tx.lockTime = 0;
  Hash256 h;
  CHECK(LegacySighash(tx, 0, Spk(), kSigHashAll, h));
  std::vector<unsigned char> sig = SignHash(h, g_key.privkey);
  sig.push_back(static_cast<unsigned char>(kSigHashAll));
  tx.vin[0].scriptSig = CreateP2PKHScriptSig(sig, g_key.pubkey);
  return tx;
}

Transaction MakeSpend(const Hash256& prev, Amount value) {
  return MakeSpendTo(prev, value, Spk());
}

// A hand-built block on `prev` at `height`/`time` carrying `txs`.
// The coinbase pays subsidy + the txs' fees to our key. Every test
// spend consumes a 50-coin input, so fee = 50 COIN - outputs.
Block MakeBlock(const Hash256& prev, int height, uint32_t time,
                const std::vector<Transaction>& txs) {
  Amount fees = 0;
  for (const Transaction& tx : txs) {
    Amount in = 50 * COIN;
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

std::string TempPath(const std::string& name) {
  return (std::filesystem::temp_directory_path() /
          ("oeeoo-test-m17a-" + name))
      .string();
}

void CleanupPath(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  std::filesystem::remove(path + "-wal", ec);
  std::filesystem::remove(path + "-shm", ec);
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

bool WriteFile(const std::string& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << content;
  return static_cast<bool>(out);
}

std::string EntryLine(const Transaction& tx, Amount fee,
                      uint32_t addedTime) {
  Json j = Json::Object();
  j.Set("txid", HashHex(tx.GetTxId()));
  j.Set("tx", BytesToHexStr(tx.Serialize()));
  j.Set("fee", static_cast<int64_t>(fee));
  j.Set("time", static_cast<int64_t>(addedTime));
  return j.Dump() + "\n";
}

const char* kHeaderLine = "{\"format\":\"oeeoo-mempool\",\"version\":1}\n";

// ---- minimal HTTP client (same shape as the RPC tests) ----
struct HttpResult {
  int status = 0;
  std::string body;
};

HttpResult HttpPost(uint16_t port, const std::string& body) {
  HttpResult result;
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return result;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return result;
  }
  const std::string request =
      "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " +
      std::to_string(body.size()) +
      "\r\nConnection: close\r\n\r\n" + body;
  std::size_t sent = 0;
  while (sent < request.size()) {
    const ssize_t n =
        ::send(fd, request.data() + sent, request.size() - sent, 0);
    if (n <= 0) {
      ::close(fd);
      return result;
    }
    sent += static_cast<std::size_t>(n);
  }
  std::string raw;
  char buf[4096];
  ssize_t n = 0;
  while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
    raw.append(buf, static_cast<std::size_t>(n));
  }
  ::close(fd);
  const std::size_t space = raw.find(' ');
  if (space != std::string::npos) {
    result.status = std::stoi(raw.substr(space + 1, 3));
  }
  const std::size_t headerEnd = raw.find("\r\n\r\n");
  if (headerEnd != std::string::npos) {
    result.body = raw.substr(headerEnd + 4);
  }
  return result;
}

Json RpcCall(uint16_t port, const std::string& method,
             const std::string& paramsJson, std::string& error) {
  const HttpResult r = HttpPost(
      port, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"" + method +
                "\",\"params\":" + paramsJson + "}");
  if (r.status != 200) {
    error = "HTTP status " + std::to_string(r.status);
    return Json();
  }
  Json reply;
  if (!Json::Parse(r.body, reply, error)) return Json();
  const Json* result = reply.Get("result");
  if (result == nullptr) {
    const Json* err = reply.Get("error");
    error = err != nullptr ? err->Dump() : "no result in reply";
    return Json();
  }
  return *result;
}

// Mine genesis + 101 empty blocks (heights 1..101) into `node`,
// so the coinbases of blocks 1 and 2 are mature (spendable at tip
// 101: height 102 - 1 = 101 and 102 - 2 = 100 confirmations).
// Returns the two mature coinbase txids.
std::pair<Hash256, Hash256> BuildMatureChain(Node& node,
                                             const Block& genesis) {
  std::string error;
  CHECK(node.BroadcastBlock(genesis, error));
  std::vector<Hash256> coinbaseTxids;
  Hash256 prev = genesis.GetHash();
  for (int h = 1; h <= 101; ++h) {
    Block b = MakeBlock(prev, h, kTime + static_cast<uint32_t>(h) * 600, {});
    coinbaseTxids.push_back(b.vtx[0].GetTxId());
    CHECK(node.BroadcastBlock(b, error));
    prev = b.GetHash();
  }
  CHECK(node.TipHeight() == 101);
  CHECK(node.LastStoreError().empty());
  return {coinbaseTxids[0], coinbaseTxids[1]};
}
}  // namespace

void TestMilestone17A() {
  EnsureKeys();

  test::BeginSection("m17a: mempool save/load round-trip (Mempool level)");
  {
    const std::string path = TempPath("roundtrip.dat");
    CleanupPath(path);
    CoinsView utxo;
    Fund(utxo, TxIdWith(0xa1), 50 * COIN);
    Fund(utxo, TxIdWith(0xa2), 50 * COIN);
    Mempool pool;
    std::string error;
    const Transaction tx1 = MakeSpend(TxIdWith(0xa1), 49 * COIN);
    const Transaction tx2 = MakeSpend(TxIdWith(0xa2), 4850000000LL);
    CHECK(pool.AddTransaction(tx1, utxo, 1, kTime, kNow, error));
    CHECK(pool.AddTransaction(tx2, utxo, 1, kTime, kNow + 10, error));
    CHECK(pool.Size() == 2);

    CHECK(SaveMempoolFile(path, pool, error));
    CHECK(std::filesystem::exists(path));
    // The file starts with the versioned header line.
    const std::string content = ReadFile(path);
    CHECK(content.rfind(kHeaderLine, 0) == 0);
    // No stray tmp file survives a successful save.
    CHECK(!std::filesystem::exists(path + ".new"));

    Mempool reloaded;
    MempoolLoadStats stats;
    CHECK(LoadMempoolFile(path, reloaded, utxo, 1, kTime, kNow + 10,
                          stats, error));
    CHECK(stats.loaded == 2);
    CHECK(stats.droppedExpired == 0);
    CHECK(stats.droppedInvalid == 0);
    CHECK(reloaded.Size() == 2);
    Mempool::Entry e1;
    CHECK(reloaded.GetEntry(tx1.GetTxId(), e1));
    CHECK(e1.fee == 1 * COIN);  // 50 - 49
    CHECK(e1.addedTime == kNow);
    Mempool::Entry e2;
    CHECK(reloaded.GetEntry(tx2.GetTxId(), e2));
    CHECK(e2.fee == 150000000LL);  // 50 COIN - 48.5 COIN
    CHECK(e2.addedTime == kNow + 10);  // original time, not a fresh stamp
    CleanupPath(path);
  }

  test::BeginSection("m17a: load drops expired, invalid, fee-mismatch, "
                     "chained and malformed entries");
  {
    CoinsView utxo;
    Fund(utxo, TxIdWith(0xb1), 50 * COIN);
    Fund(utxo, TxIdWith(0xb2), 50 * COIN);
    const Transaction tx1 = MakeSpend(TxIdWith(0xb1), 49 * COIN);
    const Transaction tx2 = MakeSpend(TxIdWith(0xb2), 49 * COIN);

    // Expired: added 15 days ago, loaded "now" — past the 14-day
    // expiry, so it is dropped without ever being validated.
    const std::string expiredPath = TempPath("expired.dat");
    CleanupPath(expiredPath);
    {
      Mempool pool;
      std::string error;
      CHECK(pool.AddTransaction(tx1, utxo, 1, kTime, kNow - 15 * 24 * 3600,
                                error));
      CHECK(SaveMempoolFile(expiredPath, pool, error));
    }
    {
      Mempool reloaded;
      MempoolLoadStats stats;
      std::string error;
      CHECK(LoadMempoolFile(expiredPath, reloaded, utxo, 1, kTime, kNow,
                            stats, error));
      CHECK(stats.loaded == 0);
      CHECK(stats.droppedExpired == 1);
      CHECK(stats.droppedInvalid == 0);
      CHECK(reloaded.Size() == 0);
    }
    CleanupPath(expiredPath);

    // Fee mismatch: the stored fee is 1 sat more than the fee
    // re-validation computes — a tampered file must not inject a
    // transaction with a wrong fee.
    const std::string feePath = TempPath("feemismatch.dat");
    CleanupPath(feePath);
    CHECK(WriteFile(feePath, std::string(kHeaderLine) +
                                 EntryLine(tx1, 1 * COIN + 1, kNow)));
    {
      Mempool reloaded;
      MempoolLoadStats stats;
      std::string error;
      CHECK(LoadMempoolFile(feePath, reloaded, utxo, 1, kTime, kNow,
                            stats, error));
      CHECK(stats.loaded == 0);
      CHECK(stats.droppedInvalid == 1);
      CHECK(reloaded.Size() == 0);
    }
    CleanupPath(feePath);

    // Chained entry: a transaction spending an unconfirmed
    // parent's output. This mempool never holds such chains —
    // a child of an unconfirmed parent goes to the orphan pool,
    // not the mempool (see node.cpp "tx" handler) — so the loader
    // drops the child as invalid while the parent loads fine.
    const std::string chainPath = TempPath("chained.dat");
    CleanupPath(chainPath);
    const Transaction child = MakeSpendTo(tx1.GetTxId(), 48 * COIN, Spk());
    CHECK(WriteFile(chainPath, std::string(kHeaderLine) +
                                   EntryLine(tx1, 1 * COIN, kNow) +
                                   EntryLine(child, 1 * COIN, kNow)));
    {
      Mempool reloaded;
      MempoolLoadStats stats;
      std::string error;
      CHECK(LoadMempoolFile(chainPath, reloaded, utxo, 1, kTime, kNow,
                            stats, error));
      CHECK(stats.loaded == 1);
      CHECK(stats.droppedInvalid == 1);
      CHECK(reloaded.Size() == 1);
      CHECK(reloaded.Contains(tx1.GetTxId()));
      CHECK(!reloaded.Contains(child.GetTxId()));
    }
    CleanupPath(chainPath);

    // Malformed entry line: dropped, the load continues with the
    // good entries. A bad header fails the whole load. A missing
    // file loads as empty.
    const std::string badPath = TempPath("badline.dat");
    CleanupPath(badPath);
    CHECK(WriteFile(badPath, std::string(kHeaderLine) +
                                 "this is not json\n" +
                                 EntryLine(tx1, 1 * COIN, kNow) +
                                 "{\"txid\":\"zz\"}\n"));
    {
      Mempool reloaded;
      MempoolLoadStats stats;
      std::string error;
      CHECK(LoadMempoolFile(badPath, reloaded, utxo, 1, kTime, kNow,
                            stats, error));
      CHECK(stats.loaded == 1);
      CHECK(stats.droppedInvalid == 2);
      CHECK(reloaded.Size() == 1);
    }
    CleanupPath(badPath);
    const std::string badHeaderPath = TempPath("badheader.dat");
    CleanupPath(badHeaderPath);
    CHECK(WriteFile(badHeaderPath, "{\"format\":\"other\",\"version\":1}\n" +
                                       EntryLine(tx1, 1 * COIN, kNow)));
    {
      Mempool reloaded;
      MempoolLoadStats stats;
      std::string error;
      CHECK(!LoadMempoolFile(badHeaderPath, reloaded, utxo, 1, kTime,
                             kNow, stats, error));
      CHECK(!error.empty());
      CHECK(reloaded.Size() == 0);
    }
    CleanupPath(badHeaderPath);
    {
      Mempool reloaded;
      MempoolLoadStats stats;
      std::string error;
      CHECK(LoadMempoolFile(TempPath("does-not-exist.dat"), reloaded,
                            utxo, 1, kTime, kNow, stats, error));
      CHECK(stats.loaded == 0);
      CHECK(reloaded.Size() == 0);
    }

    // A tampered txid (the stored txid does not match the
    // serialized transaction) is dropped, not loaded.
    const std::string badIdPath = TempPath("badid.dat");
    CleanupPath(badIdPath);
    {
      std::string content = std::string(kHeaderLine) +
                            EntryLine(tx2, 1 * COIN, kNow);
      const std::size_t pos = content.find(HashHex(tx2.GetTxId()));
      CHECK(pos != std::string::npos);
      // Flip the first hex digit deterministically (whatever it
      // is, change it), so the stored txid no longer matches.
      content[pos] = content[pos] == '0' ? 'f' : '0';
      CHECK(WriteFile(badIdPath, content));
    }
    {
      Mempool reloaded;
      MempoolLoadStats stats;
      std::string error;
      CHECK(LoadMempoolFile(badIdPath, reloaded, utxo, 1, kTime, kNow,
                            stats, error));
      CHECK(stats.loaded == 0);
      CHECK(stats.droppedInvalid == 1);
      CHECK(reloaded.Size() == 0);
    }
    CleanupPath(badIdPath);
  }

  test::BeginSection("m17a: restart E2E through a real node — same txs "
                     "and fees after restart");
  {
    const std::string dir = TempPath("e2e");
    CleanupPath(dir);
    std::filesystem::create_directories(dir);
    const Block genesis = MineGenesis();
    const std::string dbPath = dir + "/chain.db";
    const std::string mpPath = dir + "/mempool.dat";
    Hash256 txAId;
    Hash256 txBId;
    {
      ChainStore store;
      std::string error;
      CHECK(store.Open(dbPath, error));
      auto node = std::make_unique<Node>(kTestBits);
      node->SetTime(kNow);
      node->SetTransport(std::make_shared<LoopbackTransport>("loopback:m17a"));
      node->AttachStore(&store);
      node->SetMempoolPath(mpPath);
      const auto mature = BuildMatureChain(*node, genesis);
      const Transaction txA = MakeSpend(mature.first, 49 * COIN);
      const Transaction txB = MakeSpend(mature.second, 4850000000LL);
      CHECK(node->BroadcastTx(txA, error));
      CHECK(node->BroadcastTx(txB, error));
      CHECK(node->MempoolSize() == 2);
      txAId = txA.GetTxId();
      txBId = txB.GetTxId();
      // The shutdown path: an explicit save, as node_main does on
      // clean shutdown.
      CHECK(node->SaveMempool(error));
      CHECK(std::filesystem::exists(mpPath));
      CHECK(node->MempoolLoaded() == false);  // no load attempted yet
      store.Close();
    }
    // Restart: a fresh node on the same data directory.
    {
      ChainStore store2;
      std::string error;
      CHECK(store2.Open(dbPath, error));
      auto node2 = std::make_unique<Node>(kTestBits);
      node2->SetTime(kNow);
      node2->SetTransport(
          std::make_shared<LoopbackTransport>("loopback:m17a-2"));
      node2->AttachStore(&store2);
      node2->SetMempoolPath(mpPath);
      CHECK(node2->LoadFromStore(store2, error));
      CHECK(node2->TipHeight() == 101);
      CHECK(node2->MempoolLoaded() == false);
      CHECK(node2->LoadMempool(error));
      CHECK(node2->MempoolLoaded() == true);
      const MempoolLoadStats stats = node2->LastMempoolLoadStats();
      CHECK(stats.loaded == 2);
      CHECK(stats.droppedExpired == 0);
      CHECK(stats.droppedInvalid == 0);
      CHECK(node2->MempoolSize() == 2);
      Mempool::Entry eA;
      CHECK(node2->MempoolRef().GetEntry(txAId, eA));
      CHECK(eA.fee == 1 * COIN);
      CHECK(eA.addedTime == kNow);
      Mempool::Entry eB;
      CHECK(node2->MempoolRef().GetEntry(txBId, eB));
      CHECK(eB.fee == 150000000LL);
      CHECK(eB.addedTime == kNow);
      store2.Close();
    }
    CleanupPath(dir);
  }

  test::BeginSection("m17a: savemempool RPC + getmempoolinfo loaded flag");
  {
    const std::string dir = TempPath("rpc");
    CleanupPath(dir);
    std::filesystem::create_directories(dir);
    const std::string mpPath = dir + "/mempool.dat";
    const Block genesis = MineGenesis();
    std::mutex nodeMutex;
    Node node(kTestBits);
    node.SetTime(kNow);
    node.SetTransport(std::make_shared<LoopbackTransport>("loopback:m17a-rpc"));
    node.SetMempoolPath(mpPath);
    std::string error;
    CHECK(node.BroadcastBlock(genesis, error));
    const Hash256 kFund = TxIdWith(0xc1);
    Fund(node.Utxo(), kFund, 50 * COIN);
    const Transaction tx = MakeSpend(kFund, 49 * COIN);
    CHECK(node.BroadcastTx(tx, error));

    RpcServer server(node, 0, "", &nodeMutex);
    server.SetAuthEnabled(false);
    CHECK(server.Start(error));
    const uint16_t port = server.Port();
    CHECK(port != 0);

    // Before any load attempt, `loaded` is false.
    Json info = RpcCall(port, "getmempoolinfo", "[]", error);
    CHECK(error.empty());
    CHECK(info.Get("loaded") != nullptr);
    CHECK(info.Get("loaded")->AsBool(true) == false);
    CHECK(info.Get("size")->AsInt() == 1);
    // The help list names both new methods.
    Json help = RpcCall(port, "help", "[]", error);
    CHECK(error.empty());
    CHECK(help.Dump().find("savemempool") != std::string::npos);
    CHECK(help.Dump().find("importmempool") != std::string::npos);
    // No file yet: the load attempt finishes empty, `loaded`
    // becomes true (a missing file is not an error).
    CHECK(node.LoadMempool(error));
    CHECK(node.MempoolLoaded());
    info = RpcCall(port, "getmempoolinfo", "[]", error);
    CHECK(error.empty());
    CHECK(info.Get("loaded")->AsBool(false) == true);
    // savemempool writes the file (null result, like Core).
    Json saved = RpcCall(port, "savemempool", "[]", error);
    CHECK(error.empty());
    CHECK(saved.IsNull());
    CHECK(std::filesystem::exists(mpPath));
    // The file round-trips: a fresh mempool loads the one tx.
    {
      Mempool reloaded;
      MempoolLoadStats stats;
      CHECK(LoadMempoolFile(mpPath, reloaded, node.Utxo(), 1, kTime,
                            kNow, stats, error));
      CHECK(stats.loaded == 1);
      CHECK(reloaded.Contains(tx.GetTxId()));
    }
    server.Stop();
    CleanupPath(dir);
  }

  test::BeginSection("m17a: persistmempool=0 opt-out — no file written, "
                     "loaded false");
  {
    // Config parsing: CLI `--persistmempool=0` (Core-style),
    // `--no-persistmempool`, and the `persistmempool` config key.
    {
      AppConfig config;
      std::string error;
      char* argv[] = {(char*)"oeeoo-node", (char*)"--persistmempool=0"};
      CHECK(ParseArgs(2, argv, config, error));
      CHECK(config.persistMempool == false);
    }
    {
      AppConfig config;
      std::string error;
      char* argv[] = {(char*)"oeeoo-node", (char*)"--no-persistmempool"};
      CHECK(ParseArgs(2, argv, config, error));
      CHECK(config.persistMempool == false);
    }
    {
      AppConfig config;
      std::string error;
      char* argv[] = {(char*)"oeeoo-node", (char*)"--persistmempool=1"};
      CHECK(ParseArgs(2, argv, config, error));
      CHECK(config.persistMempool == true);
    }
    {
      const std::string confPath = TempPath("optout.conf");
      CleanupPath(confPath);
      CHECK(WriteFile(confPath, "persistmempool=0\n"));
      AppConfig config;
      std::string error;
      CHECK(LoadConfigFile(confPath, config, error));
      CHECK(config.persistMempool == false);
      CleanupPath(confPath);
    }
    // Default is ON.
    CHECK(AppConfig().persistMempool == true);

    // Node level: with persistence off, the load is skipped
    // (`loaded` stays false) and the shutdown save writes
    // nothing.
    const std::string dir = TempPath("optout");
    CleanupPath(dir);
    std::filesystem::create_directories(dir);
    const std::string mpPath = dir + "/mempool.dat";
    const Block genesis = MineGenesis();
    Node node(kTestBits);
    node.SetTime(kNow);
    node.SetTransport(
        std::make_shared<LoopbackTransport>("loopback:m17a-optout"));
    node.SetMempoolPath(mpPath);
    node.SetPersistMempool(false);
    CHECK(node.PersistMempool() == false);
    std::string error;
    CHECK(node.BroadcastBlock(genesis, error));
    const Hash256 kFund = TxIdWith(0xd1);
    Fund(node.Utxo(), kFund, 50 * COIN);
    const Transaction tx = MakeSpend(kFund, 49 * COIN);
    CHECK(node.BroadcastTx(tx, error));
    CHECK(node.MempoolSize() == 1);
    CHECK(!node.LoadMempool(error));  // skipped, not attempted
    CHECK(node.MempoolLoaded() == false);
    CHECK(!node.SaveMempool(error));  // skipped, nothing written
    CHECK(!std::filesystem::exists(mpPath));
    CHECK(!std::filesystem::exists(mpPath + ".new"));
    CleanupPath(dir);
  }

  test::BeginSection("m17a: invalid tx dropped on load — its input was "
                     "confirmed meanwhile");
  {
    const std::string dir = TempPath("invalid");
    CleanupPath(dir);
    std::filesystem::create_directories(dir);
    const Block genesis = MineGenesis();
    const std::string dbPath = dir + "/chain.db";
    const std::string mpPath = dir + "/mempool.dat";
    Hash256 txAId;
    Hash256 block1Coinbase;  // block-1 coinbase txid, spent below
    {
      ChainStore store;
      std::string error;
      CHECK(store.Open(dbPath, error));
      auto node = std::make_unique<Node>(kTestBits);
      node->SetTime(kNow);
      node->SetTransport(
          std::make_shared<LoopbackTransport>("loopback:m17a-inv"));
      node->AttachStore(&store);
      node->SetMempoolPath(mpPath);
      const auto mature = BuildMatureChain(*node, genesis);
      const Transaction txA = MakeSpend(mature.first, 49 * COIN);
      CHECK(node->BroadcastTx(txA, error));
      CHECK(node->MempoolSize() == 1);
      txAId = txA.GetTxId();
      block1Coinbase = mature.first;
      CHECK(node->SaveMempool(error));
    }
    {
      ChainStore store2;
      std::string error;
      CHECK(store2.Open(dbPath, error));
      auto node2 = std::make_unique<Node>(kTestBits);
      node2->SetTime(kNow);
      node2->SetTransport(
          std::make_shared<LoopbackTransport>("loopback:m17a-inv2"));
      node2->AttachStore(&store2);
      node2->SetMempoolPath(mpPath);
      CHECK(node2->LoadFromStore(store2, error));
      CHECK(node2->TipHeight() == 101);
      // Confirm a DIFFERENT spend of the same coinbase output in
      // block 102, so the saved mempool tx's input is gone.
      const Transaction conflict =
          MakeSpend(block1Coinbase, 48 * COIN);  // fee 2 COIN
      CHECK(conflict.GetTxId() != txAId);
      const Block b102 = MakeBlock(node2->TipHash(), 102, kTime + 102 * 600,
                                   {conflict});
      CHECK(node2->BroadcastBlock(b102, error));
      CHECK(node2->TipHeight() == 102);
      // Now load the saved mempool: txA's input is spent, so it
      // must be dropped as invalid.
      CHECK(node2->LoadMempool(error));
      CHECK(node2->MempoolLoaded() == true);
      const MempoolLoadStats stats = node2->LastMempoolLoadStats();
      CHECK(stats.loaded == 0);
      CHECK(stats.droppedInvalid == 1);
      CHECK(stats.droppedExpired == 0);
      CHECK(node2->MempoolSize() == 0);
      CHECK(!node2->HasTx(txAId));
      store2.Close();
    }
    CleanupPath(dir);
  }

  test::BeginSection("m17a: importmempool round-trip (node + RPC)");
  {
    const std::string dir = TempPath("import");
    CleanupPath(dir);
    std::filesystem::create_directories(dir);
    const std::string mpPath = dir + "/mempool.dat";
    const Block genesis = MineGenesis();
    std::string error;

    // Source node: one tx in the mempool, dumped to a file.
    Node source(kTestBits);
    source.SetTime(kNow);
    source.SetTransport(
        std::make_shared<LoopbackTransport>("loopback:m17a-imp-src"));
    source.SetMempoolPath(mpPath);
    CHECK(source.BroadcastBlock(genesis, error));
    const Hash256 kFund = TxIdWith(0xe1);
    Fund(source.Utxo(), kFund, 50 * COIN);
    const Transaction tx = MakeSpend(kFund, 49 * COIN);
    CHECK(source.BroadcastTx(tx, error));
    CHECK(source.DumpMempool(error));  // explicit dump, on demand
    CHECK(std::filesystem::exists(mpPath));

    // Destination node: same genesis + same funding coin (its own
    // UTXO view), empty mempool. Import merges the file's tx in.
    std::mutex nodeMutex;
    Node dest(kTestBits);
    dest.SetTime(kNow);
    dest.SetTransport(
        std::make_shared<LoopbackTransport>("loopback:m17a-imp-dst"));
    CHECK(dest.BroadcastBlock(genesis, error));
    Fund(dest.Utxo(), kFund, 50 * COIN);
    CHECK(dest.MempoolSize() == 0);
    MempoolLoadStats stats;
    CHECK(dest.ImportMempoolFile(mpPath, stats, error));
    CHECK(stats.loaded == 1);
    CHECK(stats.droppedInvalid == 0);
    CHECK(dest.MempoolSize() == 1);
    CHECK(dest.HasTx(tx.GetTxId()));
    Mempool::Entry e;
    CHECK(dest.MempoolRef().GetEntry(tx.GetTxId(), e));
    CHECK(e.fee == 1 * COIN);
    // Importing the same file again is a no-op for the duplicate
    // (already in the mempool): it is counted dropped-invalid.
    MempoolLoadStats stats2;
    CHECK(dest.ImportMempoolFile(mpPath, stats2, error));
    CHECK(stats2.loaded == 0);
    CHECK(stats2.droppedInvalid == 1);
    CHECK(dest.MempoolSize() == 1);

    // The RPC form: importmempool returns the load counts.
    RpcServer server(dest, 0, "", &nodeMutex);
    server.SetAuthEnabled(false);
    CHECK(server.Start(error));
    const uint16_t port = server.Port();
    // A second file (a different tx, funded in `dest`'s UTXO
    // view) imports via the RPC into the same node.
    const std::string mpPath2 = dir + "/mempool2.dat";
    {
      Mempool second;
      const Hash256 kFund2 = TxIdWith(0xe2);
      Fund(dest.Utxo(), kFund2, 50 * COIN);
      const Transaction tx2 = MakeSpend(kFund2, 49 * COIN);
      CHECK(second.AddTransaction(tx2, dest.Utxo(), 1, kTime, kNow,
                                  error));
      CHECK(SaveMempoolFile(mpPath2, second, error));
    }
    Json params = Json::Array();
    params.Push(mpPath2);
    Json imported = RpcCall(port, "importmempool", params.Dump(), error);
    CHECK(error.empty());
    CHECK(imported.Get("loaded") != nullptr);
    CHECK(imported.Get("loaded")->AsInt() == 1);
    CHECK(imported.Get("dropped_invalid")->AsInt() == 0);
    CHECK(dest.MempoolSize() == 2);
    // Missing file: the RPC fails loudly (-32603), it does not
    // silently import nothing.
    Json bad = RpcCall(port, "importmempool",
                       Json::Array().Dump(), error);
    CHECK(!error.empty());  // no params -> -32602
    (void)bad;
    Json paramsMissing = Json::Array();
    paramsMissing.Push(dir + "/no-such-file.dat");
    Json missing = RpcCall(port, "importmempool", paramsMissing.Dump(),
                           error);
    CHECK(!error.empty());  // no such file -> -32603
    (void)missing;
    server.Stop();
    CleanupPath(dir);
  }
}
