// Merkle tree — Bitcoin-style.
// Leaves are transaction ids (SHA256d of each transaction).
// Each level pairs hashes left-to-right and hashes the concatenation
// with SHA256d; an odd hash at any level is duplicated (paired with
// itself). One leaf: the root IS that leaf. No leaves: the root is
// the all-zero hash. Deterministic: same transaction order always
// gives the same root on every node.
#pragma once

#include <vector>

#include "crypto/sha256.h"

namespace oeeoo {

// Merkle root of `txHashes` in the given order.
Hash256 ComputeMerkleRoot(const std::vector<Hash256>& txHashes);
// Merkle branch for the leaf at `leafIndex`: the sibling hash at
// each level, bottom to top. The leaf's own hash is never included.
// (Used by Stratum: the branch is independent of the leaf's value,
// so one branch serves every coinbase the miner tries.)
std::vector<Hash256> MerkleBranch(const std::vector<Hash256>& txHashes,
                                  std::size_t leafIndex);

}  // namespace oeeoo
