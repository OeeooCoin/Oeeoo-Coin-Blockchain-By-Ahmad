// Tests: Stratum v1 mining server (Milestone 10).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "blockchain/merkle.h"
#include "consensus/consensus.h"
#include "consensus/difficulty.h"
#include "crypto/ecdsa.h"
#include "crypto/sha256.h"
#include "mining/miner.h"
#include "network/node.h"
#include "network/transport.h"
#include "rpc/json.h"
#include "rpc/rpcutil.h"
#include "rpc/stratum.h"
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

// Session difficulty -> target (same formula as the server).
bool TestDifficultyToTarget(double difficulty, Target& out) {
  Target diff1;
  if (!BitsToTarget(0x1d00ffff, diff1)) return false;
  Uint256 num = Uint256::FromBigEndian(diff1);
  num.MultiplyUint32(1000000000);
  const uint64_t denInt =
      static_cast<uint64_t>(std::llround(difficulty * 1000000000.0));
  if (denInt == 0) return false;
  Uint256 den;
  den.d[0] = static_cast<uint32_t>(denInt);
  den.d[1] = static_cast<uint32_t>(denInt >> 32);
  out = Uint256::Divide(num, den).ToBigEndian();
  return true;
}

// A minimal Stratum client over TCP.
class StratumClient {
 public:
  bool Connect(uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) !=
        0) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
    return true;
  }

  bool SendLine(const std::string& line) {
    const std::string data = line + "\n";
    std::size_t sent = 0;
    while (sent < data.size()) {
      const ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, 0);
      if (n <= 0) return false;
      sent += static_cast<std::size_t>(n);
    }
    return true;
  }

  bool ReadLine(std::string& line, int timeoutMs = 5000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
      const std::size_t newline = buffer_.find('\n');
      if (newline != std::string::npos) {
        line = buffer_.substr(0, newline);
        buffer_.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) return false;
      const int remaining = static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
              .count());
      pollfd pfd{};
      pfd.fd = fd_;
      pfd.events = POLLIN;
      const int ready = ::poll(&pfd, 1, remaining);
      if (ready <= 0) return false;
      char buf[4096];
      const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
      if (n <= 0) return false;
      buffer_.append(buf, static_cast<std::size_t>(n));
    }
  }

  // Read until the response with `id` arrives; stash notifications.
  bool WaitResponse(int64_t id, Json& response, int timeoutMs = 10000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
      std::string line;
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) return false;
      const int remaining = static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
              .count());
      if (!ReadLine(line, remaining)) return false;
      Json msg;
      std::string error;
      if (!Json::Parse(line, msg, error)) continue;
      const Json* msgId = msg.Get("id");
      if (msgId != nullptr && !msgId->IsNull() &&
          msgId->AsInt() == id) {
        response = msg;
        return true;
      }
      notifications.push_back(msg);
    }
  }

  // The latest mining.notify stashed (or the last one seen).
  bool LastNotify(Json& notify) const {
    for (auto it = notifications.rbegin(); it != notifications.rend(); ++it) {
      const Json* method = it->Get("method");
      if (method != nullptr && method->AsString() == "mining.notify") {
        notify = *it;
        return true;
      }
    }
    return false;
  }

  void Close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  std::vector<Json> notifications;

 private:
  int fd_ = -1;
  std::string buffer_;
};

bool WaitForJob(StratumServer& server, int timeoutMs = 10000) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (!server.CurrentJobId().empty()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

bool WaitForJobChange(StratumServer& server, const std::string& oldJobId,
                      int timeoutMs = 10000) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    const std::string current = server.CurrentJobId();
    if (!current.empty() && current != oldJobId) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}
}  // namespace

