// SPDX-License-Identifier: MIT
// Oeeoo Coin — chain index and chain selection (Milestone 3).
// Every block added here is validated: structure, parent linkage,
// height, Median-Time-Past and future-timestamp rules, expected
// difficulty (incl. retarget), proof of work, and cumulative work.
// The tip is always the block with the greatest cumulative valid
// proof of work — a competing branch with more work reorgs the tip
// automatically. Persistent storage lives in Milestone 7 (ChainStore);
// AddIndexEntry rebuilds the index from stored headers.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "blockchain/block.h"
#include "consensus/difficulty.h"
#include "consensus/vesting.h"
#include "crypto/sha256.h"

namespace oeeoo {

struct BlockIndex {
  BlockHeader header;
  int height = -1;
  Hash256 hash{};
  Target chainWork{};  // cumulative proof of work up to and incl. this block
  int prev = -1;       // index into ChainState::vIndex, -1 for genesis
};

// One chain tip (Milestone 16, `getchaintips`): a block that is no
// other block's parent. `branchlen` is the tip's height minus the
// height of the block where its chain diverges from the active
// chain (0 for the active tip). `status` is "active" for the
// greatest-work tip and "valid-fork" for every other tip: every
// index entry was fully validated when accepted (AddBlock) or
// re-verified on load (AddIndexEntry), so there is no headers-only
// vs full-body distinction to report — Oeeoo does not track one.
struct ChainTip {
  Hash256 hash{};
  int height = -1;
  int branchlen = 0;
  std::string status;  // "active" | "valid-fork"
};

class ChainState {
 public:
  // `powLimitBits` is the chain's difficulty limit (default: mainnet
  // genesis bits from the spec). Tests pass an easier limit so blocks
  // can be mined quickly; production must use the default.
  explicit ChainState(uint32_t powLimitBits = kGenesisBits)
      : powLimitBits_(powLimitBits) {}

  // Validate and add a block. `nowSeconds` is the caller's current
  // time (seconds since epoch) — consensus never reads the wall clock
  // itself, so tests stay deterministic. On failure returns false and
  // sets `error`.
  bool AddBlock(const Block& block, uint32_t nowSeconds, std::string& error);

  // Load-only: insert a stored header into the index (used when
  // rebuilding from the ChainStore). Re-verifies parent linkage,
  // expected bits, Median-Time-Past, proof of work, and that the
  // stored cumulative work matches recomputation. Does NOT check
  // transactions (the body is checked separately when loaded) and
  // does NOT apply the 2-hour future rule (the wall clock has moved
  // on since the block was first accepted).
  bool AddIndexEntry(const BlockHeader& header, const Target& work,
                     std::string& error);

  bool Contains(const Hash256& hash) const {
    return byHash_.find(HexStr(hash)) != byHash_.end();
  }
  int GetHeight(const Hash256& hash) const;
  // Header and parent hash of a known block (for getheaders serving).
  bool GetBlockHeader(const Hash256& hash, BlockHeader& header) const;
  bool GetPrevHash(const Hash256& hash, Hash256& prev) const;
  // Cumulative proof of work of a known block (for persistence).
  bool GetChainWork(const Hash256& hash, Target& work) const;
  int GetTipHeight() const { return tip_ < 0 ? -1 : vIndex[tip_].height; }
  Hash256 GetTipHash() const {
    return tip_ < 0 ? Hash256{} : vIndex[tip_].hash;
  }
  std::string GetTipWorkHex() const;
  std::size_t Size() const { return vIndex.size(); }
  // Median-Time-Past of the tip's chain (0 if empty).
  uint32_t GetMedianTimePast() const;
  // Difficulty bits for the next block (the tip's child), including
  // retargets. Returns the PoW limit when the chain is empty.
  uint32_t GetNextBits(std::string& error) const;
  // Hash of the main-chain block at `height` (-1..tip). Walks back
  // from the tip, so it always follows the greatest-work chain.
  bool GetBlockHashAtHeight(int height, Hash256& out) const;
  // Every chain tip (Milestone 16): the active tip plus every
  // stale-branch tip retained in the index. A block is a tip when
  // no other indexed block names it as a parent. Sorted by height
  // descending, ties broken by hash hex ascending (deterministic).
  std::vector<ChainTip> GetChainTips() const;
  // Median-Time-Past of the main-chain block at `height`.
  bool GetMedianTimePastAtHeight(int height, uint32_t& out) const;
  // Founder vesting parameters (Milestone 9). Disabled by default.
  void SetVestingParams(const VestingParams& params) { vesting_ = params; }
  const VestingParams& GetVestingParams() const { return vesting_; }
  uint32_t PowLimitBits() const { return powLimitBits_; }

 private:
  // Median of the last 11 block times ending at index `idx`.
  uint32_t MedianTimePastAt(int idx) const;
  // Expected nBits for a block whose parent is `parentIdx`.
  uint32_t ExpectedBits(int parentIdx, std::string& error) const;

  std::vector<BlockIndex> vIndex;
  std::unordered_map<std::string, int> byHash_;
  int tip_ = -1;
  uint32_t powLimitBits_;
  VestingParams vesting_;
};

}  // namespace oeeoo
