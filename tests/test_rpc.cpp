// Tests: minimal JSON + JSON-RPC server (Milestone 10).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "consensus/consensus.h"
#include "crypto/ecdsa.h"
#include "mining/miner.h"
#include "network/node.h"
#include "network/transport.h"
#include "rpc/json.h"
#include "rpc/rpcserver.h"
#include "rpc/rpcutil.h"
#include "script/script.h"
#include "test_harness.h"
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

// Minimal HTTP client for the RPC tests.
struct HttpResult {
  int status = 0;
  std::string body;
};

HttpResult HttpPost(uint16_t port, const std::string& body,
                    const std::string& authHeader = "") {
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
                        std::to_string(body.size()) + "\r\n";
  if (!authHeader.empty()) {
    request += "Authorization: " + authHeader + "\r\n";
  }
  request += "Connection: close\r\n\r\n" + body;
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

// One JSON-RPC call over HTTP. Returns the parsed response object.
Json RpcCall(uint16_t port, const std::string& method, const Json& params,
             const std::string& authHeader = "") {
  Json request = Json::Object();
  request.Set("jsonrpc", "2.0");
  request.Set("id", 1);
  request.Set("method", method);
  request.Set("params", params);
  HttpResult r = HttpPost(port, request.Dump(), authHeader);
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

std::string HexOf(const std::vector<unsigned char>& bytes) {
  return BytesToHexStr(bytes);
}

std::string HashHex(const Hash256& h) {
  return BytesToHexStr(std::vector<unsigned char>(h.begin(), h.end()));
}
}  // namespace

void TestRpc() {
  test::BeginSection("rpc: minimal JSON");
  {
    Json j;
    std::string error;
    CHECK(Json::Parse(
        R"({"a":1,"b":[true,false,null],"c":"x\ny\u00e9\ud83d\ude00"})",
        j, error));
    CHECK(j.Get("a")->AsInt() == 1);
    CHECK(j.Get("b")->AsArray().size() == 3);
    CHECK(j.Get("b")->AsArray()[0].AsBool());
    CHECK(j.Get("c")->AsString() == "x\ny\xc3\xa9\xf0\x9f\x98\x80");
    CHECK(j.Dump() ==
          "{\"a\":1,\"b\":[true,false,null],\"c\":\"x\\ny\xc3\xa9"
          "\xf0\x9f\x98\x80\"}");
    // Numbers: integers stay integers, fractions become doubles.
    CHECK(Json::Parse("42", j, error));
    CHECK(j.IsInt() && j.AsInt() == 42);
    CHECK(Json::Parse("-3.5e2", j, error));
    CHECK(j.IsDouble() && j.AsDouble() == -350.0);
    // Errors.
    CHECK(!Json::Parse("{\"a\":1} trailing", j, error));
    CHECK(!Json::Parse("\"unterminated", j, error));
    CHECK(!Json::Parse("{\"a\":01}", j, error));
    CHECK(!Json::Parse("[1,]", j, error));
    CHECK(!Json::Parse("\"bad \\q\"", j, error));
    CHECK(!Json::Parse("\"\\ud800\"", j, error));  // unpaired surrogate
    Json empty = Json::Object();
    empty.Set("x", Json::Array());
    CHECK(empty.Dump() == "{\"x\":[]}");
  }

  test::BeginSection("rpc: server methods + wallet");
  {
    EnsureKeys();
    std::mutex nodeMutex;
    Node node(kTestBits);
    node.SetTime(kNow);
    Block genesis = MineGenesis();
    std::string error;
    CHECK(node.BroadcastBlock(genesis, error));

    // Wallet funded with a regular (non-coinbase) UTXO: a coinbase
    // would need 100 blocks to mature before it could be spent.
    Wallet wallet;
    std::string mnemonic;
    for (int i = 0; i < 23; ++i) mnemonic += "abandon ";
    mnemonic += "art";
    CHECK(wallet.RestoreFromMnemonic(mnemonic, "", error));
    std::string werror;
    WalletAddress minerAddr = wallet.NewAddress(werror);
    CHECK(!minerAddr.address.empty());
    {
      Hash256 fundTxid;
      fundTxid.fill(0xAB);
      Coin fundCoin;
      fundCoin.out.value = 50 * COIN;
      fundCoin.out.scriptPubKey = minerAddr.scriptPubKey;
      fundCoin.height = 1;
      fundCoin.blockTime = kTime + 600;
      fundCoin.isCoinbase = false;
      CHECK(node.Utxo().AddCoin({fundTxid, 0}, fundCoin));
    }
    BlockTemplate templ = AssembleBlock(genesis.GetHash(), 1, kTime + 600,
                                        kTestBits, node.MempoolRef(),
                                        node.Utxo(), kTime, Spk());
    Block b1 = templ.block;
    CHECK(MineBlock(b1, 1, ""));
    CHECK(node.BroadcastBlock(b1, error));
    CHECK(node.TipHeight() == 1);

    RpcServer server(node, 0, "", &nodeMutex);
    server.AttachWallet(&wallet);
    server.SetAuthEnabled(false);
    CHECK(server.Start(error));
    const uint16_t port = server.Port();
    CHECK(port != 0);

    // Blockchain methods.
    Json count = RpcResult(RpcCall(port, "getblockcount", Json::Array()));
    CHECK(count.AsInt() == 1);
    Json best = RpcResult(RpcCall(port, "getbestblockhash", Json::Array()));
    CHECK(best.AsString() == HashHex(b1.GetHash()));
    Json params = Json::Array();
    params.Push(1);
    Json byHeight = RpcResult(RpcCall(port, "getblockhash", params));
    CHECK(byHeight.AsString() == HashHex(b1.GetHash()));
    params = Json::Array();
    params.Push(HashHex(b1.GetHash()));
    params.Push(1);
    Json block = RpcResult(RpcCall(port, "getblock", params));
    CHECK(block.Get("height")->AsInt() == 1);
    CHECK(block.Get("nTx")->AsInt() == 1);
    CHECK(block.Get("tx")->AsArray().size() == 1);
    Json info = RpcResult(RpcCall(port, "getblockchaininfo", Json::Array()));
    CHECK(info.Get("blocks")->AsInt() == 1);
    CHECK(info.Get("chain")->AsString() == "main");
    Json difficulty = RpcResult(RpcCall(port, "getdifficulty", Json::Array()));
    CHECK(difficulty.AsDouble() > 0 && difficulty.AsDouble() < 1e-9);
    // gettxout: the funding coinbase output (50 OEEOO).
    params = Json::Array();
    params.Push(HashHex(b1.vtx[0].GetTxId()));
    params.Push(0);
    Json txout = RpcResult(RpcCall(port, "gettxout", params));
    CHECK(!txout.IsNull());
    CHECK(txout.Get("value")->AsDouble() == 50.0);
    Json utxoInfo =
        RpcResult(RpcCall(port, "gettxoutsetinfo", Json::Array()));
    // 50 (wallet funding UTXO) + 50 (block 1 coinbase) + 0 (genesis).
    CHECK(utxoInfo.Get("total_amount")->AsDouble() == 100.0);
    // Mempool is empty.
    Json mempool =
        RpcResult(RpcCall(port, "getmempoolinfo", Json::Array()));
    CHECK(mempool.Get("size")->AsInt() == 0);

    // Wallet methods.
    Json balance = RpcResult(RpcCall(port, "getbalance", Json::Array()));
    CHECK(balance.AsDouble() == 50.0);
    Json unspent = RpcResult(RpcCall(port, "listunspent", Json::Array()));
    CHECK(unspent.AsArray().size() == 1);
    CHECK(unspent.AsArray()[0].Get("amount")->AsDouble() == 50.0);
    Json newAddr = RpcResult(RpcCall(port, "getnewaddress", Json::Array()));
    CHECK(newAddr.AsString().size() == 34);
    CHECK(newAddr.AsString()[0] == '7');

    // sendrawtransaction: the wallet sends 1 OEEOO to a new address.
    Transaction tx;
    Amount fee = 0;
    CHECK(wallet.CreateTransaction(newAddr.AsString(), 1 * COIN, node.Utxo(),
                                   2, node.Chain().GetMedianTimePast(), tx,
                                   fee, error, 1));
    params = Json::Array();
    params.Push(HexOf(tx.Serialize()));
    Json sent = RpcResult(RpcCall(port, "sendrawtransaction", params));
    CHECK(sent.AsString() == HashHex(tx.GetTxId()));
    Json rawpool = RpcResult(RpcCall(port, "getrawmempool", Json::Array()));
    CHECK(rawpool.AsArray().size() == 1);
    // decoderawtransaction + getrawtransaction.
    params = Json::Array();
    params.Push(HexOf(tx.Serialize()));
    Json decoded = RpcResult(RpcCall(port, "decoderawtransaction", params));
    CHECK(decoded.Get("txid")->AsString() == HashHex(tx.GetTxId()));
    CHECK(decoded.Get("vout")->AsArray().size() == 2);
    params = Json::Array();
    params.Push(HashHex(tx.GetTxId()));
    Json rawtx = RpcResult(RpcCall(port, "getrawtransaction", params));
    CHECK(rawtx.AsString() == HexOf(tx.Serialize()));
    // getblocktemplate: includes the pending tx, coinbase >= subsidy.
    Json templ2 = RpcResult(RpcCall(port, "getblocktemplate", Json::Array()));
    CHECK(templ2.Get("height")->AsInt() == 2);
    CHECK(templ2.Get("transactions")->AsArray().size() == 1);
    CHECK(templ2.Get("coinbasevalue")->AsInt() >= 50 * COIN);
    CHECK(!templ2.Get("target")->AsString().empty());
    // submitblock: mine the template and submit it.
    BlockTemplate templ3 = AssembleBlock(
        b1.GetHash(), 2, kTime + 1200, kTestBits, node.MempoolRef(),
        node.Utxo(), node.Chain().GetMedianTimePast(),
        minerAddr.scriptPubKey);
    Block b2 = templ3.block;
    CHECK(MineBlock(b2, 2, ""));
    params = Json::Array();
    params.Push(HexOf(b2.Serialize()));
    Json submitted = RpcResult(RpcCall(port, "submitblock", params));
    CHECK(submitted.IsNull());  // null = accepted
    count = RpcResult(RpcCall(port, "getblockcount", Json::Array()));
    CHECK(count.AsInt() == 2);
    // Network methods.
    Json peers = RpcResult(RpcCall(port, "getpeerinfo", Json::Array()));
    CHECK(peers.AsArray().empty());
    Json conns =
        RpcResult(RpcCall(port, "getconnectioncount", Json::Array()));
    CHECK(conns.AsInt() == 0);
    Json netinfo = RpcResult(RpcCall(port, "getnetworkinfo", Json::Array()));
    CHECK(netinfo.Get("protocolversion")->AsInt() == 70015);
    Json mining = RpcResult(RpcCall(port, "getmininginfo", Json::Array()));
    CHECK(mining.Get("blocks")->AsInt() == 2);
    CHECK(mining.Get("pooledtx")->AsInt() == 0);
    // sendtoaddress via the wallet.
    params = Json::Array();
    params.Push(newAddr.AsString());
    params.Push("1.5");
    Json sent2 = RpcResult(RpcCall(port, "sendtoaddress", params));
    CHECK(sent2.AsString().size() == 64);
    // Batch request.
    Json batch = Json::Array();
    Json r1 = Json::Object();
    r1.Set("jsonrpc", "2.0");
    r1.Set("id", 1);
    r1.Set("method", "getblockcount");
    r1.Set("params", Json::Array());
    Json r2 = Json::Object();
    r2.Set("jsonrpc", "2.0");
    r2.Set("id", 2);
    r2.Set("method", "getbestblockhash");
    r2.Set("params", Json::Array());
    batch.Push(r1);
    batch.Push(r2);
    HttpResult batchResult = HttpPost(port, batch.Dump());
    CHECK(batchResult.status == 200);
    Json batchResponse;
    CHECK(Json::Parse(batchResult.body, batchResponse, error));
    CHECK(batchResponse.AsArray().size() == 2);
    // Unknown method + bad params.
    Json unknown = RpcCall(port, "nosuchmethod", Json::Array());
    CHECK(RpcHasError(unknown));
    CHECK(unknown.Get("error")->Get("code")->AsInt() == -32601);
    params = Json::Array();
    params.Push(99999);
    Json badHeight = RpcCall(port, "getblockhash", params);
    CHECK(RpcHasError(badHeight));
    // help + stop.
    Json help = RpcResult(RpcCall(port, "help", Json::Array()));
    bool found = false;
    for (const Json& m : help.AsArray()) {
      if (m.AsString() == "getblockcount") found = true;
    }
    CHECK(found);
    Json stopped = RpcResult(RpcCall(port, "stop", Json::Array()));
    CHECK(server.StopRequested());
    server.Stop();
  }

  test::BeginSection("rpc: cookie auth");
  {
    EnsureKeys();
    std::mutex nodeMutex;
    Node node(kTestBits);
    node.SetTime(kNow);
    Block genesis = MineGenesis();
    std::string error;
    CHECK(node.BroadcastBlock(genesis, error));
    // A temp data dir for the cookie file.
    const std::string dataDir = "/tmp/oeeoo-rpc-auth-test";
    CHECK(::system(("mkdir -p " + dataDir).c_str()) == 0);
    RpcServer server(node, 0, dataDir, &nodeMutex);
    CHECK(server.Start(error));
    const uint16_t port = server.Port();
    // No credentials -> 401.
    Json request = Json::Object();
    request.Set("jsonrpc", "2.0");
    request.Set("id", 1);
    request.Set("method", "getblockcount");
    request.Set("params", Json::Array());
    HttpResult noAuth = HttpPost(port, request.Dump());
    CHECK(noAuth.status == 401);
    // Wrong credentials -> 401.
    HttpResult wrongAuth =
        HttpPost(port, request.Dump(),
                 "Basic " + Base64EncodeStr("__cookie__:wrong"));
    CHECK(wrongAuth.status == 401);
    // Right cookie -> 200.
    HttpResult okAuth =
        HttpPost(port, request.Dump(),
                 "Basic " + Base64EncodeStr("__cookie__:" + server.Cookie()));
    CHECK(okAuth.status == 200);
    // The cookie file on disk matches.
    FILE* f = std::fopen((dataDir + "/.cookie").c_str(), "r");
    CHECK(f != nullptr);
    if (f == nullptr) return;
    char fileCookie[256] = {0};
    CHECK(std::fgets(fileCookie, sizeof(fileCookie), f) != nullptr);
    std::fclose(f);
    CHECK(std::string(fileCookie) == "__cookie__:" + server.Cookie() + "\n");
    server.Stop();
    CHECK(::system(("rm -rf " + dataDir).c_str()) == 0);
  }
}
