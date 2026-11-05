// SPDX-License-Identifier: MIT
// Oeeoo Coin — difficulty implementation.
#include "consensus/difficulty.h"

#include <cstring>

#include "consensus/consensus.h"

namespace oeeoo {

Target PowLimitTarget() {
  Target t{};
  bool ok = BitsToTarget(kGenesisBits, t);
  (void)ok;  // 0x1d00ffff is a valid encoding by construction.
  return t;
}

bool BitsToTarget(uint32_t bits, Target& target) {
  target.fill(0);
  const uint32_t exponent = bits >> 24;
  const uint32_t mantissa = bits & 0x007fffff;
  // Sign bit (0x00800000) means a negative target: invalid.
  if (bits & 0x00800000) return false;
  if (mantissa == 0) return false;
  if (exponent > 32) return false;
  if (exponent <= 3) {
    // value = mantissa >> 8*(3-exponent), fits in the last bytes.
    uint32_t value = mantissa >> (8 * (3 - exponent));
    target[29] = static_cast<unsigned char>((value >> 16) & 0xff);
    target[30] = static_cast<unsigned char>((value >> 8) & 0xff);
    target[31] = static_cast<unsigned char>(value & 0xff);
  } else {
    // Mantissa's 3 bytes start at byte (32 - exponent).
    const size_t start = 32 - exponent;
    target[start] = static_cast<unsigned char>((mantissa >> 16) & 0xff);
    target[start + 1] = static_cast<unsigned char>((mantissa >> 8) & 0xff);
    target[start + 2] = static_cast<unsigned char>(mantissa & 0xff);
  }
  return true;
}

uint32_t TargetToBits(const Target& target) {
  // Find the first non-zero byte.
  size_t first = 0;
  while (first < 32 && target[first] == 0) ++first;
  if (first == 32) return 0;  // zero target
  size_t size = 32 - first;
  uint32_t mantissa;
  if (size <= 3) {
    mantissa = 0;
    for (size_t i = first; i < 32; ++i) mantissa = (mantissa << 8) | target[i];
    mantissa <<= 8 * (3 - size);
  } else {
    mantissa = (static_cast<uint32_t>(target[first]) << 16) |
               (static_cast<uint32_t>(target[first + 1]) << 8) |
               static_cast<uint32_t>(target[first + 2]);
  }
  if (mantissa & 0x00800000) {
    mantissa >>= 8;
    ++size;
  }
  return static_cast<uint32_t>((size << 24) | mantissa);
}

bool CheckProofOfWork(const Hash256& hash, uint32_t bits) {
  Target target;
  if (!BitsToTarget(bits, target)) return false;
  // Both are big-endian: plain lexicographic compare is numeric compare.
  return std::memcmp(hash.data(), target.data(), 32) <= 0;
}

uint32_t CalculateNextWorkRequired(const Target& lastTarget,
                                   int64_t actualTimespanSeconds,
                                   uint32_t powLimitBits) {
  // Clamp the timespan to [0.25x, 4x] of the 2,016-block target span.
  const int64_t targetTimespan = static_cast<int64_t>(DIFFICULTY_INTERVAL) *
                                 TARGET_BLOCK_TIME_SECONDS;  // 1,209,600
  if (actualTimespanSeconds < targetTimespan / 4) {
    actualTimespanSeconds = targetTimespan / 4;  // 302,400
  } else if (actualTimespanSeconds > targetTimespan * 4) {
    actualTimespanSeconds = targetTimespan * 4;  // 4,838,400
  }
  // newTarget = lastTarget * actual / targetTimespan (exact integer
  // math). Direct multiplication can overflow 256 bits when the target
  // is near 2^255, so decompose: target = q*T + r, then
  // newTarget = q*actual + (r*actual)/T. Both terms fit in 256 bits;
  // if q*actual still overflows, the result exceeds 2^256 and hence
  // the proof-of-work limit, so it clamps to the limit.
  Uint256 value = Uint256::FromBigEndian(lastTarget);
  const uint32_t timespan = static_cast<uint32_t>(targetTimespan);
  const uint32_t actual = static_cast<uint32_t>(actualTimespanSeconds);
  const uint32_t remainder = value.DivideUint32(timespan);  // value = q
  Target powLimit;
  if (!BitsToTarget(powLimitBits, powLimit)) return powLimitBits;
  if (value.MultiplyUint32(actual) != 0) return powLimitBits;
  Uint256 remTerm;
  remTerm.d[0] = remainder;
  remTerm.MultiplyUint32(actual);
  remTerm.DivideUint32(timespan);
  value.Add(remTerm);
  Target newTarget = value.ToBigEndian();
  if (Uint256::FromBigEndian(newTarget) >
      Uint256::FromBigEndian(powLimit)) {
    return powLimitBits;
  }
  return TargetToBits(newTarget);
}

Target ComputeBlockProof(uint32_t bits) {
  Target target;
  if (!BitsToTarget(bits, target)) {
    Target zero{};
    return zero;
  }
  Uint256 t = Uint256::FromBigEndian(target);
  Uint256 numerator = Uint256Max();
  numerator.Subtract(t);              // 2^256 - 1 - target
  Uint256 one;
  one.d[0] = 1;
  Uint256 denominator = t;
  denominator.Add(one);               // target + 1
  Uint256 proof = Uint256::Divide(numerator, denominator);
  proof.Add(one);                     // + 1
  return proof.ToBigEndian();
}

Target AddChainWork(const Target& a, const Target& b) {
  Uint256 x = Uint256::FromBigEndian(a);
  Uint256 y = Uint256::FromBigEndian(b);
  x.Add(y);  // overflow wraps mod 2^256 (unreachable in practice)
  return x.ToBigEndian();
}

}  // namespace oeeoo
