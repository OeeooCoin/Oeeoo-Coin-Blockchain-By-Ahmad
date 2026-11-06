// Tests: Milestone 18, Stream B — live fuzzing harnesses.
// Deterministic (fixed seeds), bounded iterations, runnable
// under ASan/UBSan. Three live targets — P2P (a real Node over
// loopback), RPC (a real RpcServer over HTTP), Stratum (a real
// StratumServer over TCP) — plus regression tests for the four
// findings the fuzzing turned up (M18B-R1..R4, see
// tests/fuzz/README.md). Core's caveat applies throughout: a
// fuzzer cannot produce valid PoW or valid signatures, so
// acceptance paths are unreachable from mutated bytes — the
// harness covers the rejection paths, and valid baseline
// messages cover the acceptance paths. Validation is NOT mocked
// out anywhere: the live harness runs the real Node.
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "blockchain/block.h"
#include "consensus/consensus.h"
#include "crypto/address.h"
#include "crypto/ecdsa.h"
#include "mining/miner.h"
#include "network/compact.h"
#include "network/node.h"
#include "network/protocol.h"
#include "network/transport.h"
#include "rpc/json.h"
#include "rpc/rpcserver.h"
#include "rpc/stratum.h"
#include "script/script.h"
#include "test_harness.h"
#include "transactions/sighash.h"
#include "transactions/transaction.h"
#include "transactions/validation.h"
#include "wallet/walletfile.h"

using namespace oeeoo;

