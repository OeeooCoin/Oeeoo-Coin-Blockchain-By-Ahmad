#include "consensus/subsidy.h"

#include "consensus/consensus.h"

namespace oeeoo {

Amount GetBlockSubsidy(int height) {
  if (height <= 0) return 0;  // Genesis has no mining subsidy.
  if (height <= 210000) return 50 * COIN;              // 5,000,000,000
  if (height <= 420000) return 25 * COIN;              // 2,500,000,000
  if (height <= 630000) return 1250000000LL;           // 12.5 OEEOO
  if (height <= 840000) return 625000000LL;            // 6.25 OEEOO
  if (height <= LAST_SUBSIDY_HEIGHT) return 312500000LL;  // 3.125 OEEOO
  return 0;  // Block 940,001 onward: fees only.
}

Amount GetTotalMiningIssuance() {
  // Era arithmetic with 128-bit intermediates (overflow-proof):
  // 210,000*50 + 210,000*25 + 210,000*12.5 + 210,000*6.25
  //   + 100,000*3.125  (last era is only 100,000 blocks)
  // = 10,500,000 + 5,250,000 + 2,625,000 + 1,312,500 + 312,500
  // = 20,000,000 OEEOO.
  const __int128 total_oeeoo = (__int128)210000 * 50 + (__int128)210000 * 25 +
                               (__int128)210000 * 125 / 10 +
                               (__int128)210000 * 625 / 100 +
                               (__int128)100000 * 3125 / 1000;
  const __int128 total_units = total_oeeoo * COIN;
  return static_cast<Amount>(total_units);  // 2,000,000,000,000,000
}

}  // namespace oeeoo
