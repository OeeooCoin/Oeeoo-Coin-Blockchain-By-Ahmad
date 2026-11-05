// Tests: mining (Milestone 5) — coinbase construction and reward
// validation, mempool rules, block-template assembly, nonce search
// with extraNonce rollover, and a mined block accepted by the chain.
#include <string>
#include <vector>

#include "blockchain/chain.h"
#include "consensus/consensus.h"
#include "consensus/subsidy.h"
#include "crypto/ecdsa.h"
#include "mempool/mempool.h"
#include "mining/miner.h"
#include "script/script.h"
#include "test_harness.h"
#include "transactions/sighash.h"
#include "transactions/validation.h"

using namespace oeeoo;

namespace {
constexpr uint32_t kTestBits = 0x207fffff;  // easy test-only difficulty
constexpr uint32_t kTime = 1700000000;
constexpr uint32_t kNow = 1800000000;

KeyPair g_miner;
bool g_keysReady = false;

void EnsureKeys() {
  if (!g_keysReady) {
    CHECK(GenerateKeyPair(g_miner));
    g_keysReady = true;
  }
}

std::vector<unsigned char> MinerSpk() {
  return CreateP2PKHScriptPubKey(PubKeyHash160(g_miner.pubkey));
}

Hash256 TxIdWith(unsigned char fill) {
  Hash256 h;
  h.fill(fill);
  return h;
}

void Fund(CoinsView& utxo, const Hash256& txid, Amount value, int height = 0) {
  Coin coin;
  coin.out.value = value;
  coin.out.scriptPubKey = MinerSpk();
  coin.height = height;
  coin.blockTime = kTime;
  coin.isCoinbase = false;
  CHECK(utxo.AddCoin({txid, 0}, coin));
}

std::vector<unsigned char> SignInput(const Transaction& tx, std::size_t nIn,
                                     const std::vector<unsigned char>& spk) {
  Hash256 h;
  CHECK(LegacySighash(tx, nIn, spk, kSigHashAll, h));
  std::vector<unsigned char> sig = SignHash(h, g_miner.privkey);
  CHECK(!sig.empty());
  sig.push_back(static_cast<unsigned char>(kSigHashAll));
  return sig;
}

// A one-input spend of `prev` (output 0) paying `sendValue` to the
// miner's own P2PKH (fee = 50 OEEOO - sendValue by default funding).
Transaction Spend(const Hash256& prev, Amount sendValue) {
  Transaction tx;
  tx.version = 1;
  TxIn in;
  in.prevTxHash = prev;
  in.prevOutIndex = 0;
  in.sequence = 0xffffffff;
  tx.vin.push_back(in);
  TxOut out;
  out.value = sendValue;
  out.scriptPubKey = MinerSpk();
  tx.vout.push_back(out);
  tx.lockTime = 0;
  tx.vin[0].scriptSig =
      CreateP2PKHScriptSig(SignInput(tx, 0, MinerSpk()), g_miner.pubkey);
  return tx;
}

bool AddOk(Mempool& pool, const Transaction& tx, const CoinsView& utxo,
           uint32_t now = kNow) {
  std::string error;
  bool ok = pool.AddTransaction(tx, utxo, 1, kTime, now, error);
  if (!ok) std::printf("  unexpected mempool rejection: %s\n", error.c_str());
  return ok;
}
}  // namespace

