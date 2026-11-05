// SPDX-License-Identifier: MIT
// Oeeoo Coin — Genesis block parameters (Milestone 3: structure only).
// The Genesis block is NOT finalized and NOT generated yet (Milestone 12).
// These parameters describe its shape; `finalized` stays false until the
// launch timestamp is chosen and the genesis hash is computed and
// independently verified. Nothing here may be presented as a real
// genesis hash.
#pragma once

#include <cstdint>
#include <string>

#include "consensus/difficulty.h"  // kGenesisBits lives there

namespace oeeoo {

// Genesis message — appears ONLY in the Genesis block (spec section 5.1).
inline const char* kGenesisMessage = "THINK ABOUT GOD ? IS ONLY ONE ALLAH";

// Genesis block height and version (spec sections 3.3, 5.1).
constexpr int kGenesisHeight = 0;
constexpr int32_t kGenesisVersion = 1;

// Genesis difficulty candidate (spec section 4.4, to verify):
// kGenesisBits = 0x1d00ffff (see consensus/difficulty.h).

struct GenesisParams {
  int32_t version = kGenesisVersion;
  // Actual UTC launch time, chosen at final launch. 0 = not chosen yet.
  uint32_t time = 0;
  uint32_t bits = kGenesisBits;
  uint32_t nonce = 0;
  std::string message = kGenesisMessage;
  bool finalized = false;

  // A genesis is usable only once its timestamp is set and it is marked
  // finalized after independent verification of its hash.
  bool IsUsable() const { return finalized && time != 0; }
};

}  // namespace oeeoo
