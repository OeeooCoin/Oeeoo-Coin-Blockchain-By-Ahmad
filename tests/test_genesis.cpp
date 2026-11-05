// Tests: genesis block generation (Milestone 12).
#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "app/genesis.h"
#include "blockchain/genesis.h"
#include "consensus/consensus.h"
#include "consensus/difficulty.h"
#include "consensus/vesting.h"
#include "crypto/ecdsa.h"
#include "network/node.h"
#include "network/transport.h"
#include "script/script.h"
#include "test_harness.h"

using namespace oeeoo;

namespace {
constexpr uint32_t kTestBits = 0x207fffff;
constexpr uint32_t kLaunch = 1793923200;  // 2026-11-06 00:00 UTC

std::vector<unsigned char> TestPubkey(unsigned char seed) {
  KeyPair key;
  // Deterministic-ish: generate a fresh key and use its public key.
  // (The exact key does not matter; only its size and validity do.)
  CHECK(GenerateKeyPair(key));
  (void)seed;
  return key.pubkey;
}
}  // namespace

void TestGenesis() {
  test::BeginSection("genesis: 2-of-3 multisig script");
  {
    const std::vector<unsigned char> pk1 = TestPubkey(1);
    const std::vector<unsigned char> pk2 = TestPubkey(2);
    const std::vector<unsigned char> pk3 = TestPubkey(3);
    CHECK(pk1.size() == 33 && pk2.size() == 33 && pk3.size() == 33);
    const std::vector<unsigned char> script =
        BuildMultisigScript(2, {pk1, pk2, pk3});
    CHECK(script.size() == 1 + 34 * 3 + 1 + 1);
    CHECK(script[0] == 0x52);  // OP_2
    CHECK(script[1] == 0x21);  // push 33
    CHECK(script[script.size() - 2] == 0x53);   // OP_3
    CHECK(script[script.size() - 1] == 0xae);   // OP_CHECKMULTISIG
    // Invalid parameters give an empty script.
    CHECK(BuildMultisigScript(0, {pk1, pk2, pk3}).empty());
    CHECK(BuildMultisigScript(4, {pk1, pk2, pk3}).empty());
    CHECK(BuildMultisigScript(2, {}).empty());
    // A valid 2-of-2 is fine (not empty).
    CHECK(!BuildMultisigScript(2, {pk1, pk2}).empty());
    std::vector<unsigned char> shortKey(32, 0x02);
    CHECK(BuildMultisigScript(2, {pk1, pk2, shortKey}).empty());
  }

  test::BeginSection("genesis: build, mine, verify, round-trip");
  {
    const std::vector<unsigned char> pk1 = TestPubkey(1);
    const std::vector<unsigned char> pk2 = TestPubkey(2);
    const std::vector<unsigned char> pk3 = TestPubkey(3);
    const std::vector<unsigned char> founderScript =
        BuildMultisigScript(2, {pk1, pk2, pk3});
    Block genesis = BuildGenesisBlock(founderScript, kLaunch, kTestBits);
    CHECK(genesis.header.nVersion == 1);
    CHECK(genesis.header.hashPrevBlock == Hash256{});
    CHECK(genesis.header.nTime == kLaunch);
    CHECK(genesis.header.nBits == kTestBits);
    CHECK(genesis.vtx.size() == 1);
    CHECK(genesis.vtx[0].vout.size() == 1);
    CHECK(genesis.vtx[0].vout[0].value == FOUNDER_ALLOCATION);
    CHECK(genesis.vtx[0].vout[0].scriptPubKey == founderScript);
    // The genesis message is in the coinbase scriptSig.
    const std::vector<unsigned char>& sig = genesis.vtx[0].vin[0].scriptSig;
    const std::string message(kGenesisMessage);
    bool found = false;
    for (std::size_t i = 0; i + message.size() <= sig.size(); ++i) {
      if (std::string(sig.begin() + i, sig.begin() + i + message.size()) ==
          message) {
        found = true;
      }
    }
    CHECK(found);
    // Mine it (test difficulty: instant, single thread).
    std::atomic<uint64_t> hashes{0};
    std::atomic<bool> stop{false};
    std::string error;
    CHECK(MineGenesisParallel(genesis, 1, &hashes, &stop, error));
    CHECK(hashes.load() >= 1);
    CHECK(CheckProofOfWork(genesis.GetHash(), kTestBits));
    // The founder rule accepts it.
    VestingParams vesting;
    vesting.enabled = true;
    vesting.launchTime = kLaunch;
    vesting.founderScript = founderScript;
    CHECK(ValidateGenesisCoinbase(genesis, vesting, error));
    // Round-trip through the genesis file.
    const std::string path = "/tmp/oeeoo-genesis-test.json";
    CHECK(WriteGenesisFile(path, genesis, founderScript, error));
    Block reloaded;
    std::vector<unsigned char> reloadedScript;
    CHECK(ReadGenesisFile(path, reloaded, reloadedScript, error));
    CHECK(reloaded.GetHash() == genesis.GetHash());
    CHECK(reloadedScript == founderScript);
    // A tampered file is refused.
    {
      std::ifstream in(path);
      std::string text((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
      const std::size_t pos = text.find("\"hash\":\"");
      CHECK(pos != std::string::npos);
      text[pos + 8] = (text[pos + 8] == '0' ? '1' : '0');
      std::ofstream out(path);
      out << text;
    }
    Block bad;
    std::vector<unsigned char> badScript;
    CHECK(!ReadGenesisFile(path, bad, badScript, error));
    ::remove(path.c_str());
  }

  test::BeginSection("genesis: a node accepts it as block 0");
  {
    const std::vector<unsigned char> pk1 = TestPubkey(1);
    const std::vector<unsigned char> pk2 = TestPubkey(2);
    const std::vector<unsigned char> pk3 = TestPubkey(3);
    const std::vector<unsigned char> founderScript =
        BuildMultisigScript(2, {pk1, pk2, pk3});
    Block genesis = BuildGenesisBlock(founderScript, kLaunch, kTestBits);
    std::atomic<uint64_t> hashes{0};
    std::atomic<bool> stop{false};
    std::string error;
    CHECK(MineGenesisParallel(genesis, 2, &hashes, &stop, error));
    Node node(kTestBits);
    node.SetTime(kLaunch + 600);
    VestingParams vesting;
    vesting.enabled = true;
    vesting.launchTime = kLaunch;
    vesting.founderScript = founderScript;
    node.SetVestingParams(vesting);
    CHECK(node.BroadcastBlock(genesis, error));
    CHECK(node.TipHeight() == 0);
    CHECK(node.TipHash() == genesis.GetHash());
    // The founder vault holds exactly 1,000,000.
    CHECK(FounderVaultBalance(node.Utxo(), vesting) == FOUNDER_ALLOCATION);
    // A second broadcast of the same genesis is a no-op success
    // (duplicate blocks are accepted, never re-applied).
    CHECK(node.BroadcastBlock(genesis, error));
    CHECK(node.TipHeight() == 0);
  }
}
