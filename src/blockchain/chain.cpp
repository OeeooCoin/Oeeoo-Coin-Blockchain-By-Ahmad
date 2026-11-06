// SPDX-License-Identifier: MIT
// Oeeoo Coin — chain index implementation.
#include "blockchain/chain.h"

#include <algorithm>
#include <unordered_set>

#include "blockchain/genesis.h"
#include "consensus/consensus.h"

namespace oeeoo {

namespace {
// A block timestamp may be at most 2 hours ahead of current time.
constexpr uint32_t kMaxFutureSeconds = 2 * 60 * 60;
constexpr int kMedianSpan = 11;
}  // namespace

uint32_t ChainState::MedianTimePastAt(int idx) const {
  std::vector<uint32_t> times;
  int cur = idx;
  while (cur >= 0 && static_cast<int>(times.size()) < kMedianSpan) {
    times.push_back(vIndex[cur].header.nTime);
    cur = vIndex[cur].prev;
  }
  std::sort(times.begin(), times.end());
  return times[times.size() / 2];
}

uint32_t ChainState::GetMedianTimePast() const {
  if (tip_ < 0) return 0;
  return MedianTimePastAt(tip_);
}

uint32_t ChainState::ExpectedBits(int parentIdx, std::string& error) const {
  const BlockIndex& parent = vIndex[parentIdx];
  const int nextHeight = parent.height + 1;
  if (nextHeight % DIFFICULTY_INTERVAL != 0) {
    return parent.header.nBits;  // same difficulty inside an interval
  }
  // Retarget: walk back DIFFICULTY_INTERVAL - 1 blocks to the first
  // block of the interval. Its timestamp starts the timespan.
  int firstIdx = parentIdx;
  for (int i = 0; i < DIFFICULTY_INTERVAL - 1; ++i) {
    if (firstIdx < 0) {
      error = "retarget: interval start not found";
      return 0;
    }
    firstIdx = vIndex[firstIdx].prev;
  }
  if (firstIdx < 0) {
    error = "retarget: interval start not found";
    return 0;
  }
  const int64_t actualTimespan =
      static_cast<int64_t>(parent.header.nTime) -
      static_cast<int64_t>(vIndex[firstIdx].header.nTime);
  Target lastTarget;
  if (!BitsToTarget(parent.header.nBits, lastTarget)) {
    error = "retarget: parent has invalid bits";
    return 0;
  }
  return CalculateNextWorkRequired(lastTarget, actualTimespan, powLimitBits_);
}

bool ChainState::AddBlock(const Block& block, uint32_t nowSeconds,
                          std::string& error) {
  if (!block.CheckBasic(error)) return false;
  const Hash256 hash = block.GetHash();
  if (Contains(hash)) {
    error = "block already known";
    return false;
  }

  int parentIdx = -1;
  int height = 0;
  Target chainWork{};
  if (vIndex.empty()) {
    // Genesis: height 0, zero prev hash, spec genesis bits.
    if (block.header.hashPrevBlock != Hash256{}) {
      error = "genesis block must have zero prev hash";
      return false;
    }
    if (block.header.nTime == 0) {
      error = "genesis timestamp is not set (genesis not finalized)";
      return false;
    }
    if (block.header.nBits != powLimitBits_) {
      error = "genesis bits must equal the proof-of-work limit";
      return false;
    }
    // Vesting launch time: the genesis block's timestamp IS the
    // launch time. If the operator did not set it explicitly, adopt
    // it from the genesis block.
    if (vesting_.enabled && vesting_.launchTime == 0) {
      vesting_.launchTime = block.header.nTime;
    }
    height = kGenesisHeight;
  } else {
    auto it = byHash_.find(HexStr(block.header.hashPrevBlock));
    if (it == byHash_.end()) {
      error = "parent block not found";
      return false;
    }
    parentIdx = it->second;
    height = vIndex[parentIdx].height + 1;
    // Timestamp: must be after Median-Time-Past, and not more than
    // 2 hours in the future.
    if (block.header.nTime <= MedianTimePastAt(parentIdx)) {
      error = "block timestamp is not after median-time-past";
      return false;
    }
    if (block.header.nTime > nowSeconds + kMaxFutureSeconds) {
      error = "block timestamp too far in the future";
      return false;
    }
    // Difficulty: must match the expected bits (retarget at boundaries).
    std::string bitsError;
    const uint32_t expected = ExpectedBits(parentIdx, bitsError);
    if (!bitsError.empty()) {
      error = bitsError;
      return false;
    }
    if (block.header.nBits != expected) {
      error = "block has wrong difficulty bits";
      return false;
    }
    chainWork = vIndex[parentIdx].chainWork;
  }

  // Proof of work: hash must not exceed the target of nBits.
  if (!CheckProofOfWork(hash, block.header.nBits)) {
    error = "block proof of work is invalid";
    return false;
  }
  chainWork = AddChainWork(chainWork, ComputeBlockProof(block.header.nBits));

  BlockIndex index;
  index.header = block.header;
  index.height = height;
  index.hash = hash;
  index.chainWork = chainWork;
  index.prev = parentIdx;
  const int newIdx = static_cast<int>(vIndex.size());
  vIndex.push_back(index);
  byHash_[HexStr(hash)] = newIdx;
  // Chain selection: greatest cumulative valid proof of work wins.
  if (tip_ < 0 ||
      Uint256::FromBigEndian(chainWork) >
          Uint256::FromBigEndian(vIndex[tip_].chainWork)) {
    tip_ = newIdx;
  }
  return true;
}

bool ChainState::AddIndexEntry(const BlockHeader& header, const Target& work,
                               std::string& error) {
  const Hash256 hash = header.GetHash();
  if (Contains(hash)) {
    error = "block already known";
    return false;
  }
  int parentIdx = -1;
  int height = 0;
  Target expectedWork{};
  if (vIndex.empty()) {
    if (header.hashPrevBlock != Hash256{}) {
      error = "genesis block must have zero prev hash";
      return false;
    }
    if (header.nTime == 0) {
      error = "genesis timestamp is not set (genesis not finalized)";
      return false;
    }
    if (header.nBits != powLimitBits_) {
      error = "genesis bits must equal the proof-of-work limit";
      return false;
    }
    height = kGenesisHeight;
  } else {
    auto it = byHash_.find(HexStr(header.hashPrevBlock));
    if (it == byHash_.end()) {
      error = "parent block not found";
      return false;
    }
    parentIdx = it->second;
    height = vIndex[parentIdx].height + 1;
    if (header.nTime <= MedianTimePastAt(parentIdx)) {
      error = "block timestamp is not after median-time-past";
      return false;
    }
    std::string bitsError;
    const uint32_t expected = ExpectedBits(parentIdx, bitsError);
    if (!bitsError.empty()) {
      error = bitsError;
      return false;
    }
    if (header.nBits != expected) {
      error = "block has wrong difficulty bits";
      return false;
    }
    expectedWork = vIndex[parentIdx].chainWork;
  }
  if (!CheckProofOfWork(hash, header.nBits)) {
    error = "block proof of work is invalid";
    return false;
  }
  expectedWork = AddChainWork(expectedWork, ComputeBlockProof(header.nBits));
  if (expectedWork != work) {
    error = "stored chain work does not match recomputed work";
    return false;
  }
  BlockIndex index;
  index.header = header;
  index.height = height;
  index.hash = hash;
  index.chainWork = work;
  index.prev = parentIdx;
  const int newIdx = static_cast<int>(vIndex.size());
  vIndex.push_back(index);
  byHash_[HexStr(hash)] = newIdx;
  if (tip_ < 0 ||
      Uint256::FromBigEndian(work) >
          Uint256::FromBigEndian(vIndex[tip_].chainWork)) {
    tip_ = newIdx;
  }
  return true;
}

std::vector<ChainTip> ChainState::GetChainTips() const {
  std::vector<ChainTip> tips;
  if (tip_ < 0) return tips;
  // The active chain, as a set of vIndex positions (walk back from
  // the tip). A tip's fork point is the first of its ancestors that
  // lies on this chain.
  std::unordered_set<int> active;
  for (int cur = tip_; cur >= 0; cur = vIndex[cur].prev) {
    active.insert(cur);
  }
  // Every block that is some other block's parent is not a tip.
  std::unordered_set<int> hasChild;
  for (const BlockIndex& entry : vIndex) {
    if (entry.prev >= 0) hasChild.insert(entry.prev);
  }
  for (int i = 0; i < static_cast<int>(vIndex.size()); ++i) {
    if (hasChild.count(i) != 0) continue;
    ChainTip tip;
    tip.hash = vIndex[i].hash;
    tip.height = vIndex[i].height;
    tip.status = (i == tip_) ? "active" : "valid-fork";
    if (i == tip_) {
      tip.branchlen = 0;
    } else {
      int forkHeight = 0;
      for (int cur = i; cur >= 0; cur = vIndex[cur].prev) {
        if (active.count(cur) != 0) {
          forkHeight = vIndex[cur].height;
          break;
        }
      }
      tip.branchlen = vIndex[i].height - forkHeight;
    }
    tips.push_back(tip);
  }
  std::sort(tips.begin(), tips.end(), [](const ChainTip& a,
                                         const ChainTip& b) {
    if (a.height != b.height) return a.height > b.height;
    return HexStr(a.hash) < HexStr(b.hash);
  });
  return tips;
}

uint32_t ChainState::GetNextBits(std::string& error) const {
  if (tip_ < 0) return powLimitBits_;
  return ExpectedBits(tip_, error);
}

bool ChainState::GetBlockHashAtHeight(int height, Hash256& out) const {
  if (tip_ < 0 || height < 0 || height > vIndex[tip_].height) return false;
  int idx = tip_;
  while (idx >= 0 && vIndex[idx].height > height) {
    idx = vIndex[idx].prev;
  }
  if (idx < 0 || vIndex[idx].height != height) return false;
  out = vIndex[idx].hash;
  return true;
}

bool ChainState::GetMedianTimePastAtHeight(int height, uint32_t& out) const {
  if (tip_ < 0 || height < 0 || height > vIndex[tip_].height) return false;
  int idx = tip_;
  while (idx >= 0 && vIndex[idx].height > height) {
    idx = vIndex[idx].prev;
  }
  if (idx < 0 || vIndex[idx].height != height) return false;
  out = MedianTimePastAt(idx);
  return true;
}

int ChainState::GetHeight(const Hash256& hash) const {
  auto it = byHash_.find(HexStr(hash));
  if (it == byHash_.end()) return -1;
  return vIndex[it->second].height;
}

bool ChainState::GetBlockHeader(const Hash256& hash,
                                BlockHeader& header) const {
  auto it = byHash_.find(HexStr(hash));
  if (it == byHash_.end()) return false;
  header = vIndex[it->second].header;
  return true;
}

bool ChainState::GetChainWork(const Hash256& hash, Target& work) const {
  auto it = byHash_.find(HexStr(hash));
  if (it == byHash_.end()) return false;
  work = vIndex[it->second].chainWork;
  return true;
}

bool ChainState::GetPrevHash(const Hash256& hash, Hash256& prev) const {
  auto it = byHash_.find(HexStr(hash));
  if (it == byHash_.end()) return false;
  const int prevIdx = vIndex[it->second].prev;
  if (prevIdx < 0) {
    prev = Hash256{};
    return true;
  }
  prev = vIndex[prevIdx].hash;
  return true;
}

std::string ChainState::GetTipWorkHex() const {
  if (tip_ < 0) return std::string(64, '0');
  return Uint256::FromBigEndian(vIndex[tip_].chainWork).ToHex();
}

}  // namespace oeeoo
