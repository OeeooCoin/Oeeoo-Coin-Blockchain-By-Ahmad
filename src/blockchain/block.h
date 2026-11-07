// Block header — Phase 1.
// Bitcoin-style fields, fixed sizes, little-endian serialization:
//   version      int32   (4 bytes)
//   prev hash    32 bytes (as stored, internal byte order)
//   merkle root  32 bytes (as stored, internal byte order)
//   time         uint32  (4 bytes)
//   nBits        uint32  (4 bytes)
//   nonce        uint32  (4 bytes)
// Total: exactly 80 bytes. Serialization is deterministic: the same
// header always serializes to the same 80 bytes on every platform.
// GetHash() = SHA256d(serialization). The hex display convention
// (raw order here) is recorded in docs/CONSENSUS_PHASE1.md and must
// be locked, with the genesis parameters, before mainnet — it is
// NOT silently changed later.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "crypto/sha256.h"
#include "transactions/transaction.h"

namespace oeeoo {

struct BlockHeader {
  int32_t nVersion = 1;
  Hash256 hashPrevBlock{};
  Hash256 hashMerkleRoot{};
  uint32_t nTime = 0;
  uint32_t nBits = 0;
  uint32_t nNonce = 0;

  // Deterministic 80-byte little-endian serialization.
  std::vector<unsigned char> Serialize() const;

  // Deserialize exactly 80 bytes. Returns false on wrong size.
  static bool Deserialize(const std::vector<unsigned char>& bytes,
                          BlockHeader& out);

  // SHA256d(Serialize()).
  Hash256 GetHash() const;
  std::string GetHashHex() const;
};

// Full block: header + transactions (Milestone 3).
// Serialization: 80-byte header, CompactSize tx count, then each
// transaction in order. Coinbase rules (exactly one coinbase, and it
// must be the first tx; reward checks) are enforced by CheckBasic
// (M20, OOC-001) and by CheckBlockReward / ConnectBlockTransactions:
// size, Merkle root, duplicate txids, per-tx basic validity, plus
// the exactly-one-coinbase-at-index-0 rule.
struct Block {
  BlockHeader header;
  std::vector<Transaction> vtx;

  std::vector<unsigned char> Serialize() const;
  static bool Deserialize(const std::vector<unsigned char>& bytes,
                          Block& out);
  // SHA256d of the header serialization (== header.GetHash()).
  Hash256 GetHash() const { return header.GetHash(); }

  // Structural validation. `error` explains the first failure found.
  bool CheckBasic(std::string& error) const;
  // Merkle root implied by vtx (txids in order). Empty block -> zero root.
  Hash256 ComputeMerkleRoot() const;
};

}  // namespace oeeoo
