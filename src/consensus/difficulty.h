// SPDX-License-Identifier: MIT
// Oeeoo Coin — difficulty: compact target encoding, proof-of-work check,
// retargeting and per-block proof (chain work).
//
// Conventions (Bitcoin-style, documented here so every implementation
// agrees):
//  - A target is a 32-byte BIG-ENDIAN number: byte 0 is most significant.
//  - A block hash (SHA-256d output bytes, as produced) is compared as a
//    big-endian number against the target: proof of work holds iff
//    hash <= target.
//  - Compact "bits": byte 3 (most significant) is the exponent (size in
//    bytes), the low 3 bytes are the mantissa. Identical to Bitcoin's
//    nBits encoding.
#pragma once

#include <array>
#include <cstdint>

#include "consensus/uint256.h"
#include "crypto/sha256.h"

namespace oeeoo {

// A 256-bit target / chain-work value, big-endian bytes.
using Target = std::array<unsigned char, 32>;

// The proof-of-work limit target, from GENESIS_BITS (spec section 4.4:
// nBits = 0x1d00ffff, candidate to verify). No valid block may have a
// target above this.
Target PowLimitTarget();
constexpr uint32_t kGenesisBits = 0x1d00ffff;

// Decode compact bits into a target. Returns false for an invalid
// encoding: sign bit set, zero mantissa, or exponent > 32.
bool BitsToTarget(uint32_t bits, Target& target);
// Encode a target into compact bits (Bitcoin GetCompact rules). The
// zero target encodes as 0.
uint32_t TargetToBits(const Target& target);

// True iff hash (as a big-endian number) <= target implied by bits.
bool CheckProofOfWork(const Hash256& hash, uint32_t bits);

// Bitcoin-style retarget: given the previous period's target and the
// actual timespan of the last 2016 blocks, compute the next compact
// target. The timespan is clamped to [302400, 4838400] seconds
// (0.25x..4x of the 1,209,600-second target timespan) and the result is
// clamped to the proof-of-work limit. `powLimitBits` is the chain's
// limit (default: the mainnet genesis bits); tests may pass an easier
// limit so blocks can be mined quickly.
uint32_t CalculateNextWorkRequired(const Target& lastTarget,
                                   int64_t actualTimespanSeconds,
                                   uint32_t powLimitBits = kGenesisBits);

// Per-block proof of work, Bitcoin GetBlockProof:
//   (2^256 - 1 - target) / (target + 1) + 1
// as a big-endian 256-bit number. Chain work is the sum of proofs.
Target ComputeBlockProof(uint32_t bits);
// Chain-work addition (modulo 2^256; overflow is unreachable in
// practice — it would need ~2^256 units of work).
Target AddChainWork(const Target& a, const Target& b);

}  // namespace oeeoo
