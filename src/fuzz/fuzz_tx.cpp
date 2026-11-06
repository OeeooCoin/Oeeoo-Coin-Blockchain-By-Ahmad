// SPDX-License-Identifier: MIT
// Oeeoo Coin — libFuzzer target: transaction deserialization +
// validation (Milestone 18). Seed corpus: tests/fuzz/corpus/tx/.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "consensus/consensus.h"
#include "fuzz_common.h"
#include "transactions/transaction.h"
#include "transactions/validation.h"
#include "utxo/coins.h"

using namespace oeeoo;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::vector<unsigned char> bytes(data, data + size);
  Transaction tx;
  if (Transaction::Deserialize(bytes, tx)) {
    // Round-trip: a parsed transaction must serialize back to
    // the same bytes (canonical encoding; no trailing garbage
    // may survive a parse).
    const std::vector<unsigned char> reserialized = tx.Serialize();
    if (reserialized != bytes) {
      // Non-canonical encodings are rejected by Deserialize, so
      // reaching here with different bytes is a bug. libFuzzer
      // treats a non-zero abort as a crash; use __builtin_trap
      // so the input is kept as a crashing artifact.
      __builtin_trap();
    }
    (void)tx.GetTxId();
    CoinsView utxo;
    (void)ValidateTransaction(tx, utxo, 10, 1700000000);
  }
  return 0;
}
