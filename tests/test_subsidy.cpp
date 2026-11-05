// Tests: block subsidy schedule — every boundary from the spec,
// plus the exact 20,000,000 OEEOO total issuance, verified two ways:
// era arithmetic (GetTotalMiningIssuance) and a per-block sum.
#include "consensus/subsidy.h"
#include "test_harness.h"

using namespace oeeoo;

void TestSubsidy() {
  test::BeginSection("subsidy boundaries");
  // Genesis and pre-genesis: no subsidy.
  CHECK(GetBlockSubsidy(0) == 0);
  CHECK(GetBlockSubsidy(-1) == 0);

  // Era 1: 50 OEEOO.
  CHECK(GetBlockSubsidy(1) == 5000000000LL);
  CHECK(GetBlockSubsidy(210000) == 5000000000LL);
  // Era 2: 25 OEEOO.
  CHECK(GetBlockSubsidy(210001) == 2500000000LL);
  CHECK(GetBlockSubsidy(420000) == 2500000000LL);
  // Era 3: 12.5 OEEOO.
  CHECK(GetBlockSubsidy(420001) == 1250000000LL);
  CHECK(GetBlockSubsidy(630000) == 1250000000LL);
  // Era 4: 6.25 OEEOO.
  CHECK(GetBlockSubsidy(630001) == 625000000LL);
  CHECK(GetBlockSubsidy(840000) == 625000000LL);
  // Era 5 (short, 100,000 blocks): 3.125 OEEOO.
  CHECK(GetBlockSubsidy(840001) == 312500000LL);
  CHECK(GetBlockSubsidy(940000) == 312500000LL);
  // Subsidy ends: fees only from 940,001.
  CHECK(GetBlockSubsidy(940001) == 0);
  CHECK(GetBlockSubsidy(1000000) == 0);
  CHECK(GetBlockSubsidy(10000000) == 0);

  test::BeginSection("subsidy total issuance = 20,000,000 OEEOO");
  CHECK(GetTotalMiningIssuance() == MINING_ALLOCATION);
  CHECK(GetTotalMiningIssuance() == 2000000000000000LL);

  // Independent check: sum every block 1..940,000 with 128-bit math.
  __int128 sum = 0;
  for (int h = 1; h <= LAST_SUBSIDY_HEIGHT; ++h) {
    sum += GetBlockSubsidy(h);
  }
  CHECK(sum == (__int128)MINING_ALLOCATION);
  // And one block past the end adds nothing.
  CHECK(sum + GetBlockSubsidy(940001) == (__int128)MINING_ALLOCATION);
}
