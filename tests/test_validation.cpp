// Tests: UTXO set and transaction validation (Milestone 4) —
// finality, BIP68 sequence locks, input existence, coinbase maturity,
// value balance, fees, block connection (incl. double spend in one
// block and no-commit-on-failure).
#include <string>
#include <vector>

#include "blockchain/block.h"
#include "crypto/ecdsa.h"
#include "mining/miner.h"
#include "script/script.h"
#include "test_harness.h"
#include "transactions/sighash.h"
#include "transactions/validation.h"

using namespace oeeoo;

namespace {
constexpr uint32_t kCoinTime = 1700000000;

KeyPair g_alice;
bool g_keysReady = false;

void EnsureKeys() {
  if (!g_keysReady) {
    CHECK(GenerateKeyPair(g_alice));
    g_keysReady = true;
  }
}

Hash256 PrevTxId(unsigned char fill) {
  Hash256 h;
  h.fill(fill);
  return h;
}

// Fund `utxo` with one P2PKH coin for Alice: `value`, created at
// `height`/`blockTime`, coinbase or not.
void Fund(CoinsView& utxo, const Hash256& txid, Amount value, int height,
          bool isCoinbase, uint32_t blockTime = kCoinTime) {
  Coin coin;
  coin.out.value = value;
  coin.out.scriptPubKey =
      CreateP2PKHScriptPubKey(PubKeyHash160(g_alice.pubkey));
  coin.height = height;
  coin.blockTime = blockTime;
  coin.isCoinbase = isCoinbase;
  CHECK(utxo.AddCoin({txid, 0}, coin));
}

std::vector<unsigned char> SignInput(const Transaction& tx, std::size_t nIn,
                                     const std::vector<unsigned char>& spk) {
  Hash256 h;
  CHECK(LegacySighash(tx, nIn, spk, kSigHashAll, h));
  std::vector<unsigned char> sig = SignHash(h, g_alice.privkey);
  CHECK(!sig.empty());
  sig.push_back(static_cast<unsigned char>(kSigHashAll));
  return sig;
}

// Build + sign a one-input spend of `prev` (output 0) to Alice's own
// P2PKH, sending `sendValue` (fee = 50 - sendValue in the default
// funding). The scriptPubKey spent is Alice's.
Transaction Spend(const Hash256& prev, Amount sendValue,
                  int32_t version = 1, uint32_t lockTime = 0,
                  uint32_t sequence = 0xffffffff) {
  Transaction tx;
  tx.version = version;
  TxIn in;
  in.prevTxHash = prev;
  in.prevOutIndex = 0;
  in.sequence = sequence;
  tx.vin.push_back(in);
  TxOut out;
  out.value = sendValue;
  out.scriptPubKey = CreateP2PKHScriptPubKey(PubKeyHash160(g_alice.pubkey));
  tx.vout.push_back(out);
  tx.lockTime = lockTime;
  std::vector<unsigned char> spk =
      CreateP2PKHScriptPubKey(PubKeyHash160(g_alice.pubkey));
  std::vector<unsigned char> sig = SignInput(tx, 0, spk);
  tx.vin[0].scriptSig = CreateP2PKHScriptSig(sig, g_alice.pubkey);
  return tx;
}

bool ValidateOk(const Transaction& tx, const CoinsView& utxo, int height,
                uint32_t mtp, Amount* feeOut = nullptr) {
  TxValidationResult r = ValidateTransaction(tx, utxo, height, mtp);
  if (!r.ok) std::printf("  unexpected validation failure: %s\n",
                         r.error.c_str());
  if (feeOut) *feeOut = r.fee;
  return r.ok;
}

bool ValidateFails(const Transaction& tx, const CoinsView& utxo, int height,
                   uint32_t mtp) {
  TxValidationResult r = ValidateTransaction(tx, utxo, height, mtp);
  CHECK(!r.ok);
  CHECK(!r.error.empty());
  return !r.ok;
}
}  // namespace

