// SPDX-License-Identifier: MIT
// Oeeoo Coin — orphan transaction pool (Milestone 15, Bitcoin Core's
// TxOrphanage model).
//
// A transaction whose parents are unknown cannot validate: the
// input loop in ValidateTransaction fails with "missing input", and
// CheckSequenceLocks fails with "sequence lock: missing input" for a
// version >= 2 transaction whose sequence locks reference an unknown
// coin. Either failure is the orphan trigger: instead of rejecting
// the transaction outright, the node holds it here until its parent
// arrives, then re-validates it. (Which of the two error strings
// triggers orphaning is the node wiring's decision; this pool does
// NO validation itself — the node validates before adding.)
//
// The pool NEVER relays orphans: there is deliberately no
// advertisement API. An orphan is announced to nobody, requested
// from nobody, and only leaves the pool by expiring, by being
// removed when its parent arrives and it validates (or fails),
// or by peer purge / eviction. This matches Core, where orphans
// are never relayed.
//
// Bounds (so the pool can never become a memory DoS vector):
//   kMaxOrphans (100) and kOrphanExpirySeconds (20 minutes) are
//     Bitcoin Core's TxOrphanage model, adopted unchanged.
//   kMaxOrphanBytes (5 MB) is Oeeoo's choice: 100 orphans x the
//     largest plausible transaction. A realistic transaction is a
//     few hundred bytes and even a large one is tens of kilobytes,
//     so 5 MB (100 x 50 KB) holds a full pool of unusually large
//     transactions with headroom, while bounding the pool to a
//     small, fixed amount of memory. A single transaction can
//     never exceed MAX_BLOCK_SIZE (1 MB, the per-transaction size
//     limit the codebase already enforces in Transaction::
//     CheckBasic / Deserialize), so the byte cap only ever binds
//     in aggregate.
// When the pool is full (count OR bytes), the OLDEST entries (by
// addedTime, ties broken by smallest txid for determinism) are
// evicted until the new transaction fits. Eviction never evicts
// the transaction being added.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "crypto/sha256.h"
#include "transactions/transaction.h"

namespace oeeoo {

class OrphanPool {
 public:
  // Bitcoin Core's TxOrphanage model, adopted unchanged.
  static constexpr std::size_t kMaxOrphans = 100;
  static constexpr uint32_t kOrphanExpirySeconds = 20 * 60;  // 20 minutes
  // Oeeoo's choice: 100 orphans x 50 KB largest plausible tx.
  static constexpr std::size_t kMaxOrphanBytes = 5ULL * 1024 * 1024;

  struct Entry {
    Transaction tx;
    std::string peerId;   // originating peer, for PurgePeer
    uint32_t addedTime = 0;  // seconds since epoch, for expiry/eviction
    std::size_t size = 0;    // serialized bytes
  };

  // Defaults are the production limits above. The parameters exist
  // so tests can exercise eviction with small caps; production
  // code must use the defaults.
  explicit OrphanPool(std::size_t maxOrphans = kMaxOrphans,
                      std::size_t maxBytes = kMaxOrphanBytes)
      : maxOrphans_(maxOrphans), maxBytes_(maxBytes) {}

  // Add `tx` as an orphan from `peerId` at time `nowSeconds`.
  // False + `error` when the txid is already in the pool, when the
  // serialized transaction exceeds MAX_BLOCK_SIZE (the
  // per-transaction size limit the codebase already uses), when
  // the transaction alone exceeds the pool byte cap, or when the
  // pool cannot make room for it (only reachable with a zero cap).
  // Otherwise evicts the oldest entries as needed and adds it.
  // The pool does NO validation of the transaction itself.
  bool Add(const Transaction& tx, const std::string& peerId,
           uint32_t nowSeconds, std::string& error);

  bool Contains(const Hash256& txid) const;
  // Copy of the transaction for `txid`, or false when absent.
  bool Get(const Hash256& txid, Transaction& tx) const;
  // Remove one orphan (and its prevout-index entries). False when
  // absent.
  bool Remove(const Hash256& txid);
  // Drop entries with addedTime at least kOrphanExpirySeconds old:
  // expired when nowSeconds >= addedTime + kOrphanExpirySeconds.
  void RemoveExpired(uint32_t nowSeconds);
  // Remove every orphan that came from `peerId` (called when that
  // peer disconnects). Returns how many were removed.
  std::size_t PurgePeer(const std::string& peerId);
  // Every orphan spending an output of `prevTxHash` — i.e. every
  // orphan waiting for that parent. Sorted by txid for determinism.
  std::vector<Hash256> GetOrphansDependingOn(
      const Hash256& prevTxHash) const;

  std::size_t Size() const { return entries_.size(); }
  std::size_t Bytes() const { return totalBytes_; }
  std::size_t MaxOrphans() const { return maxOrphans_; }
  std::size_t MaxBytes() const { return maxBytes_; }

 private:
  // Drop one entry's prevout-index rows (no-op rows are skipped).
  void RemoveFromIndex(const Transaction& tx, const Hash256& txid);
  // Evict the single oldest entry (addedTime, then smallest txid).
  // False when the pool is empty.
  bool EvictOldest();

  std::unordered_map<Hash256, Entry, Hash256Hash> entries_;
  // prevTxHash -> txids of orphans spending one of its outputs.
  std::unordered_map<Hash256, std::unordered_set<Hash256, Hash256Hash>,
                     Hash256Hash>
      byPrevout_;
  std::size_t totalBytes_ = 0;
  std::size_t maxOrphans_;
  std::size_t maxBytes_;
};

}  // namespace oeeoo
