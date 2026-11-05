// SPDX-License-Identifier: MIT
// Oeeoo Coin — persistent chain store (Milestone 7).
// One SQLite file holds: the block index (hash, height, header,
// cumulative work, body), the UTXO set, the tip, known peers and
// bans. Schema version 1 (PRAGMA user_version).
//
// Integrity rules:
//  - A stored block is trusted only after re-checking it on load:
//    header hash matches the key, Merkle root matches the body,
//    and CheckBasic passes. A mismatch is reported, never silently
//    accepted.
//  - A block and its UTXO effects are written in ONE transaction
//    (ApplyBlock): either both land or neither does, so a crash
//    can never leave a block without its UTXO updates.
//  - Pruning drops block BODIES below a height (headers and UTXO
//    stay); a pruned node can still validate new blocks and serve
//    headers, but not old bodies.
//  - The store does NOT re-validate the whole chain from genesis on
//    load (that is a reindex/audit step, Milestone 11). It verifies
//    structure and internal consistency of what it reads.
#pragma once

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "blockchain/block.h"
#include "consensus/difficulty.h"
#include "db/database.h"
#include "transactions/validation.h"
#include "utxo/coins.h"

namespace oeeoo {

constexpr int kChainStoreSchemaVersion = 2;

class ChainStore {
 public:
  ChainStore() = default;
  ~ChainStore() { Close(); }
  ChainStore(const ChainStore&) = delete;
  ChainStore& operator=(const ChainStore&) = delete;

  // Open (creating) the store file; creates the schema on a fresh
  // file, rejects an unknown schema version. False + `error` on
  // failure.
  bool Open(const std::string& path, std::string& error);
  void Close();
  bool IsOpen() const { return db_.IsOpen(); }
  const std::string& Path() const { return db_.Path(); }
  Database& Db() { return db_; }

  // --- blocks ---
  // Store a block (header + body + cumulative work) at `height`.
  // Upserts. The height is a parameter because a block does not
  // carry it (the coinbase height push is not authoritative here).
  bool PutBlockAtHeight(const Block& block, const Target& work, int height,
                        std::string& error);
  // Load a full block by hash. False when missing, pruned, or
  // corrupt (see `error`).
  bool GetBlock(const Hash256& hash, Block& block, std::string& error);
  bool HasBlock(const Hash256& hash) const;
  bool HasBlockBody(const Hash256& hash) const;
  bool GetBlockHeader(const Hash256& hash, BlockHeader& header,
                      std::string& error);
  bool GetBlockHeight(const Hash256& hash, int& height, std::string& error);
  bool GetChainWork(const Hash256& hash, Target& work, std::string& error);
  // All known block hashes at `height` (usually one; forks give more).
  bool GetHashesAtHeight(int height, std::vector<Hash256>& out,
                         std::string& error);
  // Every stored header with its height and cumulative work, in
  // ascending height order (parents before children) — for
  // rebuilding the chain index at startup.
  bool GetAllHeaders(
      std::vector<std::tuple<int, BlockHeader, Target>>& out,
      std::string& error);
  std::size_t BlockCount(std::string& error);

  // --- tip ---
  bool SaveTip(const Hash256& hash, std::string& error);
  // False when no tip is stored yet.
  bool LoadTip(Hash256& hash, std::string& error);

  // --- UTXO ---
  bool PutCoin(const OutPoint& outpoint, const Coin& coin,
               std::string& error);
  bool SpendCoin(const OutPoint& outpoint, std::string& error);
  bool GetCoin(const OutPoint& outpoint, Coin& coin, std::string& error);
  bool HasCoin(const OutPoint& outpoint) const;
  // Every coin, for loading the in-memory UTXO set at startup.
  bool GetAllCoins(std::vector<std::pair<OutPoint, Coin>>& out,
                   std::string& error);
  std::size_t UtxoCount(std::string& error);

  // --- one atomic block application ---
  // Writes the block, spends its inputs, adds its outputs, stores
  // the block's undo record, and moves the tip — all in one
  // transaction. The undo record's `added` list (coinbase first)
  // carries the block's height. Schema v2: undo records live in
  // the `undo` table (one row per applied block).
  bool ApplyBlock(const Block& block, const Target& work,
                  const BlockUndo& undo, std::string& error);
  // --- reorg undo (schema v2) ---
  // Reverse the tip block's application: restore its spent coins,
  // remove its added coins, delete the block row + undo row, and
  // move the tip back to the block's parent. Fails (without
  // changing anything) when the block is not the tip or has no
  // undo record.
  bool DisconnectBlock(const Hash256& hash, std::string& error);
  // The undo record for an applied block (missing = false, no error).
  bool GetUndo(const Hash256& hash, BlockUndo& undo, std::string& error);

  // --- pruning ---
  // Drop block bodies below `keepFromHeight` (headers stay).
  bool PruneBlocks(int keepFromHeight, std::string& error);
  bool LoadPruneHeight(int& height, std::string& error);

  // --- peers / bans ---
  bool PutPeer(const std::string& address, uint32_t lastSeen,
               uint64_t services, std::string& error);
  bool GetPeers(std::vector<std::pair<std::string, uint32_t>>& out,
                std::string& error);
  bool PutBan(const std::string& address, uint32_t until, int offences,
              std::string& error);
  bool GetBans(std::vector<std::tuple<std::string, uint32_t, int>>& out,
               std::string& error);
  bool ClearBan(const std::string& address, std::string& error);

 private:
  bool CreateSchema(std::string& error);
  bool CheckBlockIntegrity(const Block& block, const Hash256& expectedHash,
                           std::string& error);

  Database db_;
};

}  // namespace oeeoo
