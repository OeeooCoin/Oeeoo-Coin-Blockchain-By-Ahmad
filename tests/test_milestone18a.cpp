// Tests: Milestone 18 Stream A — outbound P2P dialing.
//
// The M17 finding (docs/MILESTONE17.md §C.1 item 8): the shipped
// node could not open an outbound P2P connection — no dial loop,
// no --addnode, no addnode RPC; Node::ConnectTo had zero
// production callers; two stock nodes never peered. These tests
// cover the M18 Stream A fix: the dial loop in Node::Poll, the
// --addnode flag / config key, the `addnode` RPC, and DNS-seed
// results actually being dialed.
//
// Ports are derived from the process id and retried on the next
// port when busy — never fixed (the M16 report notes an
// environmental flake from a fixed-port collision). Data
// directories are unique per run (pid-tagged) and removed
// afterwards. The real-binary section reuses the M17 Stream B
// spawn pattern (fork + exec of the sibling oeeoo-node).
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
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "app/cli.h"
#include "app/config.h"
#include "app/genesis.h"
#include "blockchain/block.h"
#include "consensus/amount.h"
#include "consensus/consensus.h"
#include "crypto/address.h"
#include "crypto/ecdsa.h"
#include "mining/miner.h"
#include "network/dnsseed.h"
#include "network/node.h"
#include "network/protocol.h"
#include "network/transport.h"
#include "rpc/json.h"
#include "rpc/rpcutil.h"
#include "script/script.h"
#include "test_harness.h"
#include "transactions/transaction.h"

using namespace oeeoo;

