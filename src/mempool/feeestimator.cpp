// SPDX-License-Identifier: MIT
// Oeeoo Coin — dynamic fee estimator implementation.
#include "mempool/feeestimator.h"

#include <algorithm>

namespace oeeoo {

void FeeEstimator::RecordBlock(int height,
                               const std::vector<ConfirmedTx>& txs) {
  if (txs.empty()) return;
  // Heights must be recorded in order; an out-of-order call is
  // ignored rather than corrupting the window.
  if (!heights_.empty() && height <= heights_.back()) return;
  heights_.push_back(height);
  counts_.push_back(txs.size());
  for (const ConfirmedTx& tx : txs) confirmed_.push_back(tx);
  // Keep only the last 24 blocks of data.
  while (!heights_.empty() &&
         heights_.back() - heights_.front() >= kWindowBlocks) {
    const std::size_t drop = counts_.front();
    counts_.pop_front();
    heights_.pop_front();
    for (std::size_t i = 0; i < drop && !confirmed_.empty(); ++i) {
      confirmed_.pop_front();
    }
  }
}

Amount FeeEstimator::EstimateFeeRate() const {
  if (confirmed_.empty()) return 1;
  std::vector<ConfirmedTx> sorted(confirmed_.begin(), confirmed_.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const ConfirmedTx& a, const ConfirmedTx& b) {
              const __int128 left =
                  static_cast<__int128>(a.fee) * b.size;
              const __int128 right =
                  static_cast<__int128>(b.fee) * a.size;
              if (left != right) return left < right;
              return a.fee < b.fee;
            });
  const ConfirmedTx& median = sorted[sorted.size() / 2];
  // Ceiling division: the rate in sat/vB, at least 1.
  const Amount rate = static_cast<Amount>(
      (static_cast<__int128>(median.fee) + median.size - 1) / median.size);
  return rate < 1 ? 1 : rate;
}

}  // namespace oeeoo
