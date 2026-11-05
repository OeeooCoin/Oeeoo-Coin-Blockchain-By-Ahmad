// Tests: Milestone 14 — node wallet integration.
// NodeWallet load/lock/unlock lifecycle, unlock-timeout auto-relock,
// locked-vs-unlocked spending, RPC wallet methods end to end over
// HTTP, the createwallet RPC, and the unchanged wallet file format.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "consensus/consensus.h"
#include "crypto/ecdsa.h"
#include "mining/miner.h"
#include "network/node.h"
#include "rpc/json.h"
#include "rpc/rpcserver.h"
#include "rpc/rpcutil.h"
#include "script/script.h"
#include "test_harness.h"
#include "wallet/nodewallet.h"
#include "wallet/wallet.h"
#include "wallet/walletfile.h"

using namespace oeeoo;

namespace {
constexpr uint32_t kTestBits = 0x207fffff;  // easy test-only difficulty
constexpr uint32_t kTime = 1700000000;
constexpr uint32_t kNow = 1800000000;
constexpr const char* kPassphrase = "correct horse battery staple";
// First address of the zero 24-word mnemonic (M8, verified against
// an independent pure-Python secp256k1 implementation).
constexpr const char* kZeroFirstAddress = "7L88G1Not1Gv8nmci9mchhoN9XgbXTXqX5";

std::string ZeroMnemonic() {
  std::string m;
  for (int i = 0; i < 23; ++i) m += "abandon ";
  m += "art";
  return m;
}

// 256-bit all-0xff BIP39 vector: "zoo" x23 + "vote".
std::string ZooMnemonic() {
  std::string m;
  for (int i = 0; i < 23; ++i) m += "zoo ";
  m += "vote";
  return m;
}

std::string TempPath(const std::string& name) {
  return (std::filesystem::temp_directory_path() /
          ("oeeoo-m14-" + name + ".wallet"))
      .string();
}

void RemoveQuietly(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(path + ".tmp", ec);
}

// Write a wallet file with the zero mnemonic via the plain
// wallet-file API (the same API M8/M13 tests used).
bool WriteZeroWallet(const std::string& path) {
  WalletFileData data;
  data.mnemonic = ZeroMnemonic();
  data.nextExternal = 0;
  data.nextChange = 0;
  std::string error;
  if (!SaveWalletFile(path, data, kPassphrase, error)) {
    std::printf("  cannot write wallet file: %s\n", error.c_str());
    return false;
  }
  return true;
}

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

// Fund `addr` with a regular (non-coinbase) UTXO, so it is spendable
// without waiting 100 blocks for coinbase maturity.
void FundAddress(Node& node, const WalletAddress& addr, Amount value) {
  Hash256 fundTxid;
  fundTxid.fill(0xAB);
  Coin fundCoin;
  fundCoin.out.value = value;
  fundCoin.out.scriptPubKey = addr.scriptPubKey;
  fundCoin.height = 1;
  fundCoin.blockTime = kTime + 600;
  fundCoin.isCoinbase = false;
  CHECK(node.Utxo().AddCoin({fundTxid, 0}, fundCoin));
}

std::size_t WordCount(const std::string& s) {
  if (s.empty()) return 0;
  std::size_t n = 1;
  for (char c : s) {
    if (c == ' ') ++n;
  }
  return n;
}

// --- Minimal HTTP client for the RPC tests (same shape as test_rpc).
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
  if (err == nullptr || err->IsNull()) return 0;
  const Json* code = err->Get("code");
  if (code == nullptr) return 0;
  return code->AsInt();
}

std::string RpcErrorMessage(const Json& response) {
  const Json* err = response.Get("error");
  if (err == nullptr || err->IsNull()) return "";
  const Json* message = err->Get("message");
  if (message == nullptr) return "";
  return message->AsString();
}

Json Params1(const std::string& a) {
  Json p = Json::Array();
  p.Push(a);
  return p;
}

Json Params2(const std::string& a, int64_t b) {
  Json p = Json::Array();
  p.Push(a);
  p.Push(b);
  return p;
}
}  // namespace

