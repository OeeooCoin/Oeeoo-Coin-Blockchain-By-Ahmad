#include "blockchain/merkle.h"

namespace oeeoo {

Hash256 ComputeMerkleRoot(const std::vector<Hash256>& txHashes) {
  if (txHashes.empty()) {
    return Hash256{};  // all-zero root for an empty list
  }
  std::vector<Hash256> cur = txHashes;
  std::vector<unsigned char> buf;
  buf.reserve(64);
  while (cur.size() > 1) {
    std::vector<Hash256> next;
    next.reserve((cur.size() + 1) / 2);
    for (std::size_t i = 0; i < cur.size(); i += 2) {
      const Hash256& left = cur[i];
      const Hash256& right = (i + 1 < cur.size()) ? cur[i + 1] : cur[i];
      buf.clear();
      buf.insert(buf.end(), left.begin(), left.end());
      buf.insert(buf.end(), right.begin(), right.end());
      next.push_back(Sha256d(buf));
    }
    cur = std::move(next);
  }
  return cur[0];
}

std::vector<Hash256> MerkleBranch(const std::vector<Hash256>& txHashes,
                                  std::size_t leafIndex) {
  std::vector<Hash256> branch;
  if (txHashes.empty() || leafIndex >= txHashes.size()) return branch;
  std::vector<Hash256> cur = txHashes;
  std::size_t index = leafIndex;
  std::vector<unsigned char> buf;
  buf.reserve(64);
  while (cur.size() > 1) {
    const std::size_t sibling =
        (index % 2 == 0) ? index + 1 : index - 1;
    if (sibling < cur.size()) {
      branch.push_back(cur[sibling]);
    } else {
      branch.push_back(cur[index]);  // odd node pairs with itself
    }
    std::vector<Hash256> next;
    next.reserve((cur.size() + 1) / 2);
    for (std::size_t i = 0; i < cur.size(); i += 2) {
      const Hash256& left = cur[i];
      const Hash256& right = (i + 1 < cur.size()) ? cur[i + 1] : cur[i];
      buf.clear();
      buf.insert(buf.end(), left.begin(), left.end());
      buf.insert(buf.end(), right.begin(), right.end());
      next.push_back(Sha256d(buf));
    }
    cur = std::move(next);
    index /= 2;
  }
  return branch;
}

}  // namespace oeeoo
