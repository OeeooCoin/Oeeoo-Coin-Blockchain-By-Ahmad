// SPDX-License-Identifier: MIT
// Oeeoo Coin — libFuzzer target: block + block-header
// deserialization and basic checks (Milestone 18).
// Seed corpus: tests/fuzz/corpus/block/.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "blockchain/block.h"
#include "fuzz_common.h"

using namespace oeeoo;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::vector<unsigned char> bytes(data, data + size);
  Block block;
  if (Block::Deserialize(bytes, block)) {
    std::string error;
    (void)block.CheckBasic(error);
    (void)block.GetHash();
    (void)block.ComputeMerkleRoot();
  }
  // The same bytes as a bare 80-byte header.
  if (size >= 80) {
    BlockHeader header;
    const std::vector<unsigned char> headerBytes(data, data + 80);
    if (BlockHeader::Deserialize(headerBytes, header)) {
      (void)header.GetHash();
    }
  }
  return 0;
}
