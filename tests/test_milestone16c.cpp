// Tests: Milestone 16 Stream C — RPC load control (worker pool +
// bounded work queue) and the IP-only ban key.
// The coordinator wires TestMilestone16C() into test_main.cpp.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "consensus/amount.h"
#include "consensus/consensus.h"
#include "crypto/ecdsa.h"
#include "mining/miner.h"
#include "network/node.h"
#include "network/protocol.h"
#include "network/transport.h"
#include "rpc/json.h"
#include "rpc/rpcserver.h"
#include "script/script.h"
#include "test_harness.h"

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

// A block that fails proof of work (impossible target): +100
// misbehavior, i.e. an instant ban.
Block BadBlock(const Hash256& parent) {
  Block bad;
  bad.header.nVersion = 1;
  bad.header.hashPrevBlock = parent;
  bad.header.nTime = kTime + 600;
  bad.header.nBits = 0x02003456;  // target 52: no hash can pass
  bad.vtx = {CreateCoinbase(1, Spk(), 50 * COIN)};
  bad.header.hashMerkleRoot = bad.ComputeMerkleRoot();
  return bad;
}

// True once `done()` holds, polling every 5 ms up to `timeoutMs`.
template <typename F>
bool WaitUntil(F done, int timeoutMs = 5000) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return done();
}

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

std::string GetBlockCountRequest() {
  return R"({"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]})";
}

// A raw TCP connection that sends nothing: it occupies one worker
// (blocked reading the HTTP header) until the caller closes it.
int OpenIdleConnection(uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// Handshake a bare loopback transport against a victim node.
bool HandshakeAttacker(LoopbackTransport& attacker, Node& victim,
                       const std::string& victimAddr, uint64_t nonce,
                       std::string& attackerPeerId) {
  const std::size_t before = victim.HandshookPeerCount();
  if (!attacker.Connect(victimAddr, attackerPeerId)) return false;
  VersionPayload v;
  v.version = kProtocolVersion;
  v.nonce = nonce;
  if (!attacker.Send(attackerPeerId,
                     SerializeMessage({"version", SerializeVersion(v)}))) {
    return false;
  }
  victim.Poll(0);
  TransportMessage m;
  if (!attacker.Poll(m, 0)) return false;  // victim's version
  if (!attacker.Poll(m, 0)) return false;  // victim's verack
  if (!attacker.Send(attackerPeerId,
                     SerializeMessage({"verack", {}}))) {
    return false;
  }
  victim.Poll(0);
  return victim.HandshookPeerCount() == before + 1;
}
}  // namespace

