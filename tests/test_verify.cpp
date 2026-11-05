// Tests: full chain re-validation from the store (Milestone 11).
#include <string>
#include <vector>

#include "blockchain/chain.h"
#include "blockchain/verify.h"
#include "consensus/consensus.h"
#include "crypto/ecdsa.h"
#include "db/chainstore.h"
#include "mining/miner.h"
#include "network/node.h"
#include "network/transport.h"
#include "script/script.h"
#include "test_harness.h"
#include "transactions/sighash.h"
#include "transactions/validation.h"

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

Transaction MakeSpend(const Hash256& prev, Amount value) {
  Transaction tx;
  tx.version = 1;
  TxIn in;
  in.prevTxHash = prev;
  in.prevOutIndex = 0;
  in.sequence = 0xffffffff;
  tx.vin.push_back(in);
  TxOut out;
  out.value = value;
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

// Build a 102-block chain (genesis + 101) on `store`. Block 101
// spends block 1's coinbase output (mature: 101 - 1 = 100 blocks).
// Every coin in the chain traces to a block, so a from-genesis
// replay can validate the whole chain. Returns the tip hash.
Hash256 BuildChain(ChainStore& store, std::string& error) {
  ChainState chain(kTestBits);
  CoinsView utxo;
  Block genesis = MineGenesis();
  CHECK(chain.AddBlock(genesis, kNow, error));
  {
    Amount fees = 0;
    BlockUndo undo;
    CHECK(BuildBlockUndo(genesis, utxo, 0, genesis.header.nTime, undo, error));
    CHECK(ConnectBlockTransactions(genesis, utxo, 0, kTime,
                                   genesis.header.nTime, fees, error));
    Target work;
    CHECK(chain.GetChainWork(genesis.GetHash(), work));
    CHECK(store.ApplyBlock(genesis, work, undo, error));
  }
  Hash256 prev = genesis.GetHash();
  Hash256 coinbaseTxid;
  for (int height = 1; height <= 101; ++height) {
    Mempool pool;
    if (height == 101) {
      Transaction tx = MakeSpend(coinbaseTxid, 49 * COIN);
      CHECK(pool.AddTransaction(tx, utxo, height, kTime, kNow, error));
    }
    BlockTemplate templ = AssembleBlock(prev, height, kTime + 600 * height,
                                        kTestBits, pool, utxo, kTime, Spk());
    Block block = templ.block;
    CHECK(MineBlock(block, height, ""));
    CHECK(chain.AddBlock(block, kNow, error));
    if (height == 1) coinbaseTxid = block.vtx[0].GetTxId();
    BlockUndo undo;
    CHECK(BuildBlockUndo(block, utxo, height, block.header.nTime, undo, error));
    Amount fees = 0;
    CHECK(ConnectBlockTransactions(block, utxo, height, kTime,
                                   block.header.nTime, fees, error));
    Target work;
    CHECK(chain.GetChainWork(block.GetHash(), work));
    CHECK(store.ApplyBlock(block, work, undo, error));
    prev = block.GetHash();
  }
  return prev;
}
}  // namespace

void TestVerify() {
  test::BeginSection("verify: a good chain re-validates from genesis");
  {
    EnsureKeys();
    const std::string path = "/tmp/oeeoo-verify-test.db";
    ::remove(path.c_str());
    ChainStore store;
    std::string error;
    CHECK(store.Open(path, error));
    Hash256 tip = BuildChain(store, error);
    // Direct verification.
    Hash256 verifiedTip;
    CHECK(VerifyChainFromStore(store, VestingParams{}, kTestBits,
                               verifiedTip, error));
    CHECK(verifiedTip == tip);
    // Deep load into a fresh node.
    Node node(kTestBits);
    node.SetTime(kNow);
    CHECK(node.LoadFromStore(store, error, true));
    CHECK(node.TipHash() == tip);
    CHECK(node.TipHeight() == 101);
    store.Close();
    ::remove(path.c_str());
  }

  test::BeginSection("verify: tampered UTXO set is caught");
  {
    EnsureKeys();
    const std::string path = "/tmp/oeeoo-verify-tamper-test.db";
    ::remove(path.c_str());
    ChainStore store;
    std::string error;
    CHECK(store.Open(path, error));
    Hash256 tip = BuildChain(store, error);
    (void)tip;
    // Tamper: change one stored coin's value.
    std::vector<std::pair<OutPoint, Coin>> coins;
    CHECK(store.GetAllCoins(coins, error));
    CHECK(!coins.empty());
    Coin tampered = coins[0].second;
    tampered.out.value += 1;
    CHECK(store.PutCoin(coins[0].first, tampered, error));
    Hash256 verifiedTip;
    CHECK(!VerifyChainFromStore(store, VestingParams{}, kTestBits,
                                verifiedTip, error));
    CHECK(error.find("UTXO") != std::string::npos);
    // The light load still succeeds (it trusts the UTXO table);
    // only the deep verification catches the tamper.
    Node node(kTestBits);
    node.SetTime(kNow);
    CHECK(node.LoadFromStore(store, error, false));
    Node node2(kTestBits);
    node2.SetTime(kNow);
    CHECK(!node2.LoadFromStore(store, error, true));
    store.Close();
    ::remove(path.c_str());
  }

  test::BeginSection("verify: pruned history cannot be fully re-validated");
  {
    EnsureKeys();
    const std::string path = "/tmp/oeeoo-verify-prune-test.db";
    ::remove(path.c_str());
    ChainStore store;
    std::string error;
    CHECK(store.Open(path, error));
    BuildChain(store, error);
    CHECK(store.PruneBlocks(3, error));
    Hash256 verifiedTip;
    CHECK(!VerifyChainFromStore(store, VestingParams{}, kTestBits,
                                verifiedTip, error));
    CHECK(error.find("pruned") != std::string::npos);
    store.Close();
    ::remove(path.c_str());
  }

  test::BeginSection("verify: empty store fails");
  {
    const std::string path = "/tmp/oeeoo-verify-empty-test.db";
    ::remove(path.c_str());
    ChainStore store;
    std::string error;
    CHECK(store.Open(path, error));
    Hash256 verifiedTip;
    CHECK(!VerifyChainFromStore(store, VestingParams{}, kTestBits,
                                verifiedTip, error));
    store.Close();
    ::remove(path.c_str());
  }
}