void TestStratum() {
  test::BeginSection("stratum: subscribe, mine a share, block found");
  {
    EnsureKeys();
    std::mutex nodeMutex;
    Node node(kTestBits);
    node.SetTime(kNow);
    Block genesis = MineGenesis();
    std::string error;
    CHECK(node.BroadcastBlock(genesis, error));

    Wallet wallet;
    std::string mnemonic;
    for (int i = 0; i < 23; ++i) mnemonic += "abandon ";
    mnemonic += "art";
    CHECK(wallet.RestoreFromMnemonic(mnemonic, "", error));
    std::string werror;
    WalletAddress minerAddr = wallet.NewAddress(werror);
    CHECK(!minerAddr.address.empty());

    StratumServer server(node, 0, &nodeMutex);
    server.SetMinerAddress(minerAddr.address);
    server.SetInitialDifficulty(0.000001);  // ~4,300 hashes per share
    CHECK(server.Start(error));
    CHECK(WaitForJob(server));

    StratumClient client;
    CHECK(client.Connect(server.Port()));
    // Subscribe.
    Json sub = Json::Object();
    sub.Set("id", 1);
    sub.Set("method", "mining.subscribe");
    Json subParams = Json::Array();
    subParams.Push("oeeoo-test/1.0");
    sub.Set("params", subParams);
    CHECK(client.SendLine(sub.Dump()));
    Json subResponse;
    CHECK(client.WaitResponse(1, subResponse));
    const Json* subResult = subResponse.Get("result");
    CHECK(subResult != nullptr && subResult->IsArray());
    if (subResult == nullptr) return;
    CHECK(subResult->AsArray().size() == 3);
    const std::string extranonce1 = subResult->AsArray()[1].AsString();
    CHECK(extranonce1.size() == 8);
    CHECK(subResult->AsArray()[2].AsInt() == 4);  // extranonce2 size
    // The server sent set_difficulty + notify alongside.
    Json notify;
    CHECK(client.LastNotify(notify));
    const Json* notifyParams = notify.Get("params");
    CHECK(notifyParams != nullptr && notifyParams->IsArray());
    if (notifyParams == nullptr) return;
    const auto& np = notifyParams->AsArray();
    CHECK(np.size() == 9);
    const std::string jobId = np[0].AsString();
    const std::string prevHashHex = np[1].AsString();
    const std::string coinbase1 = np[2].AsString();
    const std::string coinbase2 = np[3].AsString();
    const Json& branches = np[4];
    const uint32_t version =
        static_cast<uint32_t>(std::stoul(np[5].AsString(), nullptr, 16));
    const uint32_t nbits =
        static_cast<uint32_t>(std::stoul(np[6].AsString(), nullptr, 16));
    const uint32_t ntime =
        static_cast<uint32_t>(std::stoul(np[7].AsString(), nullptr, 16));
    CHECK(np[8].AsBool());  // clean_jobs
    CHECK(nbits == kTestBits);
    // Authorize.
    Json auth = Json::Object();
    auth.Set("id", 2);
    auth.Set("method", "mining.authorize");
    Json authParams = Json::Array();
    authParams.Push("worker1");
    authParams.Push("x");
    auth.Set("params", authParams);
    CHECK(client.SendLine(auth.Dump()));
    Json authResponse;
    CHECK(client.WaitResponse(2, authResponse));
    CHECK(authResponse.Get("result")->AsBool());

    // Mine a valid share for this job (session difficulty 1e-6).
    Target sessionTarget;
    CHECK(TestDifficultyToTarget(0.000001, sessionTarget));
    uint32_t goodNonce = 0;
    bool found = false;
    for (uint32_t nonce = 0; nonce < 1000000 && !found; ++nonce) {
      std::vector<unsigned char> coinbaseBytes;
      CHECK(HexToBytesStr(coinbase1 + extranonce1 + "00000000" + coinbase2,
                          coinbaseBytes));
      Hash256 root = Sha256d(coinbaseBytes);
      for (const Json& branchJson : branches.AsArray()) {
        std::vector<unsigned char> branchBytes;
        CHECK(HexToBytesStr(branchJson.AsString(), branchBytes));
        std::vector<unsigned char> buf;
        buf.insert(buf.end(), root.begin(), root.end());
        buf.insert(buf.end(), branchBytes.begin(), branchBytes.end());
        root = Sha256d(buf);
      }
      std::vector<unsigned char> prevBytes;
      CHECK(HexToBytesStr(prevHashHex, prevBytes));
      BlockHeader header;
      header.nVersion = static_cast<int32_t>(version);
      std::memcpy(header.hashPrevBlock.data(), prevBytes.data(), 32);
      header.hashMerkleRoot = root;
      header.nTime = ntime;
      header.nBits = nbits;
      header.nNonce = nonce;
      const Hash256 hash = header.GetHash();
      if (std::memcmp(hash.data(), sessionTarget.data(), 32) <= 0) {
        goodNonce = nonce;
        found = true;
      }
    }
    CHECK(found);
    // Submit the valid share.
    Json submit = Json::Object();
    submit.Set("id", 3);
    submit.Set("method", "mining.submit");
    Json submitParams = Json::Array();
    submitParams.Push("worker1");
    submitParams.Push(jobId);
    submitParams.Push("00000000");
    char hexBuf[16];
    std::snprintf(hexBuf, sizeof(hexBuf), "%08x", ntime);
    submitParams.Push(std::string(hexBuf));
    std::snprintf(hexBuf, sizeof(hexBuf), "%08x", goodNonce);
    submitParams.Push(std::string(hexBuf));
    submit.Set("params", submitParams);
    CHECK(client.SendLine(submit.Dump()));
    Json submitResponse;
    CHECK(client.WaitResponse(3, submitResponse));
    CHECK(submitResponse.Get("result")->AsBool());
    CHECK(server.AcceptedShares() == 1);
    CHECK(server.BlocksFound() == 1);
    {
      std::lock_guard<std::mutex> lock(nodeMutex);
      CHECK(node.TipHeight() == 1);
    }
    // The block pays the miner: the coinbase is in the chain now.
    {
      std::lock_guard<std::mutex> lock(nodeMutex);
      Block foundBlock;
      CHECK(node.GetBlock(node.TipHash(), foundBlock));
      CHECK(foundBlock.vtx[0].vout[0].value == 50 * COIN);
      CHECK(foundBlock.vtx[0].vout[0].scriptPubKey ==
            minerAddr.scriptPubKey);
    }
    // Deterministic rejections against the NEW job.
    CHECK(WaitForJobChange(server, jobId));
    std::string jobId2 = server.CurrentJobId();
    CHECK(jobId2 != jobId);
    auto submitRaw = [&](int64_t id, const std::string& jid,
                         const std::string& en2, const std::string& timeHex,
                         const std::string& nonceHex) {
      Json s = Json::Object();
      s.Set("id", id);
      s.Set("method", "mining.submit");
      Json p = Json::Array();
      p.Push("worker1");
      p.Push(jid);
      p.Push(en2);
      p.Push(timeHex);
      p.Push(nonceHex);
      s.Set("params", p);
      CHECK(client.SendLine(s.Dump()));
      Json r;
      CHECK(client.WaitResponse(id, r));
      return r;
    };
    // Unknown job.
    Json r = submitRaw(4, "deadbeef", "00000000", "00000000", "00000000");
    CHECK(r.Get("error") != nullptr && !r.Get("error")->IsNull());
    if (r.Get("error") == nullptr) return;
    CHECK(r.Get("error")->Get("message")->AsString() == "unknown job");
    // Bad extranonce2 length.
    r = submitRaw(5, jobId2, "00", "00000000", "00000000");
    CHECK(r.Get("error") != nullptr && !r.Get("error")->IsNull());
    // ntime before the job time.
    r = submitRaw(6, jobId2, "00000000", "00000001", "00000000");
    CHECK(r.Get("error") != nullptr && !r.Get("error")->IsNull());
    if (r.Get("error") == nullptr) return;
    CHECK(r.Get("error")->Get("message")->AsString().find("ntime") !=
          std::string::npos);
    CHECK(server.RejectedShares() == 3);
    client.Close();
    server.Stop();
  }

  test::BeginSection("stratum: low difficulty share is rejected");
  {
    EnsureKeys();
    std::mutex nodeMutex;
    Node node(kTestBits);
    node.SetTime(kNow);
    Block genesis = MineGenesis();
    std::string error;
    CHECK(node.BroadcastBlock(genesis, error));
    Wallet wallet;
    std::string mnemonic;
    for (int i = 0; i < 23; ++i) mnemonic += "abandon ";
    mnemonic += "art";
    CHECK(wallet.RestoreFromMnemonic(mnemonic, "", error));
    std::string werror;
    WalletAddress minerAddr = wallet.NewAddress(werror);
    StratumServer server(node, 0, &nodeMutex);
    server.SetMinerAddress(minerAddr.address);
    server.SetInitialDifficulty(1.0);  // ~4 billion hashes per share
    CHECK(server.Start(error));
    CHECK(WaitForJob(server));
    StratumClient client;
    CHECK(client.Connect(server.Port()));
    Json sub = Json::Object();
    sub.Set("id", 1);
    sub.Set("method", "mining.subscribe");
    sub.Set("params", Json::Array());
    CHECK(client.SendLine(sub.Dump()));
    Json subResponse;
    CHECK(client.WaitResponse(1, subResponse));
    Json notify;
    CHECK(client.LastNotify(notify));
    const auto& np = notify.Get("params")->AsArray();
    const std::string jobId = np[0].AsString();
    const std::string ntimeHex = np[7].AsString();
    // One arbitrary nonce at difficulty 1: essentially never valid.
    Json submit = Json::Object();
    submit.Set("id", 2);
    submit.Set("method", "mining.submit");
    Json p = Json::Array();
    p.Push("worker1");
    p.Push(jobId);
    p.Push("00000000");
    p.Push(ntimeHex);
    p.Push("00000000");
    submit.Set("params", p);
    CHECK(client.SendLine(submit.Dump()));
    Json submitResponse;
    CHECK(client.WaitResponse(2, submitResponse));
    CHECK(submitResponse.Get("error") != nullptr &&
          !submitResponse.Get("error")->IsNull());
    if (submitResponse.Get("error") == nullptr) return;
    CHECK(submitResponse.Get("error")->Get("message")->AsString() ==
          "low difficulty share");
    CHECK(server.RejectedShares() == 1);
    CHECK(server.AcceptedShares() == 0);
    client.Close();
    server.Stop();
  }
}
