// SPDX-License-Identifier: MIT
// Oeeoo Coin — thin SQLite wrapper (Milestone 7).
// A single SQLite file holds the whole node state. Pragmas:
// journal_mode=WAL (crash-safe + readers don't block writers),
// synchronous=FULL (a committed transaction survives a power loss),
// foreign_keys=ON. Schema version is tracked in PRAGMA user_version
// so future milestones can migrate.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace oeeoo {

class Database {
 public:
  class Statement;  // defined below; forward-declared for PrepareStatement
  Database() = default;
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  // Open (creating) the database file at `path`. Returns false +
  // `error` on failure.
  bool Open(const std::string& path, std::string& error);
  void Close();
  bool IsOpen() const { return db_ != nullptr; }
  const std::string& Path() const { return path_; }

  // Run a statement with no results (CREATE TABLE, PRAGMA, ...).
  bool Execute(const std::string& sql, std::string& error);
  // Used by Statement::Prepare; not part of the public API.
  bool PrepareStatement(const std::string& sql, Statement* stmt,
                        std::string& error);
  bool BeginTransaction(std::string& error);
  bool CommitTransaction(std::string& error);
  bool RollbackTransaction(std::string& error);
  int GetUserVersion(std::string& error);
  bool SetUserVersion(int version, std::string& error);

  // A prepared statement. Bind parameters, Step() through rows,
  // read columns, then Reset() to reuse it.
  class Statement {
   public:
    Statement() = default;
    ~Statement();
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    bool Prepare(Database& db, const std::string& sql, std::string& error);
    void BindBlob(int index, const unsigned char* data, std::size_t len);
    void BindBlob(int index, const std::vector<unsigned char>& data);
    void BindInt64(int index, int64_t value);
    void BindText(int index, const std::string& value);
    // Returns true while a row is available; false when done or on
    // error (check Error()).
    bool Step();
    bool HasError() const { return error_; }
    std::vector<unsigned char> ColumnBlob(int index) const;
    int64_t ColumnInt64(int index) const;
    std::string ColumnText(int index) const;
    bool ColumnIsNull(int index) const;
    void Reset();

   private:
    friend class Database;
    void Attach(sqlite3_stmt* raw);
    sqlite3_stmt* stmt_ = nullptr;
    bool error_ = false;
  };

 private:
  sqlite3* db_ = nullptr;
  std::string path_;
};

// RAII transaction: rolls back on destruction unless Commit() was
// called. Nested use is an error (SQLite has no nested transactions;
// use savepoints for that, out of scope here).
class TransactionGuard {
 public:
  explicit TransactionGuard(Database& db) : db_(db) {}
  ~TransactionGuard();
  bool Begin(std::string& error);
  bool Commit(std::string& error);
  bool Active() const { return active_; }

 private:
  Database& db_;
  bool active_ = false;
};

}  // namespace oeeoo
