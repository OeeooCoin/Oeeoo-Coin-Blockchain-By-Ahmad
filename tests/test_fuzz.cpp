// Tests: fuzz / robustness (Milestone 11).
// Deterministic (fixed seed) mutation fuzzing of the parsers and
// deserializers. The goal is not coverage — it is proving that
// hostile bytes cannot crash the node: every mutated input must
// return true or false, never segfault or hang.
#include <cstdint>
#include <string>
#include <vector>

#include "blockchain/block.h"
#include "consensus/consensus.h"
#include "crypto/ecdsa.h"
#include "mining/miner.h"
#include "network/protocol.h"
#include "rpc/json.h"
#include "script/script.h"
#include "test_harness.h"
#include "transactions/transaction.h"
#include "transactions/validation.h"
#include "utxo/coins.h"

using namespace oeeoo;

namespace {
// xorshift64 — deterministic, fast, good enough for mutation fuzzing.
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
          // Copy the slice first: inserting a range from the same
          // vector is undefined behavior when it reallocates.
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

Transaction ValidTx() {
  KeyPair key;
  CHECK(GenerateKeyPair(key));
  Transaction tx;
  tx.version = 1;
  TxIn in;
  in.prevTxHash.fill(0x55);
  in.prevOutIndex = 0;
  in.sequence = 0xffffffff;
  tx.vin.push_back(in);
  TxOut out;
  out.value = 49 * COIN;
  out.scriptPubKey = CreateP2PKHScriptPubKey(PubKeyHash160(key.pubkey));
  tx.vout.push_back(out);
  tx.lockTime = 0;
  return tx;
}

Block ValidBlock() {
  Block block;
  block.header.nVersion = 1;
  block.header.nTime = 1700000000;
  block.header.nBits = 0x207fffff;
  block.vtx = {CreateCoinbase(0, std::vector<unsigned char>{0x51}, 0, "x")};
  block.header.hashMerkleRoot = block.ComputeMerkleRoot();
  return block;
}
}  // namespace

void TestFuzz() {
  test::BeginSection("fuzz: mutated transactions never crash");
  {
    Prng rng(0x12345678);
    const std::vector<unsigned char> valid = ValidTx().Serialize();
    for (int i = 0; i < 1500; ++i) {
      const std::vector<unsigned char> mutated = Mutate(rng, valid);
      Transaction tx;
      (void)Transaction::Deserialize(mutated, tx);
      // Also feed it to the validator (must not crash on garbage).
      CoinsView utxo;
      (void)ValidateTransaction(tx, utxo, 10, 1700000000);
    }
    // Pure garbage.
    for (int i = 0; i < 500; ++i) {
      std::vector<unsigned char> garbage(rng.Below(200));
      for (auto& b : garbage) b = static_cast<unsigned char>(rng.Next() & 0xff);
      Transaction tx;
      (void)Transaction::Deserialize(garbage, tx);
    }
  }

  test::BeginSection("fuzz: mutated blocks and headers never crash");
  {
    Prng rng(0x87654321);
    const std::vector<unsigned char> valid = ValidBlock().Serialize();
    for (int i = 0; i < 1000; ++i) {
      const std::vector<unsigned char> mutated = Mutate(rng, valid);
      Block block;
      if (Block::Deserialize(mutated, block)) {
        std::string error;
        (void)block.CheckBasic(error);
        (void)block.GetHash();
      }
    }
    // Mutated headers on their own.
    BlockHeader header = ValidBlock().header;
    const std::vector<unsigned char> headerBytes = header.Serialize();
    for (int i = 0; i < 500; ++i) {
      const std::vector<unsigned char> mutated = Mutate(rng, headerBytes);
      BlockHeader h;
      (void)BlockHeader::Deserialize(mutated, h);
    }
  }

  test::BeginSection("fuzz: mutated JSON never crashes the parser");
  {
    Prng rng(0xabcdef01);
    const std::string valid =
        R"({"method":"getblockcount","params":[],"id":1,"nested":{"a":[1,2.5,"x",true,null]}})";
    for (int i = 0; i < 1500; ++i) {
      std::vector<unsigned char> bytes(valid.begin(), valid.end());
      const std::vector<unsigned char> mutated = Mutate(rng, bytes);
      const std::string text(mutated.begin(), mutated.end());
      Json j;
      std::string error;
      if (Json::Parse(text, j, error)) {
        (void)j.Dump();
      }
    }
  }

  test::BeginSection("fuzz: mutated P2P messages never crash the framer");
  {
    Prng rng(0x13579bdf);
    Message msg;
    msg.command = "tx";
    msg.payload = ValidTx().Serialize();
    const std::vector<unsigned char> valid = SerializeMessage(msg);
    for (int i = 0; i < 1000; ++i) {
      const std::vector<unsigned char> mutated = Mutate(rng, valid);
      Message parsed;
      std::size_t consumed = 0;
      (void)ParseMessage(mutated, 0, parsed, consumed);
    }
  }

  test::BeginSection("fuzz: random scripts never crash the interpreter");
  {
    Prng rng(0x2468ace0);
    for (int i = 0; i < 1000; ++i) {
      std::vector<unsigned char> script(rng.Below(64));
      for (auto& b : script) b = static_cast<unsigned char>(rng.Next() & 0xff);
      Transaction tx = ValidTx();
      std::string error;
      (void)VerifyScript(script, std::vector<unsigned char>{0x51}, tx, 0,
                         error);
    }
  }
}
