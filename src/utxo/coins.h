// SPDX-License-Identifier: MIT
// Oeeoo Coin — in-memory UTXO set (Milestone 4).
// Maps each unspent output (txid + index) to its coin: the output
// itself, the height and block time of the block that created it, and
// whether it came from a coinbase. Persistence is Milestone 7; this
// view lives only in memory.
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "crypto/sha256.h"
#include "transactions/transaction.h"

namespace oeeoo {

struct OutPoint {
  Hash256 txid{};
  uint32_t index = 0;
  bool operator==(const OutPoint& other) const {
    return txid == other.txid && index == other.index;
  }
};

struct OutPointHash {
  std::size_t operator()(const OutPoint& o) const {
    // FNV-1a over the txid bytes plus the index.
    std::size_t h = 1469598103934665603ULL;
    for (unsigned char b : o.txid) {
      h ^= b;
      h *= 1099511628211ULL;
    }
    h ^= o.index;
    h *= 1099511628211ULL;
    return h;
  }
};

struct Coin {
  TxOut out;
  int height = -1;             // height of the creating block
  uint32_t blockTime = 0;      // timestamp of the creating block
  bool isCoinbase = false;
};

class CoinsView {
 public:
  // Add a coin. False if the outpoint already exists.
  bool AddCoin(const OutPoint& outpoint, const Coin& coin);
  // Spend (remove) a coin. False if it does not exist.
  bool SpendCoin(const OutPoint& outpoint);
  bool GetCoin(const OutPoint& outpoint, Coin& coin) const;
  bool HasCoin(const OutPoint& outpoint) const;
  std::size_t Size() const { return coins_.size(); }
  // Every coin in the view (for wallet scanning).
  void GetAllCoins(std::vector<std::pair<OutPoint, Coin>>& out) const;

 private:
  std::unordered_map<OutPoint, Coin, OutPointHash> coins_;
};

}  // namespace oeeoo