namespace {
constexpr uint32_t kTestBits = 0x207fffff;
constexpr uint32_t kTime = 1700000000;
constexpr uint32_t kNow = 1800000000;

std::string PidTag() {
  return std::to_string(static_cast<long>(::getpid()));
}

// xorshift64 — deterministic, fast (same mutator as the M11
// deterministic fuzzer in tests/test_fuzz.cpp).
struct Prng {
  uint64_t state;
  explicit Prng(uint64_t seed) : state(seed ? seed : 0x9e3779b97f4a7c15ULL) {}
  uint64_t Next() {
    uint64_t x = state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    state = x;
    return x;
  }
  std::size_t Below(std::size_t n) { return static_cast<std::size_t>(Next() % n); }
};

std::vector<unsigned char> Mutate(Prng& rng,
                                  const std::vector<unsigned char>& input) {
  std::vector<unsigned char> out = input;
  const int ops = 1 + static_cast<int>(rng.Below(4));
  for (int i = 0; i < ops; ++i) {
    const int kind = static_cast<int>(rng.Below(4));
    switch (kind) {
      case 0:  // flip a random byte
        if (!out.empty()) out[rng.Below(out.size())] ^= (1u << rng.Below(8));
        break;
      case 1:  // truncate
        if (!out.empty()) out.resize(rng.Below(out.size() + 1));
        break;
      case 2:  // duplicate a random slice
        if (!out.empty()) {
          const std::size_t pos = rng.Below(out.size());
          const std::size_t len = rng.Below(out.size() - pos + 1);
          const std::vector<unsigned char> slice(
              out.begin() + static_cast<std::ptrdiff_t>(pos),
              out.begin() + static_cast<std::ptrdiff_t>(pos + len));
          out.insert(out.end(), slice.begin(), slice.end());
        }
        break;
      default:  // random garbage byte
        out.push_back(static_cast<unsigned char>(rng.Next() & 0xff));
        break;
    }
  }
  return out;
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

Hash256 TxIdWith(unsigned char b) {
  Hash256 h{};
  h.fill(b);
  return h;
}

void Fund(Node& node, const Hash256& txid, Amount value) {
  Coin coin;
  coin.out.value = value;
  coin.out.scriptPubKey = Spk();
  coin.height = 0;
  coin.blockTime = kTime;
  coin.isCoinbase = false;
  CHECK(node.Utxo().AddCoin({txid, 0}, coin));
}

Transaction MakeSpend(const Hash256& prev, Amount sendValue) {
  Transaction tx;
  tx.version = 1;
  TxIn in;
  in.prevTxHash = prev;
  in.prevOutIndex = 0;
  in.sequence = 0xffffffff;
  tx.vin.push_back(in);
  TxOut out;
  out.value = sendValue;
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

// Pump messages between nodes until `done()` or `rounds` run out.
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
  // A fuzz victim accumulates many short-lived attacker peers
  // (one per ban/reconnect); raise the cap so the test measures
  // the fuzzing, not peer eviction.
  node->SetMaxPeers(500);
  node->SetTransport(std::make_shared<LoopbackTransport>(loopbackName));
  return node;
}

// Raw-transport handshake, as in test_milestone15.cpp: connect,
// send version, read the victim's version + verack, send verack.
bool HandshakeAttacker(LoopbackTransport& attacker, Node& victim,
                       const std::string& victimAddr, uint64_t nonce,
                       std::string& attackerPeerId) {
  // Drain first: a destroyed attacker's CloseLoopback queues a
  // disconnect notification (Milestone 18 dialing change) that
  // the victim only processes on Poll — until then the victim
  // still counts the dead peer. Reading `before` after the drain
  // makes the before+1 check meaningful; without it, a stale
  // removal (count -1) and the new handshake (count +1) cancel
  // out and the handshake looks like a failure.
  victim.Poll(0);
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

std::vector<Message> DrainMessages(LoopbackTransport& transport) {
  std::vector<Message> out;
  TransportMessage m;
  while (transport.Poll(m, 0)) {
    Message msg;
    std::size_t consumed = 0;
    if (ParseMessage(m.bytes, 0, msg, consumed)) out.push_back(msg);
  }
  return out;
}

// ---- Minimal HTTP client for the RPC fuzz section ----
struct HttpResult {
  int status = 0;  // 0 = no response (timeout / connection dropped)
  std::string body;
};

bool SendAllSock(int fd, const std::string& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t n =
        ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (n <= 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

HttpResult HttpRaw(uint16_t port, const std::string& rawRequest,
                   int timeoutMs = 3000) {
  HttpResult result;
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return result;
  timeval tv{};
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return result;
  }
  if (!SendAllSock(fd, rawRequest)) {
    ::close(fd);
    return result;
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

HttpResult HttpPostJson(uint16_t port, const std::string& body) {
  return HttpRaw(port, "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: " +
                           std::to_string(body.size()) +
                           "\r\nConnection: close\r\n\r\n" + body);
}

Json RpcCall(uint16_t port, const std::string& method, const Json& params) {
  Json request = Json::Object();
  request.Set("jsonrpc", "2.0");
  request.Set("id", 1);
  request.Set("method", method);
  request.Set("params", params);
  HttpResult r = HttpPostJson(port, request.Dump());
  CHECK(r.status == 200);
  Json response;
  std::string error;
  CHECK(Json::Parse(r.body, response, error));
  return response;
}

// ---- Minimal Stratum client ----
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

  bool SendRawBytes(const std::string& data) {
    return SendAllSock(fd_, data);
  }

  bool SendLine(const std::string& line) {
    return SendRawBytes(line + "\n");
  }

  // One line, or false on timeout / close. Buffered: a single
  // recv may hold several lines.
  bool ReadLine(std::string& line, int timeoutMs = 5000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
      const std::size_t nl = buffer_.find('\n');
      if (nl != std::string::npos) {
        line = buffer_.substr(0, nl);
        buffer_.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
      }
      if (std::chrono::steady_clock::now() > deadline) return false;
      timeval tv{};
      tv.tv_sec = 0;
      tv.tv_usec = 100000;
      ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      char buf[4096];
      const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
      if (n <= 0) return false;
      buffer_.append(buf, static_cast<std::size_t>(n));
    }
  }

  // Read until the response with `id` arrives; stash other
  // messages (notifications) — same pattern as
  // tests/test_stratum.cpp. Needed because a mining.subscribe
  // reply is preceded on the wire by mining.set_difficulty and
  // mining.notify, which the server sends before the result.
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
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }

  ~StratumClient() { Close(); }

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
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

std::string NestedJson(int depth) {
  std::string s;
  for (int i = 0; i < depth; ++i) s += "[";
  s += "1";
  for (int i = 0; i < depth; ++i) s += "]";
  return s;
}
}  // namespace

void TestMilestone18B() {
  EnsureKeys();
  const Block genesis = MineGenesis();
  const std::string pid = PidTag();

  test::BeginSection("m18b: live p2p fuzz — mutated messages into a real node");
  {
    auto victim = MakeNode("loopback:m18b-victim-" + pid);
    std::string error;
    CHECK(victim->BroadcastBlock(genesis, error));
    const Hash256 kA = TxIdWith(0x41);
    Fund(*victim, kA, 50 * COIN);
    const Transaction tx0 = MakeSpend(kA, 49 * COIN);

    // The base block for mutation: a valid height-1 block EXCEPT
    // its Merkle root is zeroed, so no 1-4-op mutation can ever
    // make it valid (the stored root differs from the true root
    // in all 32 bytes). The tip must therefore stay at genesis
    // for the whole section — deterministically.
    Block block1;
    block1.header.nVersion = 1;
    block1.header.hashPrevBlock = genesis.GetHash();
    block1.header.nTime = kTime + 600;
    block1.header.nBits = kTestBits;
    block1.vtx = {CreateCoinbase(1, Spk(), 50 * COIN), tx0};
    block1.header.hashMerkleRoot = Hash256{};
    const CompactBlock cb1 = MakeCompactBlock(block1, 0xabcdefULL);

    std::vector<std::pair<std::string, std::vector<unsigned char>>> bases;
    bases.push_back({"tx", tx0.Serialize()});
    bases.push_back({"block", block1.Serialize()});
    bases.push_back({"addr", SerializeAddr({{1700000000, 1, "1.2.3.4",
                                             kMainnetPort, 0},
                                            {1700000100, 1, "9.9.9.9",
                                             kMainnetPort, 0}})});
    bases.push_back({"cmpctblock", SerializeCompactBlock(cb1)});
    bases.push_back({"inv", SerializeInventory({{kInvTx, tx0.GetTxId()},
                                                {kInvBlock, genesis.GetHash()}})});
    bases.push_back({"ping", SerializePing(1234)});
    bases.push_back({"headers", SerializeHeaders({block1.header})});

    int delivered = 0;
    int reconnects = 0;
    int attackerNo = 0;
    std::unique_ptr<Node> attacker;
    const std::string victimAddr = "loopback:m18b-victim-" + pid;
    auto connectAttacker = [&]() -> bool {
      attacker = MakeNode("loopback:m18b-attacker-" + pid + "-" +
                          std::to_string(attackerNo++));
      std::string err;
      if (!attacker->BroadcastBlock(genesis, err)) return false;
      Fund(*attacker, kA, 50 * COIN);
      if (!attacker->ConnectTo(victimAddr)) return false;
      Pump({victim.get(), attacker.get()},
           [&] {
             return victim->HandshookPeerCount() == 1 &&
                    attacker->HandshookPeerCount() == 1;
           });
      ++reconnects;
      return victim->HandshookPeerCount() == 1;
    };
    CHECK(connectAttacker());
    // Baseline: a valid tx relays before any fuzzing.
    CHECK(attacker->BroadcastTx(tx0, error));
    Pump({victim.get(), attacker.get()},
         [&] { return victim->HasTx(tx0.GetTxId()); });
    CHECK(victim->HasTx(tx0.GetTxId()));
    CHECK(victim->MempoolSize() == 1);

    Prng rng(0xbeef18b);
    constexpr int kPerCommand = 200;
    for (const auto& base : bases) {
      for (int i = 0; i < kPerCommand; ++i) {
        const std::vector<unsigned char> mutated = Mutate(rng, base.second);
        if (!attacker->SendRawToAddress(victimAddr,
                                        {base.first, mutated})) {
          // Banned or disconnected (expected: invalid txs cost
          // 10 misbehavior points each, 100 bans). Reconnect
          // under a fresh loopback name — bans are keyed by
          // address, so a fresh name is a fresh peer.
          if (!connectAttacker()) break;
          if (!attacker->SendRawToAddress(victimAddr,
                                          {base.first, mutated})) {
            break;
          }
        }
        ++delivered;
        if (i % 25 == 24) {
          Pump({victim.get(), attacker.get()}, [] { return false; }, 4);
        }
      }
      Pump({victim.get(), attacker.get()}, [] { return false; }, 6);
    }
    Pump({victim.get(), attacker.get()}, [] { return false; }, 10);
    // 7 commands x 200 = 1400 attempted; most must be delivered
    // (reconnects cover the bans).
    CHECK(delivered >= 1000);
    CHECK(reconnects >= 1);
    CHECK(victim->TipHeight() == 0);
    CHECK(victim->HasTx(tx0.GetTxId()));
    CHECK(victim->MempoolSize() == 1);
    // Mutated spends with unknown parents land in the orphan
    // pool (no punishment); the pool caps at 100.
    CHECK(victim->OrphanCount() <= 100);
    // Liveness: the victim still accepts a fresh valid tx after
    // all that abuse.
    const Hash256 kB = TxIdWith(0x42);
    Fund(*victim, kB, 50 * COIN);
    Fund(*attacker, kB, 50 * COIN);
    const Transaction tx1 = MakeSpend(kB, 49 * COIN);
    CHECK(attacker->BroadcastTx(tx1, error));
    Pump({victim.get(), attacker.get()},
         [&] { return victim->HasTx(tx1.GetTxId()); });
    CHECK(victim->HasTx(tx1.GetTxId()));
  }

  test::BeginSection("m18b: live p2p fuzz — raw byte stream (framing attacks)");
  {
    auto victim = MakeNode("loopback:m18b-victim2-" + pid);
    std::string error;
    CHECK(victim->BroadcastBlock(genesis, error));
    const Hash256 kA = TxIdWith(0x43);
    Fund(*victim, kA, 50 * COIN);
    const Transaction tx0 = MakeSpend(kA, 49 * COIN);
    const std::string victimAddr = "loopback:m18b-victim2-" + pid;

    // (a) Mutated `version` payloads, each from a FRESH
    // connection: post-handshake versions are ignored by the
    // node, so only a fresh peer exercises version parsing.
    {
      Prng rng(0xbeef18c);
      VersionPayload v;
      v.version = kProtocolVersion;
      v.nonce = 424242;
      v.userAgent = "/OeeooCoin:0.1.0/";
      const std::vector<unsigned char> base = SerializeVersion(v);
      int attempted = 0;
      for (int i = 0; i < 200; ++i) {
        LoopbackTransport attacker("loopback:m18b-ver-" + pid + "-" +
                                   std::to_string(i));
        std::string peerId;
        if (!attacker.Connect(victimAddr, peerId)) continue;
        const std::vector<unsigned char> mutated = Mutate(rng, base);
        if (!attacker.Send(peerId,
                           SerializeMessage({"version", mutated}))) {
          continue;
        }
        ++attempted;
        victim->Poll(0);
        (void)DrainMessages(attacker);
      }
      CHECK(attempted >= 150);
      CHECK(victim->TipHeight() == 0);
    }

    // (b) A valid tx message split into 1-byte chunks.
    {
      LoopbackTransport attacker("loopback:m18b-chunk-" + pid);
      std::string peerId;
      CHECK(HandshakeAttacker(attacker, *victim, victimAddr, 777001, peerId));
      (void)DrainMessages(attacker);
      const std::vector<unsigned char> framed =
          SerializeMessage({"tx", tx0.Serialize()});
      for (unsigned char b : framed) {
        CHECK(attacker.Send(peerId, {b}));
        victim->Poll(0);
      }
      for (int i = 0; i < 10; ++i) victim->Poll(0);
      CHECK(victim->HasTx(tx0.GetTxId()));
    }

    // (c) Two valid messages in a single Send.
    {
      const Hash256 kB = TxIdWith(0x44);
      Fund(*victim, kB, 50 * COIN);
      const Transaction tx1 = MakeSpend(kB, 48 * COIN);
      LoopbackTransport attacker("loopback:m18b-concat-" + pid);
      std::string peerId;
      CHECK(HandshakeAttacker(attacker, *victim, victimAddr, 777002, peerId));
      (void)DrainMessages(attacker);
      std::vector<unsigned char> both =
          SerializeMessage({"ping", SerializePing(7)});
      const std::vector<unsigned char> txMsg =
          SerializeMessage({"tx", tx1.Serialize()});
      both.insert(both.end(), txMsg.begin(), txMsg.end());
      CHECK(attacker.Send(peerId, both));
      victim->Poll(0);
      victim->Poll(0);
      CHECK(victim->HasTx(tx1.GetTxId()));
    }

    // (d) Bad magic, then a valid tx on the same peer: the
    // peer earns 50 misbehavior (not a ban) and the node
    // recovers.
    {
      const Hash256 kC = TxIdWith(0x45);
      Fund(*victim, kC, 50 * COIN);
      const Transaction tx2 = MakeSpend(kC, 47 * COIN);
      LoopbackTransport attacker("loopback:m18b-magic-" + pid);
      std::string peerId;
      CHECK(HandshakeAttacker(attacker, *victim, victimAddr, 777003, peerId));
      (void)DrainMessages(attacker);
      std::vector<unsigned char> bad = SerializeMessage({"tx", tx2.Serialize()});
      bad[0] ^= 0xff;  // bad magic
      CHECK(attacker.Send(peerId, bad));
      victim->Poll(0);
      CHECK(!victim->HasTx(tx2.GetTxId()));
      CHECK(attacker.Send(peerId, SerializeMessage({"tx", tx2.Serialize()})));
      victim->Poll(0);
      victim->Poll(0);
      CHECK(victim->HasTx(tx2.GetTxId()));
    }

    // (e) Oversize length (2 MB + 1) is refused, not allocated.
    {
      LoopbackTransport attacker("loopback:m18b-big-" + pid);
      std::string peerId;
      CHECK(HandshakeAttacker(attacker, *victim, victimAddr, 777004, peerId));
      (void)DrainMessages(attacker);
      std::vector<unsigned char> header(24, 0);
      header[0] = 0xe2;
      header[1] = 0xd8;
      header[2] = 0x04;
      header[3] = 0xd6;
      std::memcpy(header.data() + 4, "tx", 2);
      const uint32_t len = static_cast<uint32_t>(kMaxMessageLength + 1);
      header[16] = static_cast<unsigned char>(len & 0xff);
      header[17] = static_cast<unsigned char>((len >> 8) & 0xff);
      header[18] = static_cast<unsigned char>((len >> 16) & 0xff);
      header[19] = static_cast<unsigned char>((len >> 24) & 0xff);
      CHECK(attacker.Send(peerId, header));
      victim->Poll(0);
      CHECK(victim->TipHeight() == 0);
    }

    // (f) Random garbage streams: 300 sends, fresh attacker
    // every 10 (bad magic bans quickly).
    {
      Prng rng(0xbeef18d);
      int sent = 0;
      for (int batch = 0; batch < 30; ++batch) {
        LoopbackTransport attacker("loopback:m18b-garbage-" + pid + "-" +
                                   std::to_string(batch));
        std::string peerId;
        if (!HandshakeAttacker(attacker, *victim, victimAddr,
                               777100 + static_cast<uint64_t>(batch),
                               peerId)) {
          continue;
        }
        (void)DrainMessages(attacker);
        for (int i = 0; i < 10; ++i) {
          std::vector<unsigned char> garbage(rng.Below(400));
          for (auto& b : garbage) {
            b = static_cast<unsigned char>(rng.Next() & 0xff);
          }
          if (!attacker.Send(peerId, garbage)) break;
          ++sent;
          victim->Poll(0);
        }
      }
      CHECK(sent >= 100);
      CHECK(victim->TipHeight() == 0);
      // The victim still holds the earlier valid txs.
      CHECK(victim->HasTx(tx0.GetTxId()));
    }
  }

  test::BeginSection("m18b: regression — M18B-R5 OnBytes use-after-free");
  {
    // Two bad-magic messages from one handshook peer: each costs
    // 50 misbehavior, so the second reaches the 100-point ban
    // threshold and Misbehave() erases the peer from inside
    // OnBytes. The old code then called peer.recvBuffer.clear()
    // on the freed Peer (ASan heap-use-after-free, found by the
    // garbage-stream fuzzing above). The node must survive the
    // second message and the peer must be gone (banned).
    const std::string victimAddr = "loopback:m18b-r5-" + pid;
    auto victim = MakeNode(victimAddr);
    std::string error;
    CHECK(victim->BroadcastBlock(genesis, error));
    LoopbackTransport attacker("loopback:m18b-r5-attacker-" + pid);
    std::string peerId;
    CHECK(HandshakeAttacker(attacker, *victim, victimAddr, 777005, peerId));
    std::vector<unsigned char> bad =
        SerializeMessage({"ping", SerializePing(7)});
    bad[0] ^= 0xff;  // bad magic
    CHECK(attacker.Send(peerId, bad));
    victim->Poll(0);
    CHECK(victim->PeerCount() == 1);  // 50 points: warned, alive
    CHECK(attacker.Send(peerId, bad));
    victim->Poll(0);
    CHECK(victim->PeerCount() == 0);  // 100 points: banned + gone
    CHECK(victim->TipHeight() == 0);
    // A fresh valid tx still relays: the node is alive.
    const Hash256 kD = TxIdWith(0x46);
    Fund(*victim, kD, 50 * COIN);
    const Transaction tx3 = MakeSpend(kD, 49 * COIN);
    LoopbackTransport attacker2("loopback:m18b-r5b-" + pid);
    std::string peerId2;
    CHECK(HandshakeAttacker(attacker2, *victim, victimAddr, 777006, peerId2));
    CHECK(attacker2.Send(peerId2,
                         SerializeMessage({"tx", tx3.Serialize()})));
    victim->Poll(0);
    CHECK(victim->HasTx(tx3.GetTxId()));
  }

  test::BeginSection("m18b: live rpc fuzz — mutated JSON-RPC over HTTP");
  {
    auto node = MakeNode("loopback:m18b-rpc-" + pid);
    std::string error;
    CHECK(node->BroadcastBlock(genesis, error));
    std::mutex nodeMutex;
    RpcServer server(*node, 0, "", &nodeMutex);
    server.SetAuthEnabled(false);
    CHECK(server.Start(error));
    const uint16_t port = server.Port();
    CHECK(port != 0);

    // Baseline: a valid call works.
    {
      Json count = RpcCall(port, "getblockcount", Json::Array());
      CHECK(count.Has("result"));
      CHECK(count.Get("result")->AsInt() == 0);
    }

    // Byte-mutations of a valid request: the server must answer
    // every one (HTTP 200 with a JSON-RPC error, or a 400/close)
    // and stay alive. A timeout (status 0) is also acceptable
    // for requests that declare a body they never send — the
    // worker waits for the rest of the body, which is correct
    // HTTP behavior, not a hang: the client closing the socket
    // frees the worker.
    {
      Prng rng(0xbeef18e);
      Json request = Json::Object();
      request.Set("jsonrpc", "2.0");
      request.Set("id", 1);
      request.Set("method", "getblockcount");
      request.Set("params", Json::Array());
      const std::string valid = request.Dump();
      int answered = 0;
      for (int i = 0; i < 300; ++i) {
        std::vector<unsigned char> bytes(valid.begin(), valid.end());
        const std::vector<unsigned char> mutated = Mutate(rng, bytes);
        const std::string text(mutated.begin(), mutated.end());
        HttpResult r = HttpPostJson(port, text);
        if (r.status != 0) {
          ++answered;
          CHECK(r.status == 200 || r.status == 400);
        }
      }
      CHECK(answered >= 250);
    }

    // Structured cases: deep nesting, huge numbers, wrong
    // types, unknown methods, batches, bad HTTP.
    {
      // Deep nesting: the JSON parser caps depth at 64.
      Json parsed;
      HttpResult deep100 = HttpPostJson(port, NestedJson(100));
      CHECK(deep100.status == 200);
      CHECK(Json::Parse(deep100.body, parsed, error));
      CHECK(parsed.Has("error"));
      HttpResult deep1000 = HttpPostJson(port, NestedJson(1000));
      CHECK(deep1000.status == 200);
      CHECK(Json::Parse(deep1000.body, parsed, error));
      CHECK(parsed.Has("error"));
      // 60 nested arrays inside the params: parses fine.
      HttpResult deep60 = HttpPostJson(
          port, R"({"jsonrpc":"2.0","id":1,"method":"getblockcount","params":)" +
                    NestedJson(60) + "}");
      CHECK(deep60.status == 200);

      // Huge numbers: out-of-range integers and doubles.
      HttpResult hugeInt = HttpPostJson(
          port, R"({"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[9223372036854775808]})");
      CHECK(hugeInt.status == 200);
      HttpResult hugeDbl = HttpPostJson(
          port, R"({"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[1e400]})");
      CHECK(hugeDbl.status == 200);

      // Wrong types for real methods.
      {
        Json params = Json::Array();
        params.Push("not-a-number");
        Json r = RpcCall(port, "getblockhash", params);
        CHECK(r.Has("error"));
      }
      {
        Json params = Json::Array();
        params.Push(12345);
        Json r = RpcCall(port, "getblock", params);
        CHECK(r.Has("error"));
      }
      {
        Json params = Json::Array();
        params.Push(42);
        Json r = RpcCall(port, "sendrawtransaction", params);
        CHECK(r.Has("error"));
      }
      // Unknown method.
      {
        Json r = RpcCall(port, "explode", Json::Array());
        CHECK(r.Has("error"));
        CHECK(r.Get("error")->Get("code")->AsInt() == -32601);
      }
      // Batch of 500 identical calls.
      {
        Json batch = Json::Array();
        for (int i = 0; i < 500; ++i) {
          Json item = Json::Object();
          item.Set("jsonrpc", "2.0");
          item.Set("id", i);
          item.Set("method", "getblockcount");
          item.Set("params", Json::Array());
          batch.Push(item);
        }
        HttpResult r = HttpPostJson(port, batch.Dump());
        CHECK(r.status == 200);
        Json responses;
        CHECK(Json::Parse(r.body, responses, error));
        CHECK(responses.IsArray());
        CHECK(responses.AsArray().size() == 500);
      }
      // Empty body and a 1 MB body of '['.
      {
        HttpResult r = HttpPostJson(port, "");
        CHECK(r.status == 200);
        CHECK(Json::Parse(r.body, parsed, error));
        CHECK(parsed.Has("error"));
        const std::string big(1024 * 1024, '[');
        HttpResult r2 = HttpPostJson(port, big);
        CHECK(r2.status == 200);
      }
      // Wrong HTTP method.
      {
        HttpResult r = HttpRaw(port, "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                     "Connection: close\r\n\r\n");
        CHECK(r.status == 405);
      }
      // M18B-R1 regression: non-numeric Content-Length used to
      // abort the whole node (std::terminate from std::stoul).
      // Now: HTTP 400, server alive.
      {
        HttpResult r = HttpRaw(port,
                               "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: abc\r\n"
                               "Connection: close\r\n\r\n{}");
        CHECK(r.status == 400);
        CHECK(r.body == "invalid Content-Length");
      }
      // Out-of-range Content-Length: no crash; the worker waits
      // for a body that never fully arrives, the client times
      // out (status 0), and the NEXT valid call still works.
      {
        HttpResult r = HttpRaw(port,
                               "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: 99999999999999999999999\r\n"
                               "Connection: close\r\n\r\n{}");
        CHECK(r.status == 0);
      }
      // Negative Content-Length is not digits-only: 400.
      {
        HttpResult r = HttpRaw(port,
                               "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: -5\r\n"
                               "Connection: close\r\n\r\n{}");
        CHECK(r.status == 400);
      }
      // Invalid UTF-8 inside a string.
      {
        std::string bad = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"
                          "\"getblockcount\",\"params\":[\"";
        bad.push_back(static_cast<char>(0xff));
        bad.push_back(static_cast<char>(0xfe));
        bad += "\"]}";
        HttpResult r = HttpPostJson(port, bad);
        CHECK(r.status == 200);
      }
    }

    // The server still answers a valid call after all of it.
    {
      Json count = RpcCall(port, "getblockcount", Json::Array());
      CHECK(count.Has("result"));
      CHECK(count.Get("result")->AsInt() == 0);
    }
    server.Stop();
  }

  test::BeginSection("m18b: live stratum fuzz — mutated mining.submit lines");
  {
    auto node = MakeNode("loopback:m18b-stratum-" + pid);
    std::string error;
    CHECK(node->BroadcastBlock(genesis, error));
    std::mutex nodeMutex;
    StratumServer server(*node, 0, &nodeMutex);
    server.SetMinerAddress(PubKeyHashToAddress(PubKeyHash160(g_key.pubkey)));
    CHECK(server.Start(error));
    const uint16_t port = server.Port();
    CHECK(port != 0);
    CHECK(WaitForJob(server));

    // Baseline: subscribe + authorize + a garbage submit is
    // rejected, and the server answers. A subscribe is answered
    // by THREE messages on the wire (set_difficulty, notify,
    // then the result), so WaitResponse(1) skips to the result
    // and LastNotify recovers the job.
    {
      StratumClient client;
      CHECK(client.Connect(port));
      CHECK(client.SendLine(
          R"({"id":1,"method":"mining.subscribe","params":["oeeoo-test/1.0"]})"));
      Json reply;
      CHECK(client.WaitResponse(1, reply));
      const Json* subResult = reply.Get("result");
      CHECK(subResult != nullptr && subResult->IsArray());
      CHECK(subResult != nullptr && subResult->AsArray().size() == 3);
      Json notify;
      CHECK(client.LastNotify(notify));
      std::string jobId, ntimeHex;
      const Json* notifyParams = notify.Get("params");
      CHECK(notifyParams != nullptr && notifyParams->IsArray() &&
            notifyParams->AsArray().size() == 9);
      if (notifyParams != nullptr && notifyParams->IsArray() &&
          notifyParams->AsArray().size() == 9) {
        jobId = notifyParams->AsArray().at(0).AsString();
        ntimeHex = notifyParams->AsArray().at(7).AsString();
      }
      CHECK(!jobId.empty());
      CHECK(!ntimeHex.empty());
      CHECK(client.SendLine(
          R"({"id":2,"method":"mining.authorize","params":["worker1","x"]})"));
      CHECK(client.WaitResponse(2, reply));
      CHECK(reply.Get("result") != nullptr &&
            reply.Get("result")->AsBool());
      // Submit with a bogus nonce against the real job/ntime:
      // rejected (low difficulty), not a crash. Either outcome
      // is a well-formed reply (result + error keys both set).
      const std::string submit =
          std::string(
              R"({"id":3,"method":"mining.submit","params":["worker1",")") +
          jobId + R"(","00000002",")" + ntimeHex + R"(","00000000"]})";
      CHECK(client.SendLine(submit));
      CHECK(client.WaitResponse(3, reply));
      CHECK(reply.Has("result") && reply.Has("error"));
      client.Close();
    }

