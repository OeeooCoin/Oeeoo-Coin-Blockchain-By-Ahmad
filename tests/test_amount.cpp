// Tests: integer money type and consensus money constants.
#include "consensus/amount.h"
#include "consensus/consensus.h"
#include "test_harness.h"

using namespace oeeoo;

void TestAmount() {
  test::BeginSection("amount / money constants");
  CHECK(COIN == 100000000LL);
  CHECK(MAX_MONEY == 21000000LL * COIN);
  CHECK(MAX_MONEY == 2100000000000000LL);
  CHECK(MINING_ALLOCATION == 20000000LL * COIN);
  CHECK(FOUNDER_ALLOCATION == 1000000LL * COIN);
  CHECK(MINING_ALLOCATION + FOUNDER_ALLOCATION == MAX_MONEY);
  CHECK(INITIAL_BLOCK_REWARD == 50 * COIN);

  CHECK(MoneyRange(0));
  CHECK(MoneyRange(1));
  CHECK(MoneyRange(MAX_MONEY));
  CHECK(!MoneyRange(-1));
  CHECK(!MoneyRange(MAX_MONEY + 1));

  Amount out = -1;
  CHECK(OeeooToUnits(1, out) && out == COIN);
  CHECK(OeeooToUnits(21000000, out) && out == MAX_MONEY);
  CHECK(OeeooToUnits(0, out) && out == 0);
  CHECK(!OeeooToUnits(21000001, out));  // would exceed max supply
  CHECK(!OeeooToUnits(-1, out));

  Amount sum = -1;
  CHECK(AddMoney(MAX_MONEY - 1, 1, sum) && sum == MAX_MONEY);
  CHECK(!AddMoney(MAX_MONEY, 1, sum));  // overflow past max supply
  CHECK(!AddMoney(MAX_MONEY, MAX_MONEY, sum));
}
