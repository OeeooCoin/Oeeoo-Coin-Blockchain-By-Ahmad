// SPDX-License-Identifier: MIT
// Oeeoo Coin — Uint256 implementation.
#include "consensus/uint256.h"

#include <cstring>

namespace oeeoo {

bool Uint256::IsZero() const {
  for (int i = 0; i < 8; ++i) {
    if (d[i] != 0) return false;
  }
  return true;
}

int Uint256::Compare(const Uint256& other) const {
  for (int i = 7; i >= 0; --i) {
    if (d[i] < other.d[i]) return -1;
    if (d[i] > other.d[i]) return 1;
  }
  return 0;
}

bool Uint256::Add(const Uint256& other) {
  uint64_t carry = 0;
  for (int i = 0; i < 8; ++i) {
    uint64_t sum = static_cast<uint64_t>(d[i]) + other.d[i] + carry;
    d[i] = static_cast<uint32_t>(sum);
    carry = sum >> 32;
  }
  return carry != 0;
}

void Uint256::Subtract(const Uint256& other) {
  int64_t borrow = 0;
  for (int i = 0; i < 8; ++i) {
    int64_t diff = static_cast<int64_t>(d[i]) - other.d[i] - borrow;
    if (diff < 0) {
      d[i] = static_cast<uint32_t>(diff + (static_cast<int64_t>(1) << 32));
      borrow = 1;
    } else {
      d[i] = static_cast<uint32_t>(diff);
      borrow = 0;
    }
  }
}

uint64_t Uint256::MultiplyUint32(uint32_t m) {
  uint64_t carry = 0;
  for (int i = 0; i < 8; ++i) {
    uint64_t prod = static_cast<uint64_t>(d[i]) * m + carry;
    d[i] = static_cast<uint32_t>(prod);
    carry = prod >> 32;
  }
  return carry;
}

uint32_t Uint256::DivideUint32(uint32_t divisor) {
  uint64_t rem = 0;
  for (int i = 7; i >= 0; --i) {
    uint64_t cur = (rem << 32) | d[i];
    d[i] = static_cast<uint32_t>(cur / divisor);
    rem = cur % divisor;
  }
  return static_cast<uint32_t>(rem);
}

namespace {
// Bit i (0 = least significant) of a limb array.
bool GetBit(const Uint256& v, int i) { return (v.d[i / 32] >> (i % 32)) & 1u; }
void SetBit(Uint256& v, int i) { v.d[i / 32] |= (1u << (i % 32)); }
void ShiftLeftOne(Uint256& v) {
  uint32_t carry = 0;
  for (int i = 0; i < 8; ++i) {
    uint32_t next = v.d[i] >> 31;
    v.d[i] = (v.d[i] << 1) | carry;
    carry = next;
  }
}
}  // namespace

Uint256 Uint256::Divide(const Uint256& num, const Uint256& den,
                        Uint256* remainder) {
  Uint256 quotient;
  Uint256 rem;
  for (int i = 255; i >= 0; --i) {
    ShiftLeftOne(rem);
    if (GetBit(num, i)) rem.d[0] |= 1u;
    if (rem.Compare(den) >= 0) {
      rem.Subtract(den);
      SetBit(quotient, i);
    }
  }
  if (remainder) *remainder = rem;
  return quotient;
}

Uint256 Uint256::FromBigEndian(const std::array<unsigned char, 32>& bytes) {
  Uint256 v;
  for (int i = 0; i < 8; ++i) {
    // Limb 7 holds the most significant bytes (bytes[0..3]).
    uint32_t limb = 0;
    for (int j = 0; j < 4; ++j) {
      limb = (limb << 8) | bytes[(7 - i) * 4 + j];
    }
    v.d[i] = limb;
  }
  return v;
}

std::array<unsigned char, 32> Uint256::ToBigEndian() const {
  std::array<unsigned char, 32> bytes{};
  for (int i = 0; i < 8; ++i) {
    uint32_t limb = d[i];
    for (int j = 3; j >= 0; --j) {
      bytes[(7 - i) * 4 + j] = static_cast<unsigned char>(limb & 0xff);
      limb >>= 8;
    }
  }
  return bytes;
}

std::string Uint256::ToHex() const {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  auto bytes = ToBigEndian();
  for (unsigned char b : bytes) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0f]);
  }
  return out;
}

bool Uint256::FromHex(const std::string& hex, Uint256& out) {
  if (hex.size() != 64) return false;
  std::array<unsigned char, 32> bytes{};
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < 32; ++i) {
    int hi = nibble(hex[2 * i]);
    int lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    bytes[i] = static_cast<unsigned char>((hi << 4) | lo);
  }
  out = FromBigEndian(bytes);
  return true;
}

Uint256 Uint256Max() {
  Uint256 v;
  for (int i = 0; i < 8; ++i) v.d[i] = 0xffffffffu;
  return v;
}

}  // namespace oeeoo
