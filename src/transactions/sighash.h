// SPDX-License-Identifier: MIT
// Oeeoo Coin — legacy transaction signature hash (Milestone 4).
// Only SIGHASH_ALL is supported in this milestone (the standard for
// P2PKH and bare multisig at launch). Other hash types are rejected
// by the script interpreter rather than silently computed.
#pragma once

#include <cstdint>
#include <vector>

#include "crypto/sha256.h"
#include "transactions/transaction.h"

namespace oeeoo {

// SIGHASH_ALL = 1: sign all inputs and all outputs.
constexpr uint32_t kSigHashAll = 1;

// Legacy sighash for input `nIn` of `tx`, signing against
// `scriptPubKey` (the script of the output being spent):
//   - copy the transaction, blank every scriptSig,
//   - set input nIn's scriptSig to scriptPubKey,
//   - serialize, append hashType as int32 LE, SHA256d.
// Returns false for an out-of-range input or an unsupported hash type.
bool LegacySighash(const Transaction& tx, std::size_t nIn,
                   const std::vector<unsigned char>& scriptPubKey,
                   uint32_t hashType, Hash256& out);

}  // namespace oeeoo