void TestMining() {
  EnsureKeys();

  test::BeginSection("mining: coinbase construction + validation");
  {
    Transaction cb = CreateCoinbase(1, MinerSpk(), 50 * COIN);
    CHECK(cb.vin.size() == 1);
    CHECK(cb.vin[0].prevTxHash == Hash256{});
    CHECK(cb.vin[0].prevOutIndex == 0xffffffff);
    int64_t height = -1;
    CHECK(GetCoinbaseHeight(cb.vin[0].scriptSig, height));
    CHECK(height == 1);
    std::string error;
    CHECK(ValidateCoinbase(cb, 1, 50 * COIN, error));
    // Under-claim is allowed (unclaimed value is lost).
    Transaction under = CreateCoinbase(1, MinerSpk(), 49 * COIN);
    CHECK(ValidateCoinbase(under, 1, 50 * COIN, error));
    // Over-claim is not.
    Transaction over = CreateCoinbase(1, MinerSpk(), 50 * COIN + 1);
    CHECK(!ValidateCoinbase(over, 1, 50 * COIN, error));
    // Wrong height.
    CHECK(!ValidateCoinbase(cb, 2, 50 * COIN, error));
    // Era boundaries: 210001 -> 25 OEEOO, 940001 -> 0 subsidy.
    Transaction cb2 = CreateCoinbase(210001, MinerSpk(), 25 * COIN);
    CHECK(ValidateCoinbase(cb2, 210001, 25 * COIN, error));
    Transaction cb3 = CreateCoinbase(940001, MinerSpk(), 2 * COIN);
    CHECK(ValidateCoinbase(cb3, 940001, 2 * COIN, error));  // fees only
    CHECK(!ValidateCoinbase(cb3, 940001, 1 * COIN, error));
    // scriptSig too long (> 100 bytes).
    Transaction big = CreateCoinbase(1, MinerSpk(), 50 * COIN,
                                     std::string(200, 'x'));
    CHECK(!ValidateCoinbase(big, 1, 50 * COIN, error));
    // Non-coinbase input (prev hash non-zero).
    Transaction bad = cb;
    bad.vin[0].prevTxHash.fill(0x01);
    CHECK(!ValidateCoinbase(bad, 1, 50 * COIN, error));
  }

  test::BeginSection("mining: IsCoinbase + block fees with a real coinbase");
  {
    // Regression (found by Milestone 6): a real coinbase has ONE
    // input spending the null outpoint — an empty vin is NOT a
    // coinbase and must not be treated as one.
    Transaction cb = CreateCoinbase(1, MinerSpk(), 50 * COIN);
    CHECK(IsCoinbase(cb));
    Transaction normal;
    normal.vin.push_back(TxIn{});
    CHECK(!IsCoinbase(normal));
    Transaction emptyVin;
    CHECK(!IsCoinbase(emptyVin));
    // A block with a real coinbase + one spend: fees computed and
    // transactions connected without error.
    CoinsView utxo;
    const Hash256 kA = TxIdWith(0x21);
    Fund(utxo, kA, 50 * COIN);
    Transaction spend = Spend(kA, 49 * COIN);  // fee 1 OEEOO
    Block block;
    block.header.nVersion = 1;
    block.header.nTime = kTime;
    block.header.nBits = kTestBits;
    block.vtx = {cb, spend};
    block.header.hashMerkleRoot = block.ComputeMerkleRoot();
    Amount fees = 0;
    std::string error;
    CHECK(ComputeBlockFees(block, utxo, 1, kTime, fees, error));
    CHECK(fees == 1 * COIN);
    CoinsView working = utxo;
    CHECK(ConnectBlockTransactions(block, working, 1, kTime, kTime, fees,
                                   error));
    CHECK(working.Size() == 2);  // coinbase output + spend output
  }

  test::BeginSection("mining: mempool rules");
  {
    CoinsView utxo;
    const Hash256 kA = TxIdWith(0x21);
    const Hash256 kB = TxIdWith(0x22);
    Fund(utxo, kA, 50 * COIN);
    Fund(utxo, kB, 10 * COIN);
    Mempool pool;
    Transaction tx1 = Spend(kA, 49 * COIN);          // fee 1 OEEOO
    Transaction tx2 = Spend(kB, 10 * COIN - 10000);  // fee 10,000 sats
    CHECK(AddOk(pool, tx1, utxo));
    CHECK(AddOk(pool, tx2, utxo));
    CHECK(pool.Size() == 2);
    // Fee-rate order: tx1 (~4000 sat/vB) before tx2 (~40 sat/vB).
    auto ordered = pool.GetByFeeRate();
    CHECK(ordered.size() == 2);
    CHECK(ordered[0].tx.GetTxId() == tx1.GetTxId());
    CHECK(ordered[1].tx.GetTxId() == tx2.GetTxId());
    // Duplicate.
    std::string error;
    CHECK(!pool.AddTransaction(tx1, utxo, 1, kTime, kNow, error));
    // Conflict: another spend of the same input.
    Transaction conflict = Spend(kA, 48 * COIN);
    CHECK(!pool.AddTransaction(conflict, utxo, 1, kTime, kNow, error));
    // Zero fee: below the 1 sat/vB minimum relay fee. (kB is already
    // spent by tx2 in the pool, so build on a fresh coin.)
    const Hash256 kC = TxIdWith(0x23);
    Fund(utxo, kC, 10 * COIN);
    Transaction zeroFeeC = Spend(kC, 10 * COIN);
    CHECK(!pool.AddTransaction(zeroFeeC, utxo, 1, kTime, kNow, error));
    // Expiry: an entry added long ago disappears after 14 days.
    Mempool pool2;
    CHECK(AddOk(pool2, tx1, utxo, 1000));
    CHECK(pool2.Size() == 1);
    pool2.RemoveExpired(1000 + 14 * 24 * 60 * 60 - 1);
    CHECK(pool2.Size() == 1);  // one second before expiry: still there
    pool2.RemoveExpired(1000 + 14 * 24 * 60 * 60);
    CHECK(pool2.Size() == 0);
    // Size cap: a 300-byte mempool fits one ~250-byte tx, not two.
    Mempool tiny(300);
    CHECK(AddOk(tiny, tx1, utxo));
    CHECK(!tiny.AddTransaction(tx2, utxo, 1, kTime, kNow, error));
  }

  test::BeginSection("mining: block template assembly");
  {
    CoinsView utxo;
    const Hash256 kA = TxIdWith(0x21);
    const Hash256 kB = TxIdWith(0x22);
    Fund(utxo, kA, 50 * COIN);
    Fund(utxo, kB, 10 * COIN);
    Mempool pool;
    Transaction tx1 = Spend(kA, 49 * COIN);          // fee 1 OEEOO
    Transaction tx2 = Spend(kB, 10 * COIN - 10000);  // fee 10,000 sats
    CHECK(AddOk(pool, tx1, utxo));
    CHECK(AddOk(pool, tx2, utxo));
    BlockTemplate templ = AssembleBlock(Hash256{}, 1, kTime + 600, kTestBits,
                                        pool, utxo, kTime, MinerSpk());
    CHECK(templ.txCount == 2);
    CHECK(templ.totalFees == 1 * COIN + 10000);
    CHECK(templ.block.vtx.size() == 3);
    CHECK(templ.block.vtx[0].vout[0].value == 50 * COIN + templ.totalFees);
    // Fee-rate order inside the block: tx1 before tx2.
    CHECK(templ.block.vtx[1].GetTxId() == tx1.GetTxId());
    CHECK(templ.block.vtx[2].GetTxId() == tx2.GetTxId());
    std::string error;
    CHECK(templ.block.CheckBasic(error));
    CHECK(CheckBlockReward(templ.block, 1, templ.totalFees, error));
    // Empty mempool: coinbase is exactly the subsidy.
    Mempool empty;
    BlockTemplate templ2 = AssembleBlock(Hash256{}, 1, kTime + 600,
                                         kTestBits, empty, utxo, kTime,
                                         MinerSpk());
    CHECK(templ2.txCount == 0);
    CHECK(templ2.totalFees == 0);
    CHECK(templ2.block.vtx[0].vout[0].value == 50 * COIN);
  }

  test::BeginSection("mining: nonce search + extraNonce rollover");
  {
    CoinsView utxo;
    const Hash256 kA = TxIdWith(0x21);
    Fund(utxo, kA, 50 * COIN);
    Mempool pool;
    CHECK(AddOk(pool, Spend(kA, 49 * COIN), utxo));
    BlockTemplate templ = AssembleBlock(Hash256{}, 1, kTime + 600, kTestBits,
                                        pool, utxo, kTime, MinerSpk());
    Block block = templ.block;
    CHECK(MineBlock(block, 1, ""));
    CHECK(CheckProofOfWork(block.GetHash(), kTestBits));
    std::string error;
    CHECK(CheckBlockReward(block, 1, templ.totalFees, error));
    // Zero iterations: must fail.
    Block block2 = templ.block;
    CHECK(!MineBlock(block2, 1, "", 0));
    // Impossible target, few iterations: must fail.
    Block hard = templ.block;
    hard.header.nBits = 0x02003456;  // target 52
    CHECK(!MineBlock(hard, 1, "", 100));
    // Nonce-range wrap: start at 0xfffffffe with an impossible target
    // and 3 iterations -> tries 0xfffffffe, 0xffffffff, then bumps
    // the extraNonce and tries nonce 0. The coinbase must then carry
    // extraNonce 1 and still validate.
    Block wrap = templ.block;
    wrap.header.nBits = 0x02003456;
    wrap.header.nNonce = 0xfffffffe;
    CHECK(!MineBlock(wrap, 1, "", 3));
    int64_t height = -1;
    CHECK(GetCoinbaseHeight(wrap.vtx[0].vin[0].scriptSig, height));
    CHECK(height == 1);
    CHECK(wrap.vtx[0].vin[0].scriptSig.size() ==
          templ.block.vtx[0].vin[0].scriptSig.size() + 4);
    CHECK(ValidateCoinbase(wrap.vtx[0], 1, 50 * COIN + templ.totalFees,
                           error));
    // BumpExtraNonce directly: Merkle root changes, height preserved.
    Block bumped = templ.block;
    const Hash256 before = bumped.header.hashMerkleRoot;
    BumpExtraNonce(bumped, 1, "", 7);
    CHECK(bumped.header.hashMerkleRoot != before);
    CHECK(GetCoinbaseHeight(bumped.vtx[0].vin[0].scriptSig, height));
    CHECK(height == 1);
  }

  test::BeginSection("mining: mined block accepted by the chain");
  {
    ChainState chain(kTestBits);
    Block genesis;
    genesis.header.nVersion = 1;
    genesis.header.nTime = kTime;
    genesis.header.nBits = kTestBits;
    genesis.vtx = {CreateCoinbase(0, MinerSpk(), 50 * COIN)};
    genesis.header.hashMerkleRoot = genesis.ComputeMerkleRoot();
    CHECK(MineBlock(genesis, 0, ""));
    std::string error;
    CHECK(chain.AddBlock(genesis, kNow, error));
    CHECK(chain.GetTipHeight() == 0);
    // Fund the miner's coinbase output at height 0 is NOT mature yet,
    // so the template at height 1 uses a fresh non-coinbase funding.
    CoinsView utxo;
    const Hash256 kA = TxIdWith(0x21);
    Fund(utxo, kA, 50 * COIN, 0);
    Mempool pool;
    CHECK(AddOk(pool, Spend(kA, 49 * COIN), utxo));
    BlockTemplate templ = AssembleBlock(genesis.GetHash(), 1, kTime + 600,
                                        kTestBits, pool, utxo, kTime,
                                        MinerSpk());
    Block block = templ.block;
    CHECK(MineBlock(block, 1, ""));
    CHECK(chain.AddBlock(block, kNow, error));
    CHECK(chain.GetTipHeight() == 1);
    CHECK(chain.GetTipHash() == block.GetHash());
  }
}
