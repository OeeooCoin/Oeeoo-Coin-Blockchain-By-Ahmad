// Tests: Milestone 16 Stream A — getchaintips, getblockstats,
// getchaintxstats, getblocktemplate real sigops + real fees.
// (Stream B/C own the wallet-history and RPC-load-control parts of
// M16; this file covers only Stream A's scope.)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "consensus/consensus.h"
#include "consensus/subsidy.h"
#include "crypto/ecdsa.h"
#include "mining/miner.h"
#include "network/node.h"
#include "network/transport.h"
#include "rpc/json.h"
#include "rpc/rpcserver.h"
#include "rpc/rpcutil.h"
#include "script/script.h"
#include "test_harness.h"
#include "transactions/sighash.h"
#include "wallet/wallet.h"

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

// Minimal HTTP client for the RPC tests (same shape as test_rpc).
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
  std::string request = "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: " +
                        std::to_string(body.size()) +
                        "\r\nConnection: close\r\n\r\n" + body;
  std::size_t sent = 0;
  while (sent < request.size()) {
    const ssize_t n = ::send(fd, request.data() + sent,
                             request.size() - sent, 0);
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

Json RpcCall(uint16_t port, const std::string& method, const Json& params) {
  Json request = Json::Object();
  request.Set("jsonrpc", "2.0");
  request.Set("id", 1);
  request.Set("method", method);
  request.Set("params", params);
  HttpResult r = HttpPost(port, request.Dump());
  CHECK(r.status == 200);
  Json response;
  std::string error;
  CHECK(Json::Parse(r.body, response, error));
  return response;
}

Json RpcResult(const Json& response) {
  CHECK(response.Has("result"));
  CHECK(!response.Has("error") || response.Get("error")->IsNull());
  const Json* result = response.Get("result");
  if (result == nullptr) return Json();
  return *result;
}

bool RpcHasError(const Json& response) {
  const Json* err = response.Get("error");
  return err != nullptr && !err->IsNull();
}

int64_t RpcErrorCode(const Json& response) {
  const Json* err = response.Get("error");
  if (err == nullptr) return 0;
  const Json* code = err->Get("code");
  return code == nullptr ? 0 : code->AsInt();
}

// Find a getchaintips entry by block hash; null when absent.
const Json* FindTip(const Json& tips, const std::string& hashHex) {
  for (const Json& item : tips.AsArray()) {
    const Json* h = item.Get("hash");
    if (h != nullptr && h->AsString() == hashHex) return &item;
  }
  return nullptr;
}

bool Near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps;
}

std::string MinerAddress() {
  Wallet wallet;
  std::string mnemonic;
  for (int i = 0; i < 23; ++i) mnemonic += "abandon ";
  mnemonic += "art";
  std::string error;
  CHECK(wallet.RestoreFromMnemonic(mnemonic, "", error));
  WalletAddress addr = wallet.NewAddress(error);
  CHECK(!addr.address.empty());
  return addr.address;
}
}  // namespace

