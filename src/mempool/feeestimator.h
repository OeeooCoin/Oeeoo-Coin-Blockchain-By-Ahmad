// SPDX-License-Identifier: MIT
// Oeeoo Coin — dynamic fee estimator (Milestone 13).
// Records the fee rates of transactions confirmed in each block
// and estimates the rate a new transaction needs: the median fee
// rate of the last 24 blocks' confirmed transactions (spec
// section 10.2). With no data, the estimate is the minimum relay
// fee (1 sat/vB). All comparisons are exact integer arithmetic.
#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "consensus/amount.h"

namespace oeeoo {

class FeeEstimator {
 public:
  // One confirmed transaction's fee and size.
  struct ConfirmedTx {
    Amount fee = 0;
    std::size_t size = 0;
  };

  // Record the transactions confirmed in the block at `height`.
  void RecordBlock(int height, const std::vector<ConfirmedTx>& txs);
  // Median fee rate (sat/vB) over the last 24 blocks of data.
  // Returns 1 (the minimum relay fee) when there is no data.
  Amount EstimateFeeRate() const;
  // Number of blocks of data currently held (at most 24).
  int BlocksRecorded() const { return static_cast<int>(heights_.size()); }

 private:
  static constexpr int kWindowBlocks = 24;
  std::deque<int> heights_;            // heights with data, ascending
  std::deque<std::size_t> counts_;     // confirmed txs per height
  std::deque<ConfirmedTx> confirmed_;  // all recorded txs, in order
};

}  // namespace oeeoo
