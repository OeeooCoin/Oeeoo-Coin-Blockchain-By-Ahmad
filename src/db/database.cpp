// SPDX-License-Identifier: MIT
// Oeeoo Coin — SQLite wrapper implementation.
#include "db/database.h"

#include <sqlite3.h>

namespace oeeoo {

Database::~Database() { Close(); }

bool Database::Open(const std::string& path, std::string& error) {
  if (db_ != nullptr) {
    error = "database already open";
    return false;
  }
  if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
    error = db_ != nullptr ? sqlite3_errmsg(db_) : "out of memory";
    if (db_ != nullptr) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
    return false;
  }
  path_ = path;
  if (!Execute("PRAGMA journal_mode=WAL;", error)) return false;
  if (!Execute("PRAGMA synchronous=FULL;", error)) return false;
  if (!Execute("PRAGMA foreign_keys=ON;", error)) return false;
  return true;
}

void Database::Close() {
  if (db_ != nullptr) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

bool Database::Execute(const std::string& sql, std::string& error) {
  char* msg = nullptr;
  if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &msg) != SQLITE_OK) {
    error = msg != nullptr ? msg : sqlite3_errmsg(db_);
    if (msg != nullptr) sqlite3_free(msg);
    return false;
  }
  return true;
}

bool Database::BeginTransaction(std::string& error) {
  return Execute("BEGIN IMMEDIATE;", error);
}

bool Database::CommitTransaction(std::string& error) {
  return Execute("COMMIT;", error);
}

bool Database::RollbackTransaction(std::string& error) {
  return Execute("ROLLBACK;", error);
}

int Database::GetUserVersion(std::string& error) {
  Statement stmt;
  if (!stmt.Prepare(*this, "PRAGMA user_version;", error)) return -1;
  if (!stmt.Step()) return -1;
  return static_cast<int>(stmt.ColumnInt64(0));
}

bool Database::SetUserVersion(int version, std::string& error) {
  return Execute("PRAGMA user_version=" + std::to_string(version) + ";",
                 error);
}

Database::Statement::~Statement() {
  if (stmt_ != nullptr) sqlite3_finalize(stmt_);
}

bool Database::Statement::Prepare(Database& db, const std::string& sql,
                                  std::string& error) {
  if (stmt_ != nullptr) sqlite3_finalize(stmt_);
  stmt_ = nullptr;
  error_ = false;
  return db.PrepareStatement(sql, this, error);
}

bool Database::PrepareStatement(const std::string& sql, Statement* stmt,
                                std::string& error) {
  sqlite3_stmt* raw = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) {
    error = sqlite3_errmsg(db_);
    return false;
  }
  stmt->Attach(raw);
  return true;
}

void Database::Statement::Attach(sqlite3_stmt* raw) {
  if (stmt_ != nullptr) sqlite3_finalize(stmt_);
  stmt_ = raw;
  error_ = false;
}

void Database::Statement::BindBlob(int index, const unsigned char* data,
                                   std::size_t len) {
  sqlite3_bind_blob(stmt_, index, data, static_cast<int>(len),
                    SQLITE_TRANSIENT);
}

void Database::Statement::BindBlob(int index,
                                   const std::vector<unsigned char>& data) {
  BindBlob(index, data.data(), data.size());
}

void Database::Statement::BindInt64(int index, int64_t value) {
  sqlite3_bind_int64(stmt_, index, static_cast<sqlite3_int64>(value));
}

void Database::Statement::BindText(int index, const std::string& value) {
  sqlite3_bind_text(stmt_, index, value.c_str(),
                    static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

bool Database::Statement::Step() {
  const int rc = sqlite3_step(stmt_);
  if (rc == SQLITE_ROW) return true;
  if (rc == SQLITE_DONE) return false;
  error_ = true;
  return false;
}

std::vector<unsigned char> Database::Statement::ColumnBlob(int index) const {
  const void* data = sqlite3_column_blob(stmt_, index);
  const int len = sqlite3_column_bytes(stmt_, index);
  if (data == nullptr || len <= 0) return {};
  const unsigned char* bytes = static_cast<const unsigned char*>(data);
  return std::vector<unsigned char>(bytes, bytes + len);
}

int64_t Database::Statement::ColumnInt64(int index) const {
  return static_cast<int64_t>(sqlite3_column_int64(stmt_, index));
}

std::string Database::Statement::ColumnText(int index) const {
  const unsigned char* text = sqlite3_column_text(stmt_, index);
  if (text == nullptr) return {};
  return std::string(reinterpret_cast<const char*>(text));
}

bool Database::Statement::ColumnIsNull(int index) const {
  return sqlite3_column_type(stmt_, index) == SQLITE_NULL;
}

void Database::Statement::Reset() {
  sqlite3_reset(stmt_);
  sqlite3_clear_bindings(stmt_);
  error_ = false;
}

TransactionGuard::~TransactionGuard() {
  if (active_) {
    std::string ignored;
    db_.RollbackTransaction(ignored);
  }
}

bool TransactionGuard::Begin(std::string& error) {
  if (!db_.BeginTransaction(error)) return false;
  active_ = true;
  return true;
}

bool TransactionGuard::Commit(std::string& error) {
  if (!active_) {
    error = "no active transaction";
    return false;
  }
  if (!db_.CommitTransaction(error)) return false;
  active_ = false;
  return true;
}

}  // namespace oeeoo
