// SPDX-License-Identifier: MIT
// Oeeoo Coin — orphan transaction pool implementation.
#include "mempool/orphan.h"

#include <algorithm>

#include "consensus/consensus.h"

namespace oeeoo {

bool OrphanPool::Add(const Transaction& tx, const std::string& peerId,
                     uint32_t nowSeconds, std::string& error) {
  const Hash256 txid = tx.GetTxId();
  if (Contains(txid)) {
    error = "transaction already in orphan pool";
    return false;
  }
  const std::size_t size = tx.Serialize().size();
  // Per-transaction size limit: reuse MAX_BLOCK_SIZE, the limit the
  // codebase already enforces (Transaction::CheckBasic and
  // Transaction::Deserialize). No new limit is invented here.
  if (size > static_cast<std::size_t>(MAX_BLOCK_SIZE)) {
    error = "transaction exceeds maximum transaction size";
    return false;
  }
  if (size > maxBytes_) {
    error = "transaction exceeds orphan pool byte cap";
    return false;
  }
  // Full (count OR bytes): evict the oldest entries until the new
  // transaction fits. The transaction being added is never in the
  // pool yet, so eviction can never evict it.
  while (entries_.size() >= maxOrphans_ ||
         totalBytes_ + size > maxBytes_) {
    if (!EvictOldest()) {
      error = "orphan pool is full";
      return false;
    }
  }
  Entry entry;
  entry.tx = tx;
  entry.peerId = peerId;
  entry.addedTime = nowSeconds;
  entry.size = size;
  entries_.emplace(txid, std::move(entry));
  for (const TxIn& in : tx.vin) {
    byPrevout_[in.prevTxHash].insert(txid);
  }
  totalBytes_ += size;
  return true;
}

bool OrphanPool::Contains(const Hash256& txid) const {
  return entries_.find(txid) != entries_.end();
}

bool OrphanPool::Get(const Hash256& txid, Transaction& tx) const {
  const auto it = entries_.find(txid);
  if (it == entries_.end()) return false;
  tx = it->second.tx;
  return true;
}

bool OrphanPool::Remove(const Hash256& txid) {
  auto it = entries_.find(txid);
  if (it == entries_.end()) return false;
  RemoveFromIndex(it->second.tx, txid);
  totalBytes_ -= it->second.size;
  entries_.erase(it);
  return true;
}

void OrphanPool::RemoveExpired(uint32_t nowSeconds) {
  std::vector<Hash256> expired;
  for (const auto& kv : entries_) {
    // 64-bit addition: addedTime near UINT32_MAX must not wrap.
    if (static_cast<uint64_t>(kv.second.addedTime) +
            kOrphanExpirySeconds <=
        nowSeconds) {
      expired.push_back(kv.first);
    }
  }
  for (const Hash256& txid : expired) Remove(txid);
}

std::size_t OrphanPool::PurgePeer(const std::string& peerId) {
  std::vector<Hash256> doomed;
  for (const auto& kv : entries_) {
    if (kv.second.peerId == peerId) doomed.push_back(kv.first);
  }
  for (const Hash256& txid : doomed) Remove(txid);
  return doomed.size();
}

std::vector<Hash256> OrphanPool::GetOrphansDependingOn(
    const Hash256& prevTxHash) const {
  std::vector<Hash256> out;
  const auto it = byPrevout_.find(prevTxHash);
  if (it == byPrevout_.end()) return out;
  out.assign(it->second.begin(), it->second.end());
  std::sort(out.begin(), out.end());
  return out;
}

void OrphanPool::RemoveFromIndex(const Transaction& tx,
                                 const Hash256& txid) {
  for (const TxIn& in : tx.vin) {
    const auto it = byPrevout_.find(in.prevTxHash);
    if (it == byPrevout_.end()) continue;
    it->second.erase(txid);
    if (it->second.empty()) byPrevout_.erase(it);
  }
}

bool OrphanPool::EvictOldest() {
  const Entry* oldest = nullptr;
  Hash256 oldestId;
  for (const auto& kv : entries_) {
    if (oldest == nullptr ||
        kv.second.addedTime < oldest->addedTime ||
        (kv.second.addedTime == oldest->addedTime &&
         kv.first < oldestId)) {
      oldest = &kv.second;
      oldestId = kv.first;
    }
  }
  if (oldest == nullptr) return false;
  return Remove(oldestId);
}

}  // namespace oeeoo
