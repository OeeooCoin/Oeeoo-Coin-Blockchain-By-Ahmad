// SPDX-License-Identifier: MIT
// Oeeoo Coin — full chain re-validation (Milestone 11).
// Replays the stored chain from the genesis block and checks every
// rule a live node checks: headers (parent, MTP, difficulty bits,
// proof of work, cumulative work), every transaction (validation,
// coinbase reward, founder vesting), and the rebuilt UTXO set
// against the store's UTXO table. A pruned history cannot be fully
// re-validated (block bodies are required) and fails loudly.
// This is the audit-prep counterpart of the light checks in
// Node::LoadFromStore (header validation + tip match only).
#pragma once

#include <cstdint>
#include <string>

#include "consensus/vesting.h"
#include "crypto/sha256.h"
#include "db/chainstore.h"

namespace oeeoo {

// Returns false + `error` on the first bad block, on a pruned
// history, or on any mismatch with the store. `tipOut` is the
// verified tip hash on success. `vesting` should be the chain's
// current parameters (with the launch time adopted, as the node
// does when it accepts the genesis block).
bool VerifyChainFromStore(ChainStore& store, const VestingParams& vesting,
                          uint32_t powLimitBits, Hash256& tipOut,
                          std::string& error);

}  // namespace oeeoo