void TestValidation() {
  EnsureKeys();
  const Hash256 kPrev = PrevTxId(0x22);

  test::BeginSection("validation: valid spend + fee");
  {
    CoinsView utxo;
    Fund(utxo, kPrev, 50 * COIN, 0, false);
    Transaction tx = Spend(kPrev, 49 * COIN);
    Amount fee = 0;
    CHECK(ValidateOk(tx, utxo, 10, kCoinTime, &fee));
    CHECK(fee == 1 * COIN);
  }

  test::BeginSection("validation: missing input, overspend");
  {
    CoinsView utxo;
    Fund(utxo, kPrev, 50 * COIN, 0, false);
    // Missing input.
    Transaction missing = Spend(PrevTxId(0x33), 49 * COIN);
    CHECK(ValidateFails(missing, utxo, 10, kCoinTime));
    // Outputs exceed inputs (51 > 50).
    Transaction overspend = Spend(kPrev, 51 * COIN);
    CHECK(ValidateFails(overspend, utxo, 10, kCoinTime));
    // Zero-fee spend is fine (50 in, 50 out).
    Transaction zeroFee = Spend(kPrev, 50 * COIN);
    Amount fee = 0;
    CHECK(ValidateOk(zeroFee, utxo, 10, kCoinTime, &fee));
    CHECK(fee == 0);
  }

  test::BeginSection("validation: locktime finality");
  {
    CoinsView utxo;
    Fund(utxo, kPrev, 50 * COIN, 0, false);
    // Height lock: lockTime 20 at height 10, sequence 0 -> not final.
    Transaction locked = Spend(kPrev, 49 * COIN, 1, 20, 0);
    CHECK(ValidateFails(locked, utxo, 10, kCoinTime));
    // Same lock, but all sequences final -> final.
    Transaction finalSeq = Spend(kPrev, 49 * COIN, 1, 20, 0xffffffff);
    CHECK(ValidateOk(finalSeq, utxo, 10, kCoinTime));
    // LockTime reached: a height lock of 20 is final from height 21
    // (Bitcoin rule is strict: lockTime < height).
    Transaction reached = Spend(kPrev, 49 * COIN, 1, 20, 0);
    CHECK(ValidateFails(reached, utxo, 20, kCoinTime));
    CHECK(ValidateOk(reached, utxo, 21, kCoinTime));
    // Time lock: lockTime 1700001000 vs MTP 1700000000 -> not final;
    // final once MTP passes it (strict less-than again).
    Transaction timeLocked = Spend(kPrev, 49 * COIN, 1, 1700001000, 0);
    CHECK(ValidateFails(timeLocked, utxo, 10, kCoinTime));
    CHECK(ValidateFails(timeLocked, utxo, 10, 1700001000));
    CHECK(ValidateOk(timeLocked, utxo, 10, 1700001001));
  }

  test::BeginSection("validation: coinbase maturity (100 blocks)");
  {
    // A MINED coinbase (height 1) matures after 100 blocks. The
    // genesis coinbase (height 0) is exempt by design (Milestone 9:
    // the genesis block can never be reorged, and the founder vault
    // must be spendable at launch), so maturity is tested here with
    // a height-1 coinbase, not the genesis.
    CoinsView utxo;
    Fund(utxo, kPrev, 50 * COIN, 1, true);
    Transaction tx = Spend(kPrev, 49 * COIN);
    CHECK(ValidateFails(tx, utxo, 100, kCoinTime));
    CHECK(ValidateOk(tx, utxo, 101, kCoinTime));
  }

  test::BeginSection("validation: BIP68 sequence locks");
  {
    // Height lock: coin at height 10, sequence 5 -> needs height 16.
    CoinsView utxo;
    Fund(utxo, kPrev, 50 * COIN, 10, false);
    Transaction tx = Spend(kPrev, 49 * COIN, 2, 0, 5);
    CHECK(ValidateFails(tx, utxo, 15, kCoinTime));
    CHECK(ValidateOk(tx, utxo, 16, kCoinTime));
    // Time lock: sequence type flag + 10 units of 512s = 5120s after
    // the coin's block time.
    CoinsView utxo2;
    Fund(utxo2, kPrev, 50 * COIN, 10, false, kCoinTime);
    Transaction txTime = Spend(kPrev, 49 * COIN, 2, 0, 0x00400000u | 10);
    CHECK(ValidateFails(txTime, utxo2, 16, kCoinTime + 5119));
    CHECK(ValidateOk(txTime, utxo2, 16, kCoinTime + 5120));
    // Disable flag: no lock at all.
    Transaction txDis = Spend(kPrev, 49 * COIN, 2, 0, 0x80000005u);
    CHECK(ValidateOk(txDis, utxo, 11, kCoinTime));
    // Version 1: BIP68 does not apply.
    Transaction txV1 = Spend(kPrev, 49 * COIN, 1, 0, 5);
    CHECK(ValidateOk(txV1, utxo, 11, kCoinTime));
  }

  test::BeginSection("validation: block connection + double spend");
  {
    CoinsView utxo;
    Fund(utxo, kPrev, 50 * COIN, 0, false);
    const Hash256 kPrev2 = PrevTxId(0x44);
    Fund(utxo, kPrev2, 10 * COIN, 0, false);
    Transaction tx1 = Spend(kPrev, 49 * COIN);
    // tx2 spends tx1's change output.
    Transaction tx2;
    tx2.version = 1;
    TxIn in;
    in.prevTxHash = tx1.GetTxId();
    in.prevOutIndex = 0;
    in.sequence = 0xffffffff;
    tx2.vin.push_back(in);
    TxOut out;
    out.value = 48 * COIN;
    out.scriptPubKey =
        CreateP2PKHScriptPubKey(PubKeyHash160(g_alice.pubkey));
    tx2.vout.push_back(out);
    tx2.lockTime = 0;
    std::vector<unsigned char> spk =
        CreateP2PKHScriptPubKey(PubKeyHash160(g_alice.pubkey));
    tx2.vin[0].scriptSig =
        CreateP2PKHScriptSig(SignInput(tx2, 0, spk), g_alice.pubkey);

    Block block;
    block.header.nVersion = 1;
    block.header.nTime = kCoinTime + 600;
    // M20 (OOC-001): a valid block carries exactly one coinbase at
    // vtx[0]; the connection logic below is exercised with a real
    // block shape. The coinbase value is 0 here — Connect does not
    // check the reward amount (CheckBlockReward does) — so the
    // fees below are exactly tx1's 1 OEEOO + tx2's 1 OEEOO.
    Transaction coinbase = CreateCoinbase(
        1, CreateP2PKHScriptPubKey(PubKeyHash160(g_alice.pubkey)), 0);
    block.vtx = {coinbase, tx1, tx2};
    block.header.hashMerkleRoot = block.ComputeMerkleRoot();
    Amount fees = 0;
    std::string error;
    CHECK(ConnectBlockTransactions(block, utxo, 1, kCoinTime,
                                   kCoinTime + 600, fees, error));
    CHECK(fees == 2 * COIN);  // 1 + 1 OEEOO
    CHECK(utxo.Size() == 3);  // kPrev2 coin + tx2 change + coinbase
    CHECK(!utxo.HasCoin({kPrev, 0}));
    CHECK(utxo.HasCoin({tx2.GetTxId(), 0}));
    CHECK(utxo.HasCoin({coinbase.GetTxId(), 0}));

    // Block with a same-block double spend (two txs spending the live
    // kPrev2 coin) must fail and change nothing.
    Transaction txA = Spend(kPrev2, 9 * COIN);
    Transaction txB = Spend(kPrev2, 8 * COIN);
    Block badBlock;
    badBlock.header.nVersion = 1;
    badBlock.header.nTime = kCoinTime + 1200;
    // Same M20 shape: a coinbase at vtx[0] so the failure below is
    // the double spend, not a missing coinbase.
    Transaction badCoinbase = CreateCoinbase(
        2, CreateP2PKHScriptPubKey(PubKeyHash160(g_alice.pubkey)), 0);
    badBlock.vtx = {badCoinbase, txA, txB};
    badBlock.header.hashMerkleRoot = badBlock.ComputeMerkleRoot();
    const std::size_t sizeBefore = utxo.Size();
    CHECK(!ConnectBlockTransactions(badBlock, utxo, 2, kCoinTime + 600,
                                    kCoinTime + 1200, fees, error));
    CHECK(utxo.Size() == sizeBefore);
    CHECK(utxo.HasCoin({kPrev2, 0}));  // the good tx was not committed
  }

  test::BeginSection("validation: coinbase outputs enter UTXO set");
  {
    CoinsView utxo;
    // A real coinbase: one input spending the null outpoint.
    Transaction coinbase = CreateCoinbase(
        1, CreateP2PKHScriptPubKey(PubKeyHash160(g_alice.pubkey)), 50 * COIN);
    Block block;
    block.header.nVersion = 1;
    block.header.nTime = kCoinTime;
    block.vtx = {coinbase};
    block.header.hashMerkleRoot = block.ComputeMerkleRoot();
    Amount fees = 0;
    std::string error;
    CHECK(ConnectBlockTransactions(block, utxo, 1, kCoinTime, kCoinTime,
                                   fees, error));
    CHECK(utxo.Size() == 1);
    Coin coin;
    CHECK(utxo.GetCoin({coinbase.GetTxId(), 0}, coin));
    CHECK(coin.isCoinbase);
    CHECK(coin.height == 1);
  }

  test::BeginSection("utxo: add/spend basics");
  {
    CoinsView utxo;
    CHECK(!utxo.HasCoin({kPrev, 0}));
    Fund(utxo, kPrev, 5 * COIN, 3, false);
    CHECK(utxo.HasCoin({kPrev, 0}));
    CHECK(utxo.Size() == 1);
    // Duplicate add fails.
    Coin dup;
    dup.out.value = 5 * COIN;
    dup.height = 3;
    CHECK(!utxo.AddCoin({kPrev, 0}, dup));
    CHECK(utxo.SpendCoin({kPrev, 0}));
    CHECK(!utxo.HasCoin({kPrev, 0}));
    CHECK(!utxo.SpendCoin({kPrev, 0}));  // already spent
  }
}