void TestMilestone16A() {
  test::BeginSection("m16a: CountSigOps P2PKH, multisig, fallback");
  {
    EnsureKeys();
    // P2PKH output: exactly one CHECKSIG.
    CHECK(CountSigOps(Spk()) == 1);
    // CHECKSIGVERIFY counts 1 as well.
    CHECK(CountSigOps(std::vector<unsigned char>{OP_CHECKSIGVERIFY}) == 1);
    // A bare 2-of-3 multisig counts its 3 pubkeys, not 20.
    std::vector<std::vector<unsigned char>> keys;
    for (int i = 0; i < 3; ++i) {
      KeyPair k;
      CHECK(GenerateKeyPair(k));
      keys.push_back(k.pubkey);
    }
    CHECK(CountSigOps(CreateMultisigScriptPubKey(2, keys)) == 3);
    CHECK(CountSigOps(CreateMultisigScriptPubKey(1, {keys[0]})) == 1);
    // CHECKMULTISIGVERIFY variant of a bare 3-of-3.
    std::vector<unsigned char> msVerify =
        CreateMultisigScriptPubKey(3, keys);
    msVerify.back() = OP_CHECKMULTISIGVERIFY;
    CHECK(CountSigOps(msVerify) == 3);
    // A lone CHECKMULTISIG is not a bare multisig: 20.
    CHECK(CountSigOps(std::vector<unsigned char>{OP_CHECKMULTISIG}) == 20);
    CHECK(CountSigOps(std::vector<unsigned char>{OP_CHECKMULTISIGVERIFY}) ==
          20);
    // P2PKH script + a stray CHECKMULTISIG is not bare either:
    // 1 (CHECKSIG) + 20 = 21.
    std::vector<unsigned char> mixed = Spk();
    mixed.push_back(OP_CHECKMULTISIG);
    CHECK(CountSigOps(mixed) == 21);
    // Push-only scriptSig: 0.
    KeyPair k;
    CHECK(GenerateKeyPair(k));
    std::vector<unsigned char> sig = {0x01, 0x02, 0x03};
    CHECK(CountSigOps(CreateP2PKHScriptSig(sig, k.pubkey)) == 0);
    // Unparsable script falls back to a raw byte scan: a truncated
    // push whose data byte is 0xac (OP_CHECKSIG) still counts 1.
    CHECK(CountSigOps(std::vector<unsigned char>{0x02, OP_CHECKSIG}) == 1);
    // Per-tx: a P2PKH spend is 1 (output) + 0 (scriptSig).
    const Hash256 fund = TxIdWith(0x41);
    Transaction spend = MakeSpend(fund, 49 * COIN);
    CHECK(CountSigOps(spend) == 1);
    // Per-tx with a bare-multisig output: 3 (output) + 0 (input).
    Transaction msSpend = MakeSpend(fund, 49 * COIN);
    msSpend.vout[0].scriptPubKey = CreateMultisigScriptPubKey(2, keys);
    CHECK(CountSigOps(msSpend) == 3);
    // Coinbase inputs are never counted: a coinbase whose scriptSig
    // carries a raw 0xac byte still counts only its P2PKH output.
    Transaction cb = CreateCoinbase(1, Spk(), 50 * COIN,
                                    std::string(1, '\xac'));
    CHECK(CountSigOps(cb) == 1);
  }

  test::BeginSection("m16a: getchaintips incl forked tip");
  {
    EnsureKeys();
    std::mutex nodeMutex;
    Node node(kTestBits);
    node.SetTime(kNow);
    std::string error;
    const Hash256 fund1 = TxIdWith(0x51);
    const Hash256 fund2 = TxIdWith(0x52);
    Fund(node.Utxo(), fund1, 50 * COIN);
    Fund(node.Utxo(), fund2, 50 * COIN);
    RpcServer server(node, 0, "", &nodeMutex);
    server.SetAuthEnabled(false);
    CHECK(server.Start(error));
    const uint16_t port = server.Port();

    Block genesis = MineGenesis();
    CHECK(node.BroadcastBlock(genesis, error));
    // One tip: the genesis block, active, branchlen 0.
    Json tips = RpcResult(RpcCall(port, "getchaintips", Json::Array()));
    CHECK(tips.IsArray() && tips.AsArray().size() == 1);
    CHECK(FindTip(tips, HashHex(genesis.GetHash())) != nullptr);
    const Json* g = FindTip(tips, HashHex(genesis.GetHash()));
    CHECK(g->Get("height")->AsInt() == 0);
    CHECK(g->Get("branchlen")->AsInt() == 0);
    CHECK(g->Get("status")->AsString() == "active");

    Block m1 = MakeBlock(genesis.GetHash(), 1, kTime + 600, {});
    CHECK(node.BroadcastBlock(m1, error));
    Transaction tx1 = MakeSpend(fund1, 49 * COIN);
    Block m2 = MakeBlock(m1.GetHash(), 2, kTime + 1200, {tx1});
    CHECK(node.BroadcastBlock(m2, error));
    Transaction tx3 = MakeSpend(fund2, 49 * COIN);
    Block m3 = MakeBlock(m2.GetHash(), 3, kTime + 1800, {tx3});
    CHECK(node.BroadcastBlock(m3, error));
    CHECK(node.TipHeight() == 3);
    // Fork from m1: f2 spends fund1 differently, f3' is empty.
    // The fork ties the main chain at height 3, so the tip stays
    // on the main chain (strictly-greater-work rule).
    Transaction tx2 = MakeSpend(fund1, 48 * COIN);
    Block f2 = MakeBlock(m1.GetHash(), 2, kTime + 1500, {tx2});
    CHECK(node.BroadcastBlock(f2, error));
    Block f3 = MakeBlock(f2.GetHash(), 3, kTime + 2100, {});
    CHECK(node.BroadcastBlock(f3, error));
    CHECK(node.TipHash() == m3.GetHash());
    tips = RpcResult(RpcCall(port, "getchaintips", Json::Array()));
    CHECK(tips.IsArray() && tips.AsArray().size() == 2);
    const Json* mainTip = FindTip(tips, HashHex(m3.GetHash()));
    const Json* forkTip = FindTip(tips, HashHex(f3.GetHash()));
    CHECK(mainTip != nullptr && forkTip != nullptr);
    CHECK(mainTip->Get("height")->AsInt() == 3);
    CHECK(mainTip->Get("branchlen")->AsInt() == 0);
    CHECK(mainTip->Get("status")->AsString() == "active");
    CHECK(forkTip->Get("height")->AsInt() == 3);
    // Fork point is m1 (height 1): branchlen = 3 - 1 = 2.
    CHECK(forkTip->Get("branchlen")->AsInt() == 2);
    CHECK(forkTip->Get("status")->AsString() == "valid-fork");
    // f2 is no tip: it is f3's parent.
    CHECK(FindTip(tips, HashHex(f2.GetHash())) == nullptr);
    server.Stop();
  }

  test::BeginSection("m16a: getblockstats hand-computed values");
  {
    EnsureKeys();
    std::mutex nodeMutex;
    Node node(kTestBits);
    node.SetTime(kNow);
    std::string error;
    RpcServer server(node, 0, "", &nodeMutex);
    server.SetAuthEnabled(false);
    CHECK(server.Start(error));
    const uint16_t port = server.Port();

    // A chain funded ONLY by blocks, so the fee replay in
    // getblockstats can see every input: genesis + 100 empty
    // blocks, then block 101 spends block 1's coinbase output
    // (50 COIN, mature at height 101: 101 - 1 = 100).
    Block genesis = MineGenesis();
    CHECK(node.BroadcastBlock(genesis, error));
    std::vector<Block> chain;
    chain.push_back(genesis);
    for (int h = 1; h <= 100; ++h) {
      Block b = MakeBlock(chain.back().GetHash(), h,
                          kTime + static_cast<uint32_t>(h) * 600, {});
      CHECK(node.BroadcastBlock(b, error));
      chain.push_back(b);
    }
    const Block& b1 = chain[1];
    Transaction spend = MakeSpend(b1.vtx[0].GetTxId(), 49 * COIN);
    Block b101 = MakeBlock(chain.back().GetHash(), 101, kTime + 101 * 600,
                           {spend});
    CHECK(node.BroadcastBlock(b101, error));
    CHECK(node.TipHeight() == 101);

    // Hand-computed expectations for block 101:
    // fee = 50 COIN in - 49 COIN out = 1 COIN = 1.0 OEEOO;
    // non-coinbase bytes = the spend's serialized size;
    // avgfeerate = 100,000,000 sats / spend bytes;
    // size = 80 (header) + 1 (tx-count varint) + coinbase size +
    // spend size; mediantime = median of the last 11 block times
    // (heights 91..101, times kTime+91*600 .. kTime+101*600) =
    // kTime + 96*600.
    const int64_t spendBytes =
        static_cast<int64_t>(spend.Serialize().size());
    const double expectedFeeRate =
        100000000.0 / static_cast<double>(spendBytes);
    const int64_t expectedSize =
        80 + 1 + static_cast<int64_t>(b101.vtx[0].Serialize().size()) +
        spendBytes;
    Json params = Json::Array();
    params.Push(HashHex(b101.GetHash()));
    Json stats = RpcResult(RpcCall(port, "getblockstats", params));
    CHECK(stats.Get("height")->AsInt() == 101);
    CHECK(stats.Get("hash")->AsString() == HashHex(b101.GetHash()));
    CHECK(stats.Get("txs")->AsInt() == 2);
    CHECK(stats.Get("size")->AsInt() == expectedSize);
    CHECK(Near(stats.Get("totalfee")->AsDouble(), 1.0));
    CHECK(Near(stats.Get("avgfeerate")->AsDouble(), expectedFeeRate));
    CHECK(stats.Get("time")->AsInt() ==
          static_cast<int64_t>(kTime + 101 * 600));
    CHECK(stats.Get("mediantime")->AsInt() ==
          static_cast<int64_t>(kTime + 96 * 600));

    // A coinbase-only block: no fees, no non-coinbase bytes, so
    // avgfeerate is 0 (not a division by zero).
    Json p50 = Json::Array();
    p50.Push(HashHex(chain[50].GetHash()));
    Json stats50 = RpcResult(RpcCall(port, "getblockstats", p50));
    CHECK(stats50.Get("txs")->AsInt() == 1);
    CHECK(Near(stats50.Get("totalfee")->AsDouble(), 0.0));
    CHECK(Near(stats50.Get("avgfeerate")->AsDouble(), 0.0));
    CHECK(stats50.Get("mediantime")->AsInt() ==
          static_cast<int64_t>(kTime + 45 * 600));

    // A side-branch block reports its own ancestry's fees and
    // mediantime: fork one empty block off height 100 (ties the
    // main tip, so the tip does not move).
    Block f101 = MakeBlock(chain[100].GetHash(), 101, kTime + 101 * 600 + 300,
                           {});
    CHECK(node.BroadcastBlock(f101, error));
    CHECK(node.TipHash() == b101.GetHash());
    Json pf = Json::Array();
    pf.Push(HashHex(f101.GetHash()));
    Json statsF = RpcResult(RpcCall(port, "getblockstats", pf));
    CHECK(statsF.Get("height")->AsInt() == 101);
    CHECK(statsF.Get("txs")->AsInt() == 1);
    CHECK(Near(statsF.Get("totalfee")->AsDouble(), 0.0));
    // Last 11 times on the fork ancestry: heights 91..100 at
    // kTime+h*600 plus f101 at kTime+101*600+300; the median
    // (6th of 11 sorted) is kTime+96*600.
    CHECK(statsF.Get("mediantime")->AsInt() ==
          static_cast<int64_t>(kTime + 96 * 600));

    // Unknown hash: error -5.
    Json pBad = Json::Array();
    pBad.Push(HashHex(TxIdWith(0x99)));
    Json bad = RpcCall(port, "getblockstats", pBad);
    CHECK(RpcHasError(bad));
    CHECK(RpcErrorCode(bad) == -5);
    server.Stop();
  }

  test::BeginSection("m16a: getchaintxstats window math");
  {
    EnsureKeys();
    std::mutex nodeMutex;
    Node node(kTestBits);
    node.SetTime(kNow);
    std::string error;
    RpcServer server(node, 0, "", &nodeMutex);
    server.SetAuthEnabled(false);
    CHECK(server.Start(error));
    const uint16_t port = server.Port();

    // Same block-funded shape as the stats section: genesis +
    // 100 empty blocks + block 101 with one spend = 102 blocks,
    // 103 transactions total.
    Block genesis = MineGenesis();
    CHECK(node.BroadcastBlock(genesis, error));
    std::vector<Block> chain;
    chain.push_back(genesis);
    for (int h = 1; h <= 100; ++h) {
      Block b = MakeBlock(chain.back().GetHash(), h,
                          kTime + static_cast<uint32_t>(h) * 600, {});
      CHECK(node.BroadcastBlock(b, error));
      chain.push_back(b);
    }
    Transaction spend =
        MakeSpend(chain[1].vtx[0].GetTxId(), 49 * COIN);
    Block b101 = MakeBlock(chain.back().GetHash(), 101, kTime + 101 * 600,
                           {spend});
    CHECK(node.BroadcastBlock(b101, error));

    Json all = RpcResult(RpcCall(port, "getchaintxstats", Json::Array()));
    CHECK(all.Get("window_block_count")->AsInt() == 102);
    CHECK(all.Get("window_final_block_height")->AsInt() == 101);
    CHECK(all.Get("txcount")->AsInt() == 103);
    CHECK(all.Get("window_tx_count")->AsInt() == 103);
    CHECK(all.Get("window_interval")->AsInt() == 101 * 600);
    CHECK(all.Get("time")->AsInt() ==
          static_cast<int64_t>(kTime + 101 * 600));
    CHECK(Near(all.Get("txrate")->AsDouble(), 103.0 / (101.0 * 600.0)));

    Json p2 = Json::Array();
    p2.Push(2);
    Json two = RpcResult(RpcCall(port, "getchaintxstats", p2));
    CHECK(two.Get("window_block_count")->AsInt() == 2);
    CHECK(two.Get("window_final_block_height")->AsInt() == 101);
    // txcount is the whole-chain total even for a small window.
    CHECK(two.Get("txcount")->AsInt() == 103);
    CHECK(two.Get("window_tx_count")->AsInt() == 3);  // 1 + 2
    CHECK(two.Get("window_interval")->AsInt() == 600);
    CHECK(Near(two.Get("txrate")->AsDouble(), 3.0 / 600.0));

    // nblocks < 1 is a client error.
    Json p0 = Json::Array();
    p0.Push(0);
    Json bad = RpcCall(port, "getchaintxstats", p0);
    CHECK(RpcHasError(bad));
    CHECK(RpcErrorCode(bad) == -8);
    server.Stop();
  }

  test::BeginSection("m16a: getblocktemplate real sigops and fees");
  {
    EnsureKeys();
    std::mutex nodeMutex;
    Node node(kTestBits);
    node.SetTime(kNow);
    std::string error;
    const Hash256 fund = TxIdWith(0x61);
    Fund(node.Utxo(), fund, 50 * COIN);
    Block genesis = MineGenesis();
    CHECK(node.BroadcastBlock(genesis, error));
    // A P2PKH spend paying 49 of 50 COIN: fee 1 COIN, sigops 1.
    Transaction spend = MakeSpend(fund, 49 * COIN);
    CHECK(node.BroadcastTx(spend, error));
    RpcServer server(node, 0, "", &nodeMutex);
    server.SetAuthEnabled(false);
    CHECK(server.Start(error));
    const uint16_t port = server.Port();
    Json params = Json::Array();
    params.Push(MinerAddress());
    Json templ = RpcResult(RpcCall(port, "getblocktemplate", params));
    CHECK(templ.Get("height")->AsInt() == 1);
    CHECK(templ.Get("sigoplimit")->AsInt() == 20000);
    const Json* txs = templ.Get("transactions");
    CHECK(txs != nullptr && txs->AsArray().size() == 1);
    const Json& item = txs->AsArray()[0];
    CHECK(item.Get("txid")->AsString() == HashHex(spend.GetTxId()));
    // fee is integer satoshis (Bitcoin's unit), NOT OEEOO.
    CHECK(item.Get("fee")->IsInt());
    CHECK(item.Get("fee")->AsInt() == 100000000);
    CHECK(item.Get("sigops")->AsInt() == 1);
    // coinbasevalue = subsidy (50 COIN) + fee (1 COIN).
    CHECK(templ.Get("coinbasevalue")->AsInt() == 51 * COIN);
    server.Stop();
  }

  test::BeginSection("m16a: getblocktemplate bare multisig sigops");
  {
    EnsureKeys();
    std::mutex nodeMutex;
    Node node(kTestBits);
    node.SetTime(kNow);
    std::string error;
    const Hash256 fund = TxIdWith(0x62);
    Fund(node.Utxo(), fund, 50 * COIN);
    Block genesis = MineGenesis();
    CHECK(node.BroadcastBlock(genesis, error));
    // Spend to a bare 2-of-3 multisig output: the template tx
    // reports sigops == 3 (the output's pubkey count), fee 1 COIN.
    std::vector<std::vector<unsigned char>> keys;
    for (int i = 0; i < 3; ++i) {
      KeyPair k;
      CHECK(GenerateKeyPair(k));
      keys.push_back(k.pubkey);
    }
    Transaction spend = MakeSpendTo(fund, 49 * COIN,
                                    CreateMultisigScriptPubKey(2, keys));
    CHECK(node.BroadcastTx(spend, error));
    RpcServer server(node, 0, "", &nodeMutex);
    server.SetAuthEnabled(false);
    CHECK(server.Start(error));
    const uint16_t port = server.Port();
    Json params = Json::Array();
    params.Push(MinerAddress());
    Json templ = RpcResult(RpcCall(port, "getblocktemplate", params));
    const Json* txs = templ.Get("transactions");
    CHECK(txs != nullptr && txs->AsArray().size() == 1);
    const Json& item = txs->AsArray()[0];
    CHECK(item.Get("txid")->AsString() == HashHex(spend.GetTxId()));
    CHECK(item.Get("fee")->AsInt() == 100000000);
    CHECK(item.Get("sigops")->AsInt() == 3);
    server.Stop();
  }
}
