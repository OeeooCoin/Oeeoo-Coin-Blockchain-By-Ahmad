// SPDX-License-Identifier: MIT
// Oeeoo Coin — persistent chain store implementation.
#include "db/chainstore.h"

#include <cstring>

#include "consensus/consensus.h"

namespace oeeoo {

namespace {
const char* kSchemaSql = R"(
CREATE TABLE IF NOT EXISTS meta(
  key TEXT PRIMARY KEY,
  value BLOB NOT NULL
);
CREATE TABLE IF NOT EXISTS blocks(
  hash BLOB PRIMARY KEY,
  height INTEGER NOT NULL,
  header BLOB NOT NULL,
  work BLOB NOT NULL,
  body BLOB,
  time INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_blocks_height ON blocks(height);
CREATE TABLE IF NOT EXISTS utxo(
  txid BLOB NOT NULL,
  vout INTEGER NOT NULL,
  value INTEGER NOT NULL,
  script BLOB NOT NULL,
  height INTEGER NOT NULL,
  block_time INTEGER NOT NULL,
  is_coinbase INTEGER NOT NULL,
  PRIMARY KEY (txid, vout)
);
CREATE TABLE IF NOT EXISTS peers(
  address TEXT PRIMARY KEY,
  last_seen INTEGER NOT NULL,
  services INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS bans(
  address TEXT PRIMARY KEY,
  until INTEGER NOT NULL,
  offences INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS undo(
  hash BLOB PRIMARY KEY,
  data BLOB NOT NULL
);
)";

bool BlobToHash(const std::vector<unsigned char>& blob, Hash256& hash) {
  if (blob.size() != 32) return false;
  std::memcpy(hash.data(), blob.data(), 32);
  return true;
}

std::vector<unsigned char> HashToBlob(const Hash256& hash) {
  return std::vector<unsigned char>(hash.begin(), hash.end());
}

// --- undo record serialization (schema v2) ---
// Format (little-endian): u32 spentCount, then per spent entry:
// txid(32) + vout(u32) + value(i64) + scriptLen(u32) + script +
// height(i32) + blockTime(u32) + isCoinbase(u8); then u32 addedCount
// and the same per-entry format for the added outputs.
void AppendU32(std::vector<unsigned char>& out, uint32_t v) {
  out.push_back(static_cast<unsigned char>(v & 0xff));
  out.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
  out.push_back(static_cast<unsigned char>((v >> 16) & 0xff));
  out.push_back(static_cast<unsigned char>((v >> 24) & 0xff));
}

void AppendI64(std::vector<unsigned char>& out, int64_t v) {
  AppendU32(out, static_cast<uint32_t>(v & 0xffffffffULL));
  AppendU32(out, static_cast<uint32_t>(
                     (static_cast<uint64_t>(v) >> 32) & 0xffffffffULL));
}

void AppendCoinEntry(std::vector<unsigned char>& out, const OutPoint& op,
                     const Coin& coin) {
  out.insert(out.end(), op.txid.begin(), op.txid.end());
  AppendU32(out, op.index);
  AppendI64(out, coin.out.value);
  AppendU32(out, static_cast<uint32_t>(coin.out.scriptPubKey.size()));
  out.insert(out.end(), coin.out.scriptPubKey.begin(),
             coin.out.scriptPubKey.end());
  AppendU32(out, static_cast<uint32_t>(coin.height));
  AppendU32(out, coin.blockTime);
  out.push_back(coin.isCoinbase ? 1 : 0);
}

std::vector<unsigned char> SerializeBlockUndo(const BlockUndo& undo) {
  std::vector<unsigned char> out;
  AppendU32(out, static_cast<uint32_t>(undo.spent.size()));
  for (const auto& entry : undo.spent) {
    AppendCoinEntry(out, entry.first, entry.second);
  }
  AppendU32(out, static_cast<uint32_t>(undo.added.size()));
  for (const auto& entry : undo.added) {
    AppendCoinEntry(out, entry.first, entry.second);
  }
  return out;
}

bool ReadU32(const std::vector<unsigned char>& data, std::size_t& pos,
             uint32_t& v) {
  if (pos + 4 > data.size()) return false;
  v = static_cast<uint32_t>(data[pos]) |
      (static_cast<uint32_t>(data[pos + 1]) << 8) |
      (static_cast<uint32_t>(data[pos + 2]) << 16) |
      (static_cast<uint32_t>(data[pos + 3]) << 24);
  pos += 4;
  return true;
}

bool ReadCoinEntry(const std::vector<unsigned char>& data, std::size_t& pos,
                   OutPoint& op, Coin& coin) {
  if (pos + 32 + 4 + 8 + 4 > data.size()) return false;
  std::memcpy(op.txid.data(), data.data() + pos, 32);
  pos += 32;
  uint32_t vout = 0;
  if (!ReadU32(data, pos, vout)) return false;
  op.index = vout;
  uint32_t lo = 0, hi = 0;
  if (!ReadU32(data, pos, lo) || !ReadU32(data, pos, hi)) return false;
  const int64_t value = static_cast<int64_t>(
      (static_cast<uint64_t>(hi) << 32) | lo);
  uint32_t scriptLen = 0;
  if (!ReadU32(data, pos, scriptLen)) return false;
  if (pos + scriptLen + 4 + 4 + 1 > data.size()) return false;
  coin.out.value = value;
  coin.out.scriptPubKey.assign(data.begin() + pos,
                               data.begin() + pos + scriptLen);
  pos += scriptLen;
  uint32_t height = 0, blockTime = 0;
  if (!ReadU32(data, pos, height) || !ReadU32(data, pos, blockTime)) {
    return false;
  }
  coin.height = static_cast<int>(height);
  coin.blockTime = blockTime;
  coin.isCoinbase = data[pos++] != 0;
  return true;
}

bool DeserializeBlockUndo(const std::vector<unsigned char>& data,
                          BlockUndo& undo) {
  std::size_t pos = 0;
  uint32_t spentCount = 0;
  if (!ReadU32(data, pos, spentCount)) return false;
  if (spentCount > 1000000) return false;
  undo.spent.clear();
  for (uint32_t i = 0; i < spentCount; ++i) {
    OutPoint op;
    Coin coin;
    if (!ReadCoinEntry(data, pos, op, coin)) return false;
    undo.spent.push_back({op, coin});
  }
  uint32_t addedCount = 0;
  if (!ReadU32(data, pos, addedCount)) return false;
  if (addedCount > 1000000) return false;
  undo.added.clear();
  for (uint32_t i = 0; i < addedCount; ++i) {
    OutPoint op;
    Coin coin;
    if (!ReadCoinEntry(data, pos, op, coin)) return false;
    undo.added.push_back({op, coin});
  }
  return pos == data.size();
}
}  // namespace

bool ChainStore::Open(const std::string& path, std::string& error) {
  if (!db_.Open(path, error)) return false;
  std::string err;
  const int version = db_.GetUserVersion(err);
  if (version < 0) {
    error = "cannot read schema version: " + err;
    return false;
  }
  if (version == 0) {
    if (!CreateSchema(error)) return false;
    if (!db_.SetUserVersion(kChainStoreSchemaVersion, error)) return false;
  } else if (version == 1) {
    // Migration 1 -> 2: the undo table (for reorg rollbacks).
    if (!db_.Execute(
            "CREATE TABLE IF NOT EXISTS undo(\n"
            "  hash BLOB PRIMARY KEY,\n"
            "  data BLOB NOT NULL\n"
            ");",
            error)) {
      return false;
    }
    if (!db_.SetUserVersion(kChainStoreSchemaVersion, error)) return false;
  } else if (version != kChainStoreSchemaVersion) {
    error = "unsupported chain store schema version " +
            std::to_string(version) + " (expected " +
            std::to_string(kChainStoreSchemaVersion) + ")";
    return false;
  }
  return true;
}

void ChainStore::Close() { db_.Close(); }

bool ChainStore::CreateSchema(std::string& error) {
  return db_.Execute(kSchemaSql, error);
}

bool ChainStore::CheckBlockIntegrity(const Block& block,
                                     const Hash256& expectedHash,
                                     std::string& error) {
  if (block.GetHash() != expectedHash) {
    error = "stored block hash does not match its key";
    return false;
  }
  std::string basicError;
  if (!block.CheckBasic(basicError)) {
    error = "stored block fails structural checks: " + basicError;
    return false;
  }
  return true;
}

bool ChainStore::PutBlockAtHeight(const Block& block, const Target& work,
                                  int height, std::string& error) {
  std::string basicError;
  if (!block.CheckBasic(basicError)) {
    error = "refusing to store an invalid block: " + basicError;
    return false;
  }
  Database::Statement stmt;
  if (!stmt.Prepare(db_,
                    "INSERT OR REPLACE INTO blocks"
                    "(hash, height, header, work, body, time) "
                    "VALUES (?1, ?2, ?3, ?4, ?5, ?6);",
                    error)) {
    return false;
  }
  stmt.BindBlob(1, HashToBlob(block.GetHash()));
  stmt.BindInt64(2, height);
  stmt.BindBlob(3, block.header.Serialize());
  stmt.BindBlob(4, std::vector<unsigned char>(work.begin(), work.end()));
  stmt.BindBlob(5, block.Serialize());
  stmt.BindInt64(6, block.header.nTime);
  stmt.Step();
  if (stmt.HasError()) {
    error = "database error writing block";
    return false;
  }
  return true;
}

bool ChainStore::GetBlock(const Hash256& hash, Block& block,
                          std::string& error) {
  Database::Statement stmt;
  if (!stmt.Prepare(db_, "SELECT body FROM blocks WHERE hash=?1;", error)) {
    return false;
  }
  stmt.BindBlob(1, HashToBlob(hash));
  if (!stmt.Step()) {
    if (stmt.HasError()) {
      error = "database error reading block";
      return false;
    }
    error = "block not found";
    return false;
  }
  if (stmt.ColumnIsNull(0)) {
    error = "block body pruned";
    return false;
  }
  std::vector<unsigned char> body = stmt.ColumnBlob(0);
  if (!Block::Deserialize(body, block)) {
    error = "stored block body does not deserialize";
    return false;
  }
  return CheckBlockIntegrity(block, hash, error);
}

bool ChainStore::HasBlock(const Hash256& hash) const {
  Database::Statement stmt;
  std::string error;
  if (!stmt.Prepare(const_cast<Database&>(db_),
                    "SELECT 1 FROM blocks WHERE hash=?1;", error)) {
    return false;
  }
  stmt.BindBlob(1, HashToBlob(hash));
  return stmt.Step();
}

bool ChainStore::HasBlockBody(const Hash256& hash) const {
  Database::Statement stmt;
  std::string error;
  if (!stmt.Prepare(const_cast<Database&>(db_),
                    "SELECT body FROM blocks WHERE hash=?1;", error)) {
    return false;
  }
  stmt.BindBlob(1, HashToBlob(hash));
  if (!stmt.Step()) return false;
  return !stmt.ColumnIsNull(0);
}

bool ChainStore::GetBlockHeader(const Hash256& hash, BlockHeader& header,
                                std::string& error) {
  Database::Statement stmt;
  if (!stmt.Prepare(db_, "SELECT header FROM blocks WHERE hash=?1;",
                    error)) {
    return false;
  }
  stmt.BindBlob(1, HashToBlob(hash));
  if (!stmt.Step()) {
    error = stmt.HasError() ? "database error reading header"
                            : "block not found";
    return false;
  }
  std::vector<unsigned char> bytes = stmt.ColumnBlob(0);
  if (!BlockHeader::Deserialize(bytes, header)) {
    error = "stored header does not deserialize";
    return false;
  }
  return true;
}

bool ChainStore::GetBlockHeight(const Hash256& hash, int& height,
                                std::string& error) {
  Database::Statement stmt;
  if (!stmt.Prepare(db_, "SELECT height FROM blocks WHERE hash=?1;",
                    error)) {
    return false;
  }
  stmt.BindBlob(1, HashToBlob(hash));
  if (!stmt.Step()) {
    error = stmt.HasError() ? "database error reading height"
                            : "block not found";
    return false;
  }
  height = static_cast<int>(stmt.ColumnInt64(0));
  return true;
}

bool ChainStore::GetChainWork(const Hash256& hash, Target& work,
                              std::string& error) {
  Database::Statement stmt;
  if (!stmt.Prepare(db_, "SELECT work FROM blocks WHERE hash=?1;", error)) {
    return false;
  }
  stmt.BindBlob(1, HashToBlob(hash));
  if (!stmt.Step()) {
    error = stmt.HasError() ? "database error reading work"
                            : "block not found";
    return false;
  }
  std::vector<unsigned char> blob = stmt.ColumnBlob(0);
  if (blob.size() != 32) {
    error = "stored work has wrong size";
    return false;
  }
  std::memcpy(work.data(), blob.data(), 32);
  return true;
}

bool ChainStore::GetHashesAtHeight(int height, std::vector<Hash256>& out,
                                   std::string& error) {
  Database::Statement stmt;
  if (!stmt.Prepare(db_, "SELECT hash FROM blocks WHERE height=?1;",
                    error)) {
    return false;
  }
  stmt.BindInt64(1, height);
  out.clear();
  while (stmt.Step()) {
    Hash256 hash;
    if (!BlobToHash(stmt.ColumnBlob(0), hash)) {
      error = "stored hash has wrong size";
      return false;
    }
    out.push_back(hash);
  }
  if (stmt.HasError()) {
    error = "database error listing hashes";
    return false;
  }
  return true;
}

bool ChainStore::GetAllHeaders(
    std::vector<std::tuple<int, BlockHeader, Target>>& out,
    std::string& error) {
  Database::Statement stmt;
  if (!stmt.Prepare(db_,
                    "SELECT height, header, work FROM blocks "
                    "ORDER BY height ASC;",
                    error)) {
    return false;
  }
  out.clear();
  while (stmt.Step()) {
    const int height = static_cast<int>(stmt.ColumnInt64(0));
    BlockHeader header;
    if (!BlockHeader::Deserialize(stmt.ColumnBlob(1), header)) {
      error = "stored header does not deserialize";
      return false;
    }
    Target work;
    std::vector<unsigned char> blob = stmt.ColumnBlob(2);
    if (blob.size() != 32) {
      error = "stored work has wrong size";
      return false;
    }
    std::memcpy(work.data(), blob.data(), 32);
    out.push_back({height, header, work});
  }
  if (stmt.HasError()) {
    error = "database error listing headers";
    return false;
  }
  return true;
}

std::size_t ChainStore::BlockCount(std::string& error) {
  Database::Statement stmt;
  if (!stmt.Prepare(db_, "SELECT COUNT(*) FROM blocks;", error)) return 0;
  if (!stmt.Step()) return 0;
  return static_cast<std::size_t>(stmt.ColumnInt64(0));
}

bool ChainStore::SaveTip(const Hash256& hash, std::string& error) {
  Database::Statement stmt;
  if (!stmt.Prepare(db_,
                    "INSERT OR REPLACE INTO meta(key, value) "
                    "VALUES ('tip', ?1);",
                    error)) {
    return false;
  }
  stmt.BindBlob(1, HashToBlob(hash));
  stmt.Step();
  if (stmt.HasError()) {
    error = "database error saving tip";
    return false;
  }
  return true;
}

bool ChainStore::LoadTip(Hash256& hash, std::string& error) {
  Database::Statement stmt;
  if (!stmt.Prepare(db_, "SELECT value FROM meta WHERE key='tip';",
                    error)) {
    return false;
  }
  if (!stmt.Step()) {
    if (stmt.HasError()) {
      error = "database error loading tip";
      return false;
    }
    error = "no tip stored yet";
    return false;
  }
  if (!BlobToHash(stmt.ColumnBlob(0), hash)) {
    error = "stored tip has wrong size";
    return false;
  }
  return true;
}

bool ChainStore::PutCoin(const OutPoint& outpoint, const Coin& coin,
                         std::string& error) {
  Database::Statement stmt;
  if (!stmt.Prepare(db_,
                    "INSERT OR REPLACE INTO utxo"
                    "(txid, vout, value, script, height, block_time, "
                    "is_coinbase) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);",
                    error)) {
    return false;
  }
  stmt.BindBlob(1, HashToBlob(outpoint.txid));
  stmt.BindInt64(2, outpoint.index);
  stmt.BindInt64(3, coin.out.value);
  stmt.BindBlob(4, coin.out.scriptPubKey);
  stmt.BindInt64(5, coin.height);
  stmt.BindInt64(6, coin.blockTime);
  stmt.BindInt64(7, coin.isCoinbase ? 1 : 0);
  stmt.Step();
  if (stmt.HasError()) {
    error = "database error writing coin";
    return false;
  }
  return true;
}

bool ChainStore::SpendCoin(const OutPoint& outpoint, std::string& error) {
  Database::Statement stmt;
  if (!stmt.Prepare(db_, "DELETE FROM utxo WHERE txid=?1 AND vout=?2;",
                    error)) {
    return false;
  }
  stmt.BindBlob(1, HashToBlob(outpoint.txid));
  stmt.BindInt64(2, outpoint.index);
  stmt.Step();
  if (stmt.HasError()) {
    error = "database error spending coin";
    return false;
  }
  return true;
}

bool ChainStore::GetCoin(const OutPoint& outpoint, Coin& coin,
                         std::string& error) {
  Database::Statement stmt;
  if (!stmt.Prepare(db_,
                    "SELECT value, script, height, block_time, is_coinbase "
                    "FROM utxo WHERE txid=?1 AND vout=?2;",
                    error)) {
    return false;
  }
  stmt.BindBlob(1, HashToBlob(outpoint.txid));
  stmt.BindInt64(2, outpoint.index);
  if (!stmt.Step()) {
    error = stmt.HasError() ? "database error reading coin"
                            : "coin not found";
    return false;
  }
  coin.out.value = stmt.ColumnInt64(0);
  coin.out.scriptPubKey = stmt.ColumnBlob(1);
  coin.height = static_cast<int>(stmt.ColumnInt64(2));
  coin.blockTime = static_cast<uint32_t>(stmt.ColumnInt64(3));
  coin.isCoinbase = stmt.ColumnInt64(4) != 0;
  if (!MoneyRange(coin.out.value)) {
    error = "stored coin value out of range";
    return false;
  }
  return true;
}

bool ChainStore::HasCoin(const OutPoint& outpoint) const {
  Database::Statement stmt;
  std::string error;
  if (!stmt.Prepare(const_cast<Database&>(db_),
                    "SELECT 1 FROM utxo WHERE txid=?1 AND vout=?2;",
                    error)) {
    return false;
  }
  stmt.BindBlob(1, HashToBlob(outpoint.txid));
  stmt.BindInt64(2, outpoint.index);
  return stmt.Step();
}

bool ChainStore::GetAllCoins(
    std::vector<std::pair<OutPoint, Coin>>& out, std::string& error) {
  Database::Statement stmt;
  if (!stmt.Prepare(db_,
                    "SELECT txid, vout, value, script, height, block_time, "
                    "is_coinbase FROM utxo;",
                    error)) {
    return false;
  }
  out.clear();
  while (stmt.Step()) {
    OutPoint op;
    if (!BlobToHash(stmt.ColumnBlob(0), op.txid)) {
      error = "stored txid has wrong size";
      return false;
    }
    op.index = static_cast<uint32_t>(stmt.ColumnInt64(1));
    Coin coin;
    coin.out.value = stmt.ColumnInt64(2);
    coin.out.scriptPubKey = stmt.ColumnBlob(3);
    coin.height = static_cast<int>(stmt.ColumnInt64(4));
    coin.blockTime = static_cast<uint32_t>(stmt.ColumnInt64(5));
    coin.isCoinbase = stmt.ColumnInt64(6) != 0;
    if (!MoneyRange(coin.out.value)) {
      error = "stored coin value out of range";
      return false;
    }
    out.push_back({op, coin});
  }
  if (stmt.HasError()) {
    error = "database error listing coins";
    return false;
  }
  return true;
}

std::size_t ChainStore::UtxoCount(std::string& error) {
  Database::Statement stmt;
  if (!stmt.Prepare(db_, "SELECT COUNT(*) FROM utxo;", error)) return 0;
  if (!stmt.Step()) return 0;
  return static_cast<std::size_t>(stmt.ColumnInt64(0));
}

bool ChainStore::ApplyBlock(const Block& block, const Target& work,
                            const BlockUndo& undo, std::string& error) {
  std::string basicError;
  if (!block.CheckBasic(basicError)) {
    error = "refusing to store an invalid block: " + basicError;
    return false;
  }
  // The height comes from the caller through the coinbase outputs:
  // every added coin carries its creation height. All coins of one
  // block share the same height.
  if (undo.added.empty()) {
    error = "ApplyBlock needs at least the coinbase output";
    return false;
  }
  const int height = undo.added[0].second.height;
  TransactionGuard txn(db_);
  if (!txn.Begin(error)) return false;
  if (!PutBlockAtHeight(block, work, height, error)) return false;
  for (const auto& entry : undo.spent) {
    if (!SpendCoin(entry.first, error)) return false;
  }
  for (const auto& entry : undo.added) {
    if (!PutCoin(entry.first, entry.second, error)) return false;
  }
  {
    Database::Statement stmt;
    if (!stmt.Prepare(db_,
                      "INSERT OR REPLACE INTO undo(hash, data) "
                      "VALUES (?1, ?2);",
                      error)) {
      return false;
    }
    stmt.BindBlob(1, HashToBlob(block.GetHash()));
    stmt.BindBlob(2, SerializeBlockUndo(undo));
    stmt.Step();
    if (stmt.HasError()) {
      error = "database error storing the undo record";
      return false;
    }
  }
  if (!SaveTip(block.GetHash(), error)) return false;
  if (!txn.Commit(error)) return false;
  return true;
}

bool ChainStore::GetUndo(const Hash256& hash, BlockUndo& undo,
                         std::string& error) {
  Database::Statement stmt;
  if (!stmt.Prepare(db_, "SELECT data FROM undo WHERE hash=?1;", error)) {
    return false;
  }
  stmt.BindBlob(1, HashToBlob(hash));
  if (!stmt.Step()) {
    if (stmt.HasError()) {
      error = "database error reading the undo record";
      return false;
    }
    error = "no undo record";
    return false;
  }
  const std::vector<unsigned char> data = stmt.ColumnBlob(0);
  if (!DeserializeBlockUndo(data, undo)) {
    error = "stored undo record is corrupt";
    return false;
  }
  return true;
}

bool ChainStore::DisconnectBlock(const Hash256& hash, std::string& error) {
  Hash256 tip;
  if (!LoadTip(tip, error)) return false;
  if (tip != hash) {
    error = "can only disconnect the tip block";
    return false;
  }
  BlockHeader header;
  if (!GetBlockHeader(hash, header, error)) return false;
  BlockUndo undo;
  if (!GetUndo(hash, undo, error)) return false;
  TransactionGuard txn(db_);
  if (!txn.Begin(error)) return false;
  for (const auto& entry : undo.added) {
    if (!SpendCoin(entry.first, error)) return false;
  }
  for (const auto& entry : undo.spent) {
    if (!PutCoin(entry.first, entry.second, error)) return false;
  }
  {
    Database::Statement stmt;
    if (!stmt.Prepare(db_, "DELETE FROM blocks WHERE hash=?1;", error)) {
      return false;
    }
    stmt.BindBlob(1, HashToBlob(hash));
    stmt.Step();
    if (stmt.HasError()) {
      error = "database error deleting the block row";
      return false;
    }
  }
  {
    Database::Statement stmt;
    if (!stmt.Prepare(db_, "DELETE FROM undo WHERE hash=?1;", error)) {
      return false;
    }
    stmt.BindBlob(1, HashToBlob(hash));
    stmt.Step();
    if (stmt.HasError()) {
      error = "database error deleting the undo record";
      return false;
    }
  }
  if (!SaveTip(header.hashPrevBlock, error)) return false;
  if (!txn.Commit(error)) return false;
  return true;
}

bool ChainStore::PruneBlocks(int keepFromHeight, std::string& error) {
  TransactionGuard txn(db_);
  if (!txn.Begin(error)) return false;
  {
    Database::Statement stmt;
    if (!stmt.Prepare(db_,
                      "UPDATE blocks SET body=NULL WHERE height < ?1;",
                      error)) {
      return false;
    }
    stmt.BindInt64(1, keepFromHeight);
    stmt.Step();
    if (stmt.HasError()) {
      error = "database error pruning blocks";
      return false;
    }
  }
  {
    Database::Statement stmt;
    if (!stmt.Prepare(db_,
                      "INSERT OR REPLACE INTO meta(key, value) "
                      "VALUES ('prune_height', ?1);",
                      error)) {
      return false;
    }
    std::vector<unsigned char> value(4);
    value[0] = static_cast<unsigned char>(keepFromHeight & 0xff);
    value[1] = static_cast<unsigned char>((keepFromHeight >> 8) & 0xff);
    value[2] = static_cast<unsigned char>((keepFromHeight >> 16) & 0xff);
    value[3] = static_cast<unsigned char>((keepFromHeight >> 24) & 0xff);
    stmt.BindBlob(1, value);
    stmt.Step();
    if (stmt.HasError()) {
      error = "database error saving prune height";
      return false;
    }
  }
  if (!txn.Commit(error)) return false;
  return true;
}

bool ChainStore::LoadPruneHeight(int& height, std::string& error) {
  Database::Statement stmt;
  if (!stmt.Prepare(db_, "SELECT value FROM meta WHERE key='prune_height';",
                    error)) {
    return false;
  }
  if (!stmt.Step()) {
    if (stmt.HasError()) {
      error = "database error loading prune height";
      return false;
    }
    height = -1;  // no pruning yet
    return true;
  }
  std::vector<unsigned char> value = stmt.ColumnBlob(0);
  if (value.size() != 4) {
    error = "stored prune height has wrong size";
    return false;
  }
  height = static_cast<int>(value[0]) | (static_cast<int>(value[1]) << 8) |
           (static_cast<int>(value[2]) << 16) |
           (static_cast<int>(value[3]) << 24);
  return true;
}

bool ChainStore::PutPeer(const std::string& address, uint32_t lastSeen,
                         uint64_t services, std::string& error) {
  Database::Statement stmt;
  if (!stmt.Prepare(db_,
                    "INSERT OR REPLACE INTO peers"
                    "(address, last_seen, services) VALUES (?1, ?2, ?3);",
                    error)) {
    return false;
  }
  stmt.BindText(1, address);
  stmt.BindInt64(2, lastSeen);
  stmt.BindInt64(3, static_cast<int64_t>(services));
  stmt.Step();
  if (stmt.HasError()) {
    error = "database error writing peer";
    return false;
  }
  return true;
}

bool ChainStore::GetPeers(
    std::vector<std::pair<std::string, uint32_t>>& out, std::string& error) {
  Database::Statement stmt;
  if (!stmt.Prepare(db_, "SELECT address, last_seen FROM peers;", error)) {
    return false;
  }
  out.clear();
  while (stmt.Step()) {
    out.push_back({stmt.ColumnText(0),
                   static_cast<uint32_t>(stmt.ColumnInt64(1))});
  }
  if (stmt.HasError()) {
    error = "database error listing peers";
    return false;
  }
  return true;
}

bool ChainStore::PutBan(const std::string& address, uint32_t until,
                        int offences, std::string& error) {
  Database::Statement stmt;
  if (!stmt.Prepare(db_,
                    "INSERT OR REPLACE INTO bans"
                    "(address, until, offences) VALUES (?1, ?2, ?3);",
                    error)) {
    return false;
  }
  stmt.BindText(1, address);
  stmt.BindInt64(2, until);
  stmt.BindInt64(3, offences);
  stmt.Step();
  if (stmt.HasError()) {
    error = "database error writing ban";
    return false;
  }
  return true;
}

bool ChainStore::GetBans(
    std::vector<std::tuple<std::string, uint32_t, int>>& out,
    std::string& error) {
  Database::Statement stmt;
  if (!stmt.Prepare(db_, "SELECT address, until, offences FROM bans;",
                    error)) {
    return false;
  }
  out.clear();
  while (stmt.Step()) {
    out.push_back({stmt.ColumnText(0),
                   static_cast<uint32_t>(stmt.ColumnInt64(1)),
                   static_cast<int>(stmt.ColumnInt64(2))});
  }
  if (stmt.HasError()) {
    error = "database error listing bans";
    return false;
  }
  return true;
}

bool ChainStore::ClearBan(const std::string& address, std::string& error) {
  Database::Statement stmt;
  if (!stmt.Prepare(db_, "DELETE FROM bans WHERE address=?1;", error)) {
    return false;
  }
  stmt.BindText(1, address);
  stmt.Step();
  if (stmt.HasError()) {
    error = "database error clearing ban";
    return false;
  }
  return true;
}

}  // namespace oeeoo
