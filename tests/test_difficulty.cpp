// Tests: difficulty — compact target encoding, proof-of-work check,
// retargeting, block proof and chain-work arithmetic.
// All expected values were computed independently in Python (Bitcoin
// compact-target rules) before being embedded here.
#include <cstring>
#include <string>

#include "consensus/difficulty.h"
#include "test_harness.h"

using namespace oeeoo;

namespace {
std::string HexOf(const Target& t) {
  return Uint256::FromBigEndian(t).ToHex();
}

Target TargetFromHex(const std::string& hex) {
  Uint256 v;
  CHECK(Uint256::FromHex(hex, v));
  return v.ToBigEndian();
}

Hash256 HashFromHex(const std::string& hex) {
  Hash256 h{};
  Uint256 v;
  CHECK(Uint256::FromHex(hex, v));
  auto bytes = v.ToBigEndian();
  std::memcpy(h.data(), bytes.data(), 32);
  return h;
}
}  // namespace

void TestDifficulty() {
  test::BeginSection("difficulty bits <-> target vectors");
  struct Vec {
    uint32_t bits;
    const char* target;
  };
  const Vec vecs[] = {
      {0x1d00ffff,
       "00000000ffff0000000000000000000000000000000000000000000000000000"},
      {0x1b0404cb,
       "00000000000404cb000000000000000000000000000000000000000000000000"},
      {0x207fffff,
       "7fffff0000000000000000000000000000000000000000000000000000000000"},
      {0x1c00ffff,
       "0000000000ffff00000000000000000000000000000000000000000000000000"},
  };
  for (const Vec& v : vecs) {
    Target t;
    CHECK(BitsToTarget(v.bits, t));
    CHECK(HexOf(t) == std::string(v.target));
    CHECK(TargetToBits(t) == v.bits);
  }

  test::BeginSection("difficulty invalid bits rejected");
  Target t;
  CHECK(!BitsToTarget(0x1d80ffff, t));  // sign bit set
  CHECK(!BitsToTarget(0x1d000000, t));  // zero mantissa
  CHECK(!BitsToTarget(0x2100ffff, t));  // exponent > 32
  Target zero{};
  CHECK(TargetToBits(zero) == 0);

  test::BeginSection("difficulty proof-of-work check");
  const Target genesisTarget =
      TargetFromHex("00000000ffff000000000000000000000000000000000000000000000"
                    "0000000");
  Hash256 allZero{};
  CHECK(CheckProofOfWork(allZero, 0x1d00ffff));
  Hash256 allFf;
  allFf.fill(0xff);
  CHECK(!CheckProofOfWork(allFf, 0x1d00ffff));
  // hash == target passes; hash == target + 1 fails.
  Hash256 atTarget = HashFromHex(HexOf(genesisTarget));
  CHECK(CheckProofOfWork(atTarget, 0x1d00ffff));
  Uint256 plusOne = Uint256::FromBigEndian(genesisTarget);
  Uint256 one;
  one.d[0] = 1;
  plusOne.Add(one);
  Hash256 aboveTarget = HashFromHex(plusOne.ToHex());
  CHECK(!CheckProofOfWork(aboveTarget, 0x1d00ffff));
  CHECK(!CheckProofOfWork(allZero, 0x1d80ffff));  // invalid bits

  test::BeginSection("difficulty retarget (mainnet limit 0x1d00ffff)");
  const Target limitTarget = PowLimitTarget();
  CHECK(HexOf(limitTarget) ==
        std::string("00000000ffff00000000000000000000000000000000000000000000"
                    "00000000"));
  auto retarget = [&](uint32_t bits, int64_t actual) {
    Target last;
    CHECK(BitsToTarget(bits, last));
    return CalculateNextWorkRequired(last, actual);
  };
  CHECK(retarget(0x1d00ffff, 1209600) == 0x1d00ffff);  // exact: unchanged
  CHECK(retarget(0x1d00ffff, 604800) == 0x1c7fff80);   // 2x fast: halve
  CHECK(retarget(0x1d00ffff, 302400) == 0x1c3fffc0);   // 4x fast: quarter
  CHECK(retarget(0x1d00ffff, 100000) == 0x1c3fffc0);   // clamped to 4x
  CHECK(retarget(0x1d00ffff, 4838400) == 0x1d00ffff);  // 4x slow: limit
  CHECK(retarget(0x1d00ffff, 10000000) == 0x1d00ffff); // clamped to 4x
  CHECK(retarget(0x1d00ffff, 2419200) == 0x1d00ffff);  // 2x slow: limit

  test::BeginSection("difficulty retarget (test limit 0x207fffff)");
  auto retargetEasy = [&](uint32_t bits, int64_t actual) {
    Target last;
    CHECK(BitsToTarget(bits, last));
    return CalculateNextWorkRequired(last, actual, 0x207fffff);
  };
  CHECK(retargetEasy(0x207fffff, 1209600) == 0x207fffff);
  CHECK(retargetEasy(0x207fffff, 604800) == 0x203fffff);
  CHECK(retargetEasy(0x207fffff, 302400) == 0x201fffff);
  CHECK(retargetEasy(0x207fffff, 100000) == 0x201fffff);  // clamped
  CHECK(retargetEasy(0x207fffff, 4838400) == 0x207fffff); // clamped to limit

  test::BeginSection("difficulty block proof + chain work");
  CHECK(HexOf(ComputeBlockProof(0x1d00ffff)) ==
        std::string("00000000000000000000000000000000000000000000000000000001"
                    "00010001"));
  CHECK(HexOf(ComputeBlockProof(0x207fffff)) ==
        std::string("00000000000000000000000000000000000000000000000000000000"
                    "00000002"));
  // 6 easy blocks = 6 * 2 = 12 work.
  Target work{};
  for (int i = 0; i < 6; ++i) {
    work = AddChainWork(work, ComputeBlockProof(0x207fffff));
  }
  CHECK(HexOf(work) ==
        std::string("00000000000000000000000000000000000000000000000000000000"
                    "0000000c"));

  test::BeginSection("uint256 divide");
  Uint256 num;
  num.d[0] = 100;
  Uint256 den;
  den.d[0] = 7;
  Uint256 rem;
  Uint256 quot = Uint256::Divide(num, den, &rem);
  CHECK(quot.d[0] == 14 && quot.d[1] == 0);
  CHECK(rem.d[0] == 2);
  // (2^256 - 1) / 2 = 2^255 - 1
  Uint256 max = Uint256Max();
  Uint256 two;
  two.d[0] = 2;
  Uint256 half = Uint256::Divide(max, two);
  CHECK(half.d[7] == 0x7fffffffu && half.d[0] == 0xffffffffu);
}
