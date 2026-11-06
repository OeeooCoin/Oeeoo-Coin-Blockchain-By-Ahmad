// Tests: reorg handling + undo records (Milestone 11).
#include <memory>
#include <string>
#include <vector>

#include "blockchain/chain.h"
#include "consensus/consensus.h"
#include "consensus/subsidy.h"
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

Hash256 TxIdWith(unsigned char fill) {
  Hash256 h;
  h.fill(fill);
  return h;
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

// A spend of `prev` (a non-coinbase funding coin) paying `value` to
// our own key, signed with it.
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

void Fund(CoinsView& utxo, const Hash256& txid, Amount value) {
  Coin coin;
  coin.out.value = value;
  coin.out.scriptPubKey = Spk();
  coin.height = 0;
  coin.blockTime = kTime;
  coin.isCoinbase = false;
  CHECK(utxo.AddCoin({txid, 0}, coin));
}

// A hand-built block on `prev` at `height`/`time` carrying `txs`.
// The coinbase pays subsidy + the txs' fees to our key.
Block MakeBlock(const Hash256& prev, int height, uint32_t time,
                const std::vector<Transaction>& txs) {
  Amount fees = 0;
  for (const Transaction& tx : txs) {
    Amount in = 50 * COIN;  // every test spend consumes a 50-coin fund
    Amount out = 0;
    for (const TxOut& o : tx.vout) out += o.value;
    fees += in - out;
  }
  Block block;
  block.header.nVersion = 1;
  block.header.hashPrevBlock = prev;
  block.header.nTime = time;
  block.header.nBits = kTestBits;
  block.vtx.push_back(
      CreateCoinbase(height, Spk(), GetBlockSubsidy(height) + fees, ""));
  for (const Transaction& tx : txs) block.vtx.push_back(tx);
  block.header.hashMerkleRoot = block.ComputeMerkleRoot();
  CHECK(MineBlock(block, height, ""));
  return block;
}

bool UtxoHas(const CoinsView& utxo, const Hash256& txid, uint32_t vout) {
  Coin coin;
  return utxo.GetCoin({txid, vout}, coin);
}
}  // namespace