    // Mutated lines over one connection: non-JSON, truncated
    // JSON, huge fields, wrong types, unknown methods, deep
    // nesting, oversize lines. Unparsable lines get no reply
    // by design; parsable ones must get a reply or a clean
    // disconnect — never a crash, and the server must keep
    // serving new clients afterwards.
    {
      StratumClient client;
      CHECK(client.Connect(port));
      CHECK(client.SendLine(
          R"({"id":1,"method":"mining.subscribe","params":["oeeoo-test/1.0"]})"));
      Json reply;
      CHECK(client.WaitResponse(1, reply));
      Json notify;
      CHECK(client.LastNotify(notify));
      std::string jobId, ntimeHex;
      const Json* notifyParams = notify.Get("params");
      CHECK(notifyParams != nullptr && notifyParams->IsArray() &&
            notifyParams->AsArray().size() == 9);
      if (notifyParams != nullptr && notifyParams->IsArray() &&
          notifyParams->AsArray().size() == 9) {
        jobId = notifyParams->AsArray().at(0).AsString();
        ntimeHex = notifyParams->AsArray().at(7).AsString();
      }
      CHECK(!jobId.empty());
      Prng rng(0xbeef18f);
      const std::string baseSubmit =
          std::string(
              R"({"id":3,"method":"mining.submit","params":["worker1",")") +
          jobId + R"(","00000002",")" + ntimeHex + R"(","5f4c3b2a"]})";
      int sent = 0;
      for (int i = 0; i < 300; ++i) {
        std::string mutated;
        switch (i % 6) {
          case 0: {  // byte-mutated submit
            std::vector<unsigned char> bytes(baseSubmit.begin(),
                                             baseSubmit.end());
            const std::vector<unsigned char> m = Mutate(rng, bytes);
            mutated.assign(m.begin(), m.end());
            break;
          }
          case 1:  // huge extranonce2 (10 KB hex)
            mutated = std::string(
                          R"({"id":3,"method":"mining.submit","params":["worker1",")") +
                      jobId + R"(",")" + std::string(10240, 'a') +
                      R"(","5db8d0a9","5f4c3b2a"]})";
            break;
          case 2:  // non-hex / huge ntime and nonce
            mutated = std::string(
                          R"({"id":3,"method":"mining.submit","params":["worker1",")") +
                      jobId +
                      R"(","00000002","zzzzzzzz","ffffffff"]})";
            break;
          case 3:  // wrong types: numbers where strings belong
            mutated = std::string(
                          R"({"id":3,"method":"mining.submit","params":[123,456,789,0,0]})");
            break;
          case 4:  // unknown method + deep nesting
            mutated = std::string(
                          R"({"id":9,"method":"mining.explode","params":)") +
                      NestedJson(100) + "}";
            break;
          default:  // truncated JSON / empty-ish lines
            mutated = baseSubmit.substr(0, rng.Below(baseSubmit.size() + 1));
            break;
        }
        if (!client.SendLine(mutated)) break;
        ++sent;
        // Drain at most one reply without blocking the run:
        // most mutated lines are skipped silently.
        std::string replyLine;
        (void)client.ReadLine(replyLine, 100);
      }
      CHECK(sent >= 250);
      client.Close();
    }

    // A 70 KB line is skipped by design (line cap 65536) and
    // must NOT drop the connection: the R3 cap is 1 MB, above
    // the longest skipped line, so a long terminated line is
    // skipped and the client still gets its subscribe reply.
    {
      StratumClient client;
      CHECK(client.Connect(port));
      CHECK(client.SendLine(std::string(70000, 'x')));
      CHECK(client.SendLine(
          R"({"id":1,"method":"mining.subscribe","params":["oeeoo-test/1.0"]})"));
      Json reply;
      CHECK(client.WaitResponse(1, reply));
      CHECK(reply.Has("result"));
      client.Close();
    }

    // M18B-R3 regression: 5 MB with no newline. The server must
    // drop the flooding client (a tail past 1 MB can never
    // become a valid line) and keep serving new clients.
    {
      StratumClient flooder;
      CHECK(flooder.Connect(port));
      const std::string chunk(4096, 'A');
      for (int i = 0; i < 1280; ++i) {  // 5 MB total
        if (!flooder.SendRawBytes(chunk)) break;
      }
      // Same connection is dead: a subscribe on it gets no reply.
      std::string line;
      (void)flooder.SendLine(
          R"({"id":1,"method":"mining.subscribe","params":["oeeoo-test/1.0"]})");
      CHECK(!flooder.ReadLine(line, 2000));
      flooder.Close();
      // A new client works fine: the server survived.
      StratumClient client;
      CHECK(client.Connect(port));
      CHECK(client.SendLine(
          R"({"id":1,"method":"mining.subscribe","params":["oeeoo-test/1.0"]})"));
      Json reply;
      CHECK(client.WaitResponse(1, reply));
      CHECK(reply.Has("result"));
      client.Close();
    }
    server.Stop();
  }

  test::BeginSection("m18b: regressions — fuzzing finds R2/R4 + JSON depth");
  {
    // M18B-R2: wallet payload length overflow. Build a wallet
    // file (with the right passphrase, so the GCM tag verifies)
    // whose plaintext declares mnemonicLen = 0xFFFFFFF0. The
    // old code computed `16 + mnemonicLen` in uint32, wrapped
    // to 0, and read ~4 GB out of a 16-byte buffer (ASan
    // heap-buffer-overflow). Now it must fail cleanly.
    const std::string path = "/tmp/oeeoo-m18b-" + pid + "-wallet.oeeoo";
    {
      const std::string passphrase = "repro-passphrase";
      unsigned char salt[16];
      unsigned char nonce[12];
      CHECK(RAND_bytes(salt, sizeof(salt)) == 1);
      CHECK(RAND_bytes(nonce, sizeof(nonce)) == 1);
      unsigned char key[32];
      PKCS5_PBKDF2_HMAC(passphrase.c_str(),
                        static_cast<int>(passphrase.size()), salt,
                        sizeof(salt), 200000, EVP_sha512(), 32, key);
      std::vector<unsigned char> plaintext;
      const auto appendLE32 = [](std::vector<unsigned char>& out,
                                 uint32_t v) {
        out.push_back(static_cast<unsigned char>(v & 0xff));
        out.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
        out.push_back(static_cast<unsigned char>((v >> 16) & 0xff));
        out.push_back(static_cast<unsigned char>((v >> 24) & 0xff));
      };
      appendLE32(plaintext, 1);
      appendLE32(plaintext, 0xFFFFFFF0u);
      for (int i = 0; i < 8; ++i) plaintext.push_back(0x41);
      std::vector<unsigned char> ciphertext(plaintext.size());
      unsigned char tag[16];
      EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
      CHECK(ctx != nullptr);
      CHECK(EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr,
                               nullptr) == 1);
      CHECK(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) ==
            1);
      CHECK(EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, nonce) == 1);
      int outLen = 0;
      CHECK(EVP_EncryptUpdate(ctx, ciphertext.data(), &outLen,
                              plaintext.data(),
                              static_cast<int>(plaintext.size())) == 1);
      CHECK(EVP_EncryptFinal_ex(ctx, ciphertext.data() + outLen, &outLen) ==
            1);
      CHECK(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1);
      EVP_CIPHER_CTX_free(ctx);
      std::vector<unsigned char> file;
      const char* magic = "OEEOOWALLET1";
      file.insert(file.end(), magic, magic + 12);
      file.insert(file.end(), salt, salt + 16);
      file.insert(file.end(), nonce, nonce + 12);
      file.insert(file.end(), ciphertext.begin(), ciphertext.end());
      file.insert(file.end(), tag, tag + 16);
      FILE* f = std::fopen(path.c_str(), "wb");
      CHECK(f != nullptr);
      CHECK(std::fwrite(file.data(), 1, file.size(), f) == file.size());
      std::fclose(f);
      WalletFileData data;
      std::string werror;
      CHECK(!LoadWalletFile(path, passphrase, data, werror));
      CHECK(werror == "wallet payload is corrupt");
      std::remove(path.c_str());
    }
    // A valid wallet round-trips through the same loader.
    {
      WalletFileData data;
      data.mnemonic = "abandon abandon abandon abandon abandon abandon "
                      "abandon abandon abandon abandon abandon about";
      data.nextExternal = 3;
      data.nextChange = 1;
      std::string werror;
      CHECK(SaveWalletFile(path, data, "m18b-test-pass", werror));
      WalletFileData loaded;
      CHECK(LoadWalletFile(path, "m18b-test-pass", loaded, werror));
      CHECK(loaded.mnemonic == data.mnemonic);
      CHECK(loaded.nextExternal == 3);
      CHECK(loaded.nextChange == 1);
      CHECK(!LoadWalletFile(path, "wrong-pass", loaded, werror));
      std::remove(path.c_str());
    }
    // M18B-R4: IntFromString used to throw out of its own catch
    // block on garbage input.
    CHECK(Json::IntFromString("abc").IsNull());
    CHECK(Json::IntFromString("").IsNull());
    CHECK(Json::IntFromString("123").AsInt() == 123);
    // std::stoll takes the leading-int prefix ("1.5" -> 1) and
    // only falls back to stod when no int parses at all.
    CHECK(Json::IntFromString("1.5").AsInt() == 1);
    CHECK(Json::IntFromString("-42").AsInt() == -42);
    // JSON depth limit: 63 nested arrays parse, 100 do not.
    {
      Json j;
      std::string parseError;
      CHECK(Json::Parse(NestedJson(63), j, parseError));
      CHECK(!Json::Parse(NestedJson(100), j, parseError));
      CHECK(!parseError.empty());
    }
  }
}
