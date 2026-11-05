// Integer money type for Oeeoo Coin.
// All monetary values are int64 counts of the smallest unit
// (1 OEEOO = 100,000,000 units). Floating point is forbidden
// for money anywhere in the codebase.
#pragma once

#include <cstdint>

#include "consensus.h"

namespace oeeoo {

using Amount = int64_t;  // smallest units (satoshis-equivalent)

// True iff the amount is a legal money value: 0 <= amount <= MAX_MONEY.
inline bool MoneyRange(Amount amount) {
  return amount >= 0 && amount <= MAX_MONEY;
}

// Convert a whole-OEEOO count to smallest units, with overflow check.
// Returns false (and leaves `out` untouched) if the result would not
// fit in a legal money value. No floating point involved.
inline bool OeeooToUnits(int64_t oeeoo, Amount& out) {
  __int128 v = static_cast<__int128>(oeeoo) * COIN;
  if (v < 0 || v > MAX_MONEY) return false;
  out = static_cast<Amount>(v);
  return true;
}

// Overflow-checked addition for money. Returns false on overflow or
// if the result leaves the legal money range.
inline bool AddMoney(Amount a, Amount b, Amount& out) {
  __int128 v = static_cast<__int128>(a) + static_cast<__int128>(b);
  if (v < 0 || v > MAX_MONEY) return false;
  out = static_cast<Amount>(v);
  return true;
}

}  // namespace oeeoo
