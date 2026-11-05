// SPDX-License-Identifier: MIT
// Oeeoo Coin — founder vesting (Milestone 9, spec section 6).
// 1,000,000 OEEOO in 10 tranches of 100,000: 100,000 at launch, then
// 100,000 at each calendar anniversary, 1,000,000 total by the 9th
// anniversary. Leap-year rule: a Feb 29 launch anniversary falls on
// Feb 28 in non-leap years.
// Enforcement design (protocol-level, reorg-safe):
//  - The genesis block pays exactly FOUNDER_ALLOCATION to the
//    founder's 2-of-3 multisig script (one UTXO, the "vault").
//  - A transaction may move founder coins only while the vault
//    keeps at least the UNVESTED amount: after the transaction,
//    vaultBalance >= FOUNDER_ALLOCATION - vested(blockTime).
//    The check reads only the UTXO set + the block time, so a
//    reorg that removes a founder spend restores the vault.
//  - The rule applies only to transactions that touch founder
//    coins; ordinary transactions are unaffected, and a chain whose
//    genesis never funded the vault is not bricked by it.
// The founder script and launch time are chain parameters set at
// launch (the founder's real keys never enter code, logs, or tests
// outside throwaway test keys). Until they are set, vesting is
// disabled and nothing is enforced.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "blockchain/block.h"
#include "consensus/consensus.h"
#include "transactions/transaction.h"
#include "utxo/coins.h"

namespace oeeoo {

struct VestingParams {
  bool enabled = false;
  uint32_t launchTime = 0;  // genesis block time (UTC seconds)
  std::vector<unsigned char> founderScript;  // 2-of-3 multisig scriptPubKey
};

constexpr int64_t kVestingTranche = 100000LL * COIN;
constexpr int kVestingTranches = 10;

// --- UTC calendar (Howard Hinnant's days-from-civil algorithm) ---
struct CivilDate {
  int year = 1970;
  unsigned month = 1;
  unsigned day = 1;
};
bool IsLeapYear(int year);
// Days since 1970-01-01 for a civil date (proleptic Gregorian).
int64_t DaysFromCivil(int year, unsigned month, unsigned day);
// Civil date for days since 1970-01-01.
CivilDate CivilFromDays(int64_t days);
// Timestamp of the `years`-th anniversary of `launchTime`
// (same month/day/time-of-day; Feb 29 -> Feb 28 in non-leap years).
uint32_t AnniversaryTime(uint32_t launchTime, int years);

// Amount of the founder allocation vested at `time`.
// 100,000 at launch, +100,000 per anniversary, capped at 1,000,000.
// Returns FOUNDER_ALLOCATION when vesting is disabled (no restriction).
Amount VestedAmount(const VestingParams& params, uint32_t time);

// Total value of UTXOs paying to the founder script.
Amount FounderVaultBalance(const CoinsView& utxo,
                           const VestingParams& params);

// The founder-spend rule (see the header comment). `utxo` is the
// view BEFORE `tx` applies; `blockTime` is the including block's
// timestamp. Returns false + `error` on a violation.
bool CheckFounderSpend(const CoinsView& utxo, const Transaction& tx,
                       const VestingParams& params, uint32_t blockTime,
                       std::string& error);

// Genesis rule: with vesting enabled, the height-0 coinbase must be
// exactly one output paying FOUNDER_ALLOCATION to the founder script,
// and the block time must equal the launch time (when set).
bool ValidateGenesisCoinbase(const Block& block,
                             const VestingParams& params, std::string& error);

}  // namespace oeeoo
