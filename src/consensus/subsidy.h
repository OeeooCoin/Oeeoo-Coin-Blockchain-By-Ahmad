// Block subsidy (mining issuance) schedule — consensus-critical.
// Finalized schedule from the master specification:
//   heights 1-210,000       : 50 OEEOO
//   heights 210,001-420,000 : 25 OEEOO
//   heights 420,001-630,000 : 12.5 OEEOO
//   heights 630,001-840,000 : 6.25 OEEOO
//   heights 840,001-940,000 : 3.125 OEEOO
//   heights 940,001+        : 0 OEEOO
// Genesis (height 0) has no subsidy. Total mining issuance is exactly
// 20,000,000 OEEOO; verified by tests/subsidy tests, not by assertion.
#pragma once

#include <cstdint>

#include "amount.h"

namespace oeeoo {

// Subsidy for the block at `height`, in smallest units.
// Returns 0 for height <= 0 (genesis) and for height > 940,000.
Amount GetBlockSubsidy(int height);

// Exact total mining issuance over heights 1..LAST_SUBSIDY_HEIGHT,
// computed by era arithmetic (no per-block loop). Must equal
// MINING_ALLOCATION (20,000,000 OEEOO).
Amount GetTotalMiningIssuance();

}  // namespace oeeoo
