// SPDX-License-Identifier: MIT
// Oeeoo Coin — transaction validation implementation.
#include "transactions/validation.h"

#include "consensus/consensus.h"
#include "script/script.h"

namespace oeeoo {

namespace {
// Height/time threshold: values below it are block heights, at or
// above it are Unix timestamps (Bitcoin rule).
constexpr uint32_t kLockTimeThreshold = 500000000;
// BIP68 sequence flags.
constexpr uint32_t kSequenceDisableFlag = 0x80000000;
constexpr uint32_t kSequenceTypeFlag = 0x00400000;
constexpr uint32_t kSequenceMask = 0x0000ffff;
constexpr int kSequenceGranularity = 9;  // time locks: 512-second units
}  // namespace

bool IsFinalTx(const Transaction& tx, int height, uint32_t medianTimePast) {
  if (tx.lockTime == 0) return true;
  const int64_t lockTime = tx.lockTime;
  const int64_t threshold =
      lockTime < static_cast<int64_t>(kLockTimeThreshold)
          ? height
          : static_cast<int64_t>(medianTimePast);
  if (lockTime < threshold) return true;
  // Locktime reached only if every input sequence is final.
  for (const TxIn& in : tx.vin) {
    if (in.sequence != 0xffffffff) return false;
  }
  return true;
}

bool CheckSequenceLocks(const Transaction& tx, const CoinsView& utxo,
                        int height, uint32_t medianTimePast,
                        std::string& error) {
  // BIP68 applies only to transactions with version >= 2.
  if (tx.version < 2) return true;
  int minHeight = -1;
  uint32_t minTime = 0;
  for (const TxIn& in : tx.vin) {
    if (in.sequence & kSequenceDisableFlag) continue;
    Coin coin;
    if (!utxo.GetCoin({in.prevTxHash, in.prevOutIndex}, coin)) {
      error = "sequence lock: missing input";
      return false;
    }
    const uint32_t masked = in.sequence & kSequenceMask;
    if (in.sequence & kSequenceTypeFlag) {
      // Time-based: coin's block time + masked * 512 seconds.
      const uint64_t required =
          static_cast<uint64_t>(coin.blockTime) +
          (static_cast<uint64_t>(masked) << kSequenceGranularity);
      if (required > minTime) minTime = static_cast<uint32_t>(required);
    } else {
      // Height-based: coin's height + masked blocks.
      const int required = coin.height + static_cast<int>(masked);
      if (required > minHeight) minHeight = required;
    }
  }
  if (minHeight >= height) {
    error = "sequence lock: height not reached";
    return false;
  }
  if (minTime > medianTimePast) {
    error = "sequence lock: time not reached";
    return false;
  }
  return true;
}

TxValidationResult ValidateTransaction(const Transaction& tx,
                                       const CoinsView& utxo, int height,
                                       uint32_t medianTimePast) {
  TxValidationResult result;
  if (!tx.CheckBasic()) {
    result.error = "structural checks failed";
    return result;
  }
  if (tx.vin.empty()) {
    result.error = "coinbase transactions are validated in Milestone 5";
    return result;
  }
  if (!IsFinalTx(tx, height, medianTimePast)) {
    result.error = "transaction is not final";
    return result;
  }
  std::string seqError;
  if (!CheckSequenceLocks(tx, utxo, height, medianTimePast, seqError)) {
    result.error = seqError;
    return result;
  }
  __int128 totalIn = 0;
  for (const TxIn& in : tx.vin) {
    Coin coin;
    if (!utxo.GetCoin({in.prevTxHash, in.prevOutIndex}, coin)) {
      result.error = "missing input";
      return result;
    }
    // Coinbase maturity: mined rewards wait 100 blocks (reorg
    // protection). The GENESIS coinbase (height 0) is exempt: the
    // genesis block can never be reorged away — it is the chain's
    // identity — and the founder vault it pays must be spendable
    // at launch (Milestone 9).
    if (coin.isCoinbase && coin.height != 0 &&
        height - coin.height < COINBASE_MATURITY) {
      result.error = "coinbase output not yet mature";
      return result;
    }
    totalIn += coin.out.value;
    if (totalIn > MAX_MONEY) {
      result.error = "input total exceeds max money";
      return result;
    }
  }
  __int128 totalOut = 0;
  for (const TxOut& out : tx.vout) {
    totalOut += out.value;
  }
  if (totalIn < totalOut) {
    result.error = "inputs are less than outputs";
    return result;
  }
  for (std::size_t i = 0; i < tx.vin.size(); ++i) {
    Coin coin;
    utxo.GetCoin({tx.vin[i].prevTxHash, tx.vin[i].prevOutIndex}, coin);
    std::string scriptError;
    if (!VerifyScript(tx.vin[i].scriptSig, coin.out.scriptPubKey, tx, i,
                      scriptError)) {
      result.error = "script failed for input " + std::to_string(i) + ": " +
                     scriptError;
      return result;
    }
  }
  result.fee = static_cast<Amount>(totalIn - totalOut);
  result.ok = true;
  return result;
}

bool ConnectBlockTransactions(const Block& block, CoinsView& utxo,
                              int height, uint32_t medianTimePast,
                              uint32_t blockTime, Amount& totalFees,
                              std::string& error,
                              const VestingParams& vesting) {
  CoinsView working = utxo;  // commit only if every tx succeeds
  totalFees = 0;
  if (height == 0 && !ValidateGenesisCoinbase(block, vesting, error)) {
    return false;
  }
  for (std::size_t txIndex = 0; txIndex < block.vtx.size(); ++txIndex) {
    const Transaction& tx = block.vtx[txIndex];
    if (IsCoinbase(tx)) {
      // OOC-001 (M20): only vtx[0] may be a coinbase. A second
      // coinbase is a HARD REJECTION — never a mint, never silently
      // skipped. (Before M20 this branch minted EVERY coinbase tx's
      // outputs with only a CheckFounderSpend check, so a miner
      // could add a second coinbase and mint beyond subsidy + fees.)
      if (txIndex != 0) {
        error = "coinbase transaction at index " +
                std::to_string(txIndex) +
                ": only the first transaction may be a coinbase";
        return false;
      }
      // Coinbase: validated by CheckBlockReward (subsidy + fees
      // cap) before this function runs on every acceptance path.
      // Its outputs enter the UTXO set as coinbase coins. The
      // founder-spend rule still applies (a coinbase paying the
      // founder script is a vault deposit; on a funded chain it
      // always passes).
      if (!CheckFounderSpend(working, tx, vesting, blockTime, error)) {
        return false;
      }
      for (std::size_t i = 0; i < tx.vout.size(); ++i) {
        Coin coin;
        coin.out = tx.vout[i];
        coin.height = height;
        coin.blockTime = blockTime;
        coin.isCoinbase = true;
        if (!working.AddCoin({tx.GetTxId(), static_cast<uint32_t>(i)},
                             coin)) {
          error = "coinbase output already exists";
          return false;
        }
      }
      continue;
    }
    // OOC-001 (M20): belt-and-braces — vtx[0] must be the coinbase.
    // A block whose first transaction is not a coinbase is invalid
    // even if that transaction would validate on its own.
    if (txIndex == 0) {
      error = "first transaction is not a coinbase";
      return false;
    }
    TxValidationResult r =
        ValidateTransaction(tx, working, height, medianTimePast);
    if (!r.ok) {
      error = r.error;
      return false;
    }
    if (!CheckFounderSpend(working, tx, vesting, blockTime, error)) {
      return false;
    }
    totalFees += r.fee;
    for (const TxIn& in : tx.vin) {
      if (!working.SpendCoin({in.prevTxHash, in.prevOutIndex})) {
        error = "failed to spend input (double spend in block?)";
        return false;
      }
    }
    for (std::size_t i = 0; i < tx.vout.size(); ++i) {
      Coin coin;
      coin.out = tx.vout[i];
      coin.height = height;
      coin.blockTime = blockTime;
      coin.isCoinbase = false;
      if (!working.AddCoin({tx.GetTxId(), static_cast<uint32_t>(i)},
                           coin)) {
        error = "output already exists";
        return false;
      }
    }
  }
  utxo = std::move(working);
  return true;
}

bool BuildBlockUndo(const Block& block, const CoinsView& preState, int height,
                    uint32_t blockTime, BlockUndo& undo, std::string& error) {
  undo.spent.clear();
  undo.added.clear();
  for (const Transaction& tx : block.vtx) {
    if (!IsCoinbase(tx)) {
      for (const TxIn& in : tx.vin) {
        const OutPoint op{in.prevTxHash, in.prevOutIndex};
        Coin coin;
        if (!preState.GetCoin(op, coin)) {
          error = "undo record needs a missing input coin";
          return false;
        }
        undo.spent.push_back({op, coin});
      }
    }
    for (std::size_t i = 0; i < tx.vout.size(); ++i) {
      Coin coin;
      coin.out = tx.vout[i];
      coin.height = height;
      coin.blockTime = blockTime;
      coin.isCoinbase = IsCoinbase(tx);
      undo.added.push_back(
          {{tx.GetTxId(), static_cast<uint32_t>(i)}, coin});
    }
  }
  return true;
}

bool DisconnectBlockTransactions(const Block& block, CoinsView& utxo,
                                 const BlockUndo& undo, std::string& error) {
  // Fail-closed: the added coins must still be present with exactly
  // the recorded values, and no spent coin may already exist.
  for (const auto& entry : undo.added) {
    Coin current;
    if (!utxo.GetCoin(entry.first, current)) {
      error = "undo: added coin is missing from the UTXO set";
      return false;
    }
    const Coin& recorded = entry.second;
    if (current.out.value != recorded.out.value ||
        current.out.scriptPubKey != recorded.out.scriptPubKey ||
        current.height != recorded.height ||
        current.blockTime != recorded.blockTime ||
        current.isCoinbase != recorded.isCoinbase) {
      error = "undo: added coin does not match the recorded value";
      return false;
    }
  }
  for (const auto& entry : undo.spent) {
    if (utxo.HasCoin(entry.first)) {
      error = "undo: spent coin already exists in the UTXO set";
      return false;
    }
  }
  CoinsView working = utxo;
  for (const auto& entry : undo.added) {
    if (!working.SpendCoin(entry.first)) {
      error = "undo: failed to remove an added coin";
      return false;
    }
  }
  for (const auto& entry : undo.spent) {
    if (!working.AddCoin(entry.first, entry.second)) {
      error = "undo: failed to restore a spent coin";
      return false;
    }
  }
  // The block's own outputs must not be re-added by the spent list
  // (a block never spends its own outputs), so this is exact.
  (void)block;
  utxo = std::move(working);
  return true;
}

}  // namespace oeeoo
