// SPDX-License-Identifier: MIT
// Oeeoo Coin — libFuzzer target: compact blocks (BIP152-style)
// + getblocktxn / blocktxn (Milestone 18). A parsed compact
// block is also fed to ReconstructBlock against an empty mempool
// (the missing-transaction path). Seed corpus:
// tests/fuzz/corpus/compact/.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "mempool/mempool.h"
#include "network/compact.h"

using namespace oeeoo;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::vector<unsigned char> bytes(data, data + size);
  {
    CompactBlock cb;
    if (ParseCompactBlock(bytes, cb)) {
      Mempool mempool;
      Block out;
      std::vector<uint32_t> missing;
      (void)ReconstructBlock(cb, mempool, out, missing);
    }
  }
  {
    Hash256 hash;
    std::vector<uint32_t> indexes;
    (void)ParseGetBlockTxn(bytes, hash, indexes);
  }
  {
    Hash256 hash;
    std::vector<std::pair<uint32_t, Transaction>> txs;
    (void)ParseBlockTxn(bytes, hash, txs);
  }
  return 0;
}
