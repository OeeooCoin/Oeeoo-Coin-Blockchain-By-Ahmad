// Tests: persistent chain store (Milestone 7) — SQLite schema,
// block + UTXO round-trips, node restart, atomicity, pruning,
// corruption detection, peers/bans, fork retention.
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "blockchain/chain.h"
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

std::string DbPath(const std::string& name) {
  return (std::filesystem::temp_directory_path() /
          ("oeeoo-test-" + name + ".db"))
      .string();
}

void CleanupDb(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::remove(path + "-wal", ec);
  std::filesystem::remove(path + "-shm", ec);
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

void Fund(CoinsView& utxo, const Hash256& txid, Amount value) {
  Coin coin;
  coin.out.value = value;
  coin.out.scriptPubKey = Spk();
  coin.height = 0;
  coin.blockTime = kTime;
  coin.isCoinbase = false;
  CHECK(utxo.AddCoin({txid, 0}, coin));
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

// Apply a block to the store with its undo record (same delta the
// node computes in AcceptBlock).
bool StoreApply(ChainStore& store, const Block& block,
                const ChainState& chain, const BlockUndo& undo,
                std::string& error) {
  Target work;
  if (!chain.GetChainWork(block.GetHash(), work)) {
    error = "no chain work";
    return false;
  }
  return store.ApplyBlock(block, work, undo, error);
}

// Build a chain of `count` blocks (genesis + count-1) on a ChainState,
// storing each block; block 1 carries one spend of `fundTxid`.
std::vector<Block> BuildChain(ChainState& chain, CoinsView& utxo,
                              ChainStore& store, int count,
                              const Hash256& fundTxid, std::string& error) {
  std::vector<Block> blocks;
  Block genesis = MineGenesis();
  if (!chain.AddBlock(genesis, kNow, error)) return {};
  {
    // Apply the genesis coinbase to the in-memory UTXO view too, so
    // it matches what the store records.
    BlockUndo undo;
    if (!BuildBlockUndo(genesis, utxo, 0, genesis.header.nTime, undo,
                        error)) {
      return {};
    }
    Amount fees = 0;
    CoinsView working = utxo;
    if (!ConnectBlockTransactions(genesis, working, 0, kTime,
                                  genesis.header.nTime, fees, error)) {
      return {};
    }
    utxo = std::move(working);
    if (!StoreApply(store, genesis, chain, undo, error)) return {};
  }
  blocks.push_back(genesis);
  Hash256 prev = genesis.GetHash();
  for (int height = 1; height < count; ++height) {
    Mempool pool;
    if (height == 1) {
      Transaction tx = MakeSpend(fundTxid, 49 * COIN);
      if (!pool.AddTransaction(tx, utxo, height, kTime, kNow, error)) {
        return {};
      }
    }
    BlockTemplate templ = AssembleBlock(prev, height, kTime + 600 * height,
                                        kTestBits, pool, utxo, kTime, Spk());
    Block block = templ.block;
    if (!MineBlock(block, height, "")) {
      error = "mining failed";
      return {};
    }
    if (!chain.AddBlock(block, kNow, error)) return {};
    // Apply the block's transactions to the test UTXO view.
    BlockUndo undo;
    if (!BuildBlockUndo(block, utxo, height, block.header.nTime, undo,
                        error)) {
      return {};
    }
    Amount fees = 0;
    CoinsView working = utxo;
    if (!ConnectBlockTransactions(block, working, height, kTime,
                                  block.header.nTime, fees, error)) {
      return {};
    }
    utxo = std::move(working);
    if (!StoreApply(store, block, chain, undo, error)) return {};
    blocks.push_back(block);
    prev = block.GetHash();
  }
  return blocks;
}
}  // namespace

void TestDb() {
  EnsureKeys();

  test::BeginSection("db: open, schema version, tip meta");
  {
    const std::string path = DbPath("meta");
    CleanupDb(path);
    ChainStore store;
    std::string error;
    CHECK(store.Open(path, error));
    Hash256 tip;
    CHECK(!store.LoadTip(tip, error));  // nothing stored yet
    Hash256 want;
    want.fill(0x77);
    CHECK(store.SaveTip(want, error));
    CHECK(store.LoadTip(tip, error));
    CHECK(tip == want);
    store.Close();
    // Reopen: the tip survives.
    ChainStore store2;
    CHECK(store2.Open(path, error));
    CHECK(store2.LoadTip(tip, error));
    CHECK(tip == want);
    store2.Close();
    // Unknown schema version is rejected.
    Database raw;
    CHECK(raw.Open(path, error));
    CHECK(raw.SetUserVersion(99, error));
    raw.Close();
    ChainStore store3;
    CHECK(!store3.Open(path, error));
    CleanupDb(path);
  }

  test::BeginSection("db: block + UTXO round-trip");
  {
    const std::string path = DbPath("roundtrip");
    CleanupDb(path);
    ChainStore store;
    std::string error;
    CHECK(store.Open(path, error));
    ChainState chain(kTestBits);
    CoinsView utxo;
    const Hash256 kFund = [] {
      Hash256 h;
      h.fill(0x51);
      return h;
    }();
    Fund(utxo, kFund, 50 * COIN);
    std::vector<Block> blocks =
        BuildChain(chain, utxo, store, 6, kFund, error);
    CHECK(blocks.size() == 6);
    CHECK(chain.GetTipHeight() == 5);
    CHECK(store.BlockCount(error) == 6);
    CHECK(store.UtxoCount(error) == utxo.Size());
    Hash256 tip;
    CHECK(store.LoadTip(tip, error));
    CHECK(tip == chain.GetTipHash());
    // Every block reloads identically, with header + work.
    for (const Block& b : blocks) {
      Block reloaded;
      CHECK(store.GetBlock(b.GetHash(), reloaded, error));
      CHECK(reloaded.GetHash() == b.GetHash());
      CHECK(reloaded.Serialize() == b.Serialize());
      BlockHeader header;
      CHECK(store.GetBlockHeader(b.GetHash(), header, error));
      CHECK(header.GetHash() == b.GetHash());
      Target work, work2;
      CHECK(store.GetChainWork(b.GetHash(), work, error));
      CHECK(chain.GetChainWork(b.GetHash(), work2));
      CHECK(work == work2);
    }
    std::vector<Hash256> atHeight;
    CHECK(store.GetHashesAtHeight(1, atHeight, error));
    CHECK(atHeight.size() == 1 && atHeight[0] == blocks[1].GetHash());
    // The spent funding coin is gone; the spend output is present.
    CHECK(!store.HasCoin({kFund, 0}));
    CHECK(store.HasCoin({blocks[1].vtx[1].GetTxId(), 0}));
    store.Close();
    // Reopen and verify again from disk.
    ChainStore store2;
    CHECK(store2.Open(path, error));
    CHECK(store2.BlockCount(error) == 6);
    Block reloaded;
    CHECK(store2.GetBlock(blocks[5].GetHash(), reloaded, error));
    CHECK(reloaded.GetHash() == blocks[5].GetHash());
    store2.Close();
    CleanupDb(path);
  }

  test::BeginSection("db: node restart restores state");
  {
    const std::string path = DbPath("restart");
    CleanupDb(path);
    ChainStore store;
    std::string error;
    CHECK(store.Open(path, error));
    const Block genesis = MineGenesis();
    Hash256 tipAfter;
    std::size_t utxoAfter = 0;
    {
      auto a = std::make_unique<Node>(kTestBits);
      a->SetTime(kNow);
      a->SetTransport(std::make_shared<LoopbackTransport>("loopback:db-a"));
      a->AttachStore(&store);
      CHECK(a->BroadcastBlock(genesis, error));
      const Hash256 kFund = [] {
        Hash256 h;
        h.fill(0x52);
        return h;
      }();
      Fund(a->Utxo(), kFund, 50 * COIN);
      Transaction tx = MakeSpend(kFund, 49 * COIN);
      CHECK(a->BroadcastTx(tx, error));
      BlockTemplate templ =
          AssembleBlock(genesis.GetHash(), 1, kTime + 600, kTestBits,
                        a->MempoolRef(), a->Utxo(), kTime, Spk());
      Block block = templ.block;
      CHECK(MineBlock(block, 1, ""));
      CHECK(a->BroadcastBlock(block, error));
      CHECK(a->TipHeight() == 1);
      CHECK(a->LastStoreError().empty());
      tipAfter = a->TipHash();
      utxoAfter = a->Utxo().Size();
    }
    store.Close();
    // A fresh node loads the same state from disk.
    ChainStore store2;
    CHECK(store2.Open(path, error));
    auto b = std::make_unique<Node>(kTestBits);
    b->SetTime(kNow);
    CHECK(b->LoadFromStore(store2, error));
    CHECK(b->TipHeight() == 1);
    CHECK(b->TipHash() == tipAfter);
    CHECK(b->Utxo().Size() == utxoAfter);
    CHECK(b->HasBlock(genesis.GetHash()));
    CHECK(b->HasBlock(tipAfter));
    // The loaded index accepts a new block on top.
    BlockTemplate templ2 = AssembleBlock(tipAfter, 2, kTime + 1200, kTestBits,
                                         b->MempoolRef(), b->Utxo(), kTime,
                                         Spk());
    Block block2 = templ2.block;
    CHECK(MineBlock(block2, 2, ""));
    CHECK(b->BroadcastBlock(block2, error));
    CHECK(b->TipHeight() == 2);
    store2.Close();
    CleanupDb(path);
  }

  test::BeginSection("db: atomicity — rollback discards, commit keeps");
  {
    const std::string path = DbPath("atomic");
    CleanupDb(path);
    ChainStore store;
    std::string error;
    CHECK(store.Open(path, error));
    const OutPoint op{Hash256{}, 0};
    Coin coin;
    coin.out.value = 1000;
    coin.out.scriptPubKey = Spk();
    coin.height = 1;
    coin.blockTime = kTime;
    // Uncommitted write disappears.
    CHECK(store.Db().BeginTransaction(error));
    CHECK(store.PutCoin(op, coin, error));
    CHECK(store.HasCoin(op));
    CHECK(store.Db().RollbackTransaction(error));
    CHECK(!store.HasCoin(op));
    // Committed write survives a reopen.
    CHECK(store.Db().BeginTransaction(error));
    CHECK(store.PutCoin(op, coin, error));
    CHECK(store.Db().CommitTransaction(error));
    CHECK(store.HasCoin(op));
    store.Close();
    ChainStore store2;
    CHECK(store2.Open(path, error));
    CHECK(store2.HasCoin(op));
    store2.Close();
    CleanupDb(path);
  }

  test::BeginSection("db: pruning drops bodies, keeps headers + UTXO");
  {
    const std::string path = DbPath("prune");
    CleanupDb(path);
    ChainStore store;
    std::string error;
    CHECK(store.Open(path, error));
    ChainState chain(kTestBits);
    CoinsView utxo;
    const Hash256 kFund = [] {
      Hash256 h;
      h.fill(0x53);
      return h;
    }();
    Fund(utxo, kFund, 50 * COIN);
    std::vector<Block> blocks =
        BuildChain(chain, utxo, store, 6, kFund, error);
    CHECK(blocks.size() == 6);
    CHECK(store.PruneBlocks(4, error));
    int pruneHeight = -1;
    CHECK(store.LoadPruneHeight(pruneHeight, error));
    CHECK(pruneHeight == 4);
    for (int i = 0; i < 6; ++i) {
      CHECK(store.HasBlock(blocks[i].GetHash()));
      if (i < 4) {
        CHECK(!store.HasBlockBody(blocks[i].GetHash()));
        Block b;
        CHECK(!store.GetBlock(blocks[i].GetHash(), b, error));
      } else {
        CHECK(store.HasBlockBody(blocks[i].GetHash()));
      }
      BlockHeader h;
      CHECK(store.GetBlockHeader(blocks[i].GetHash(), h, error));
    }
    CHECK(store.UtxoCount(error) == utxo.Size());
    // A node loading the pruned store still reaches the tip.
    store.Close();
    ChainStore store2;
    CHECK(store2.Open(path, error));
    auto node = std::make_unique<Node>(kTestBits);
    node->SetTime(kNow);
    CHECK(node->LoadFromStore(store2, error));
    CHECK(node->TipHeight() == 5);
    CHECK(node->Utxo().Size() == utxo.Size());
    store2.Close();
    CleanupDb(path);
  }

  test::BeginSection("db: corruption is detected, never accepted");
  {
    const std::string path = DbPath("corrupt");
    CleanupDb(path);
    ChainStore store;
    std::string error;
    CHECK(store.Open(path, error));
    ChainState chain(kTestBits);
    CoinsView utxo;
    const Hash256 kFund = [] {
      Hash256 h;
      h.fill(0x54);
      return h;
    }();
    Fund(utxo, kFund, 50 * COIN);
    std::vector<Block> blocks =
        BuildChain(chain, utxo, store, 3, kFund, error);
    CHECK(blocks.size() == 3);
    const Hash256 target = blocks[2].GetHash();
    // Corrupt the stored body: GetBlock must refuse it.
    {
      Database::Statement stmt;
      CHECK(stmt.Prepare(store.Db(),
                         "UPDATE blocks SET body=?1 WHERE hash=?2;", error));
      std::vector<unsigned char> garbage(100, 0xAA);
      stmt.BindBlob(1, garbage);
      std::vector<unsigned char> hashBytes(target.begin(), target.end());
      stmt.BindBlob(2, hashBytes);
      stmt.Step();
      CHECK(!stmt.HasError());
    }
    Block b;
    CHECK(!store.GetBlock(target, b, error));
    // Corrupt the stored work: a node loading the store must fail.
    {
      Database::Statement stmt;
      CHECK(stmt.Prepare(store.Db(),
                         "UPDATE blocks SET body=?1 WHERE hash=?2;", error));
      stmt.BindBlob(1, blocks[2].Serialize());
      std::vector<unsigned char> hashBytes(target.begin(), target.end());
      stmt.BindBlob(2, hashBytes);
      stmt.Step();
      CHECK(!stmt.HasError());
      Database::Statement stmt2;
      CHECK(stmt2.Prepare(store.Db(),
                          "UPDATE blocks SET work=?1 WHERE hash=?2;", error));
      std::vector<unsigned char> zeroWork(32, 0);
      stmt2.BindBlob(1, zeroWork);
      stmt2.BindBlob(2, hashBytes);
      stmt2.Step();
      CHECK(!stmt2.HasError());
    }
    store.Close();
    ChainStore store2;
    CHECK(store2.Open(path, error));
    auto node = std::make_unique<Node>(kTestBits);
    node->SetTime(kNow);
    CHECK(!node->LoadFromStore(store2, error));
    store2.Close();
    CleanupDb(path);
  }

  test::BeginSection("db: peers and bans persist");
  {
    const std::string path = DbPath("peers");
    CleanupDb(path);
    ChainStore store;
    std::string error;
    CHECK(store.Open(path, error));
    CHECK(store.PutPeer("1.2.3.4:64775", 1700000000, 1, error));
    CHECK(store.PutPeer("5.6.7.8:64775", 1700000100, 1, error));
    CHECK(store.PutBan("9.9.9.9:64775", 1800000000, 2, error));
    store.Close();
    ChainStore store2;
    CHECK(store2.Open(path, error));
    std::vector<std::pair<std::string, uint32_t>> peers;
    CHECK(store2.GetPeers(peers, error));
    CHECK(peers.size() == 2);
    std::vector<std::tuple<std::string, uint32_t, int>> bans;
    CHECK(store2.GetBans(bans, error));
    CHECK(bans.size() == 1);
    CHECK(std::get<0>(bans[0]) == "9.9.9.9:64775");
    CHECK(std::get<1>(bans[0]) == 1800000000);
    CHECK(std::get<2>(bans[0]) == 2);
    CHECK(store2.ClearBan("9.9.9.9:64775", error));
    CHECK(store2.GetBans(bans, error));
    CHECK(bans.empty());
    store2.Close();
    CleanupDb(path);
  }

  test::BeginSection("db: fork blocks are retained after a reorg");
  {
    const std::string path = DbPath("fork");
    CleanupDb(path);
    ChainStore store;
    std::string error;
    CHECK(store.Open(path, error));
    ChainState chain(kTestBits);
    CoinsView utxo;
    const Hash256 kFund = [] {
      Hash256 h;
      h.fill(0x55);
      return h;
    }();
    Fund(utxo, kFund, 50 * COIN);
    std::vector<Block> blocks =
        BuildChain(chain, utxo, store, 2, kFund, error);
    CHECK(blocks.size() == 2);
    const Hash256 oldTip = blocks[1].GetHash();
    // A competing branch: two blocks on top of genesis (more work).
    BlockTemplate templA = AssembleBlock(blocks[0].GetHash(), 1, kTime + 700,
                                         kTestBits, Mempool(), utxo, kTime,
                                         Spk());
    Block fork1 = templA.block;
    CHECK(MineBlock(fork1, 1, "fork"));
    CHECK(chain.AddBlock(fork1, kNow, error));
    BlockUndo forkUndo1;
    CHECK(BuildBlockUndo(fork1, utxo, 1, fork1.header.nTime, forkUndo1,
                         error));
    CHECK(StoreApply(store, fork1, chain, forkUndo1, error));
    BlockTemplate templB =
        AssembleBlock(fork1.GetHash(), 2, kTime + 1300, kTestBits, Mempool(),
                      utxo, kTime, Spk());
    Block fork2 = templB.block;
    CHECK(MineBlock(fork2, 2, "fork"));
    CHECK(chain.AddBlock(fork2, kNow, error));
    BlockUndo forkUndo2;
    CHECK(BuildBlockUndo(fork2, utxo, 2, fork2.header.nTime, forkUndo2,
                         error));
    CHECK(StoreApply(store, fork2, chain, forkUndo2, error));
    CHECK(chain.GetTipHeight() == 2);
    CHECK(chain.GetTipHash() == fork2.GetHash());
    // Two blocks at height 1; the old fork is still stored.
    std::vector<Hash256> atHeight;
    CHECK(store.GetHashesAtHeight(1, atHeight, error));
    CHECK(atHeight.size() == 2);
    Block reloaded;
    CHECK(store.GetBlock(oldTip, reloaded, error));
    CHECK(reloaded.GetHash() == oldTip);
    Hash256 tip;
    CHECK(store.LoadTip(tip, error));
    CHECK(tip == fork2.GetHash());
    store.Close();
    CleanupDb(path);
  }
}
