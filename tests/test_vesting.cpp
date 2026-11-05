// Tests: founder vesting (Milestone 9) — calendar math, schedule,
// founder-spend rule, genesis founder output, 2-of-3 multisig.
#include <memory>
#include <string>
#include <vector>

#include "blockchain/chain.h"
#include "consensus/consensus.h"
#include "consensus/vesting.h"
#include "crypto/ecdsa.h"
#include "mining/miner.h"
#include "network/node.h"
#include "network/transport.h"
#include "script/script.h"
#include "test_harness.h"
#include "transactions/sighash.h"
#include "transactions/validation.h"
#include "utxo/coins.h"

using namespace oeeoo;

namespace {
constexpr uint32_t kTestBits = 0x207fffff;
// Launch = 2026-11-06 00:00:00 UTC; the node's clock sits 10 minutes
// after launch, so "now" is still inside the first vesting tranche.
constexpr uint32_t kLaunch = 1793923200;
constexpr uint32_t kNow = 1793923200 + 600;

std::vector<KeyPair> g_founderKeys;
std::vector<unsigned char> g_founderScript;
bool g_founderReady = false;

void EnsureFounder() {
  if (g_founderReady) return;
  for (int i = 0; i < 3; ++i) {
    KeyPair kp;
    CHECK(GenerateKeyPair(kp));
    g_founderKeys.push_back(kp);
  }
  std::vector<std::vector<unsigned char>> pubs;
  for (const KeyPair& kp : g_founderKeys) pubs.push_back(kp.pubkey);
  g_founderScript = CreateMultisigScriptPubKey(2, pubs);
  g_founderReady = true;
}

VestingParams Params(uint32_t launch = kLaunch) {
  VestingParams p;
  p.enabled = true;
  p.launchTime = launch;
  p.founderScript = g_founderScript;
  return p;
}

Block MineVestingGenesis(uint32_t time = kLaunch) {
  Block genesis;
  genesis.header.nVersion = 1;
  genesis.header.nTime = time;
  genesis.header.nBits = kTestBits;
  genesis.vtx = {CreateCoinbase(0, g_founderScript, FOUNDER_ALLOCATION,
                                "THINK ABOUT GOD?")};
  genesis.header.hashMerkleRoot = genesis.ComputeMerkleRoot();
  CHECK(MineBlock(genesis, 0, "THINK ABOUT GOD?"));
  return genesis;
}

// A founder spend: spends the vault UTXO (`vaultTxid`, `vaultIndex`,
// `vaultValue`), pays `amount` to `destScript`, returns the change
// to the founder script. `sigCount` of the 3 founder keys sign
// (2 = valid, 1 = invalid).
Transaction MakeFounderSpend(const Hash256& vaultTxid, uint32_t vaultIndex,
                             Amount vaultValue, Amount amount,
                             const std::vector<unsigned char>& destScript,
                             int sigCount, Amount fee = 1000) {
  Transaction tx;
  tx.version = 1;
  TxIn in;
  in.prevTxHash = vaultTxid;
  in.prevOutIndex = vaultIndex;
  in.sequence = 0xffffffff;
  tx.vin.push_back(in);
  TxOut payment;
  payment.value = amount;
  payment.scriptPubKey = destScript;
  tx.vout.push_back(payment);
  TxOut change;
  change.value = vaultValue - amount - fee;
  change.scriptPubKey = g_founderScript;
  tx.vout.push_back(change);
  tx.lockTime = 0;
  Hash256 sighash;
  CHECK(LegacySighash(tx, 0, g_founderScript, kSigHashAll, sighash));
  std::vector<std::vector<unsigned char>> sigs;
  for (int i = 0; i < sigCount; ++i) {
    std::vector<unsigned char> sig = SignHash(sighash, g_founderKeys[i].privkey);
    sig.push_back(static_cast<unsigned char>(kSigHashAll));
    sigs.push_back(sig);
  }
  tx.vin[0].scriptSig = CreateMultisigScriptSig(sigs);
  return tx;
}

std::vector<unsigned char> DestScript() {
  KeyPair dest;
  CHECK(GenerateKeyPair(dest));
  return CreateP2PKHScriptPubKey(PubKeyHash160(dest.pubkey));
}

// Mine an empty block on `prev` at `height`/`time` and return it.
Block MineEmptyBlock(const Hash256& prev, int height, uint32_t time,
                     uint32_t medianTimePast, const Mempool& pool,
                     CoinsView& utxo) {
  BlockTemplate templ = AssembleBlock(prev, height, time, kTestBits, pool,
                                      utxo, medianTimePast, DestScript());
  Block block = templ.block;
  CHECK(MineBlock(block, height, ""));
  return block;
}
}  // namespace

