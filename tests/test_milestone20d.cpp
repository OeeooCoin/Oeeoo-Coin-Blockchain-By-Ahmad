// Tests: Milestone 20 Worker D — network-identity separation
// (audit OOC-SA-001 §3.3, decision B2).
//
// Mainnet and the test network have DISTINCT identities (magic,
// default ports, user agent) so a node on one network can never
// handshake with a node on the other. The active identity is
// process-wide (protocol.cpp, default mainnet); node_main selects
// it once from --testnet / `network=testnet`. These tests switch
// it only around the code under test and always restore mainnet
// (NetworkGuard, RAII) so later test files are unaffected — the
// default is mainnet, so tests that never opt into testnet are
// unaffected.
//
// Ports are derived from the process id and retried on the next
// port when busy — never fixed (the M16 report notes an
// environmental flake from a fixed-port collision). Data
// directories are not needed here (in-process nodes, no store).
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "app/config.h"
#include "blockchain/block.h"
#include "consensus/amount.h"
#include "consensus/consensus.h"
#include "crypto/address.h"
#include "crypto/ecdsa.h"
#include "mining/miner.h"
#include "network/node.h"
#include "network/protocol.h"
#include "network/transport.h"
#include "script/script.h"
#include "test_harness.h"
#include "transactions/transaction.h"

using namespace oeeoo;