void TestReorg() {
  test::BeginSection("reorg: disconnect restores the UTXO set exactly");
  {
    EnsureKeys();
    std::string error;
    CoinsView utxo;
    const Hash256 fund = TxIdWith(0x11);
    Fund(utxo, fund, 50 * COIN);
    Block genesis = MineGenesis();
    Amount fees = 0;
    CoinsView view = utxo;
    CHECK(ConnectBlockTransactions(genesis, view, 0, kTime,
                                   genesis.header.nTime, fees, error));
    // Block 1 spends the funding coin.
    Transaction spend = MakeSpend(fund, 49 * COIN);
    Block b1 = MakeBlock(genesis.GetHash(), 1, kTime + 600, {spend});
    BlockUndo undo;
    CHECK(BuildBlockUndo(b1, view, 1, b1.header.nTime, undo, error));
    CHECK(ConnectBlockTransactions(b1, view, 1, kTime, b1.header.nTime,
                                   fees, error));
    CHECK(!UtxoHas(view, fund, 0));
    CHECK(UtxoHas(view, spend.GetTxId(), 0));
    // Disconnect it: the exact pre-state comes back.
    CHECK(DisconnectBlockTransactions(b1, view, undo, error));
    CHECK(UtxoHas(view, fund, 0));
    CHECK(!UtxoHas(view, spend.GetTxId(), 0));
    // Disconnect is fail-closed: a tampered undo record is refused.
    BlockUndo bad = undo;
    bad.added[0].second.out.value += 1;
    CHECK(!DisconnectBlockTransactions(b1, view, bad, error));
    CHECK(UtxoHas(view, fund, 0));
  }

  test::BeginSection("reorg: store disconnect rolls the tip back");
  {
    EnsureKeys();
    const std::string path = "/tmp/oeeoo-reorg-test.db";
    ::remove(path.c_str());
    ChainStore store;
    std::string error;
    CHECK(store.Open(path, error));
    ChainState chain(kTestBits);
    CoinsView utxo;
    const Hash256 fund = TxIdWith(0x22);
    Fund(utxo, fund, 50 * COIN);
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
    Transaction spend = MakeSpend(fund, 49 * COIN);
    Block b1 = MakeBlock(genesis.GetHash(), 1, kTime + 600, {spend});
    CHECK(chain.AddBlock(b1, kNow, error));
    BlockUndo undo1;
    CHECK(BuildBlockUndo(b1, utxo, 1, b1.header.nTime, undo1, error));
    {
      Amount fees = 0;
      CHECK(ConnectBlockTransactions(b1, utxo, 1, kTime, b1.header.nTime,
                                     fees, error));
    }
    {
      Target work;
      CHECK(chain.GetChainWork(b1.GetHash(), work));
      CHECK(store.ApplyBlock(b1, work, undo1, error));
    }
    Hash256 tip;
    CHECK(store.LoadTip(tip, error));
    CHECK(tip == b1.GetHash());
    CHECK(!UtxoHas(utxo, fund, 0));
    // Disconnecting a non-tip block fails.
    CHECK(!store.DisconnectBlock(genesis.GetHash(), error));
    // Disconnect the tip: UTXO set and tip roll back.
    CHECK(store.DisconnectBlock(b1.GetHash(), error));
    CHECK(store.LoadTip(tip, error));
    CHECK(tip == genesis.GetHash());
    std::vector<std::pair<OutPoint, Coin>> coins;
    CHECK(store.GetAllCoins(coins, error));
    bool fundBack = false;
    bool spendGone = true;
    for (const auto& entry : coins) {
      if (entry.first.txid == fund) fundBack = true;
      if (entry.first.txid == spend.GetTxId()) spendGone = false;
    }
    CHECK(fundBack);
    CHECK(spendGone);
    // The undo record was deleted with the block.
    BlockUndo gone;
    CHECK(!store.GetUndo(b1.GetHash(), gone, error));
    // The genesis undo record is still there.
    BlockUndo genesisUndo;
    CHECK(store.GetUndo(genesis.GetHash(), genesisUndo, error));
    store.Close();
    ::remove(path.c_str());
  }

  test::BeginSection("reorg: node follows the heavier fork, store follows");
  {
    EnsureKeys();
    const std::string path = "/tmp/oeeoo-reorg-node-test.db";
    ::remove(path.c_str());
    ChainStore store;
    std::string error;
    CHECK(store.Open(path, error));
    Node node(kTestBits);
    node.SetTime(kNow);
    node.AttachStore(&store);
    // Two funding coins (non-coinbase, immediately spendable).
    const Hash256 fund1 = TxIdWith(0x31);
    const Hash256 fund2 = TxIdWith(0x32);
    Fund(node.Utxo(), fund1, 50 * COIN);
    Fund(node.Utxo(), fund2, 50 * COIN);
    Block genesis = MineGenesis();
    CHECK(node.BroadcastBlock(genesis, error));
    // Main chain: block 1 (empty), block 2 spends fund1, block 3
    // spends fund2.
    Block m1 = MakeBlock(genesis.GetHash(), 1, kTime + 600, {});
    CHECK(node.BroadcastBlock(m1, error));
    Transaction tx1 = MakeSpend(fund1, 49 * COIN);
    Block m2 = MakeBlock(m1.GetHash(), 2, kTime + 1200, {tx1});
    CHECK(node.BroadcastBlock(m2, error));
    Transaction tx3 = MakeSpend(fund2, 49 * COIN);
    Block m3 = MakeBlock(m2.GetHash(), 3, kTime + 1800, {tx3});
    CHECK(node.BroadcastBlock(m3, error));
    CHECK(node.TipHeight() == 3);
    CHECK(UtxoHas(node.Utxo(), tx1.GetTxId(), 0));
    CHECK(UtxoHas(node.Utxo(), tx3.GetTxId(), 0));
    CHECK(!UtxoHas(node.Utxo(), fund1, 0));
    // Fork from block 1: 2' spends fund1 differently, 3' and 4'
    // are empty. The fork only wins once it has more work (4').
    Transaction tx2 = MakeSpend(fund1, 48 * COIN);
    Block f2 = MakeBlock(m1.GetHash(), 2, kTime + 1500, {tx2});
    CHECK(node.BroadcastBlock(f2, error));
    CHECK(node.TipHeight() == 3);  // side branch: tip does not move
    CHECK(node.TipHash() == m3.GetHash());
    CHECK(UtxoHas(node.Utxo(), tx1.GetTxId(), 0));  // UTXO unchanged
    Hash256 storeTip;
    CHECK(store.LoadTip(storeTip, error));
    CHECK(storeTip == m3.GetHash());  // store tip unchanged too
    CHECK(store.HasBlock(f2.GetHash()));  // but the side block is stored
    Block f3 = MakeBlock(f2.GetHash(), 3, kTime + 2100, {});
    CHECK(node.BroadcastBlock(f3, error));
    CHECK(node.TipHeight() == 3);  // equal work: still no reorg
    Block f4 = MakeBlock(f3.GetHash(), 4, kTime + 2700, {});
    CHECK(node.BroadcastBlock(f4, error));
    // The reorg happened.
    CHECK(node.TipHeight() == 4);
    CHECK(node.TipHash() == f4.GetHash());
    CHECK(!UtxoHas(node.Utxo(), tx1.GetTxId(), 0));  // loser's output gone
    CHECK(UtxoHas(node.Utxo(), tx2.GetTxId(), 0));   // winner's output here
    CHECK(!UtxoHas(node.Utxo(), fund1, 0));          // spent by tx2
    CHECK(UtxoHas(node.Utxo(), fund2, 0));  // tx3's input restored
    CHECK(!UtxoHas(node.Utxo(), tx3.GetTxId(), 0));
    // tx3 (disconnected, not conflicting) is back in the mempool;
    // tx1 (conflicting with tx2) is dropped.
    CHECK(node.HasTx(tx3.GetTxId()));
    CHECK(!node.HasTx(tx1.GetTxId()));
    // The store followed the reorg.
    CHECK(store.LoadTip(storeTip, error));
    CHECK(storeTip == f4.GetHash());
    std::vector<std::pair<OutPoint, Coin>> coins;
    CHECK(store.GetAllCoins(coins, error));
    bool hasTx2 = false, hasTx1 = false, hasFund2 = false;
    for (const auto& entry : coins) {
      if (entry.first.txid == tx2.GetTxId()) hasTx2 = true;
      if (entry.first.txid == tx1.GetTxId()) hasTx1 = true;
      if (entry.first.txid == fund2) hasFund2 = true;
    }
    CHECK(hasTx2);
    CHECK(!hasTx1);
    CHECK(hasFund2);
    // Undo records: the fork's tip has one, the old main tip's is gone.
    BlockUndo forkUndo;
    CHECK(store.GetUndo(f4.GetHash(), forkUndo, error));
    BlockUndo gone;
    CHECK(!store.GetUndo(m3.GetHash(), gone, error));
    CHECK(node.LastStoreError().empty());
    store.Close();
    ::remove(path.c_str());
  }

  // Milestone 17 (Stream B): 100-deep reorg.
  //
  // Attribution — the "100" is OEEOO'S OWN TARGET, NOT a Bitcoin Core
  // number (no single verified Core reorg-depth figure exists). The
  // rationale is that 100 equals coinbase maturity
  // (COINBASE_MATURITY), so a 100-deep reorg is the deepest that can
  // strand matured coinbase spends. Do not attribute the number to
  // Core.
  //
  // Shape: a 100-block main chain (heights 1..100) forked at the
  // genesis block, vs a 101-block fork from the same genesis. The
  // fork only wins on its 101st block (strictly more work), so the
  // reorg disconnects exactly 100 main-chain blocks. Block 50 of
  // each chain spends the same funding coin differently (conflict);
  // main block 60 spends a second funding coin and must come back
  // to the mempool when it is disconnected.
  test::BeginSection("reorg: depth-100 reorg (coinbase-maturity bound)");
  {
    EnsureKeys();
    const std::string path = "/tmp/oeeoo-reorg-depth100-test.db";
    ::remove(path.c_str());
    ChainStore store;
    std::string error;
    CHECK(store.Open(path, error));
    Node node(kTestBits);
    node.SetTime(kNow);
    node.AttachStore(&store);
    const Hash256 fundA = TxIdWith(0x51);
    const Hash256 fundB = TxIdWith(0x52);
    Fund(node.Utxo(), fundA, 50 * COIN);
    Fund(node.Utxo(), fundB, 50 * COIN);
    Block genesis = MineGenesis();
    CHECK(node.BroadcastBlock(genesis, error));
    // Main chain: heights 1..100. Block 50 spends fundA (txA),
    // block 60 spends fundB (txB).
    std::vector<Block> mainChain;
    Hash256 prev = genesis.GetHash();
    Transaction txA;
    Transaction txB;
    for (int h = 1; h <= 100; ++h) {
      std::vector<Transaction> txs;
      if (h == 50) {
        txA = MakeSpend(fundA, 49 * COIN);
        txs.push_back(txA);
      }
      if (h == 60) {
        txB = MakeSpend(fundB, 49 * COIN);
        txs.push_back(txB);
      }
      Block b = MakeBlock(prev, h, kTime + 600 * static_cast<uint32_t>(h),
                          txs);
      CHECK(node.BroadcastBlock(b, error));
      mainChain.push_back(b);
      prev = b.GetHash();
    }
    CHECK(node.TipHeight() == 100);
    CHECK(node.TipHash() == mainChain[99].GetHash());
    CHECK(UtxoHas(node.Utxo(), txA.GetTxId(), 0));
    CHECK(UtxoHas(node.Utxo(), txB.GetTxId(), 0));
    CHECK(!UtxoHas(node.Utxo(), fundA, 0));
    CHECK(!UtxoHas(node.Utxo(), fundB, 0));
    // Fork from the genesis block: heights 1..101. Block 50 spends
    // fundA differently (txA2, conflicts with txA); the rest are
    // empty. Times are offset by +300 s so the fork's blocks differ
    // from the main chain's while keeping 600 s spacing.
    Transaction txA2 = MakeSpend(fundA, 48 * COIN);
    std::vector<Block> forkChain;
    prev = genesis.GetHash();
    for (int h = 1; h <= 101; ++h) {
      std::vector<Transaction> txs;
      if (h == 50) txs.push_back(txA2);
      Block b = MakeBlock(prev, h,
                          kTime + 600 * static_cast<uint32_t>(h) + 300, txs);
      CHECK(node.BroadcastBlock(b, error));
      forkChain.push_back(b);
      prev = b.GetHash();
      if (h <= 100) {
        // Not heavier yet (equal work at h == 100): no reorg.
        CHECK(node.TipHeight() == 100);
        CHECK(node.TipHash() == mainChain[99].GetHash());
        CHECK(UtxoHas(node.Utxo(), txA.GetTxId(), 0));
      }
    }
    // The fork's 101st block makes it heavier: the 100-deep reorg.
    CHECK(node.TipHeight() == 101);
    CHECK(node.TipHash() == forkChain[100].GetHash());
    // Undo correctness: the winner's output is present, the loser's
    // is gone, the disconnected non-conflicting spend's input is
    // restored and its output is gone (it is unconfirmed again).
    CHECK(UtxoHas(node.Utxo(), txA2.GetTxId(), 0));
    CHECK(!UtxoHas(node.Utxo(), txA.GetTxId(), 0));
    CHECK(!UtxoHas(node.Utxo(), fundA, 0));  // spent by txA2
    CHECK(UtxoHas(node.Utxo(), fundB, 0));   // txB's input restored
    CHECK(!UtxoHas(node.Utxo(), txB.GetTxId(), 0));
    // Mempool re-adding: txB (disconnected, not conflicting) is
    // back; txA (conflicting with txA2) is dropped.
    CHECK(node.HasTx(txB.GetTxId()));
    CHECK(!node.HasTx(txA.GetTxId()));
    // The store followed the reorg.
    Hash256 storeTip;
    CHECK(store.LoadTip(storeTip, error));
    CHECK(storeTip == forkChain[100].GetHash());
    std::vector<std::pair<OutPoint, Coin>> coins;
    CHECK(store.GetAllCoins(coins, error));
    bool hasTxA2 = false, hasTxA = false, hasFundB = false, hasTxB = false;
    for (const auto& entry : coins) {
      if (entry.first.txid == txA2.GetTxId()) hasTxA2 = true;
      if (entry.first.txid == txA.GetTxId()) hasTxA = true;
      if (entry.first.txid == fundB) hasFundB = true;
      if (entry.first.txid == txB.GetTxId()) hasTxB = true;
    }
    CHECK(hasTxA2);
    CHECK(!hasTxA);
    CHECK(hasFundB);
    CHECK(!hasTxB);
    // Undo records: the fork tip's exists; the old main tip's and
    // the disconnected block-50's are gone.
    BlockUndo forkUndo;
    CHECK(store.GetUndo(forkChain[100].GetHash(), forkUndo, error));
    BlockUndo gone;
    CHECK(!store.GetUndo(mainChain[99].GetHash(), gone, error));
    CHECK(!store.GetUndo(mainChain[49].GetHash(), gone, error));
    CHECK(node.LastStoreError().empty());
    // The store's UTXO set must equal the node's in-memory UTXO set
    // exactly after a 100-deep reorg (every coin: outpoint, value,
    // script, height, block time, coinbase flag). A from-genesis
    // replay (VerifyChainFromStore) cannot be used here instead:
    // the funding coins are test-only UTXO injections whose origin
    // is not in any block — the same limitation M16 documents for
    // getblockstats ("a coin whose origin is not in a stored block
    // cannot be priced"). The node view is the ground truth; the
    // store must mirror it coin for coin.
    {
      std::vector<std::pair<OutPoint, Coin>> storeCoins;
      CHECK(store.GetAllCoins(storeCoins, error));
      std::vector<std::pair<OutPoint, Coin>> nodeCoins;
      node.Utxo().GetAllCoins(nodeCoins);
      CHECK(storeCoins.size() == nodeCoins.size());
      for (const auto& entry : nodeCoins) {
        Coin stored;
        CHECK(store.GetCoin(entry.first, stored, error));
        CHECK(stored.out.value == entry.second.out.value);
        CHECK(stored.out.scriptPubKey == entry.second.out.scriptPubKey);
        CHECK(stored.height == entry.second.height);
        CHECK(stored.blockTime == entry.second.blockTime);
        CHECK(stored.isCoinbase == entry.second.isCoinbase);
      }
    }
    store.Close();
    ::remove(path.c_str());
  }
}
