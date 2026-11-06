// SPDX-License-Identifier: MIT
// Oeeoo Coin — transaction mempool (Milestone 5, extended in 13).
// Holds validated, unconfirmed transactions for block-template
// assembly. Entry rules: the transaction must validate against the
// UTXO set, pay at least the 1 sat/vB minimum relay fee, have no
// dust output (dynamic, script-aware threshold), not already be
// present, and not conflict with another entry — unless it is a
// valid opt-in RBF replacement (BIP125-style, see AddTransaction).
// Size is capped (default 300 MB, configurable); when full, the
// lowest-fee-rate entries are evicted. Entries expire after 14
// days (RemoveExpired, called by the node on a timer).
#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "consensus/amount.h"
#include "consensus/vesting.h"
#include "crypto/sha256.h"
#include "transactions/transaction.h"
#include "utxo/coins.h"

namespace oeeoo {

class Mempool {
 public:
  // Entries expire after this long (spec section 10.4). Shared by
  // the live expiry sweep (RemoveExpired) and the persistence
  // loader (Milestone 17), which drops entries already older than
  // this instead of re-validating them.
  static constexpr uint32_t kExpirySeconds = 14 * 24 * 60 * 60;  // 14 days

  struct Entry {
    Transaction tx;
    Amount fee = 0;
    std::size_t size = 0;      // serialized bytes
    uint32_t addedTime = 0;    // seconds since epoch, for expiry
  };

  explicit Mempool(std::size_t maxBytes = 300ULL * 1024 * 1024)
      : maxBytes_(maxBytes) {}

  // Validate `tx` against `utxo` and add it. `nowSeconds` stamps the
  // entry for expiry and is the vesting clock for founder spends.
  // False + `error` on any rule failure. A transaction that
  // conflicts with entries already in the pool is accepted only as
  // an opt-in RBF replacement: every directly conflicting entry
  // must signal replaceability (an input with nSequence below
  // 0xfffffffe), the replacement must pay a higher absolute fee
  // than the replaced set (conflicting entries plus their
  // descendants, at most 100 transactions), a higher fee rate than
  // each replaced entry, and must not add new unconfirmed inputs.
  bool AddTransaction(const Transaction& tx, const CoinsView& utxo,
                      int height, uint32_t medianTimePast,
                      uint32_t nowSeconds, std::string& error,
                      const VestingParams& vesting = VestingParams{});
  // Re-add a transaction previously saved to disk (Milestone 17),
  // preserving its ORIGINAL `addedTime` for expiry — a reloaded
  // entry does not get a fresh 14 days. Validates exactly like
  // AddTransaction (fee, dust, script, conflicts, RBF rules) with
  // `nowSeconds` as the validation clock; only the entry's
  // timestamp comes from `addedTime`.
  bool AddLoadedTransaction(const Transaction& tx, const CoinsView& utxo,
                            int height, uint32_t medianTimePast,
                            uint32_t addedTime, uint32_t nowSeconds,
                            std::string& error,
                            const VestingParams& vesting = VestingParams{});

  // Entries sorted by fee rate (fee per byte), highest first.
  std::vector<Entry> GetByFeeRate() const;
  bool Contains(const Hash256& txid) const;
  // Copy of the entry for `txid`, or false when absent.
  bool GetEntry(const Hash256& txid, Entry& entry) const;
  bool Remove(const Hash256& txid);
  // Drop entries older than 14 days (spec section 10.4).
  void RemoveExpired(uint32_t nowSeconds);

  std::size_t Size() const { return entries_.size(); }
  std::size_t Bytes() const { return totalBytes_; }
  std::size_t MaxBytes() const { return maxBytes_; }
  void SetMaxBytes(std::size_t maxBytes) { maxBytes_ = maxBytes; }

 private:
  // Shared admission path. `nowSeconds` is the validation clock
  // (vesting, sequence locks); `entryTime` is the timestamp the
  // entry keeps for expiry — equal to `nowSeconds` for a fresh
  // admission, the stored time for a reloaded transaction.
  bool AddTransactionInternal(const Transaction& tx, const CoinsView& utxo,
                              int height, uint32_t medianTimePast,
                              uint32_t nowSeconds, uint32_t entryTime,
                              std::string& error,
                              const VestingParams& vesting);
  // Entries that spend one of `tx`'s inputs.
  std::vector<Hash256> FindConflicts(const Transaction& tx) const;
  // `roots` plus every entry spending their outputs, transitively.
  std::vector<Hash256> FindDescendants(
      const std::vector<Hash256>& roots) const;
  // Evict the lowest-fee-rate entries (excluding `keep`) until
  // `needed` bytes are freed. Returns false when there is nothing
  // left to evict, or when the lowest entry's fee rate is not below
  // the new transaction's (a worse tx never evicts a better one).
  bool EvictFor(std::size_t needed, Amount newFee, std::size_t newSize,
                const std::unordered_set<Hash256, Hash256Hash>& keep);

  std::unordered_map<Hash256, Entry, Hash256Hash> entries_;
  // Outpoints already spent by a mempool entry (conflict detection).
  // Stored as txid bytes + index packed into a string key.
  std::unordered_set<std::string> spentInputs_;
  std::size_t totalBytes_ = 0;
  std::size_t maxBytes_;
};

}  // namespace oeeoo
