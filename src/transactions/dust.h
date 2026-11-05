// SPDX-License-Identifier: MIT
// Oeeoo Coin — dynamic, script-aware dust threshold (Milestone 13).
// An output is dust when it costs more to spend than it is worth:
//   threshold = (serialized output size + typical input size for
//   this script type) * min relay fee rate.
// The input size depends on the script type (a P2PKH input is
// ~148 bytes; a bare 2-of-3 multisig input is ~191 bytes), so the
// threshold is computed per script, not hardcoded. This replaces
// the wallet's provisional 546-sat rule.
#pragma once

#include <cstdint>
#include <vector>

#include "consensus/amount.h"

namespace oeeoo {

// Dust threshold in satoshis for an output paying to
// `scriptPubKey`, at `minRelayFeeSatPerVb` sat/vB.
Amount GetDustThreshold(const std::vector<unsigned char>& scriptPubKey,
                        Amount minRelayFeeSatPerVb);
// True when `value` is below the dust threshold for this script.
bool IsDust(Amount value, const std::vector<unsigned char>& scriptPubKey,
            Amount minRelayFeeSatPerVb);

}  // namespace oeeoo
