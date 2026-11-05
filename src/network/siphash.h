// SPDX-License-Identifier: MIT
// Oeeoo Coin — SipHash-2-4 (used for compact-block short transaction
// IDs, BIP152-style). Reference implementation of the public-domain
// SipHash algorithm by Aumasson and Bernstein; no external code.
#pragma once

#include <cstdint>
#include <vector>

namespace oeeoo {

// SipHash-2-4 of `data` with a 128-bit key (k0, k1). Returns the
// 64-bit hash.
uint64_t SipHash24(uint64_t k0, uint64_t k1,
                   const std::vector<unsigned char>& data);

}  // namespace oeeoo