void TestMilestone14() {
  test::BeginSection("m14: node wallet load/lock/unlock lifecycle");
  {
    const std::string path = TempPath("lifecycle");
    const std::string missing = TempPath("lifecycle-missing");
    const std::string fresh = TempPath("lifecycle-fresh");
    RemoveQuietly(path);
    RemoveQuietly(missing);
    RemoveQuietly(fresh);

    // A missing file is remembered, not an error: the node can start
    // with --wallet <new-path> and the wallet is created later with
    // the createwallet RPC.
    NodeWallet pending;
    std::string error;
    CHECK(pending.LoadFile(missing, error));
    CHECK(!pending.HasWallet());
    CHECK(pending.Path() == missing);
    CHECK(pending.IsLocked());
    CHECK(pending.UnlockedUntil() == 0);
    CHECK(pending.GetWallet() == nullptr);

    // A corrupt file is refused at load time, not at first unlock.
    {
      std::ofstream bad(path, std::ios::binary);
      bad << "this is not a wallet file";
    }
    NodeWallet corrupt;
    CHECK(!corrupt.LoadFile(path, error));
    CHECK(!corrupt.HasWallet());
    RemoveQuietly(path);

    // A real wallet file loads LOCKED, with an empty watch-only view
    // until the first unlock (the keys live inside the file).
    CHECK(WriteZeroWallet(path));
    NodeWallet nw;
    CHECK(nw.LoadFile(path, error));
    CHECK(nw.HasWallet());
    CHECK(nw.IsLocked());
    CHECK(nw.UnlockedUntil() == 0);
    CHECK(nw.GetWallet() != nullptr);
    nw.GetWallet()->ScanUtxo(CoinsView());
    CHECK(nw.GetWallet()->GetBalance() == 0);

    // Wrong and empty passphrases are rejected; the wallet stays
    // locked.
    CHECK(!nw.Unlock("wrong passphrase", 60, error));
    CHECK(error.find("incorrect passphrase") != std::string::npos);
    CHECK(nw.IsLocked());
    CHECK(!nw.Unlock("", 60, error));
    CHECK(nw.IsLocked());

    // Correct passphrase: unlocked, deadline set, first address is
    // the known M8 address for the zero mnemonic.
    const uint64_t before = static_cast<uint64_t>(std::time(nullptr));
    CHECK(nw.Unlock(kPassphrase, 60, error));
    CHECK(!nw.IsLocked());
    // The unlock reads the clock itself (PBKDF2 takes ~100 ms, so a
    // second boundary can pass between `before` and the unlock), so
    // bound the deadline by both readings, not by `before` alone.
    const uint64_t after = static_cast<uint64_t>(std::time(nullptr));
    const uint64_t until = nw.UnlockedUntil();
    CHECK(until > before && until <= after + 60);
    std::string werror;
    WalletAddress first = nw.GetWallet()->NewAddress(werror);
    CHECK(first.address == kZeroFirstAddress);
    CHECK(nw.GetWallet()->GetNextExternal() == 1);

    // Lock: the watch-only view survives (the xpub is cached), so
    // the balance stays visible while locked — the same funded view
    // still scans to 50 OEEOO after the keys are wiped.
    CoinsView view;
    {
      Coin coin;
      coin.out.value = 50 * COIN;
      coin.out.scriptPubKey = first.scriptPubKey;
      coin.height = 1;
      coin.blockTime = kTime + 600;
      coin.isCoinbase = false;
      Hash256 txid;
      txid.fill(0xAB);
      CHECK(view.AddCoin({txid, 0}, coin));
      nw.GetWallet()->ScanUtxo(view);
      CHECK(nw.GetWallet()->GetBalance() == 50 * COIN);
    }
    nw.Lock();
    CHECK(nw.IsLocked());
    CHECK(nw.UnlockedUntil() == 0);
    nw.GetWallet()->ScanUtxo(view);
    CHECK(nw.GetWallet()->GetBalance() == 50 * COIN);

    // Timeout 0 means "until walletlock".
    CHECK(nw.Unlock(kPassphrase, 0, error));
    CHECK(nw.UnlockedUntil() == UINT64_MAX);
    nw.Lock();

    // createwallet refuses an existing file (a fresh NodeWallet, so
    // the failure is the file check, not "a wallet is already
    // loaded").
    NodeWallet existing;
    CHECK(!existing.CreateFile(path, kPassphrase, werror, error));
    CHECK(error.find("already exists") != std::string::npos);

    // A fresh wallet file: 24-word mnemonic, left LOCKED.
    NodeWallet freshW;
    std::string mnemonic;
    CHECK(freshW.CreateFile(fresh, kPassphrase, mnemonic, error));
    CHECK(WordCount(mnemonic) == 24);
    CHECK(freshW.HasWallet());
    CHECK(freshW.IsLocked());
    CHECK(freshW.Unlock(kPassphrase, 60, error));
    CHECK(!freshW.IsLocked());
    freshW.Lock();

    // Indices persist across lock/unlock: no address is ever reused.
    CHECK(nw.Unlock(kPassphrase, 60, error));
    WalletAddress second = nw.GetWallet()->NewAddress(werror);
    CHECK(!second.address.empty());
    CHECK(second.address != kZeroFirstAddress);
    nw.Lock();  // persists the advanced indices
    NodeWallet reloaded;
    CHECK(reloaded.LoadFile(path, error));
    CHECK(reloaded.Unlock(kPassphrase, 60, error));
    CHECK(reloaded.GetWallet()->GetNextExternal() == 2);
    WalletAddress third = reloaded.GetWallet()->NewAddress(werror);
    CHECK(third.address != kZeroFirstAddress);
    CHECK(third.address != second.address);
    reloaded.Lock();

    RemoveQuietly(path);
    RemoveQuietly(missing);
    RemoveQuietly(fresh);
  }

  test::BeginSection("m14: unlock timeout auto-relocks");
  {
    const std::string path = TempPath("timeout");
    RemoveQuietly(path);
    CHECK(WriteZeroWallet(path));
    NodeWallet nw;
    std::string error;
    CHECK(nw.LoadFile(path, error));
    CHECK(nw.Unlock(kPassphrase, 1, error));
    CHECK(!nw.IsLocked());
    std::this_thread::sleep_for(std::chrono::seconds(2));
    CHECK(nw.IsLocked());
    CHECK(nw.UnlockedUntil() == 0);
    // Re-unlock after expiry works.
    CHECK(nw.Unlock(kPassphrase, 60, error));
    CHECK(!nw.IsLocked());
    nw.Lock();
    RemoveQuietly(path);
  }

  test::BeginSection("m14: locked wallet cannot spend, unlocked can");
  {
    EnsureKeys();
    std::mutex nodeMutex;
    Node node(kTestBits);
    node.SetTime(kNow);
    Block genesis = MineGenesis();
    std::string error;
    CHECK(node.BroadcastBlock(genesis, error));

    const std::string path = TempPath("spend");
    RemoveQuietly(path);
    CHECK(WriteZeroWallet(path));
    NodeWallet nw;
    CHECK(nw.LoadFile(path, error));
    CHECK(nw.Unlock(kPassphrase, 60, error));
    std::string werror;
    WalletAddress first = nw.GetWallet()->NewAddress(werror);
    CHECK(first.address == kZeroFirstAddress);
    FundAddress(node, first, 50 * COIN);
    nw.Lock();

    // Locked: balance is visible (watch-only), spending is refused.
    nw.GetWallet()->ScanUtxo(node.Utxo());
    CHECK(nw.GetWallet()->GetBalance() == 50 * COIN);
    Wallet dest;
    CHECK(dest.RestoreFromMnemonic(ZooMnemonic(), "", error));
    WalletAddress destAddr = dest.NewAddress(werror);
    CHECK(!destAddr.address.empty());
    Transaction tx;
    Amount fee = 0;
    CHECK(!nw.GetWallet()->CreateTransaction(
        destAddr.address, 10 * COIN, node.Utxo(), 1, kTime, tx, fee,
        error, 1));
    CHECK(error.find("watch-only") != std::string::npos);

    // Unlocked: the same spend builds and broadcasts.
    CHECK(nw.Unlock(kPassphrase, 60, error));
    nw.GetWallet()->ScanUtxo(node.Utxo());
    CHECK(nw.GetWallet()->CreateTransaction(
        destAddr.address, 10 * COIN, node.Utxo(), 1, kTime, tx, fee,
        error, 1));
    CHECK(node.BroadcastTx(tx, error));
    CHECK(node.MempoolRef().Size() == 1);
    nw.Lock();
    RemoveQuietly(path);
  }

  test::BeginSection("m14: RPC wallet methods end to end");
  {
    EnsureKeys();
    std::mutex nodeMutex;
    Node node(kTestBits);
    node.SetTime(kNow);
    Block genesis = MineGenesis();
    std::string error;
    CHECK(node.BroadcastBlock(genesis, error));

    const std::string path = TempPath("rpc");
    RemoveQuietly(path);
    CHECK(WriteZeroWallet(path));
    NodeWallet nw;
    CHECK(nw.LoadFile(path, error));
    // Fund the wallet's first address before the RPC server starts.
    Wallet probe;
    CHECK(probe.RestoreFromMnemonic(ZeroMnemonic(), "", error));
    std::string werror;
    WalletAddress first = probe.NewAddress(werror);
    CHECK(first.address == kZeroFirstAddress);
    // The second address: the funding coin pays to index 0, so the
    // wallet's scan moves the next external index past it and the
    // first getnewaddress returns index 1, not index 0. (Deriving
    // index 0 again would be address reuse.)
    WalletAddress second = probe.NewAddress(werror);
    CHECK(second.address != kZeroFirstAddress);
    FundAddress(node, first, 50 * COIN);

    RpcServer server(node, 0, "", &nodeMutex);
    server.AttachNodeWallet(&nw);
    server.SetAuthEnabled(false);
    CHECK(server.Start(error));
    const uint16_t port = server.Port();
    CHECK(port != 0);

    // Locked, never unlocked: the watch-only view is empty (the
    // keys are still inside the encrypted file).
    Json balance =
        RpcResult(RpcCall(port, "getbalance", Json::Array()));
    CHECK(balance.AsDouble() == 0.0);
    Json lockedAddr = RpcCall(port, "getnewaddress", Json::Array());
    CHECK(RpcHasError(lockedAddr));
    CHECK(RpcErrorCode(lockedAddr) == -13);
    CHECK(RpcErrorMessage(lockedAddr).find("locked") != std::string::npos);
    Json lockedTemplate =
        RpcCall(port, "getblocktemplate", Json::Array());
    CHECK(RpcHasError(lockedTemplate));
    CHECK(RpcErrorCode(lockedTemplate) == -13);
    Json lockedSend = RpcCall(port, "sendtoaddress",
                              Params2(first.address, 1));
    CHECK(RpcHasError(lockedSend));
    CHECK(RpcErrorCode(lockedSend) == -13);

    // Wrong passphrase: Bitcoin's -14.
    Json wrong = RpcCall(port, "walletpassphrase",
                         Params2("wrong passphrase", 60));
    CHECK(RpcHasError(wrong));
    CHECK(RpcErrorCode(wrong) == -14);
    CHECK(nw.IsLocked());

    // Correct passphrase: unlock, then the wallet is live.
    Json unlocked = RpcCall(port, "walletpassphrase",
                            Params2(kPassphrase, 60));
    CHECK(RpcResult(unlocked).AsString() == "wallet unlocked");
    Json info = RpcResult(RpcCall(port, "getwalletinfo", Json::Array()));
    CHECK(info.Get("unlocked_until")->AsInt() > 0);
    CHECK(info.Get("balance")->AsDouble() == 50.0);
    balance = RpcResult(RpcCall(port, "getbalance", Json::Array()));
    CHECK(balance.AsDouble() == 50.0);
    Json addr = RpcResult(RpcCall(port, "getnewaddress", Json::Array()));
    CHECK(addr.AsString() == second.address);
    Json unspent =
        RpcResult(RpcCall(port, "listunspent", Json::Array()));
    CHECK(unspent.AsArray().size() == 1);
    CHECK(unspent.AsArray()[0].Get("amount")->AsDouble() == 50.0);
    CHECK(unspent.AsArray()[0].Get("spendable")->AsBool());
    // getblocktemplate derives a miner address from the wallet.
    Json templ = RpcResult(RpcCall(port, "getblocktemplate", Json::Array()));
    CHECK(templ.Get("coinbasevalue")->AsInt() == 50 * COIN);
    CHECK(templ.Get("transactions")->AsArray().empty());

    // Send 10 OEEOO to a second wallet's address.
    Wallet dest;
    CHECK(dest.RestoreFromMnemonic(ZooMnemonic(), "", error));
    WalletAddress destAddr = dest.NewAddress(werror);
    Json params = Json::Array();
    params.Push(destAddr.address);
    params.Push("10");
    Json sent = RpcResult(RpcCall(port, "sendtoaddress", params));
    CHECK(sent.AsString().size() == 64);
    Json mempool = RpcResult(RpcCall(port, "getmempoolinfo", Json::Array()));
    CHECK(mempool.Get("size")->AsInt() == 1);

    // Lock again: balance stays visible, spending is refused.
    Json locked = RpcResult(RpcCall(port, "walletlock", Json::Array()));
    CHECK(locked.AsString() == "wallet locked");
    info = RpcResult(RpcCall(port, "getwalletinfo", Json::Array()));
    CHECK(info.Get("unlocked_until")->AsInt() == 0);
    CHECK(info.Get("balance")->AsDouble() == 50.0);
    balance = RpcResult(RpcCall(port, "getbalance", Json::Array()));
    CHECK(balance.AsDouble() == 50.0);
    unspent = RpcResult(RpcCall(port, "listunspent", Json::Array()));
    CHECK(unspent.AsArray().size() == 1);
    CHECK(!unspent.AsArray()[0].Get("spendable")->AsBool());
    Json lockedAddr2 = RpcCall(port, "getnewaddress", Json::Array());
    CHECK(RpcErrorCode(lockedAddr2) == -13);
    Json lockedSend2 = RpcCall(port, "sendtoaddress",
                               Params2(destAddr.address, 1));
    CHECK(RpcErrorCode(lockedSend2) == -13);

    // createwallet refuses when a wallet is already loaded.
    Json existing = RpcCall(port, "createwallet", Params1(kPassphrase));
    CHECK(RpcErrorCode(existing) == -13);
    server.Stop();

    // No node wallet at all: wallet methods fail -13, createwallet
    // has nowhere to write.
    RpcServer bare(node, 0, "", &nodeMutex);
    bare.SetAuthEnabled(false);
    CHECK(bare.Start(error));
    const uint16_t barePort = bare.Port();
    Json noWallet = RpcCall(barePort, "getbalance", Json::Array());
    CHECK(RpcErrorCode(noWallet) == -13);
    Json noPath = RpcCall(barePort, "createwallet", Params1(kPassphrase));
    CHECK(RpcErrorCode(noPath) == -13);
    CHECK(RpcErrorMessage(noPath).find("--wallet") != std::string::npos);
    Json noPass = RpcCall(barePort, "walletpassphrase",
                          Params2(kPassphrase, 60));
    CHECK(RpcErrorCode(noPass) == -13);
    Json noLock = RpcCall(barePort, "walletlock", Json::Array());
    CHECK(RpcErrorCode(noLock) == -13);
    bare.Stop();
    RemoveQuietly(path);
  }

  test::BeginSection("m14: createwallet RPC creates a fresh wallet");
  {
    std::mutex nodeMutex;
    Node node(kTestBits);
    node.SetTime(kNow);
    Block genesis = MineGenesis();
    std::string error;
    CHECK(node.BroadcastBlock(genesis, error));

    const std::string path = TempPath("create");
    RemoveQuietly(path);
    NodeWallet nw;
    CHECK(nw.LoadFile(path, error));  // missing file: remembered
    CHECK(!nw.HasWallet());

    RpcServer server(node, 0, "", &nodeMutex);
    server.AttachNodeWallet(&nw);
    server.SetAuthEnabled(false);
    CHECK(server.Start(error));
    const uint16_t port = server.Port();

    // Empty passphrase is rejected.
    Json empty = RpcCall(port, "createwallet", Params1(""));
    CHECK(RpcErrorCode(empty) == -8);

    Json created = RpcResult(
        RpcCall(port, "createwallet", Params1(kPassphrase)));
    const Json* mnemonic = created.Get("mnemonic");
    CHECK(mnemonic != nullptr && mnemonic->IsString());
    CHECK(WordCount(mnemonic->AsString()) == 24);
    const Json* warning = created.Get("warning");
    CHECK(warning != nullptr && !warning->AsString().empty());
    CHECK(nw.HasWallet());
    CHECK(nw.IsLocked());
    CHECK(std::filesystem::exists(path));

    // The returned mnemonic really is this wallet's mnemonic.
    Json unlocked = RpcCall(port, "walletpassphrase",
                            Params2(kPassphrase, 60));
    CHECK(RpcResult(unlocked).AsString() == "wallet unlocked");
    Json addr = RpcResult(RpcCall(port, "getnewaddress", Json::Array()));
    Wallet restored;
    CHECK(restored.RestoreFromMnemonic(mnemonic->AsString(), "", error));
    std::string werror;
    WalletAddress first = restored.NewAddress(werror);
    CHECK(first.address == addr.AsString());

    // A second createwallet refuses: a wallet is already loaded.
    Json again = RpcCall(port, "createwallet", Params1(kPassphrase));
    CHECK(RpcErrorCode(again) == -13);
    server.Stop();
    RemoveQuietly(path);
  }

  test::BeginSection("m14: wallet file format unchanged");
  {
    const std::string path = TempPath("format");
    const std::string trunc = TempPath("format-trunc");
    RemoveQuietly(path);
    RemoveQuietly(trunc);

    // A file created by the node wallet reads back through the plain
    // wallet-file API, with the same magic and payload.
    NodeWallet nw;
    std::string mnemonic;
    std::string error;
    CHECK(nw.CreateFile(path, kPassphrase, mnemonic, error));
    CHECK(WordCount(mnemonic) == 24);
    std::ifstream in(path, std::ios::binary);
    char magic[12];
    CHECK(in.read(magic, 12));
    CHECK(std::string(magic, 12) == "OEEOOWALLET1");
    in.close();
    WalletFileData data;
    CHECK(LoadWalletFile(path, kPassphrase, data, error));
    CHECK(data.mnemonic == mnemonic);
    CHECK(data.nextExternal == 0 && data.nextChange == 0);
    CHECK(!LoadWalletFile(path, "wrong passphrase", data, error));

    // And a file written by the plain API loads through the node
    // wallet and unlocks to the known first address.
    NodeWallet plain;
    CHECK(plain.LoadFile(path, error));
    CHECK(plain.Unlock(kPassphrase, 60, error));
    std::string werror;
    WalletAddress first = plain.GetWallet()->NewAddress(werror);
    Wallet restored;
    CHECK(restored.RestoreFromMnemonic(mnemonic, "", error));
    WalletAddress expected = restored.NewAddress(werror);
    CHECK(first.address == expected.address);
    plain.Lock();

    // A file cut below the header is refused at load time.
    {
      std::ofstream dst(trunc, std::ios::binary);
      dst << "short";
    }
    NodeWallet cut;
    CHECK(!cut.LoadFile(trunc, error));
    RemoveQuietly(trunc);
    // A file cut in the middle of the ciphertext is NOT refused by
    // the load probe — the probe cannot distinguish a decryption
    // failure caused by a wrong passphrase from one caused by a
    // truncated ciphertext, and it must treat both as "locked".
    // What matters is that it fails closed at unlock: the GCM tag
    // check rejects it there.
    {
      std::ifstream src(path, std::ios::binary);
      std::vector<char> bytes((std::istreambuf_iterator<char>(src)),
                              std::istreambuf_iterator<char>());
      std::ofstream dst(trunc, std::ios::binary);
      dst.write(bytes.data(), static_cast<std::streamsize>(bytes.size() / 2));
    }
    NodeWallet half;
    CHECK(half.LoadFile(trunc, error));
    CHECK(half.IsLocked());
    CHECK(!half.Unlock(kPassphrase, 60, error));
    RemoveQuietly(path);
    RemoveQuietly(trunc);
  }
}