namespace {
constexpr uint32_t kTestBits = 0x207fffff;  // easy test-only difficulty
constexpr uint32_t kTime = 1700000000;
constexpr uint32_t kGenesisTime = 1793923200;  // 2026-11-06 00:00 UTC

std::string PidTag() { return std::to_string(static_cast<long>(::getpid())); }

void RmRf(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
}

// ---- in-process node helpers (TCP, real time) ----

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

// A genesis block mined at test difficulty (the M15 pattern).
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

// A block that always fails validation (impossible target): +100
// misbehavior, an instant ban (the M15 pattern).
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

// Pid-derived TCP ports for in-process listeners. The base moves
// with the pid so two suite runs on one machine do not collide;
// NextTestPort hands out distinct ports within a run.
uint16_t NextTestPort() {
  static int next = 0;
  const int base = 48000 + (static_cast<int>(::getpid()) % 300) * 7;
  return static_cast<uint16_t>(base + (next++) * 13);
}

// A node on a TcpTransport. Does NOT listen unless ListenOn is
// called (a pure dialer needs no listen socket).
std::unique_ptr<Node> TcpNode() {
  auto node = std::make_unique<Node>(kTestBits);
  node->SetTransport(std::make_shared<TcpTransport>());
  return node;
}

// Listen on a fresh pid-derived port (retried while busy).
// False when no port in the batch would bind.
bool ListenOn(Node& node, uint16_t& portOut) {
  for (int attempt = 0; attempt < 10; ++attempt) {
    const uint16_t port = NextTestPort();
    if (node.Listen(port)) {
      portOut = port;
      return true;
    }
  }
  return false;
}

// Pump Poll on all nodes (real time, small timeout) until `done`
// or `rounds` run out. Returns the final state of `done()`.
template <typename F>
bool PumpTcp(const std::vector<Node*>& nodes, F done, int rounds = 400) {
  for (int i = 0; i < rounds; ++i) {
    for (Node* n : nodes) n->Poll(5);
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return done();
}

// ---- real-binary helpers (the M17 Stream B pattern) ----

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

bool RpcRaw(const std::string& dataDir, uint16_t rpcPort,
            const std::string& method, const Json& params, Json& response,
            std::string& error) {
  return CliCall(dataDir, rpcPort, method, params, response, error);
}

// True only when the transport worked AND the response has no
// "error". `result` is the response's "result" on success.
bool RpcOk(const std::string& dataDir, uint16_t rpcPort,
           const std::string& method, const Json& params, Json& result,
           std::string& error) {
  Json response;
  if (!RpcRaw(dataDir, rpcPort, method, params, response, error)) {
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

// The RPC error code for a call, or -99999 when the call
// succeeded / the transport failed. Used to assert Core-shaped
// addnode errors (-23 already added, -24 not added, -29 not
// connected).
int RpcErrorCode(const std::string& dataDir, uint16_t rpcPort,
                 const std::string& method, const Json& params) {
  Json response;
  std::string error;
  if (!RpcRaw(dataDir, rpcPort, method, params, response, error)) {
    return -99999;
  }
  const Json* err = response.Get("error");
  if (err == nullptr || err->IsNull()) return -99999;
  const Json* code = err->Get("code");
  if (code == nullptr) return -99999;
  return static_cast<int>(code->AsInt(-99999));
}

bool RpcPeerCount(const std::string& dataDir, uint16_t rpcPort,
                  std::size_t& count) {
  Json result;
  std::string error;
  if (!RpcOk(dataDir, rpcPort, "getpeerinfo", Json::Array(), result,
             error)) {
    return false;
  }
  if (!result.IsArray()) return false;
  count = result.AsArray().size();
  return true;
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
    Json result;
    std::string error;
    if (RpcOk(dataDir, rpcPort, "getblockcount", Json::Array(), result,
              error)) {
      return true;
    }
    if (!ChildAlive(pid)) return false;  // died (e.g. port busy)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

// Wait until a node's getpeerinfo reports at least `want` peers
// (or the timeout expires). False on timeout / RPC failure.
bool WaitPeerCount(const std::string& dataDir, uint16_t rpcPort,
                   std::size_t want, int timeoutSec) {
  for (int i = 0; i < timeoutSec * 10; ++i) {
    std::size_t count = 0;
    if (RpcPeerCount(dataDir, rpcPort, count) && count >= want) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

// Fork + exec the node. `extraArgs` are appended verbatim (used
// for --addnode). `rlimitFsize` is unused here (kept for parity
// with the M17 helper); pass -1.
pid_t SpawnNode(const std::string& nodeBin, const std::string& dataDir,
                const std::string& genesisPath, int p2pPort, int rpcPort,
                const std::string& logPath,
                const std::vector<std::string>& extraArgs) {
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
    for (const std::string& a : extraArgs) args.push_back(a);
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    ::execv(nodeBin.c_str(), argv.data());
    ::_exit(127);
  }
  return pid;
}

// Start the node and wait until its RPC answers. Retries on the
// next pid-derived port pair when the child dies (port busy) or
// does not answer in time. Returns false when no attempt came up.
bool StartNodeReady(const std::string& nodeBin, const std::string& dataDir,
                    const std::string& genesisPath,
                    const std::string& logPath,
                    const std::vector<std::string>& extraArgs,
                    pid_t& pidOut, uint16_t& rpcPortOut,
                    uint16_t& p2pPortOut) {
  const int pidPart = static_cast<int>(::getpid()) % 400;
  for (int attempt = 0; attempt < 4; ++attempt) {
    const int base = 48000 + ((pidPart + attempt * 53) % 400) * 3;
    const uint16_t p2pPort = static_cast<uint16_t>(base);
    const uint16_t rpcPort = static_cast<uint16_t>(base + 1);
    const pid_t pid = SpawnNode(nodeBin, dataDir, genesisPath, p2pPort,
                                rpcPort, logPath, extraArgs);
    if (pid < 0) return false;
    if (WaitRpcReady(dataDir, rpcPort, pid, 10)) {
      pidOut = pid;
      rpcPortOut = rpcPort;
      p2pPortOut = p2pPort;
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

// A throwaway test genesis (the M17 Stream B pattern): fresh
// founder keys per run, test difficulty, test data dirs only.
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

bool FileContains(const std::string& path, const std::string& needle) {
  std::ifstream in(path);
  if (!in) return false;
  std::string text((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  return text.find(needle) != std::string::npos;
}
}  // namespace

void TestMilestone18A() {
  EnsureKeys();
  const Block genesis = MineGenesis();

  test::BeginSection("m18a: config — --addnode flag and addnode key");
  {
    // CLI: repeatable --addnode lands in AppConfig::addNodes.
    AppConfig config;
    std::string error;
    char* argv[] = {(char*)"oeeoo-node", (char*)"--addnode",
                    (char*)"127.0.0.1:64775", (char*)"--addnode",
                    (char*)"127.0.0.1:64776", nullptr};
    CHECK(ParseArgs(5, argv, config, error));
    CHECK(config.addNodes.size() == 2);
    CHECK(config.addNodes[0] == "127.0.0.1:64775");
    CHECK(config.addNodes[1] == "127.0.0.1:64776");
    // --addnode with no value is an error, like --dns-seed.
    AppConfig config2;
    char* badArgv[] = {(char*)"oeeoo-node", (char*)"--addnode", nullptr};
    CHECK(!ParseArgs(2, badArgv, config2, error));
    CHECK(!error.empty());
    // Config file: repeatable `addnode` key.
    const std::string confPath = "/tmp/oeeoo-m18a-" + PidTag() + ".conf";
    {
      std::ofstream out(confPath);
      out << "addnode = 127.0.0.1:64775\naddnode = 127.0.0.1:64776\n";
    }
    AppConfig config3;
    CHECK(LoadConfigFile(confPath, config3, error));
    CHECK(config3.addNodes.size() == 2);
    CHECK(config3.addNodes[1] == "127.0.0.1:64776");
    ::remove(confPath.c_str());
    // Usage documents the flag.
    CHECK(UsageText("oeeoo-node").find("--addnode") != std::string::npos);
  }

  test::BeginSection("m18a: AddManualPeer / RemoveManualPeer (node API)");
  {
    Node node(kTestBits);
    std::string error;
    CHECK(node.AddManualPeer("127.0.0.1:64775", error));
    CHECK(node.AddrMan().Contains("127.0.0.1", 64775));
    CHECK(node.IsKnownPeer("127.0.0.1:64775"));
    // Duplicate add is refused.
    CHECK(!node.AddManualPeer("127.0.0.1:64775", error));
    CHECK(error.find("already added") != std::string::npos);
    // Malformed addresses are refused, never stored.
    CHECK(!node.AddManualPeer("not-an-address", error));
    CHECK(!node.AddManualPeer("127.0.0.1", error));
    CHECK(!node.AddManualPeer("127.0.0.1:99999", error));
    CHECK(node.AddrMan().Size() == 1);
    // Remove of an unknown address is false; remove of a known
    // one drops it from the book.
    CHECK(!node.RemoveManualPeer("127.0.0.1:64776"));
    CHECK(node.RemoveManualPeer("127.0.0.1:64775"));
    CHECK(!node.IsKnownPeer("127.0.0.1:64775"));
    CHECK(node.AddrMan().Size() == 0);
  }

  test::BeginSection("m18a: two nodes peer via the outbound dial loop");
  {
    // The M17 finding, fixed: two stock nodes peer with nobody
    // dialing by hand — B's dial loop opens the connection.
    auto a = TcpNode();
    auto b = TcpNode();
    std::string error;
    CHECK(a->BroadcastBlock(genesis, error));
    CHECK(b->BroadcastBlock(genesis, error));
    uint16_t portA = 0;
    CHECK(ListenOn(*a, portA));
    const std::string addrA =
        "127.0.0.1:" + std::to_string(portA);
    CHECK(b->AddManualPeer(addrA, error));
    // B has never dialed: no peers on either side.
    CHECK(a->PeerCount() == 0);
    CHECK(b->PeerCount() == 0);
    // Poll drives the dial loop: B dials A on its first pass.
    const bool peered = PumpTcp({a.get(), b.get()}, [&] {
      return a->HandshookPeerCount() == 1 && b->HandshookPeerCount() == 1;
    });
    CHECK(peered);
    CHECK(a->PeerCount() == 1);
    CHECK(b->PeerCount() == 1);
    CHECK(b->OutboundPeerCount() == 1);  // B dialed out
    CHECK(a->OutboundPeerCount() == 0);  // A's peer is inbound
    CHECK(b->IsConnectedTo(addrA));
    // The handshake also resets the dialed address's failure
    // count (a peer that failed twice and then connects is not
    // dead): the entry survives, failures 0.
    CHECK(b->AddrMan().Contains("127.0.0.1", portA));
  }

  test::BeginSection("m18a: dial loop never dials a banned address");
  {
    // D bans 127.0.0.1 (an invalid block from H earns +100), then
    // the address is put in D's book: the dial loop must skip it
    // — no connection, and the entry is NOT dropped (skipping is
    // not a failure).
    auto h = TcpNode();
    auto d = TcpNode();
    std::string error;
    CHECK(h->BroadcastBlock(genesis, error));
    CHECK(d->BroadcastBlock(genesis, error));
    uint16_t portH = 0;
    CHECK(ListenOn(*h, portH));
    const std::string addrH = "127.0.0.1:" + std::to_string(portH);
    CHECK(d->ConnectTo(addrH));
    const bool connected = PumpTcp({h.get(), d.get()}, [&] {
      return h->HandshookPeerCount() == 1 && d->HandshookPeerCount() == 1;
    });
    CHECK(connected);
    // H sends an invalid block to D: D bans the IP and drops H.
    std::string dAddr;
    for (const Peer& p : h->GetPeers()) dAddr = p.address;
    CHECK(!dAddr.empty());
    CHECK(h->SendRawToAddress(dAddr, {"block", BadBlock(genesis.GetHash())
                                                  .Serialize()}));
    const bool banned = PumpTcp({h.get(), d.get()}, [&] {
      return d->Bans().Size() == 1;
    });
    CHECK(banned);
    CHECK(d->PeerCount() == 0);
    // H learns the peer is gone too: D's ban-disconnect closed
    // the socket, and the transport reports the close to H's
    // node (Milestone 18 fix — before it, H kept a dead peer
    // entry forever and could never re-dial that address).
    const bool hDropped = PumpTcp({h.get(), d.get()}, [&] {
      return h->PeerCount() == 0;
    });
    CHECK(hDropped);
    // Now the banned address enters D's book, and the dial loop
    // runs with the throttle off (every pass is a real attempt).
    CHECK(d->AddManualPeer(addrH, error));
    d->SetDialIntervalSeconds(0);
    for (int i = 0; i < 3; ++i) d->TryDialPeers();
    PumpTcp({h.get(), d.get()}, [&] { return false; }, 20);
    CHECK(d->PeerCount() == 0);  // never dialed
    CHECK(h->PeerCount() == 0);  // nobody connected to H
    CHECK(d->AddrMan().Contains("127.0.0.1", portH));  // not dropped
  }

  test::BeginSection("m18a: a dropped peer can be re-dialed");
  {
    // The disconnect notification is what makes the dial loop
    // work long-term: when A goes away B learns it (no dead
    // entry), and when A comes back on the same port the loop
    // dials it again. Without the notification B would keep a
    // dead entry for A forever and skip it on every pass.
    auto a = TcpNode();
    auto b = TcpNode();
    std::string error;
    CHECK(a->BroadcastBlock(genesis, error));
    CHECK(b->BroadcastBlock(genesis, error));
    uint16_t portA = 0;
    CHECK(ListenOn(*a, portA));
    const std::string addrA = "127.0.0.1:" + std::to_string(portA);
    CHECK(b->AddManualPeer(addrA, error));
    // 2 s dial interval: a pass that lands in the short window
    // while A is down costs at most one failure (the entry
    // survives — 3 failures drop it), and the next pass dials
    // the returned peer.
    b->SetDialIntervalSeconds(2);
    const bool peered = PumpTcp({a.get(), b.get()}, [&] {
      return a->HandshookPeerCount() == 1 && b->HandshookPeerCount() == 1;
    });
    CHECK(peered);
    // A goes away: destroying the node closes its sockets, and
    // B's next Poll reports the close.
    a.reset();
    const bool dropped = PumpTcp({b.get()}, [&] {
      return b->PeerCount() == 0;
    });
    CHECK(dropped);
    // A comes back on the same port: the dial loop re-dials it.
    auto a2 = TcpNode();
    CHECK(a2->BroadcastBlock(genesis, error));
    CHECK(a2->Listen(portA));
    const bool repeered = PumpTcp({a2.get(), b.get()},
                                  [&] {
                                    return a2->HandshookPeerCount() == 1 &&
                                           b->HandshookPeerCount() == 1;
                                  },
                                  600);
    CHECK(repeered);
    CHECK(b->PeerCount() == 1);
    CHECK(b->OutboundPeerCount() == 1);
    CHECK(b->IsConnectedTo(addrA));
  }

  test::BeginSection("m18a: dial loop respects the outbound cap");
  {
    // Four listeners, cap 2: exactly two get dialed, the other
    // two never see a connection — even with the throttle off.
    auto d = TcpNode();
    std::string error;
    CHECK(d->BroadcastBlock(genesis, error));
    d->SetMaxOutboundPeers(2);
    d->SetDialIntervalSeconds(0);
    std::vector<std::unique_ptr<Node>> listeners;
    std::vector<uint16_t> ports;
    for (int i = 0; i < 4; ++i) {
      listeners.push_back(TcpNode());
      CHECK(listeners.back()->BroadcastBlock(genesis, error));
      uint16_t port = 0;
      CHECK(ListenOn(*listeners.back(), port));
      ports.push_back(port);
      CHECK(d->AddManualPeer("127.0.0.1:" + std::to_string(port), error));
    }
    d->TryDialPeers();
    std::vector<Node*> all{d.get()};
    for (auto& l : listeners) all.push_back(l.get());
    const bool capped = PumpTcp(all, [&] {
      return d->OutboundPeerCount() == 2;
    });
    CHECK(capped);
    CHECK(d->PeerCount() == 2);
    // A second dial pass with the throttle off still dials
    // nothing: the cap, not the throttle, is what holds.
    d->TryDialPeers();
    PumpTcp(all, [&] { return false; }, 20);
    CHECK(d->OutboundPeerCount() == 2);
    CHECK(d->PeerCount() == 2);
    // Exactly two listeners were dialed.
    int dialed = 0;
    for (auto& l : listeners) {
      if (l->PeerCount() == 1) ++dialed;
    }
    CHECK(dialed == 2);
    // The default cap is 10 (Oeeoo's own choice).
    Node fresh(kTestBits);
    CHECK(fresh.MaxOutboundPeers() == 10);
  }

  test::BeginSection(
      "m18a: dial loop skips already-connected peers and itself");
  {
    // B's book holds A (a live peer) and B's own listen address.
    // Repeated dial passes must open no second connection to A
    // and no self-connection.
    auto a = TcpNode();
    auto b = TcpNode();
    std::string error;
    CHECK(a->BroadcastBlock(genesis, error));
    CHECK(b->BroadcastBlock(genesis, error));
    uint16_t portA = 0, portB = 0;
    CHECK(ListenOn(*a, portA));
    CHECK(ListenOn(*b, portB));
    const std::string addrA = "127.0.0.1:" + std::to_string(portA);
    const std::string addrB = "127.0.0.1:" + std::to_string(portB);
    CHECK(b->AddManualPeer(addrA, error));
    CHECK(b->AddManualPeer(addrB, error));
    b->SetDialIntervalSeconds(0);
    const bool peered = PumpTcp({a.get(), b.get()}, [&] {
      return a->HandshookPeerCount() == 1 && b->HandshookPeerCount() == 1;
    });
    CHECK(peered);
    CHECK(b->PeerCount() == 1);
    // Five more dial passes: still exactly one peer, and it is A.
    for (int i = 0; i < 5; ++i) b->TryDialPeers();
    PumpTcp({a.get(), b.get()}, [&] { return false; }, 20);
    CHECK(b->PeerCount() == 1);
    CHECK(b->OutboundPeerCount() == 1);
    CHECK(b->IsConnectedTo(addrA));
    CHECK(!b->IsConnectedTo(addrB));  // never dialed itself
    CHECK(a->PeerCount() == 1);       // no duplicate inbound either
    // ConnectTo itself refuses only our own listen address (the
    // already-connected guard lives in the dial loop and the
    // `addnode onetry` RPC, not in ConnectTo — see node.h).
    CHECK(!b->ConnectTo(addrB));
  }

  test::BeginSection("m18a: DNS-seed results are dialed");
  {
    // A fresh node with an empty AddrMan queries its seeds and
    // the dial loop dials what comes back — this is the exact
    // M17 gap (seeds filled the book, nothing dialed it).
    auto a = TcpNode();
    auto dialer = TcpNode();
    std::string error;
    CHECK(a->BroadcastBlock(genesis, error));
    CHECK(dialer->BroadcastBlock(genesis, error));
    dialer->AddDnsSeed("localhost");
    CHECK(dialer->AddrMan().Size() == 0);
    const std::size_t added = dialer->QueryDnsSeeds(error);
    CHECK(added >= 1);
    CHECK(dialer->AddrMan().Contains("127.0.0.1", 64775));
    // The seed port is the mainnet P2P port (64775): if it is
    // free, a listener there proves the full path end to end
    // (seed -> book -> dial -> peer). If another process owns
    // it, the fallback proves the dial loop still ATTEMPTS the
    // seed address (3 failed passes drop it from the book, or a
    // foreign listener answers and it peers).
    if (a->Listen(64775)) {
      const bool peered = PumpTcp({a.get(), dialer.get()}, [&] {
        return a->HandshookPeerCount() == 1 &&
               dialer->HandshookPeerCount() == 1;
      });
      CHECK(peered);
      CHECK(dialer->OutboundPeerCount() == 1);
      CHECK(dialer->IsConnectedTo("127.0.0.1:64775"));
    } else {
      dialer->SetDialIntervalSeconds(0);
      for (int i = 0; i < 3; ++i) dialer->TryDialPeers();
      CHECK(!dialer->AddrMan().Contains("127.0.0.1", 64775) ||
            dialer->PeerCount() >= 1);
    }
  }

  test::BeginSection("m18a: dial loop drops a dead address after 3 failures");
  {
    // Backoff, observed: a dead address is retried once per dial
    // pass and dropped after 3 failures — the loop does not spin
    // on it forever.
    auto d = TcpNode();
    std::string error;
    uint16_t deadPort = 0;
    // A port nothing listens on: bind a listener, read its port,
    // then destroy it, so the port is certainly dead.
    {
      auto tmp = TcpNode();
      CHECK(ListenOn(*tmp, deadPort));
    }
    CHECK(d->AddManualPeer("127.0.0.1:" + std::to_string(deadPort), error));
    d->SetDialIntervalSeconds(0);
    CHECK(d->AddrMan().Contains("127.0.0.1", deadPort));
    d->TryDialPeers();
    CHECK(d->AddrMan().Contains("127.0.0.1", deadPort));  // 1 failure
    d->TryDialPeers();
    CHECK(d->AddrMan().Contains("127.0.0.1", deadPort));  // 2 failures
    d->TryDialPeers();
    CHECK(!d->AddrMan().Contains("127.0.0.1", deadPort));  // dropped
    CHECK(d->PeerCount() == 0);
  }

  // ---- real-binary end to end: --addnode + addnode RPC ----
  test::BeginSection("m18a: E2E — two stock nodes peer via --addnode, "
                     "addnode RPC add/remove/onetry");
  {
    const std::string nodeBin = NodeBinaryPath();
    CHECK(::access(nodeBin.c_str(), X_OK) == 0);
    if (::access(nodeBin.c_str(), X_OK) != 0) {
      std::printf("  [m18a] oeeoo-node binary not found next to the test "
                  "binary (looked for %s); E2E tests cannot run\n",
                  nodeBin.c_str());
      return;
    }
    const std::string tag = "/tmp/oeeoo-m18a-" + PidTag();
    const std::string genesisPath = tag + "-genesis.json";
    {
      std::string error;
      CHECK(BuildTestGenesis(genesisPath, error));
    }
    const std::string dataA = tag + "-a-data";
    const std::string dataB = tag + "-b-data";
    const std::string logA = tag + "-a.log";
    const std::string logB = tag + "-b.log";
    RmRf(dataA);
    RmRf(dataB);
    pid_t pidA = -1, pidB = -1;
    uint16_t rpcA = 0, rpcB = 0, p2pA = 0, p2pB = 0;
    int status = 0;
    // Node A: stock, no --addnode. Node B: --addnode pointing at
    // A. Nobody dials by hand — B's dial loop must peer them.
    CHECK(StartNodeReady(nodeBin, dataA, genesisPath, logA, {}, pidA,
                         rpcA, p2pA));
    const std::string addrA = "127.0.0.1:" + std::to_string(p2pA);
    CHECK(StartNodeReady(nodeBin, dataB, genesisPath, logB,
                         {"--addnode", addrA}, pidB, rpcB, p2pB));
    CHECK(WaitPeerCount(dataA, rpcA, 1, 20));
    CHECK(WaitPeerCount(dataB, rpcB, 1, 20));
    // onetry to the peer B is already connected to is refused
    // (no duplicate connection) — the guard lives at the RPC,
    // since ConnectTo itself always attempts a dial.
    Json onetryConnected = Json::Array();
    onetryConnected.Push(Json(addrA));
    onetryConnected.Push(Json("onetry"));
    CHECK(RpcErrorCode(dataB, rpcB, "addnode", onetryConnected) ==
          -32602);
    // addnode add: A is already known on B (via --addnode) -> -23.
    Json params = Json::Array();
    params.Push(Json(addrA));
    params.Push(Json("add"));
    CHECK(RpcErrorCode(dataB, rpcB, "addnode", params) == -23);
    // addnode add/remove round-trip on a dead address.
    Json dead = Json::Array();
    dead.Push(Json("127.0.0.1:1"));
    dead.Push(Json("add"));
    Json result;
    std::string error;
    CHECK(RpcOk(dataB, rpcB, "addnode", dead, result, error));
    CHECK(result.IsNull());
    CHECK(RpcErrorCode(dataB, rpcB, "addnode", dead) == -23);  // dup
    Json deadRemove = Json::Array();
    deadRemove.Push(Json("127.0.0.1:1"));
    deadRemove.Push(Json("remove"));
    CHECK(RpcOk(dataB, rpcB, "addnode", deadRemove, result, error));
    CHECK(RpcErrorCode(dataB, rpcB, "addnode", deadRemove) ==
          -24);  // gone
    // onetry to a dead port cannot connect -> -29.
    Json onetry = Json::Array();
    onetry.Push(Json("127.0.0.1:1"));
    onetry.Push(Json("onetry"));
    CHECK(RpcErrorCode(dataB, rpcB, "addnode", onetry) == -29);
    // Bad address / bad command are parameter errors.
    Json badAddr = Json::Array();
    badAddr.Push(Json("not-an-address"));
    badAddr.Push(Json("add"));
    CHECK(RpcErrorCode(dataB, rpcB, "addnode", badAddr) == -32602);
    Json badCmd = Json::Array();
    badCmd.Push(Json("127.0.0.1:1"));
    badCmd.Push(Json("explode"));
    CHECK(RpcErrorCode(dataB, rpcB, "addnode", badCmd) == -32602);
    // addnode remove of the live peer disconnects it, and adding
    // it back re-peers via the dial loop.
    Json removeA = Json::Array();
    removeA.Push(Json(addrA));
    removeA.Push(Json("remove"));
    CHECK(RpcOk(dataB, rpcB, "addnode", removeA, result, error));
    bool dropped = false;
    for (int i = 0; i < 100 && !dropped; ++i) {
      std::size_t count = 0;
      if (RpcPeerCount(dataB, rpcB, count)) dropped = (count == 0);
      if (!dropped) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
    CHECK(dropped);
    Json addA = Json::Array();
    addA.Push(Json(addrA));
    addA.Push(Json("add"));
    CHECK(RpcOk(dataB, rpcB, "addnode", addA, result, error));
    CHECK(WaitPeerCount(dataB, rpcB, 1, 20));  // dial loop re-dials
    CHECK(WaitPeerCount(dataA, rpcA, 1, 20));
    CHECK(StopNode(pidB, status));
    CHECK(StopNode(pidA, status));
    RmRf(dataA);
    RmRf(dataB);
    ::remove(logA.c_str());
    ::remove(logB.c_str());
    ::remove(genesisPath.c_str());
  }

  test::BeginSection("m18a: E2E — malformed --addnode refuses startup");
  {
    const std::string nodeBin = NodeBinaryPath();
    CHECK(::access(nodeBin.c_str(), X_OK) == 0);
    if (::access(nodeBin.c_str(), X_OK) != 0) {
      std::printf("  [m18a] oeeoo-node binary not found next to the test "
                  "binary (looked for %s); E2E tests cannot run\n",
                  nodeBin.c_str());
      return;
    }
    const std::string tag = "/tmp/oeeoo-m18a-" + PidTag() + "-badadd";
    const std::string genesisPath = tag + "-genesis.json";
    {
      std::string error;
      CHECK(BuildTestGenesis(genesisPath, error));
    }
    const std::string dataDir = tag + "-data";
    const std::string logPath = tag + ".log";
    RmRf(dataDir);
    const uint16_t p2pPort = NextTestPort();
    const uint16_t rpcPort = NextTestPort();
    const pid_t pid =
        SpawnNode(nodeBin, dataDir, genesisPath, p2pPort, rpcPort, logPath,
                  {"--addnode", "not-an-address"});
    CHECK(pid > 0);
    int status = 0;
    CHECK(WaitExit(pid, 10, status));
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(FileContains(logPath, "invalid peer address"));
    RmRf(dataDir);
    ::remove(logPath.c_str());
    ::remove(genesisPath.c_str());
  }
}
