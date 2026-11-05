// SPDX-License-Identifier: MIT
// Oeeoo Coin — legacy sighash implementation.
#include "transactions/sighash.h"

#include "serialize/endian.h"

namespace oeeoo {

bool LegacySighash(const Transaction& tx, std::size_t nIn,
                   const std::vector<unsigned char>& scriptPubKey,
                   uint32_t hashType, Hash256& out) {
  if (hashType != kSigHashAll) return false;
  if (nIn >= tx.vin.size()) return false;
  Transaction copy = tx;
  for (std::size_t i = 0; i < copy.vin.size(); ++i) {
    copy.vin[i].scriptSig.clear();
  }
  copy.vin[nIn].scriptSig = scriptPubKey;
  std::vector<unsigned char> bytes = copy.Serialize();
  AppendLE32(bytes, hashType);
  out = Sha256d(bytes);
  return true;
}

}  // namespace oeeoo
