// Tests: Merkle root against independently computed vectors.
// Leaves are SHA256d("tx0"), SHA256d("tx1"), ... (Python hashlib).
#include "blockchain/merkle.h"
#include "crypto/sha256.h"
#include "test_harness.h"

using namespace oeeoo;

void TestMerkle() {
  test::BeginSection("merkle root vectors");
  // Empty list -> all-zero root.
  CHECK(ComputeMerkleRoot({}) == Hash256{});

  std::vector<Hash256> leaves;
  for (int i = 0; i < 5; ++i) {
    leaves.push_back(Sha256d("tx" + std::to_string(i)));
  }
  // 1 leaf: root is the leaf itself.
  CHECK(HexStr(ComputeMerkleRoot({leaves[0]})) ==
        "ba7b78fe1b215636d326b297f0a60df4f20b9e3cbaa0bc0e76a093b4d88d087c");
  // 2 leaves.
  CHECK(HexStr(ComputeMerkleRoot({leaves[0], leaves[1]})) ==
        "f7ed2a56db7927143defd32e380fb083f74970e0d6f31c00b8aded86aefdf1d2");
  // 3 leaves (odd: last leaf duplicated at the first level).
  CHECK(HexStr(ComputeMerkleRoot({leaves[0], leaves[1], leaves[2]})) ==
        "e723898188fe36f157a1a97e4a608823a4c64cc81ac658dba49c4b66eef59a1f");
  // 5 leaves.
  CHECK(HexStr(ComputeMerkleRoot(leaves)) ==
        "dbbb0e2d40a03cef04f27cf8109829d6020eddec6f99f2b37c05fa560fe8a7ee");
  // Order matters: swapping two leaves changes the root.
  CHECK(ComputeMerkleRoot({leaves[1], leaves[0]}) !=
        ComputeMerkleRoot({leaves[0], leaves[1]}));
  // Determinism.
  CHECK(ComputeMerkleRoot(leaves) == ComputeMerkleRoot(leaves));
}
