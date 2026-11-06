// Tests: Milestone 16 (Stream B) — wallet transaction journal
// (sidecar) + listtransactions / gettransaction +
// walletpassphrasechange.
// Journal round-trip, ScanUtxo receive recording, RPC round-trip
// after a real wallet send, passphrase change (success + wrong-old
// + locked + empty-new), journal survival across lock/unlock and
// across a fresh NodeWallet load, and the no-sidecar migration rule.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
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
#include "wallet/txjournal.h"
#include "wallet/wallet.h"
#include "wallet/walletfile.h"

using namespace oeeoo;

namespace {
constexpr uint32_t kTestBits = 0x207fffff;  // easy test-only difficulty
constexpr uint32_t kTime = 1700000000;
constexpr uint32_t kNow = 1800000000;
constexpr const char* kPassphrase = "correct horse battery staple";
constexpr const char* kZeroFirstAddress = "7L88G1Not1Gv8nmci9mchhoN9XgbXTXqX5";

std::string ZeroMnemonic() {
  std::string m;
  for (int i = 0; i < 23; ++i) m += "abandon ";
  m += "art";
  return m;
}

std::string ZooMnemonic() {
  std::string m;
  for (int i = 0; i < 23; ++i) m += "zoo ";
  m += "vote";
  return m;
}

std::string TempPath(const std::string& name) {
  return (std::filesystem::temp_directory_path() /
          ("oeeoo-m16b-" + name + ".wallet"))
      .string();
}

void RemoveQuietly(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(path + ".tmp", ec);
  std::filesystem::remove(path + ".txjournal", ec);
  std::filesystem::remove(path + ".txjournal.tmp", ec);
}

bool WriteZeroWallet(const std::string& path) {
  WalletFileData data;
  data.mnemonic = ZeroMnemonic();
  data.nextExternal = 0;
  data.nextChange = 0;
  std::string error;
  return SaveWalletFile(path, data, kPassphrase, error);
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

std::string HashHex(const Hash256& h) {
  return BytesToHexStr(std::vector<unsigned char>(h.begin(), h.end()));
}

// Fund `addr` with a regular (non-coinbase) UTXO at height 0, so it
// is spendable immediately and confirms at the genesis tip.
void FundAddress(Node& node, const WalletAddress& addr, Amount value) {
  Hash256 fundTxid;
  fundTxid.fill(0xAB);
  Coin fundCoin;
  fundCoin.out.value = value;
  fundCoin.out.scriptPubKey = addr.scriptPubKey;
  fundCoin.height = 0;
  fundCoin.blockTime = kTime;
  fundCoin.isCoinbase = false;
  CHECK(node.Utxo().AddCoin({fundTxid, 0}, fundCoin));
}

WalletTxRecord MakeRecord(const std::string& txid,
                          const std::string& category, int64_t amount,
                          int64_t fee, int64_t time, int blockheight,
                          const std::string& address) {
  WalletTxRecord r;
  r.txid = txid;
  r.category = category;
  r.amount = amount;
  r.fee = fee;
  r.time = time;
  r.blockheight = blockheight;
  r.address = address;
  r.label = "";
  return r;
}

std::string TxIdOf(unsigned char fill) {
  Hash256 h;
  h.fill(fill);
  return HashHex(h);
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

Json Params2s(const std::string& a, const std::string& b) {
  Json p = Json::Array();
  p.Push(a);
  p.Push(b);
  return p;
}
}  // namespace

void TestMilestone16B() {
  test::BeginSection("m16b: tx journal round-trip (sidecar file)");
  {
    const std::string journal = TempPath("journal") + ".txjournal";
    const std::string missing = TempPath("journal-missing") + ".txjournal";
    RemoveQuietly(TempPath("journal"));
    RemoveQuietly(TempPath("journal-missing"));
    std::string error;

    // A missing sidecar loads as an empty journal, not an error.
    std::vector<WalletTxRecord> loaded;
    CHECK(LoadTxJournal(missing, loaded, error));
    CHECK(loaded.empty());

    // Round-trip: receives and sends survive save + reload identical.
    std::vector<WalletTxRecord> records;
    records.push_back(MakeRecord(TxIdOf(0xAB), "receive", 50 * COIN, 0,
                                 kTime, 0, kZeroFirstAddress));
    records.push_back(MakeRecord(TxIdOf(0xCD), "send", -10 * COIN, 226,
                                 kNow, -1, "7AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
    CHECK(SaveTxJournal(journal, records, error));
    CHECK(LoadTxJournal(journal, loaded, error));
    CHECK(loaded.size() == 2);
    CHECK(loaded[0] == records[0]);
    CHECK(loaded[1] == records[1]);

    // Duplicate txid on load: latest wins, never duplicated.
    {
      std::ofstream out(journal, std::ios::app);
      WalletTxRecord newer = records[0];
      newer.amount = 51 * COIN;  // same txid, newer values
      Json j = Json::Object();
      j.Set("txid", newer.txid);
      j.Set("category", newer.category);
      j.Set("amount", newer.amount);
      j.Set("fee", newer.fee);
      j.Set("time", newer.time);
      j.Set("blockheight", static_cast<int64_t>(newer.blockheight));
      j.Set("address", newer.address);
      j.Set("label", newer.label);
      out << j.Dump() << "\n";
    }
    CHECK(LoadTxJournal(journal, loaded, error));
    CHECK(loaded.size() == 2);
    CHECK(loaded[0].amount == 51 * COIN);

    // A malformed line fails the load (fail-closed, no silent
    // history loss) and leaves no partial records behind.
    {
      std::ofstream out(journal, std::ios::app);
      out << "{not json\n";
    }
    CHECK(!LoadTxJournal(journal, loaded, error));
    CHECK(loaded.empty());

    RemoveQuietly(TempPath("journal"));
    RemoveQuietly(TempPath("journal-missing"));
  }

  test::BeginSection("m16b: ScanUtxo populates receive records");
  {
    Wallet wallet;
    std::string error;
    CHECK(wallet.RestoreFromMnemonic(ZeroMnemonic(), "", error));
    std::string werror;
    WalletAddress first = wallet.NewAddress(werror);
    CHECK(first.address == kZeroFirstAddress);
    CHECK(wallet.ListTxRecords().empty());

    CoinsView view;
    auto addCoin = [&](unsigned char fill, Amount value, uint32_t height,
                       uint32_t blockTime, bool isCoinbase,
                       uint32_t index = 0) {
      Coin coin;
      coin.out.value = value;
      coin.out.scriptPubKey = first.scriptPubKey;
      coin.height = static_cast<int>(height);
      coin.blockTime = blockTime;
      coin.isCoinbase = isCoinbase;
      Hash256 txid;
      txid.fill(fill);
      CHECK(view.AddCoin({txid, index}, coin));
    };
    // One regular receive (50 OEEOO, height 3, block time kTime).
    addCoin(0x11, 50 * COIN, 3, kTime, false);
    // One immature coinbase (5 OEEOO, height 5): a receive even
    // though it is not spendable yet.
    addCoin(0x22, 5 * COIN, 5, kTime + 1200, true);
    // One tx paying the wallet in TWO outputs (7 + 3 OEEOO): a
    // single aggregated receive record, not two.
    addCoin(0x33, 7 * COIN, 4, kTime + 600, false, 0);
    addCoin(0x33, 3 * COIN, 4, kTime + 600, false, 1);

    wallet.ScanUtxo(view, 6);
    std::vector<WalletTxRecord> recs = wallet.ListTxRecords();
    CHECK(recs.size() == 3);
    const WalletTxRecord* regular = nullptr;
    const WalletTxRecord* coinbase = nullptr;
    const WalletTxRecord* multi = nullptr;
    for (const WalletTxRecord& r : recs) {
      if (r.txid == TxIdOf(0x11)) regular = &r;
      if (r.txid == TxIdOf(0x22)) coinbase = &r;
      if (r.txid == TxIdOf(0x33)) multi = &r;
    }
    CHECK(regular != nullptr);
    CHECK(regular->category == "receive");
    CHECK(regular->amount == 50 * COIN);
    CHECK(regular->fee == 0);
    CHECK(regular->time == kTime);
    CHECK(regular->blockheight == 3);
    CHECK(regular->address == kZeroFirstAddress);
    CHECK(coinbase != nullptr);
    CHECK(coinbase->amount == 5 * COIN);
    CHECK(coinbase->blockheight == 5);
    CHECK(coinbase->time == kTime + 1200);
    CHECK(multi != nullptr);
    CHECK(multi->amount == 10 * COIN);  // 7 + 3 aggregated
    CHECK(multi->blockheight == 4);

    // A second scan does not duplicate anything (upsert by txid).
    wallet.ScanUtxo(view, 6);
    CHECK(wallet.ListTxRecords().size() == 3);
    // RecordTx upsert: latest wins.
    WalletTxRecord updated = *regular;
    updated.amount = 60 * COIN;
    wallet.RecordTx(updated);
    CHECK(wallet.ListTxRecords().size() == 3);
    bool foundUpdated = false;
    for (const WalletTxRecord& r : wallet.ListTxRecords()) {
      if (r.txid == TxIdOf(0x11) && r.amount == 60 * COIN) {
        foundUpdated = true;
      }
    }
    CHECK(foundUpdated);
  }

  test::BeginSection("m16b: listtransactions/gettransaction RPC round-trip");
  {
    EnsureKeys();
    std::mutex nodeMutex;
    Node node(kTestBits);
    node.SetTime(kNow);
    Block genesis = MineGenesis();
    std::string error;
    CHECK(node.BroadcastBlock(genesis, error));
    const std::string genesisHash = HashHex(node.TipHash());

    const std::string path = TempPath("rpc");
    RemoveQuietly(path);
    CHECK(WriteZeroWallet(path));
    NodeWallet nw;
    CHECK(nw.LoadFile(path, error));
    CHECK(nw.Unlock(kPassphrase, 60, error));
    std::string werror;
    WalletAddress first = nw.GetWallet()->NewAddress(werror);
    CHECK(first.address == kZeroFirstAddress);
    FundAddress(node, first, 50 * COIN);

    RpcServer server(node, 0, "", &nodeMutex);
    server.AttachNodeWallet(&nw);
    server.SetAuthEnabled(false);
    CHECK(server.Start(error));
    const uint16_t port = server.Port();
    CHECK(port != 0);

    // The first listtransactions call rescans itself, so it already
    // discovers the funded receive (no prior getbalance needed).
    Json firstList =
        RpcResult(RpcCall(port, "listtransactions", Json::Array()));
    CHECK(firstList.AsArray().size() == 1);

    // getbalance triggers the rescan, which records the receive.
    Json balance = RpcResult(RpcCall(port, "getbalance", Json::Array()));
    CHECK(balance.AsDouble() == 50.0);
    Json txs = RpcResult(RpcCall(port, "listtransactions", Json::Array()));
    CHECK(txs.AsArray().size() == 1);
    const Json& recv = txs.AsArray()[0];
    CHECK(recv.Get("category")->AsString() == "receive");
    CHECK(recv.Get("amount")->AsDouble() == 50.0);
    CHECK(recv.Get("address")->AsString() == kZeroFirstAddress);
    CHECK(recv.Get("confirmations")->AsInt() == 1);
    CHECK(recv.Get("blockhash")->AsString() == genesisHash);
    CHECK(recv.Get("blocktime")->AsInt() == kTime);
    CHECK(recv.Get("blockindex")->AsInt() == -1);
    CHECK(recv.Get("txid")->AsString() == TxIdOf(0xAB));
    CHECK(!recv.Has("fee"));  // fee is sends-only

    // gettransaction finds the receive too.
    Json gtx = RpcResult(
        RpcCall(port, "gettransaction", Params1(TxIdOf(0xAB))));
    CHECK(gtx.Get("amount")->AsDouble() == 50.0);
    CHECK(gtx.Get("confirmations")->AsInt() == 1);
    CHECK(gtx.Get("details")->AsArray().size() == 1);
    CHECK(gtx.Get("details")->AsArray()[0].Get("category")->AsString() ==
          "receive");

    // Send 10 OEEOO to a second wallet's address.
    Wallet dest;
    CHECK(dest.RestoreFromMnemonic(ZooMnemonic(), "", error));
    WalletAddress destAddr = dest.NewAddress(werror);
    Json params = Json::Array();
    params.Push(destAddr.address);
    params.Push("10");
    Json sent = RpcResult(RpcCall(port, "sendtoaddress", params));
    CHECK(sent.AsString().size() == 64);
    const std::string sendTxid = sent.AsString();

    // Newest first: the send (time = node now) before the receive.
    txs = RpcResult(RpcCall(port, "listtransactions", Json::Array()));
    CHECK(txs.AsArray().size() == 2);
    const Json& sendEntry = txs.AsArray()[0];
    CHECK(sendEntry.Get("category")->AsString() == "send");
    CHECK(sendEntry.Get("amount")->AsDouble() == -10.0);
    CHECK(std::fabs(sendEntry.Get("fee")->AsDouble() - 0.00000226) < 1e-9);
    CHECK(sendEntry.Get("address")->AsString() == destAddr.address);
    CHECK(sendEntry.Get("confirmations")->AsInt() == 0);
    CHECK(sendEntry.Get("blockhash")->AsString().empty());
    CHECK(sendEntry.Get("blocktime")->AsInt() == 0);
    CHECK(sendEntry.Get("txid")->AsString() == sendTxid);
    CHECK(sendEntry.Get("time")->AsInt() == kNow);
    CHECK(txs.AsArray()[1].Get("category")->AsString() == "receive");

    // count/skip paging: count=1 gives only the send; skip=1
    // gives only the receive.
    Json page = Json::Array();
    page.Push("*");
    page.Push(1);
    page.Push(0);
    Json one = RpcResult(RpcCall(port, "listtransactions", page));
    CHECK(one.AsArray().size() == 1);
    CHECK(one.AsArray()[0].Get("txid")->AsString() == sendTxid);
    Json page2 = Json::Array();
    page2.Push("*");
    page2.Push(10);
    page2.Push(1);
    Json skipped = RpcResult(RpcCall(port, "listtransactions", page2));
    CHECK(skipped.AsArray().size() == 1);
    CHECK(skipped.AsArray()[0].Get("category")->AsString() == "receive");

    // gettransaction on the send: amount negative excl. fee, fee
    // present, details has one entry.
    Json gsend = RpcResult(RpcCall(port, "gettransaction", Params1(sendTxid)));
    CHECK(gsend.Get("amount")->AsDouble() == -10.0);
    CHECK(std::fabs(gsend.Get("fee")->AsDouble() - 0.00000226) < 1e-9);
    CHECK(gsend.Get("confirmations")->AsInt() == 0);
    CHECK(gsend.Get("details")->AsArray().size() == 1);
    CHECK(gsend.Get("details")->AsArray()[0].Get("amount")->AsDouble() ==
          -10.0);

    // Unknown and malformed txids: -5, Bitcoin's message.
    Json unknown = RpcCall(port, "gettransaction", Params1(TxIdOf(0x00)));
    CHECK(RpcHasError(unknown));
    CHECK(RpcErrorCode(unknown) == -5);
    CHECK(RpcErrorMessage(unknown).find("Invalid or non-wallet") !=
          std::string::npos);
    Json malformed = RpcCall(port, "gettransaction", Params1("xyz"));
    CHECK(RpcErrorCode(malformed) == -5);

    // The journal holds no secrets: history still works locked.
    Json locked = RpcResult(RpcCall(port, "walletlock", Json::Array()));
    CHECK(locked.AsString() == "wallet locked");
    Json lockedTxs =
        RpcResult(RpcCall(port, "listtransactions", Json::Array()));
    CHECK(lockedTxs.AsArray().size() == 2);
    Json lockedGtx =
        RpcResult(RpcCall(port, "gettransaction", Params1(sendTxid)));
    CHECK(lockedGtx.Get("amount")->AsDouble() == -10.0);

    // The sidecar file exists on disk next to the wallet file.
    CHECK(std::filesystem::exists(path + ".txjournal"));
    server.Stop();

    // A fresh NodeWallet on the same files reloads the journal:
    // the send survives the restart (this is the migration-proof
    // half — the receive would also be rebuilt by rescan).
    NodeWallet reloaded;
    CHECK(reloaded.LoadFile(path, error));
    CHECK(reloaded.Unlock(kPassphrase, 60, error));
    bool foundSend = false;
    bool foundReceive = false;
    for (const WalletTxRecord& r :
         reloaded.GetWallet()->ListTxRecords()) {
      if (r.txid == sendTxid && r.category == "send") foundSend = true;
      if (r.txid == TxIdOf(0xAB) && r.category == "receive") {
        foundReceive = true;
      }
    }
    CHECK(foundSend);
    CHECK(foundReceive);
    reloaded.Lock();
    RemoveQuietly(path);
  }

  test::BeginSection("m16b: walletpassphrasechange");
  {
    const std::string path = TempPath("passchange");
    RemoveQuietly(path);
    CHECK(WriteZeroWallet(path));
    std::string error;

    // No wallet loaded: refused.
    NodeWallet bare;
    CHECK(!bare.ChangePassphrase(kPassphrase, "new pass", error));
    CHECK(error.find("no wallet") != std::string::npos);

    NodeWallet nw;
    CHECK(nw.LoadFile(path, error));
    // Locked: must be unlocked to change (Bitcoin requires this).
    CHECK(!nw.ChangePassphrase(kPassphrase, "new pass", error));
    CHECK(error.find("locked") != std::string::npos);

    CHECK(nw.Unlock(kPassphrase, 60, error));
    // Empty new passphrase: refused.
    CHECK(!nw.ChangePassphrase(kPassphrase, "", error));
    CHECK(error.find("must not be empty") != std::string::npos);
    // Wrong old passphrase: -14-style "incorrect passphrase" —
    // never distinguished from a corrupt file.
    CHECK(!nw.ChangePassphrase("wrong passphrase", "new pass", error));
    CHECK(error == "incorrect passphrase");
    CHECK(!nw.IsLocked());  // a failed change leaves the wallet open
    // Success: re-encrypts atomically; the wallet stays unlocked
    // and the in-memory passphrase is the new one.
    CHECK(nw.ChangePassphrase(kPassphrase, "new pass phrase", error));
    CHECK(!nw.IsLocked());
    nw.Lock();
    // The old passphrase no longer unlocks; the new one does.
    CHECK(!nw.Unlock(kPassphrase, 60, error));
    CHECK(error == "incorrect passphrase");
    CHECK(nw.Unlock("new pass phrase", 60, error));
    CHECK(!nw.IsLocked());
    // The wallet file format is unchanged by the rotation.
    std::ifstream in(path, std::ios::binary);
    char magic[12];
    CHECK(in.read(magic, 12));
    CHECK(std::string(magic, 12) == "OEEOOWALLET1");
    in.close();
    nw.Lock();

    // RPC end to end: wrong old -> -14, empty new -> -8, locked
    // -> -13, success -> unlock with the new passphrase works.
    std::mutex nodeMutex;
    Node node(kTestBits);
    node.SetTime(kNow);
    Block genesis = MineGenesis();
    CHECK(node.BroadcastBlock(genesis, error));
    const std::string rpcPath = TempPath("passchange-rpc");
    RemoveQuietly(rpcPath);
    CHECK(WriteZeroWallet(rpcPath));
    NodeWallet rnw;
    CHECK(rnw.LoadFile(rpcPath, error));
    RpcServer server(node, 0, "", &nodeMutex);
    server.AttachNodeWallet(&rnw);
    server.SetAuthEnabled(false);
    CHECK(server.Start(error));
    const uint16_t port = server.Port();
    Json lockedChange = RpcCall(port, "walletpassphrasechange",
                                Params2s(kPassphrase, "brand new"));
    CHECK(RpcErrorCode(lockedChange) == -13);
    Json unlocked = RpcCall(port, "walletpassphrase",
                            Params2(kPassphrase, 60));
    CHECK(RpcResult(unlocked).AsString() == "wallet unlocked");
    Json wrongOld = RpcCall(port, "walletpassphrasechange",
                            Params2s("wrong passphrase", "brand new"));
    CHECK(RpcErrorCode(wrongOld) == -14);
    Json emptyNew = RpcCall(port, "walletpassphrasechange",
                            Params2s(kPassphrase, ""));
    CHECK(RpcErrorCode(emptyNew) == -8);
    Json changed = RpcResult(RpcCall(port, "walletpassphrasechange",
                                     Params2s(kPassphrase, "brand new")));
    CHECK(changed.AsString() == "passphrase changed");
    Json relock = RpcResult(RpcCall(port, "walletlock", Json::Array()));
    CHECK(relock.AsString() == "wallet locked");
    Json oldFails = RpcCall(port, "walletpassphrase",
                            Params2(kPassphrase, 60));
    CHECK(RpcErrorCode(oldFails) == -14);
    Json newWorks = RpcCall(port, "walletpassphrase",
                            Params2("brand new", 60));
    CHECK(RpcResult(newWorks).AsString() == "wallet unlocked");
    server.Stop();
    RemoveQuietly(path);
    RemoveQuietly(rpcPath);
  }

  test::BeginSection("m16b: journal survives lock/unlock cycle");
  {
    const std::string path = TempPath("cycle");
    RemoveQuietly(path);
    CHECK(WriteZeroWallet(path));
    NodeWallet nw;
    std::string error;
    CHECK(nw.LoadFile(path, error));
    CHECK(nw.Unlock(kPassphrase, 60, error));
    // Two records in memory, one confirmed receive + one send.
    nw.GetWallet()->RecordTx(MakeRecord(TxIdOf(0xAB), "receive", 50 * COIN,
                                        0, kTime, 0, kZeroFirstAddress));
    nw.GetWallet()->RecordTx(MakeRecord(TxIdOf(0xCD), "send", -10 * COIN,
                                        226, kNow, -1,
                                        "7AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
    CHECK(nw.GetWallet()->TxRecordsDirty());
    // Lock persists the journal (Lock saves before wiping keys).
    nw.Lock();
    CHECK(nw.IsLocked());
    CHECK(std::filesystem::exists(path + ".txjournal"));
    CHECK(!nw.GetWallet()->TxRecordsDirty());
    // The records are still in the (locked, watch-only) wallet.
    CHECK(nw.GetWallet()->ListTxRecords().size() == 2);
    // Unlock reloads the journal from disk: identical records.
    CHECK(nw.Unlock(kPassphrase, 60, error));
    std::vector<WalletTxRecord> recs = nw.GetWallet()->ListTxRecords();
    CHECK(recs.size() == 2);
    CHECK(recs[0].txid == TxIdOf(0xAB));
    CHECK(recs[1].txid == TxIdOf(0xCD));
    CHECK(recs[1].amount == -10 * COIN && recs[1].fee == 226);
    // A second lock/unlock round-trip is equally stable.
    nw.Lock();
    CHECK(nw.Unlock(kPassphrase, 60, error));
    CHECK(nw.GetWallet()->ListTxRecords().size() == 2);
    nw.Lock();
    RemoveQuietly(path);
  }

  test::BeginSection("m16b: migration — wallet without a sidecar");
  {
    // A wallet file with NO sidecar starts with an empty journal
    // (not an error); history is rebuilt from ScanUtxo (receives)
    // on the next rescan. Sends made before the journal existed
    // are NOT recoverable — there is nothing to rebuild them from.
    const std::string path = TempPath("migration");
    RemoveQuietly(path);
    CHECK(WriteZeroWallet(path));
    CHECK(!std::filesystem::exists(path + ".txjournal"));
    NodeWallet nw;
    std::string error;
    CHECK(nw.LoadFile(path, error));
    CHECK(nw.GetWallet()->ListTxRecords().empty());
    CHECK(nw.Unlock(kPassphrase, 60, error));

    Wallet probe;
    CHECK(probe.RestoreFromMnemonic(ZeroMnemonic(), "", error));
    std::string werror;
    WalletAddress first = probe.NewAddress(werror);
    CoinsView view;
    Coin coin;
    coin.out.value = 25 * COIN;
    coin.out.scriptPubKey = first.scriptPubKey;
    coin.height = 2;
    coin.blockTime = kTime + 1200;
    coin.isCoinbase = false;
    Hash256 txid;
    txid.fill(0x77);
    CHECK(view.AddCoin({txid, 0}, coin));
    nw.GetWallet()->ScanUtxo(view, 3);
    // The receive is rebuilt by the rescan — but no send records
    // exist, because no sends were ever journaled for this wallet.
    std::vector<WalletTxRecord> recs = nw.GetWallet()->ListTxRecords();
    CHECK(recs.size() == 1);
    CHECK(recs[0].category == "receive");
    CHECK(recs[0].amount == 25 * COIN);
    CHECK(recs[0].blockheight == 2);
    // Persisting the rebuilt journal creates the sidecar, so the
    // next load finds it.
    nw.Lock();
    CHECK(std::filesystem::exists(path + ".txjournal"));
    NodeWallet reloaded;
    CHECK(reloaded.LoadFile(path, error));
    CHECK(reloaded.GetWallet()->ListTxRecords().size() == 1);
    reloaded.Lock();
    RemoveQuietly(path);
  }
}
