// SPDX-License-Identifier: MIT
// Oeeoo Coin — libFuzzer target: JSON parser + serializer
// (Milestone 18). Covers the RPC/Stratum wire format. The
// parser already caps nesting at 64 levels; this target proves
// deep nesting, huge numbers and malformed UTF-8 cannot crash
// it or hang it. Seed corpus: tests/fuzz/corpus/json/.
#include <cstddef>
#include <cstdint>
#include <string>

#include "rpc/json.h"

using namespace oeeoo;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string text(reinterpret_cast<const char*>(data), size);
  Json j;
  std::string error;
  if (Json::Parse(text, j, error)) {
    // Dump must round-trip through Parse again.
    const std::string dumped = j.Dump();
    Json reparsed;
    std::string error2;
    if (!Json::Parse(dumped, reparsed, error2)) {
      __builtin_trap();  // Dump produced invalid JSON: a bug.
    }
  }
  // Also exercise the string-to-number helper on the raw input:
  // it used to throw out of its own catch block on garbage
  // (fixed in Milestone 18, find M18B-R4).
  (void)Json::IntFromString(text);
  return 0;
}
