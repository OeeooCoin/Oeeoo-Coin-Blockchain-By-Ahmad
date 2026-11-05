// SPDX-License-Identifier: MIT
// Oeeoo Coin — founder vesting implementation.
#include "consensus/vesting.h"

#include "transactions/transaction.h"

namespace oeeoo {

bool IsLeapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int64_t DaysFromCivil(int year, unsigned month, unsigned day) {
  // Howard Hinnant's days_from_civil (public domain algorithm).
  const int y = (month <= 2) ? year - 1 : year;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) -
         719468;
}

CivilDate CivilFromDays(int64_t days) {
  // Howard Hinnant's civil_from_days (public domain algorithm).
  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe =
      static_cast<unsigned>(z - era * 146097);  // [0, 146096]
  const unsigned yoe =
      (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;  // [0, 11]
  const unsigned day = doy - (153 * mp + 2) / 5 + 1;
  const unsigned month = mp < 10 ? mp + 3 : mp - 9;
  CivilDate out;
  out.year = static_cast<int>(y + (month <= 2 ? 1 : 0));
  out.month = month;
  out.day = day;
  return out;
}

uint32_t AnniversaryTime(uint32_t launchTime, int years) {
  const int64_t days = static_cast<int64_t>(launchTime) / 86400;
  const int64_t secondsOfDay = static_cast<int64_t>(launchTime) % 86400;
  const CivilDate launch = CivilFromDays(days);
  int targetYear = launch.year + years;
  unsigned targetDay = launch.day;
  if (launch.month == 2 && launch.day == 29 &&
      !IsLeapYear(targetYear)) {
    targetDay = 28;  // Feb 29 -> Feb 28 in non-leap years
  }
  const int64_t targetDays =
      DaysFromCivil(targetYear, launch.month, targetDay);
  const int64_t targetSeconds = targetDays * 86400 + secondsOfDay;
  // A launch near the uint32_t limit (year ~2106) would wrap the
  // anniversary past 2^32 seconds; clamp it so the tranche vests at
  // the latest representable time instead of an earlier wrong one.
  if (targetSeconds > static_cast<int64_t>(UINT32_MAX)) {
    return UINT32_MAX;
  }
  return static_cast<uint32_t>(targetSeconds);
}

Amount VestedAmount(const VestingParams& params, uint32_t time) {
  if (!params.enabled) return FOUNDER_ALLOCATION;
  if (time < params.launchTime) return 0;
  int tranches = 0;
  for (int k = 0; k < kVestingTranches; ++k) {
    if (time >= AnniversaryTime(params.launchTime, k)) {
      ++tranches;
    } else {
      break;
    }
  }
  const Amount vested = static_cast<Amount>(tranches) * kVestingTranche;
  return vested > FOUNDER_ALLOCATION ? FOUNDER_ALLOCATION : vested;
}

Amount FounderVaultBalance(const CoinsView& utxo,
                           const VestingParams& params) {
  if (!params.enabled || params.founderScript.empty()) return 0;
  std::vector<std::pair<OutPoint, Coin>> coins;
  utxo.GetAllCoins(coins);
  Amount total = 0;
  for (const auto& entry : coins) {
    if (entry.second.out.scriptPubKey == params.founderScript) {
      total += entry.second.out.value;
    }
  }
  return total;
}

bool CheckFounderSpend(const CoinsView& utxo, const Transaction& tx,
                       const VestingParams& params, uint32_t blockTime,
                       std::string& error) {
  if (!params.enabled || params.founderScript.empty()) return true;
  Amount founderIn = 0;
  bool touchesFounder = false;
  for (const TxIn& in : tx.vin) {
    Coin coin;
    if (!utxo.GetCoin({in.prevTxHash, in.prevOutIndex}, coin)) {
      continue;  // missing inputs fail validation elsewhere
    }
    if (coin.out.scriptPubKey == params.founderScript) {
      touchesFounder = true;
      founderIn += coin.out.value;
    }
  }
  Amount founderOut = 0;
  for (const TxOut& out : tx.vout) {
    if (out.scriptPubKey == params.founderScript) {
      touchesFounder = true;
      founderOut += out.value;
    }
  }
  if (!touchesFounder) return true;
  const Amount before = FounderVaultBalance(utxo, params);
  // A chain whose vault was never funded must not be bricked by a
  // donation to the founder script alone: if the vault is empty
  // and this tx only ADDS founder coins, there is nothing to protect.
  if (before == 0 && founderIn == 0) return true;
  const Amount vested = VestedAmount(params, blockTime);
  const Amount floor = FOUNDER_ALLOCATION - vested;
  // after = before - founderIn + founderOut (all non-negative here:
  // founderIn <= before because the inputs are part of the vault).
  const Amount after = before - founderIn + founderOut;
  if (after < floor) {
    error = "founder spend exceeds the vested amount (vault would fall "
            "below the unvested balance)";
    return false;
  }
  return true;
}

bool ValidateGenesisCoinbase(const Block& block,
                             const VestingParams& params, std::string& error) {
  if (!params.enabled) return true;
  if (block.vtx.empty() || !IsCoinbase(block.vtx[0])) {
    error = "genesis block has no coinbase";
    return false;
  }
  if (params.launchTime != 0 && block.header.nTime != params.launchTime) {
    error = "genesis timestamp does not match the vesting launch time";
    return false;
  }
  const Transaction& coinbase = block.vtx[0];
  if (coinbase.vout.size() != 1) {
    error = "genesis coinbase must have exactly one output (founder vault)";
    return false;
  }
  if (coinbase.vout[0].value != FOUNDER_ALLOCATION) {
    error = "genesis coinbase must pay exactly the founder allocation";
    return false;
  }
  if (coinbase.vout[0].scriptPubKey != params.founderScript) {
    error = "genesis coinbase must pay the founder multisig script";
    return false;
  }
  return true;
}

}  // namespace oeeoo
