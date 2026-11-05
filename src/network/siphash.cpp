// SPDX-License-Identifier: MIT
// Oeeoo Coin — SipHash-2-4 implementation.
#include "network/siphash.h"

#include <cstring>

namespace oeeoo {

namespace {
inline uint64_t Rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }

inline void SipRound(uint64_t& v0, uint64_t& v1, uint64_t& v2, uint64_t& v3) {
  v0 += v1;
  v1 = Rotl(v1, 13);
  v1 ^= v0;
  v0 = Rotl(v0, 32);
  v2 += v3;
  v3 = Rotl(v3, 16);
  v3 ^= v2;
  v0 += v3;
  v3 = Rotl(v3, 21);
  v3 ^= v0;
  v2 += v1;
  v1 = Rotl(v1, 17);
  v1 ^= v2;
  v2 = Rotl(v2, 32);
}
}  // namespace

uint64_t SipHash24(uint64_t k0, uint64_t k1,
                   const std::vector<unsigned char>& data) {
  uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
  uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
  uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
  uint64_t v3 = 0x7465646279746573ULL ^ k1;

  const std::size_t len = data.size();
  const unsigned char* p = data.data();
  std::size_t i = 0;
  const uint64_t last = len % 8;
  const uint64_t b = (static_cast<uint64_t>(len & 0xff) << 56);
  uint64_t m = 0;
  while (i + 8 <= len) {
    std::memcpy(&m, p + i, 8);
    v3 ^= m;
    SipRound(v0, v1, v2, v3);
    SipRound(v0, v1, v2, v3);
    v0 ^= m;
    i += 8;
  }
  // Last (partial) block: remaining bytes + length in the top byte.
  uint64_t tail = b;
  for (std::size_t j = 0; j < last; ++j) {
    tail |= static_cast<uint64_t>(p[i + j]) << (8 * j);
  }
  v3 ^= tail;
  SipRound(v0, v1, v2, v3);
  SipRound(v0, v1, v2, v3);
  v0 ^= tail;
  v2 ^= 0xff;
  SipRound(v0, v1, v2, v3);
  SipRound(v0, v1, v2, v3);
  SipRound(v0, v1, v2, v3);
  SipRound(v0, v1, v2, v3);
  return v0 ^ v1 ^ v2 ^ v3;
}

}  // namespace oeeoo
