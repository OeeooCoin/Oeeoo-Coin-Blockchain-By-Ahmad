// Tests: oeeoo-cli RPC client (Milestone 12).
#include <cstdio>
#include <mutex>
#include <string>

#include "app/cli.h"
#include "blockchain/chain.h"
#include "consensus/consensus.h"
#include "crypto/ecdsa.h"
#include "mining/miner.h"
#include "network/node.h"
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
}  // namespace

void TestCli() {
  test::BeginSection("cli: calls the local RPC server");
  {
    EnsureKeys();
    const std::string dataDir = "/tmp/oeeoo-cli-test";
    CHECK(::system(("mkdir -p " + dataDir).c_str()) == 0);
    std::mutex nodeMutex;
    Node node(kTestBits);
    node.SetTime(kNow);
    Block genesis = MineGenesis();
    std::string error;
    CHECK(node.BroadcastBlock(genesis, error));
    RpcServer server(node, 0, dataDir, &nodeMutex);
    CHECK(server.Start(error));
    const uint16_t port = server.Port();
    CHECK(port != 0);
    Json response;
    CHECK(CliCall(dataDir, port, "getblockcount", Json::Array(), response,
                  error));
    CHECK(response.Get("result") != nullptr);
    if (response.Get("result") == nullptr) return;
    CHECK(response.Get("result")->AsInt() == 0);
    CHECK(CliCall(dataDir, port, "getbestblockhash", Json::Array(),
                  response, error));
    CHECK(response.Get("result")->AsString().size() == 64);
    CHECK(CliCall(dataDir, port, "getblockcount", Json::Array(), response,
                  error));
    // An unknown method comes back as a JSON-RPC error, not a
    // transport failure.
    CHECK(CliCall(dataDir, port, "nosuchmethod", Json::Array(), response,
                  error));
    const Json* err = response.Get("error");
    CHECK(err != nullptr && !err->IsNull());
    if (err == nullptr) return;
    CHECK(err->Get("code")->AsInt() == -32601);
    // A missing cookie directory fails cleanly.
    Json ignored;
    CHECK(!CliCall("/tmp/oeeoo-cli-no-such-dir", port, "getblockcount",
                   Json::Array(), ignored, error));
    // A dead port fails cleanly.
    CHECK(!CliCall(dataDir, 1, "getblockcount", Json::Array(), ignored,
                   error));
    server.Stop();
    CHECK(::system(("rm -rf " + dataDir).c_str()) == 0);
  }
}