namespace {
constexpr uint32_t kTestBits = 0x207fffff;  // easy test-only difficulty
constexpr uint32_t kTime = 1700000000;

std::string PidTag() { return std::to_string(static_cast<long>(::getpid())); }

// Restores the previously active network on destruction, so a
// section that selects testnet can never leak it into the next
// test file (the suite default is mainnet).
struct NetworkGuard {
  NetworkId saved = ActiveNetwork();
  ~NetworkGuard() { SelectNetwork(saved); }
};

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

// Pump Poll on mixed-network nodes (real time, small timeout)
// until `done` or `rounds` run out. The active network is set to
// each node's own network before its Poll — that is exactly what
// node_main does once per process, replayed per node here because
// two networks share this one test process. Returns the final
// state of `done()`. Mainnet is restored before returning.
template <typename F>
bool PumpMixed(const std::vector<std::pair<Node*, NetworkId>>& nodes,
               F done, int rounds = 600) {
  for (int i = 0; i < rounds; ++i) {
    for (const auto& entry : nodes) {
      SelectNetwork(entry.second);
      entry.first->Poll(5);
    }
    SelectNetwork(NetworkId::kMainnet);
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  SelectNetwork(NetworkId::kMainnet);
  return done();
}

bool WriteFile(const std::string& path, const std::string& content) {
  std::ofstream out(path);
  out << content;
  return static_cast<bool>(out);
}
}  // namespace

void TestMilestone20D() {
  EnsureKeys();

  test::BeginSection("m20d: mainnet and testnet identities differ");
  {
    NetworkGuard guard;
    // Exact mainnet identity (FINALIZED, unchanged by M20).
    const NetworkIdentity& main = NetworkIdentityFor(NetworkId::kMainnet);
    CHECK(main.magic == kMainnetMagic);
    CHECK(main.magic ==
          (std::array<unsigned char, 4>{0xe2, 0xd8, 0x04, 0xd6}));
    CHECK(main.p2pPort == 64775);
    CHECK(main.rpcPort == 64776);
    CHECK(main.stratumPort == 64777);
    CHECK(std::string(main.userAgent) == "/OeeooCoin:0.1.0/");
    CHECK(std::string(main.name) == "mainnet");
    // The historical kNetworkMagic constant is the mainnet magic.
    CHECK(kNetworkMagic == kMainnetMagic);
    // Exact testnet identity. Magic = first 4 bytes of SHA-256 of
    // "Oeeoo Coin Testnet Network Magic", the SAME derivation as
    // mainnet (verified: the mainnet string hashes to e2 d8 04
    // d6). Computed with python3 hashlib: d9 73 08 c2.
    const NetworkIdentity& test = NetworkIdentityFor(NetworkId::kTestnet);
    CHECK(test.magic == kTestnetMagic);
    CHECK(test.magic ==
          (std::array<unsigned char, 4>{0xd9, 0x73, 0x08, 0xc2}));
    CHECK(test.p2pPort == 64779);
    CHECK(test.rpcPort == 64780);
    CHECK(test.stratumPort == 64781);
    CHECK(std::string(test.userAgent) == "/OeeooCoin:0.1.0(testnet)/");
    CHECK(std::string(test.name) == "testnet");
    // Every field differs between the networks ...
    CHECK(main.magic != test.magic);
    CHECK(main.p2pPort != test.p2pPort);
    CHECK(main.rpcPort != test.rpcPort);
    CHECK(main.stratumPort != test.stratumPort);
    CHECK(std::string(main.userAgent) != std::string(test.userAgent));
    // ... except the protocol version, which is 70015 on BOTH
    // networks by design: the magic mismatch separates them.
    CHECK(kProtocolVersion == 70015);
    // Default active network is mainnet.
    CHECK(ActiveNetwork() == NetworkId::kMainnet);
    CHECK(ActiveNetworkMagic() == kMainnetMagic);
    CHECK(ActiveUserAgent() == "/OeeooCoin:0.1.0/");
    // Selection round-trips.
    SelectNetwork(NetworkId::kTestnet);
    CHECK(ActiveNetwork() == NetworkId::kTestnet);
    CHECK(ActiveNetworkMagic() == kTestnetMagic);
    CHECK(ActiveUserAgent() == "/OeeooCoin:0.1.0(testnet)/");
    SelectNetwork(NetworkId::kMainnet);
    CHECK(ActiveNetwork() == NetworkId::kMainnet);
  }

  test::BeginSection("m20d: wire framing uses the active magic, "
                     "cross-network bytes are rejected");
  {
    NetworkGuard guard;
    const Message msg{"version", {1, 2, 3}};
    // Framed under mainnet: mainnet magic, parses under mainnet.
    SelectNetwork(NetworkId::kMainnet);
    const std::vector<unsigned char> mainBytes = SerializeMessage(msg);
    CHECK(mainBytes.size() >= 4);
    CHECK(mainBytes[0] == 0xe2 && mainBytes[1] == 0xd8 &&
          mainBytes[2] == 0x04 && mainBytes[3] == 0xd6);
    Message parsed;
    std::size_t consumed = 0;
    CHECK(ParseMessage(mainBytes, 0, parsed, consumed));
    CHECK(parsed.command == "version");
    // Framed under testnet: testnet magic, parses under testnet.
    SelectNetwork(NetworkId::kTestnet);
    const std::vector<unsigned char> testBytes = SerializeMessage(msg);
    CHECK(testBytes[0] == 0xd9 && testBytes[1] == 0x73 &&
          testBytes[2] == 0x08 && testBytes[3] == 0xc2);
    CHECK(ParseMessage(testBytes, 0, parsed, consumed));
    CHECK(parsed.command == "version");
    // Cross-network: mainnet bytes do NOT parse under testnet,
    // and testnet bytes do NOT parse under mainnet. This is the
    // check that drops a cross-network peer's every message.
    CHECK(!ParseMessage(mainBytes, 0, parsed, consumed));
    SelectNetwork(NetworkId::kMainnet);
    CHECK(!ParseMessage(testBytes, 0, parsed, consumed));
  }

  test::BeginSection("m20d: a testnet node cannot peer with a "
                     "mainnet node");
  {
    NetworkGuard guard;
    const Block genesis = MineGenesis();
    auto a = TcpNode();  // mainnet listener
    auto b = TcpNode();  // testnet dialer
    std::string error;
    CHECK(a->BroadcastBlock(genesis, error));
    CHECK(b->BroadcastBlock(genesis, error));
    uint16_t portA = 0;
    CHECK(ListenOn(*a, portA));
    const std::string addrA = "127.0.0.1:" + std::to_string(portA);
    CHECK(b->AddManualPeer(addrA, error));
    b->SetDialIntervalSeconds(0);
    // B dials A and sends its version framed with the testnet
    // magic; A drops it at the magic check. A never sends a
    // version back (it never parses one), so B hears nothing
    // either. Pump the full window: no handshake may EVER
    // complete.
    PumpMixed({{a.get(), NetworkId::kMainnet},
               {b.get(), NetworkId::kTestnet}},
              [] { return false; });
    CHECK(a->HandshookPeerCount() == 0);
    CHECK(b->HandshookPeerCount() == 0);
    // The dial DID happen — the connection is up on both sides —
    // so the zero handshakes are a rejection, not a failure to
    // connect. (Bad magic earns +50 misbehavior, below the 100
    // ban threshold, so the unhandshook peer entry lingers;
    // no version/verack ever completes and no block or tx can
    // flow, since every message is magic-checked.)
    CHECK(b->PeerCount() == 1);
    CHECK(a->PeerCount() == 1);
    CHECK(b->OutboundPeerCount() == 1);
    CHECK(b->IsConnectedTo(addrA));
  }

  test::BeginSection("m20d: control — two mainnet nodes DO peer");
  {
    // Same harness, same genesis, both mainnet: the handshake
    // completes. This proves the section above fails on the
    // network mismatch, not on the harness.
    NetworkGuard guard;
    const Block genesis = MineGenesis();
    auto a = TcpNode();
    auto b = TcpNode();
    std::string error;
    CHECK(a->BroadcastBlock(genesis, error));
    CHECK(b->BroadcastBlock(genesis, error));
    uint16_t portA = 0;
    CHECK(ListenOn(*a, portA));
    const std::string addrA = "127.0.0.1:" + std::to_string(portA);
    CHECK(b->AddManualPeer(addrA, error));
    const bool peered = PumpMixed({{a.get(), NetworkId::kMainnet},
                                   {b.get(), NetworkId::kMainnet}},
                                  [&] {
                                    return a->HandshookPeerCount() == 1 &&
                                           b->HandshookPeerCount() == 1;
                                  });
    CHECK(peered);
    CHECK(a->PeerCount() == 1);
    CHECK(b->PeerCount() == 1);
  }

  test::BeginSection("m20d: --testnet / network=testnet select "
                     "testnet, mainnet is the default");
  {
    NetworkGuard guard;
    // Default config: mainnet, mainnet effective ports.
    AppConfig config;
    CHECK(!config.testnet);
    CHECK(config.port == 64775);
    CHECK(config.rpcPort == 64776);
    CHECK(config.stratumPort == 64777);
    CHECK(config.EffectivePort() == 64775);
    CHECK(config.EffectiveRpcPort() == 64776);
    CHECK(config.EffectiveStratumPort() == 64777);
    // --testnet: testnet identity, testnet effective ports. The
    // raw members stay the mainnet defaults — Effective*() is
    // the resolved value node_main listens on.
    std::string error;
    AppConfig testnetConfig;
    char* testnetArgv[] = {(char*)"oeeoo-node", (char*)"--testnet",
                           nullptr};
    CHECK(ParseArgs(2, testnetArgv, testnetConfig, error));
    CHECK(testnetConfig.testnet);
    CHECK(testnetConfig.port == 64775);  // raw member untouched
    CHECK(testnetConfig.EffectivePort() == 64779);
    CHECK(testnetConfig.EffectiveRpcPort() == 64780);
    CHECK(testnetConfig.EffectiveStratumPort() == 64781);
    // --network testnet / mainnet.
    AppConfig netConfig;
    char* netArgv[] = {(char*)"oeeoo-node", (char*)"--network",
                       (char*)"testnet", nullptr};
    CHECK(ParseArgs(3, netArgv, netConfig, error));
    CHECK(netConfig.testnet);
    CHECK(netConfig.EffectivePort() == 64779);
    char* netMainArgv[] = {(char*)"oeeoo-node", (char*)"--network",
                           (char*)"mainnet", nullptr};
    AppConfig netMainConfig;
    CHECK(ParseArgs(3, netMainArgv, netMainConfig, error));
    CHECK(!netMainConfig.testnet);
    CHECK(netMainConfig.EffectivePort() == 64775);
    // Later flag wins: --testnet then --network mainnet.
    AppConfig laterConfig;
    char* laterArgv[] = {(char*)"oeeoo-node", (char*)"--testnet",
                         (char*)"--network", (char*)"mainnet", nullptr};
    CHECK(ParseArgs(4, laterArgv, laterConfig, error));
    CHECK(!laterConfig.testnet);
    // Config file: network=testnet / network=mainnet.
    const std::string confPath = "/tmp/oeeoo-m20d-" + PidTag() + ".conf";
    CHECK(WriteFile(confPath, "network = testnet\n"));
    AppConfig fileConfig;
    CHECK(LoadConfigFile(confPath, fileConfig, error));
    CHECK(fileConfig.testnet);
    CHECK(fileConfig.EffectivePort() == 64779);
    CHECK(fileConfig.EffectiveRpcPort() == 64780);
    CHECK(fileConfig.EffectiveStratumPort() == 64781);
    CHECK(WriteFile(confPath, "network = mainnet\n"));
    AppConfig fileMainConfig;
    CHECK(LoadConfigFile(confPath, fileMainConfig, error));
    CHECK(!fileMainConfig.testnet);
    CHECK(fileMainConfig.EffectivePort() == 64775);
    // An explicit port always wins over the testnet default,
    // whether from the CLI or the file (and regardless of order).
    AppConfig explicitConfig;
    char* explicitArgv[] = {(char*)"oeeoo-node", (char*)"--testnet",
                            (char*)"--port", (char*)"65000",
                            (char*)"--rpcport", (char*)"65001",
                            nullptr};
    CHECK(ParseArgs(6, explicitArgv, explicitConfig, error));
    CHECK(explicitConfig.testnet);
    CHECK(explicitConfig.EffectivePort() == 65000);
    CHECK(explicitConfig.EffectiveRpcPort() == 65001);
    CHECK(explicitConfig.EffectiveStratumPort() == 64781);
    CHECK(WriteFile(confPath,
                    "network = testnet\nport = 65002\n"));
    AppConfig explicitFileConfig;
    CHECK(LoadConfigFile(confPath, explicitFileConfig, error));
    CHECK(explicitFileConfig.EffectivePort() == 65002);
    // CLI overrides the file: file says testnet, CLI says mainnet.
    AppConfig overrideConfig;
    char* overrideArgv[] = {(char*)"oeeoo-node", (char*)"--config",
                            (char*)confPath.c_str(), (char*)"--network",
                            (char*)"mainnet", nullptr};
    CHECK(ParseArgs(5, overrideArgv, overrideConfig, error));
    CHECK(!overrideConfig.testnet);
    CHECK(overrideConfig.EffectivePort() == 65002);  // file port kept
    ::remove(confPath.c_str());
    // An invalid network value is REJECTED (never a silent
    // fallback to mainnet — a typo must not silently put a test
    // node on Mainnet or vice versa), in the file and on the CLI.
    const std::string badPath = "/tmp/oeeoo-m20d-bad-" + PidTag() + ".conf";
    CHECK(WriteFile(badPath, "network = foo\n"));
    AppConfig badConfig;
    CHECK(!LoadConfigFile(badPath, badConfig, error));
    CHECK(error.find("mainnet/testnet") != std::string::npos);
    ::remove(badPath.c_str());
    AppConfig badCliConfig;
    char* badArgv[] = {(char*)"oeeoo-node", (char*)"--network",
                       (char*)"foo", nullptr};
    CHECK(!ParseArgs(3, badArgv, badCliConfig, error));
    CHECK(error.find("mainnet/testnet") != std::string::npos);
    // --network with no value is an error, like --port.
    AppConfig missingConfig;
    char* missingArgv[] = {(char*)"oeeoo-node", (char*)"--network",
                           nullptr};
    CHECK(!ParseArgs(2, missingArgv, missingConfig, error));
    // Usage documents the switch.
    CHECK(UsageText("oeeoo-node").find("--testnet") != std::string::npos);
    CHECK(UsageText("oeeoo-node").find("--network") != std::string::npos);
    // End-to-end of the node_main selection step: a config
    // parsed with --testnet selects the testnet magic.
    SelectNetwork(testnetConfig.testnet ? NetworkId::kTestnet
                                       : NetworkId::kMainnet);
    CHECK(ActiveNetworkMagic() == kTestnetMagic);
    SelectNetwork(config.testnet ? NetworkId::kTestnet
                                 : NetworkId::kMainnet);
    CHECK(ActiveNetworkMagic() == kMainnetMagic);
  }
}
