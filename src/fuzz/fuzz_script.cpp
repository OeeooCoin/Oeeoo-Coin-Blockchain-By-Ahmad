// SPDX-License-Identifier: MIT
// Oeeoo Coin — libFuzzer target: script interpreter
// (Milestone 18). The input is used as the scriptSig against a
// fixed valid P2PKH scriptPubKey, and as the scriptPubKey against
// a fixed scriptSig, so both execution directions are covered.
// Seed corpus: tests/fuzz/corpus/script/.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "fuzz_common.h"
#include "script/script.h"
#include "transactions/transaction.h"

using namespace oeeoo;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::vector<unsigned char> script(data, data + size);
  Transaction tx = fuzz::ValidTx();
  std::string error;
  // Input as scriptSig, valid P2PKH scriptPubKey.
  (void)VerifyScript(script, fuzz::FuzzSpk(), tx, 0, error);
  // Input as scriptPubKey, valid P2PKH scriptSig.
  (void)VerifyScript(tx.vin[0].scriptSig, script, tx, 0, error);
  // Input as both sides (self-referential scripts).
  (void)VerifyScript(script, script, tx, 0, error);
  return 0;
}
