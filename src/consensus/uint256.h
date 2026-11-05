// SPDX-License-Identifier: MIT
// Oeeoo Coin — minimal 256-bit unsigned integer.
// Used for difficulty targets and cumulative chain work. Little-endian
// uint32 limbs. No external dependency (Boost is not available on the
// build VM). Operations are constant-size and deterministic.
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace oeeoo {

// 256-bit unsigned integer, limbs little-endian: d[0] is least significant.
struct Uint256 {
  uint32_t d[8] = {0, 0, 0, 0, 0, 0, 0, 0};

  bool IsZero() const;
  // -1 if this < other, 0 if equal, +1 if this > other.
  int Compare(const Uint256& other) const;
  bool operator==(const Uint256& other) const { return Compare(other) == 0; }
  bool operator<(const Uint256& other) const { return Compare(other) < 0; }
  bool operator>(const Uint256& other) const { return Compare(other) > 0; }

  // this += other. Returns false if the sum overflowed past 256 bits
  // (the stored value wraps modulo 2^256).
  bool Add(const Uint256& other);
  // Requires this >= other. this -= other.
  void Subtract(const Uint256& other);
  // this *= m (m is a 32-bit multiplier). Returns the carry out of the
  // top limb (0 when the product fits in 256 bits).
  uint64_t MultiplyUint32(uint32_t m);
  // this /= d. Returns the remainder. d must be non-zero.
  uint32_t DivideUint32(uint32_t d);
  // this = num / den, sets remainder = num % den. den must be non-zero.
  // Binary long division, exact.
  static Uint256 Divide(const Uint256& num, const Uint256& den,
                        Uint256* remainder = nullptr);

  // Big-endian 32-byte form (byte 0 = most significant), as used for
  // targets, hashes and chain-work values on the wire / in hex.
  static Uint256 FromBigEndian(const std::array<unsigned char, 32>& bytes);
  std::array<unsigned char, 32> ToBigEndian() const;
  std::string ToHex() const;  // 64 lowercase hex chars, big-endian.
  static bool FromHex(const std::string& hex, Uint256& out);
};

// All-ones value: 2^256 - 1.
Uint256 Uint256Max();
}  // namespace oeeoo