void TestMilestone16C() {
  test::BeginSection("m16c: rpc work queue full returns HTTP 500, "
                     "server stays alive");
  {
    EnsureKeys();
    std::mutex nodeMutex;
    Node node(kTestBits);
    node.SetTime(kNow);
    const Block genesis = MineGenesis();
    std::string error;
    CHECK(node.BroadcastBlock(genesis, error));

    RpcServer server(node, 0, "", &nodeMutex);
    server.SetMaxThreads(1);  // one worker, so one idle connection
    server.SetWorkQueueSize(1);  // blocks the whole server
    server.SetAuthEnabled(false);
    CHECK(server.Start(error));
    const uint16_t port = server.Port();
    CHECK(port != 0);
    CHECK(server.MaxThreads() == 1);
    CHECK(server.WorkQueueSize() == 1);

    // The idle connection occupies the only worker.
    const int idleFd = OpenIdleConnection(port);
    CHECK(idleFd >= 0);
    CHECK(WaitUntil([&] { return server.ActiveConnections() == 1; }));
    // A real request now waits in the (one-deep) queue.
    HttpResult queuedResult;
    std::thread queuedThread(
        [&] { queuedResult = HttpPost(port, GetBlockCountRequest()); });
    CHECK(WaitUntil([&] { return server.WorkQueueDepth() == 1; }));
    // The queue is full: the next connection gets HTTP 500 with a
    // JSON-RPC -32603 "RPC work queue is full" error, and the
    // server keeps running.
    HttpResult full = HttpPost(port, GetBlockCountRequest());
    CHECK(full.status == 500);
    CHECK(full.body.find("-32603") != std::string::npos);
    CHECK(full.body.find("RPC work queue is full") != std::string::npos);
    CHECK(full.body.find("\"jsonrpc\"") != std::string::npos);
    // Free the worker: the queued request is answered, and a
    // later request succeeds — the server survived the overload.
    CHECK(::close(idleFd) == 0);
    queuedThread.join();
    CHECK(queuedResult.status == 200);
    CHECK(queuedResult.body.find("\"result\"") != std::string::npos);
    HttpResult after = HttpPost(port, GetBlockCountRequest());
    CHECK(after.status == 200);
    CHECK(after.body.find("\"result\":0") != std::string::npos);
    server.Stop();
  }

  test::BeginSection("m16c: rpc pool shutdown is clean with a blocked "
                     "worker");
  {
    EnsureKeys();
    std::mutex nodeMutex;
    Node node(kTestBits);
    node.SetTime(kNow);
    const Block genesis = MineGenesis();
    std::string error;
    CHECK(node.BroadcastBlock(genesis, error));

    RpcServer server(node, 0, "", &nodeMutex);
    server.SetAuthEnabled(false);
    CHECK(server.Start(error));
    const uint16_t port = server.Port();
    HttpResult ok = HttpPost(port, GetBlockCountRequest());
    CHECK(ok.status == 200);
    // A worker blocked on an idle client must not hang Stop():
    // Stop() shuts the socket down, the worker's recv returns,
    // and every worker is joined before Stop() returns.
    const int idleFd = OpenIdleConnection(port);
    CHECK(idleFd >= 0);
    CHECK(WaitUntil([&] { return server.ActiveConnections() == 1; }));
    const auto t0 = std::chrono::steady_clock::now();
    server.Stop();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    CHECK(elapsed.count() < 5000);
    CHECK(!server.IsRunning());
    server.Stop();  // second Stop() is a no-op, not a hang
    CHECK(::close(idleFd) == 0);
    // The port is closed after Stop().
    HttpResult after = HttpPost(port, GetBlockCountRequest());
    CHECK(after.status == 0);
  }

  test::BeginSection("m16c: ip-only ban key survives a reconnect from a "
                     "new ephemeral port (real TCP)");
  {
    // BanKey rules, unit level.
    CHECK(BanManager::BanKey("1.2.3.4:56789") == "1.2.3.4");
    CHECK(BanManager::BanKey("loopback:ban-x") == "loopback:ban-x");
    CHECK(BanManager::BanKey("1.2.3.4") == "1.2.3.4");
    CHECK(BanManager::BanKey("::ffff:1.2.3.4:1234") ==
          "::ffff:1.2.3.4:1234");  // IPv4-mapped: left as-is
    CHECK(BanManager::BanKey("not-an-address") == "not-an-address");
    BanManager bans;
    bans.Ban("9.9.9.9:1111", kNow);
    CHECK(bans.IsBanned("9.9.9.9:2222", kNow));  // same IP, new port
    CHECK(!bans.IsBanned("9.9.9.10:1111", kNow));  // different IP
    bans.Unban("9.9.9.9:9999");  // unban by IP alone also works
    CHECK(!bans.IsBanned("9.9.9.9:1111", kNow));

    // Real TCP: ban, then reconnect from a new ephemeral port.
    auto h = std::make_unique<Node>(kTestBits);
    h->SetTransport(std::make_shared<TcpTransport>());
    CHECK(h->Listen(46480));
    const Block genesis = MineGenesis();
    std::string error;
    CHECK(h->BroadcastBlock(genesis, error));
    auto i = std::make_unique<Node>(kTestBits);
    i->SetTransport(std::make_shared<TcpTransport>());
    CHECK(i->ConnectTo("127.0.0.1:46480"));
    bool connected = false;
    for (int round = 0; round < 40 && !connected; ++round) {
      h->Poll(50);
      i->Poll(50);
      connected = h->PeerCount() == 1 && i->PeerCount() == 1;
    }
    CHECK(connected);
    // Peer.address still shows the full "ip:port" (display /
    // diagnostics) — only the BAN key is IP-only.
    bool sawFullAddress = false;
    for (const Peer& p : h->GetPeers()) {
      if (!p.outbound && p.address.rfind("127.0.0.1:", 0) == 0) {
        sawFullAddress = true;
      }
    }
    CHECK(sawFullAddress);
    Block bad = BadBlock(genesis.GetHash());
    CHECK(i->SendRawToAddress("127.0.0.1:46480",
                              {"block", bad.Serialize()}));
    bool banned = false;
    for (int round = 0; round < 40 && !banned; ++round) {
      h->Poll(50);
      i->Poll(50);
      banned = h->Bans().Size() == 1;
    }
    CHECK(banned);
    CHECK(h->PeerCount() == 0);
    // Fresh node, fresh transport: the OS gives it a different
    // ephemeral source port, so under "ip:port" keying this would
    // be a fresh key and the ban would not hold.
    auto i2 = std::make_unique<Node>(kTestBits);
    i2->SetTransport(std::make_shared<TcpTransport>());
    CHECK(i2->ConnectTo("127.0.0.1:46480"));
    for (int round = 0; round < 40; ++round) {
      h->Poll(50);
      i2->Poll(50);
    }
    CHECK(h->PeerCount() == 0);  // still refused, new port or not
    CHECK(h->Bans().Size() == 1);
  }

  test::BeginSection("m16c: loopback bans still work");
  {
    auto victim = std::make_unique<Node>(kTestBits);
    victim->SetTime(kNow);
    victim->SetTransport(
        std::make_shared<LoopbackTransport>("loopback:m16c-victim"));
    const Block genesis = MineGenesis();
    std::string error;
    CHECK(victim->BroadcastBlock(genesis, error));
    LoopbackTransport attacker("loopback:m16c-attacker");
    std::string attackerPeerId;
    CHECK(HandshakeAttacker(attacker, *victim, "loopback:m16c-victim",
                            333333ULL, attackerPeerId));
    CHECK(victim->PeerCount() == 1);
    Block bad = BadBlock(genesis.GetHash());
    CHECK(attacker.Send(attackerPeerId,
                        SerializeMessage({"block", bad.Serialize()})));
    victim->Poll(0);
    CHECK(victim->Bans().Size() == 1);
    CHECK(victim->PeerCount() == 0);
    // Reconnect under the same stable loopback name: refused.
    std::string attackerPeerId2;
    CHECK(attacker.Connect("loopback:m16c-victim", attackerPeerId2));
    VersionPayload v;
    v.version = kProtocolVersion;
    v.nonce = 444444ULL;
    CHECK(attacker.Send(attackerPeerId2,
                        SerializeMessage({"version", SerializeVersion(v)})));
    victim->Poll(0);
    CHECK(victim->PeerCount() == 0);
    CHECK(victim->Bans().Size() == 1);
  }
}
