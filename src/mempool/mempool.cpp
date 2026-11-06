// SPDX-License-Identifier: MIT
// Oeeoo Coin — mempool implementation.
#include "mempool/mempool.h"

#include <algorithm>

#include "consensus/consensus.h"
#include "transactions/dust.h"
#include "transactions/validation.h"

namespace oeeoo {

namespace {
constexpr Amount kMinRelayFeeSatPerVb = MIN_RELAY_FEE_SAT_VB;  // 1
constexpr std::size_t kMaxReplacementCount = 100;  // BIP125 rule 5
constexpr uint32_t kRbfSignalSequence = 0xfffffffe;  // below this = signals

std::string InputKey(const Hash256& txid, uint32_t index) {
  std::string key(reinterpret_cast<const char*>(txid.data()), txid.size());
  key.append(reinterpret_cast<const char*>(&index), sizeof(index));
  return key;
}

// True when the transaction signals opt-in replaceability: any
// input with nSequence below 0xfffffffe (BIP125).
bool SignalsRbf(const Transaction& tx) {
  for (const TxIn& in : tx.vin) {
    if (in.sequence < kRbfSignalSequence) return true;
  }
  return false;
}

// Exact fee-rate comparison: a.fee/a.size > b.fee/b.size.
bool FeeRateHigher(Amount feeA, std::size_t sizeA, Amount feeB,
                   std::size_t sizeB) {
  return static_cast<__int128>(feeA) * sizeB >
         static_cast<__int128>(feeB) * sizeA;
}
}  // namespace

std::vector<Hash256> Mempool::FindConflicts(
    const Transaction& tx) const {
  std::vector<Hash256> out;
  for (const TxIn& in : tx.vin) {
    for (const auto& kv : entries_) {
      for (const TxIn& otherIn : kv.second.tx.vin) {
        if (otherIn.prevTxHash == in.prevTxHash &&
            otherIn.prevOutIndex == in.prevOutIndex) {
          if (std::find(out.begin(), out.end(), kv.first) == out.end()) {
            out.push_back(kv.first);
          }
        }
      }
    }
  }
  return out;
}

std::vector<Hash256> Mempool::FindDescendants(
    const std::vector<Hash256>& roots) const {
  std::vector<Hash256> out = roots;
  std::unordered_set<Hash256, Hash256Hash> seen(out.begin(), out.end());
  std::size_t i = 0;
  while (i < out.size()) {
    const Hash256 parent = out[i++];
    for (const auto& kv : entries_) {
      if (seen.count(kv.first) != 0) continue;
      for (const TxIn& in : kv.second.tx.vin) {
        if (in.prevTxHash == parent) {
          seen.insert(kv.first);
          out.push_back(kv.first);
          break;
        }
      }
    }
  }
  return out;
}

bool Mempool::EvictFor(std::size_t needed, Amount newFee,
                       std::size_t newSize,
                       const std::unordered_set<Hash256, Hash256Hash>& keep) {
  std::size_t freed = 0;
  while (freed < needed) {
    // Lowest fee rate first (excluding `keep`); ties broken by the
    // largest txid for determinism.
    const Entry* lowest = nullptr;
    Hash256 lowestId;
    for (const auto& kv : entries_) {
      if (keep.count(kv.first) != 0) continue;
      if (lowest == nullptr ||
          FeeRateHigher(lowest->fee, lowest->size, kv.second.fee,
                        kv.second.size) ||
          (!FeeRateHigher(kv.second.fee, kv.second.size, lowest->fee,
                          lowest->size) &&
           kv.first > lowestId)) {
        lowest = &kv.second;
        lowestId = kv.first;
      }
    }
    if (lowest == nullptr) return false;
    // A worse tx never evicts a better one.
    if (!FeeRateHigher(newFee, newSize, lowest->fee, lowest->size)) {
      return false;
    }
    freed += lowest->size;
    Remove(lowestId);
  }
  return true;
}

bool Mempool::AddTransaction(const Transaction& tx, const CoinsView& utxo,
                             int height, uint32_t medianTimePast,
                             uint32_t nowSeconds, std::string& error,
                             const VestingParams& vesting) {
  return AddTransactionInternal(tx, utxo, height, medianTimePast,
                                nowSeconds, nowSeconds, error, vesting);
}

bool Mempool::AddLoadedTransaction(const Transaction& tx,
                                   const CoinsView& utxo, int height,
                                   uint32_t medianTimePast,
                                   uint32_t addedTime, uint32_t nowSeconds,
                                   std::string& error,
                                   const VestingParams& vesting) {
  return AddTransactionInternal(tx, utxo, height, medianTimePast,
                                nowSeconds, addedTime, error, vesting);
}

bool Mempool::AddTransactionInternal(const Transaction& tx,
                                     const CoinsView& utxo, int height,
                                     uint32_t medianTimePast,
                                     uint32_t nowSeconds, uint32_t entryTime,
                                     std::string& error,
                                     const VestingParams& vesting) {
  if (tx.vin.empty()) {
    error = "coinbase transactions do not enter the mempool";
    return false;
  }
  const Hash256 txid = tx.GetTxId();
  if (Contains(txid)) {
    error = "transaction already in mempool";
    return false;
  }
  TxValidationResult r = ValidateTransaction(tx, utxo, height, medianTimePast);
  if (!r.ok) {
    error = r.error;
    return false;
  }
  if (!CheckFounderSpend(utxo, tx, vesting, nowSeconds, error)) {
    return false;
  }
  const std::size_t size = tx.Serialize().size();
  // Minimum relay fee: 1 sat/vB (spec section 10.1).
  if (static_cast<__int128>(r.fee) * 1000 <
      static_cast<__int128>(kMinRelayFeeSatPerVb) * size) {
    error = "fee below minimum relay fee (1 sat/vB)";
    return false;
  }
  // Dust: no output may be below its dynamic, script-aware
  // threshold (spec section 10.3).
  for (const TxOut& out : tx.vout) {
    if (IsDust(out.value, out.scriptPubKey, kMinRelayFeeSatPerVb)) {
      error = "dust output below the dynamic threshold";
      return false;
    }
  }
  // Conflicts: either a plain rejection, or an opt-in RBF
  // replacement of the conflicting entries and their descendants.
  std::vector<Hash256> replaced;
  const std::vector<Hash256> conflicts = FindConflicts(tx);
  if (!conflicts.empty()) {
    for (const Hash256& id : conflicts) {
      const auto it = entries_.find(id);
      if (it == entries_.end() || !SignalsRbf(it->second.tx)) {
        error = "input already spent by a mempool transaction";
        return false;
      }
    }
    replaced = FindDescendants(conflicts);
    if (replaced.size() > kMaxReplacementCount) {
      error = "replacement would replace too many transactions";
      return false;
    }
    // No new unconfirmed inputs: every input must be confirmed or
    // an input of a replaced transaction.
    for (const TxIn& in : tx.vin) {
      if (Contains(in.prevTxHash) &&
          std::find(replaced.begin(), replaced.end(), in.prevTxHash) ==
              replaced.end()) {
        error = "replacement adds a new unconfirmed input";
        return false;
      }
    }
    // The replacement must pay a higher absolute fee than the
    // replaced set, and a higher fee rate than each replaced entry.
    __int128 replacedFees = 0;
    std::size_t replacedBytes = 0;
    for (const Hash256& id : replaced) {
      const auto it = entries_.find(id);
      if (it == entries_.end()) continue;
      replacedFees += it->second.fee;
      replacedBytes += it->second.size;
      if (!FeeRateHigher(r.fee, size, it->second.fee, it->second.size)) {
        error = "replacement fee rate is not higher than the replaced "
                "transaction";
        return false;
      }
    }
    if (static_cast<__int128>(r.fee) <= replacedFees) {
      error = "replacement fee is not higher than the replaced "
              "transactions";
      return false;
    }
    (void)replacedBytes;
  }
  // Size: the replaced set is freed first, so only the remaining
  // shortfall needs eviction from the other entries. Eviction runs
  // before any removal, so a failed replacement leaves the pool
  // untouched.
  std::size_t freed = 0;
  for (const Hash256& id : replaced) {
    const auto it = entries_.find(id);
    if (it != entries_.end()) freed += it->second.size;
  }
  if (totalBytes_ + size > maxBytes_ + freed) {
    const std::size_t needed = totalBytes_ + size - maxBytes_ - freed;
    const std::unordered_set<Hash256, Hash256Hash> keep(replaced.begin(),
                                                        replaced.end());
    if (!EvictFor(needed, r.fee, size, keep)) {
      error = "mempool is full (fee rate too low to evict)";
      return false;
    }
  }
  for (const Hash256& id : replaced) Remove(id);
  Entry entry;
  entry.tx = tx;
  entry.fee = r.fee;
  entry.size = size;
  entry.addedTime = entryTime;
  entries_.emplace(txid, std::move(entry));
  for (const TxIn& in : tx.vin) {
    spentInputs_.insert(InputKey(in.prevTxHash, in.prevOutIndex));
  }
  totalBytes_ += size;
  return true;
}

std::vector<Mempool::Entry> Mempool::GetByFeeRate() const {
  std::vector<Entry> out;
  out.reserve(entries_.size());
  for (const auto& kv : entries_) out.push_back(kv.second);
  // Highest fee per byte first; exact integer comparison, no floats.
  std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
    const __int128 left = static_cast<__int128>(a.fee) * b.size;
    const __int128 right = static_cast<__int128>(b.fee) * a.size;
    if (left != right) return left > right;
    return a.tx.GetTxId() < b.tx.GetTxId();  // deterministic tie-break
  });
  return out;
}

bool Mempool::Contains(const Hash256& txid) const {
  return entries_.find(txid) != entries_.end();
}

bool Mempool::GetEntry(const Hash256& txid, Entry& entry) const {
  const auto it = entries_.find(txid);
  if (it == entries_.end()) return false;
  entry = it->second;
  return true;
}

bool Mempool::Remove(const Hash256& txid) {
  auto it = entries_.find(txid);
  if (it == entries_.end()) return false;
  for (const TxIn& in : it->second.tx.vin) {
    spentInputs_.erase(InputKey(in.prevTxHash, in.prevOutIndex));
  }
  totalBytes_ -= it->second.size;
  entries_.erase(it);
  return true;
}

void Mempool::RemoveExpired(uint32_t nowSeconds) {
  std::vector<Hash256> expired;
  for (const auto& kv : entries_) {
    if (nowSeconds >= kv.second.addedTime + kExpirySeconds) {
      expired.push_back(kv.first);
    }
  }
  for (const Hash256& txid : expired) Remove(txid);
}

}  // namespace oeeoo
