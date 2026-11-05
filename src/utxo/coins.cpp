// SPDX-License-Identifier: MIT
// Oeeoo Coin — UTXO set implementation.
#include "utxo/coins.h"

namespace oeeoo {

bool CoinsView::AddCoin(const OutPoint& outpoint, const Coin& coin) {
  return coins_.emplace(outpoint, coin).second;
}

bool CoinsView::SpendCoin(const OutPoint& outpoint) {
  return coins_.erase(outpoint) == 1;
}

bool CoinsView::GetCoin(const OutPoint& outpoint, Coin& coin) const {
  auto it = coins_.find(outpoint);
  if (it == coins_.end()) return false;
  coin = it->second;
  return true;
}

bool CoinsView::HasCoin(const OutPoint& outpoint) const {
  return coins_.find(outpoint) != coins_.end();
}

void CoinsView::GetAllCoins(
    std::vector<std::pair<OutPoint, Coin>>& out) const {
  out.clear();
  out.reserve(coins_.size());
  for (const auto& kv : coins_) out.push_back(kv);
}

}  // namespace oeeoo