void TestVesting() {
  EnsureFounder();

  test::BeginSection("vesting: calendar math");
  {
    CHECK(IsLeapYear(2000));
    CHECK(!IsLeapYear(1900));
    CHECK(IsLeapYear(2024));
    CHECK(!IsLeapYear(2100));
    CHECK(IsLeapYear(2028));
    CHECK(!IsLeapYear(2023));
    // Known day counts (verified against Python datetime).
    CHECK(DaysFromCivil(1970, 1, 1) == 0);
    CHECK(DaysFromCivil(2000, 2, 29) == 11016);
    CHECK(DaysFromCivil(2026, 11, 6) == 20763);
    CHECK(DaysFromCivil(1999, 12, 31) == 10956);
    // Round-trip over a spread of dates.
    const int64_t days[] = {0, 1, -1, 11016, 20763, 47481, 10956, 25569};
    for (int64_t d : days) {
      CivilDate date = CivilFromDays(d);
      CHECK(DaysFromCivil(date.year, date.month, date.day) == d);
    }
    CivilDate d = CivilFromDays(11016);
    CHECK(d.year == 2000 && d.month == 2 && d.day == 29);
    d = CivilFromDays(20763);
    CHECK(d.year == 2026 && d.month == 11 && d.day == 6);
    // Anniversaries of 2026-11-06 00:00 UTC (verified in Python).
    const uint32_t launch = 1793923200;
    const uint32_t expected[] = {1793923200, 1825459200, 1857081600,
                                 1888617600, 1920153600, 1951689600,
                                 1983312000, 2014848000, 2046384000,
                                 2077920000};
    for (int k = 0; k < 10; ++k) {
      CHECK(AnniversaryTime(launch, k) == expected[k]);
    }
    // Feb 29 launch: anniversaries fall on Feb 28 in non-leap years.
    const uint32_t launch29 = 1709208000;  // 2024-02-29 12:00 UTC
    const uint32_t expected29[] = {1709208000, 1740744000, 1772280000,
                                   1803816000, 1835438400, 1866974400};
    for (int k = 0; k < 6; ++k) {
      CHECK(AnniversaryTime(launch29, k) == expected29[k]);
    }
  }

  test::BeginSection("vesting: schedule");
  {
    VestingParams p = Params(1793923200);  // 2026-11-06
    CHECK(VestedAmount(p, 1793923199) == 0);  // 1s before launch
    CHECK(VestedAmount(p, 1793923200) == 100000 * COIN);
    CHECK(VestedAmount(p, 1825459199) == 100000 * COIN);
    CHECK(VestedAmount(p, 1825459200) == 200000 * COIN);  // 1st anniversary
    CHECK(VestedAmount(p, 1951689600) == 600000 * COIN);  // 5th
    CHECK(VestedAmount(p, 2077920000) == 1000000 * COIN);  // 9th
    CHECK(VestedAmount(p, 2077920000u + 86400u * 365u * 5u) ==
          1000000 * COIN);  // capped
    // Disabled: no restriction.
    VestingParams off;
    CHECK(VestedAmount(off, 0) == FOUNDER_ALLOCATION);
    // Feb 29 launch: 1st anniversary is 2025-02-28.
    VestingParams p29 = Params(1709208000);
    CHECK(VestedAmount(p29, 1740743999) == 100000 * COIN);
    CHECK(VestedAmount(p29, 1740744000) == 200000 * COIN);
  }

  test::BeginSection("vesting: genesis founder output + spend rule");
  {
    auto node = std::make_unique<Node>(kTestBits);
    node->SetTime(kNow);
    node->SetTransport(std::make_shared<LoopbackTransport>("loopback:vest"));
    node->SetVestingParams(Params());
    std::string error;
    Block genesis = MineVestingGenesis();
    CHECK(node->BroadcastBlock(genesis, error));
    CHECK(node->TipHeight() == 0);
    CHECK(FounderVaultBalance(node->Utxo(), Params()) == FOUNDER_ALLOCATION);
    const Hash256 vaultTxid = genesis.vtx[0].GetTxId();
    const std::vector<unsigned char> dest = DestScript();
    // Over-spend at launch (150k > 100k vested) is rejected.
    Transaction over = MakeFounderSpend(vaultTxid, 0, FOUNDER_ALLOCATION,
                                        150000 * COIN, dest, 2);
    CHECK(!node->BroadcastTx(over, error));
    CHECK(error.find("vested") != std::string::npos);
    // A block containing the over-spend is rejected too.
    {
      Mempool pool;
      CHECK(pool.AddTransaction(over, node->Utxo(), 1,
                                node->Chain().GetMedianTimePast(), kNow,
                                error, Params()) == false);
    }
    // 1-of-3 signatures: vesting would allow 50k, but the script fails.
    Transaction oneSig = MakeFounderSpend(vaultTxid, 0, FOUNDER_ALLOCATION,
                                          50000 * COIN, dest, 1);
    CHECK(!node->BroadcastTx(oneSig, error));
    CHECK(error.find("script") != std::string::npos);
    // Valid spend at launch: 99,999 + 1000 sats fee = 99,999.00001
    // out of the vault, inside the vested 100k. (The fee counts:
    // spending exactly 100k + a fee would exceed the vested amount
    // by the fee — the rule is honest about that.)
    Transaction ok = MakeFounderSpend(vaultTxid, 0, FOUNDER_ALLOCATION,
                                      99999 * COIN, dest, 2);
    CHECK(node->BroadcastTx(ok, error));
    Block b1 = MineEmptyBlock(genesis.GetHash(), 1, kLaunch + 600, kLaunch,
                              node->MempoolRef(), node->Utxo());
    CHECK(b1.vtx.size() == 2);  // coinbase + founder spend
    CHECK(node->BroadcastBlock(b1, error));
    CHECK(node->TipHeight() == 1);
    CHECK(FounderVaultBalance(node->Utxo(), Params()) ==
          900001 * COIN - 1000);
    // The change UTXO is the vault now (900k - 1000 sats).
    const Hash256 changeTxid = ok.GetTxId();
    Coin changeCoin;
    CHECK(node->Utxo().GetCoin({changeTxid, 1}, changeCoin));
    const Amount changeValue = changeCoin.out.value;
    // Another 50k before the 1st anniversary: vault would fall below
    // the 900k floor. Rejected.
    Transaction early = MakeFounderSpend(changeTxid, 1, changeValue,
                                         50000 * COIN, dest, 2);
    CHECK(!node->BroadcastTx(early, error));
    CHECK(error.find("vested") != std::string::npos);
    // Past the 1st anniversary (vested 200k, spent 99,999.00001):
    // a further 100k spend is allowed (vault stays above the 800k
    // floor).
    const uint32_t anniv1 = AnniversaryTime(kLaunch, 1);
    node->SetTime(anniv1 + 100);
    Transaction later = MakeFounderSpend(changeTxid, 1, changeValue,
                                         100000 * COIN, dest, 2);
    CHECK(node->BroadcastTx(later, error));
    Block b2 = MineEmptyBlock(b1.GetHash(), 2, anniv1 + 60, kLaunch + 600,
                              node->MempoolRef(), node->Utxo());
    CHECK(node->BroadcastBlock(b2, error));
    CHECK(node->TipHeight() == 2);
    CHECK(FounderVaultBalance(node->Utxo(), Params()) ==
          800001 * COIN - 2000);
  }

  test::BeginSection("vesting: wrong genesis is rejected, disabled is fine");
  {
    // Genesis paying a P2PKH address instead of the founder script.
    auto node = std::make_unique<Node>(kTestBits);
    node->SetTime(kNow);
    node->SetTransport(std::make_shared<LoopbackTransport>("loopback:vest2"));
    node->SetVestingParams(Params());
    std::string error;
    KeyPair dest;
    CHECK(GenerateKeyPair(dest));
    Block bad;
    bad.header.nVersion = 1;
    bad.header.nTime = kLaunch;
    bad.header.nBits = kTestBits;
    bad.vtx = {CreateCoinbase(
        0, CreateP2PKHScriptPubKey(PubKeyHash160(dest.pubkey)),
        FOUNDER_ALLOCATION, "THINK ABOUT GOD?")};
    bad.header.hashMerkleRoot = bad.ComputeMerkleRoot();
    CHECK(MineBlock(bad, 0, "THINK ABOUT GOD?"));
    CHECK(!node->BroadcastBlock(bad, error));
    // Genesis paying the right script but the wrong amount.
    Block badAmount;
    badAmount.header.nVersion = 1;
    badAmount.header.nTime = kLaunch;
    badAmount.header.nBits = kTestBits;
    badAmount.vtx = {CreateCoinbase(0, g_founderScript, 500000 * COIN,
                                    "THINK ABOUT GOD?")};
    badAmount.header.hashMerkleRoot = badAmount.ComputeMerkleRoot();
    CHECK(MineBlock(badAmount, 0, "THINK ABOUT GOD?"));
    CHECK(!node->BroadcastBlock(badAmount, error));
    // Genesis with a different timestamp than the launch time.
    Block badTime;
    badTime.header.nVersion = 1;
    badTime.header.nTime = kLaunch + 60;
    badTime.header.nBits = kTestBits;
    badTime.vtx = {CreateCoinbase(0, g_founderScript, FOUNDER_ALLOCATION,
                                  "THINK ABOUT GOD?")};
    badTime.header.hashMerkleRoot = badTime.ComputeMerkleRoot();
    CHECK(MineBlock(badTime, 0, "THINK ABOUT GOD?"));
    CHECK(!node->BroadcastBlock(badTime, error));
    // Vesting disabled: the old zero-value genesis still works.
    auto plain = std::make_unique<Node>(kTestBits);
    plain->SetTime(kNow);
    plain->SetTransport(
        std::make_shared<LoopbackTransport>("loopback:vest3"));
    Block genesis;
    genesis.header.nVersion = 1;
    genesis.header.nTime = kLaunch;
    genesis.header.nBits = kTestBits;
    KeyPair k;
    CHECK(GenerateKeyPair(k));
    genesis.vtx = {CreateCoinbase(
        0, CreateP2PKHScriptPubKey(PubKeyHash160(k.pubkey)), 0,
        "THINK ABOUT GOD?")};
    genesis.header.hashMerkleRoot = genesis.ComputeMerkleRoot();
    CHECK(MineBlock(genesis, 0, "THINK ABOUT GOD?"));
    CHECK(plain->BroadcastBlock(genesis, error));
    CHECK(plain->TipHeight() == 0);
  }
}
