// SPDX-License-Identifier: MIT
// Oeeoo Coin — transaction validation (Milestone 4).
// Checks a transaction against the UTXO set: finality (nLockTime),
// BIP68 sequence locks, input existence, coinbase maturity (100
// blocks), value balance (inputs >= outputs, fee), and script
// execution for every input. Coinbase transactions themselves are
// NOT validated here — their construction and reward checks are
// Milestone 5; block connection adds their outputs to the UTXO set
// without validating the coinbase.
#pragma once

#include <cstdint>
#include <string>

#include "blockchain/block.h"
#include "consensus/amount.h"
#include "consensus/vesting.h"
#include "utxo/coins.h"

namespace oeeoo {

// True iff the transaction is final at `height` / `medianTimePast`
// (Bitcoin IsFinalTx rules: lockTime 0, all sequences final, or
// lockTime below the height/time threshold).
bool IsFinalTx(const Transaction& tx, int height, uint32_t medianTimePast);

// BIP68 relative locktimes: every input whose sequence does not have
// the disable flag must have its lock satisfied by `height` /
// `medianTimePast`, measured from the spent coin's creation height /
// block time. False + `error` on violation.
bool CheckSequenceLocks(const Transaction& tx, const CoinsView& utxo,
                        int height, uint32_t medianTimePast,
                        std::string& error);

struct TxValidationResult {
  bool ok = false;
  std::string error;
  Amount fee = 0;  // inputs minus outputs, valid only when ok
};

// Full validation of one non-coinbase transaction against `utxo`.
// `height` is the height of the block the transaction would enter;
// `medianTimePast` is that block's parent-chain MTP.
TxValidationResult ValidateTransaction(const Transaction& tx,
                                       const CoinsView& utxo, int height,
                                       uint32_t medianTimePast);

// Validate and apply every transaction of `block` in order against
// `utxo` (committed only if all succeed). Coinbase transactions are
// not validated; their outputs are added to the set as coinbase
// coins. `blockTime` is the block's timestamp (recorded on new
// coins for BIP68). `totalFees` accumulates the fees of the validated
// transactions. When `vesting` is enabled, the genesis coinbase and
// every founder spend are checked against the vesting rules.
// Returns false + `error` on the first failure.
bool ConnectBlockTransactions(const Block& block, CoinsView& utxo,
                              int height, uint32_t medianTimePast,
                              uint32_t blockTime, Amount& totalFees,
                              std::string& error,
                              const VestingParams& vesting = VestingParams{});

// The undo record for one block: everything ConnectBlockTransactions
// consumed (with values) and created. Disconnecting a block is the
// exact inverse of connecting it, so a reorg can roll the UTXO set
// back to the fork point without re-validating history.
struct BlockUndo {
  // Inputs consumed by non-coinbase transactions, with the coin
  // that was spent (needed to put it back).
  std::vector<std::pair<OutPoint, Coin>> spent;
  // Outputs created by the block (coinbase first), with values.
  std::vector<std::pair<OutPoint, Coin>> added;
};

// Build the undo record for `block` from the UTXO set BEFORE the
// block is connected. False when an input's coin is missing (the
// block cannot be connected from this view at all).
bool BuildBlockUndo(const Block& block, const CoinsView& preState, int height,
                    uint32_t blockTime, BlockUndo& undo, std::string& error);
// Reverse ConnectBlockTransactions for `block` using its undo
// record. Fail-closed: every added coin must still be present with
// the recorded value, and no spent coin may already exist.
bool DisconnectBlockTransactions(const Block& block, CoinsView& utxo,
                                 const BlockUndo& undo, std::string& error);

}  // namespace oeeoo
