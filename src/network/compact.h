// SPDX-License-Identifier: MIT
// Oeeoo Coin — compact blocks (BIP152-style, Milestone 6).
// A compact block sends the header, a nonce, 6-byte short IDs for
// each transaction (except prefilled ones), and the prefilled
// transactions themselves (usually just the coinbase). The receiver
// rebuilds the full block from its mempool; missing transactions are
// requested with getblocktxn / answered with blocktxn.
// Short IDs: SipHash-2-4 keyed by the first 16 bytes of
// SHA256d(header bytes || nonce), over the txid, first 6 bytes
// (little-endian order of the 64-bit hash).
#pragma once

#include <cstdint>
#include <vector>

#include "blockchain/block.h"
#include "mempool/mempool.h"

namespace oeeoo {

struct CompactBlock {
  BlockHeader header;
  uint64_t nonce = 0;
  // Total transaction count of the full block. Needed because the
  // receiver adds received transactions to `prefilled`, so the count
  // cannot be derived from shortIds + prefilled after that point.
  uint32_t txCount = 0;
  // Short IDs for every non-prefilled transaction, in block order.
  std::vector<std::array<unsigned char, 6>> shortIds;
  // Prefilled transactions: (block index, transaction).
  std::vector<std::pair<uint32_t, Transaction>> prefilled;
};

// 6-byte short ID of `txid` under the compact block's SipHash keys.
std::array<unsigned char, 6> ShortTxId(const BlockHeader& header,
                                       uint64_t nonce, const Hash256& txid);

// Build a compact block from a full block. `prefillIndexes` lists the
// block indexes to send in full (the coinbase, index 0, is always
// prefilled). `nonce` should be random per announcement.
CompactBlock MakeCompactBlock(const Block& block, uint64_t nonce);

// Serialize / deserialize a compact block (cmpctblock payload).
std::vector<unsigned char> SerializeCompactBlock(const CompactBlock& cb);
bool ParseCompactBlock(const std::vector<unsigned char>& payload,
                       CompactBlock& out);

// Serialize / deserialize a getblocktxn request: block hash + absolute
// transaction indexes (our protocol uses absolute indexes, not the
// differential encoding of BIP152 — documented simplification).
std::vector<unsigned char> SerializeGetBlockTxn(
    const Hash256& blockHash, const std::vector<uint32_t>& indexes);
bool ParseGetBlockTxn(const std::vector<unsigned char>& payload,
                      Hash256& blockHash, std::vector<uint32_t>& indexes);

// Serialize / deserialize a blocktxn answer: block hash + (index, tx)
// pairs.
std::vector<unsigned char> SerializeBlockTxn(
    const Hash256& blockHash,
    const std::vector<std::pair<uint32_t, Transaction>>& txs);
bool ParseBlockTxn(const std::vector<unsigned char>& payload,
                   Hash256& blockHash,
                   std::vector<std::pair<uint32_t, Transaction>>& txs);

// Rebuild the full block from a compact block using the mempool.
// Returns false + `missing` (indexes whose short IDs were not found)
// when transactions are missing; the caller then sends getblocktxn.
bool ReconstructBlock(const CompactBlock& cb, const Mempool& mempool,
                      Block& out, std::vector<uint32_t>& missing);

}  // namespace oeeoo
