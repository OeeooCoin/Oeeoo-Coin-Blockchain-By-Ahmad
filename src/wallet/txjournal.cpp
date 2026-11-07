// SPDX-License-Identifier: MIT
// Oeeoo Coin — wallet transaction journal implementation (Milestone 16).
#include "wallet/txjournal.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <cstdio>
#include <filesystem>

#include <unistd.h>

#include "rpc/json.h"

namespace oeeoo {

std::string TxJournalPathFor(const std::string& walletPath) {
  return walletPath + ".txjournal";
}

namespace {
// Parse one journal line into a record. False + `error` when the
// line is not a well-formed record object.
bool ParseRecordLine(const std::string& line, WalletTxRecord& out,
                     std::string& error) {
  Json j;
  if (!Json::Parse(line, j, error)) {
    error = "bad journal line: " + error;
    return false;
  }
  if (!j.IsObject()) {
    error = "bad journal line: not an object";
    return false;
  }
  const Json* txid = j.Get("txid");
  const Json* category = j.Get("category");
  const Json* amount = j.Get("amount");
  const Json* fee = j.Get("fee");
  const Json* time = j.Get("time");
  const Json* blockheight = j.Get("blockheight");
  const Json* address = j.Get("address");
  const Json* label = j.Get("label");
  if (txid == nullptr || !txid->IsString() || txid->AsString().size() != 64 ||
      category == nullptr || !category->IsString() ||
      (category->AsString() != "send" &&
       category->AsString() != "receive") ||
      amount == nullptr || !amount->IsInt() || fee == nullptr ||
      !fee->IsInt() || time == nullptr || !time->IsInt() ||
      blockheight == nullptr || !blockheight->IsInt() ||
      address == nullptr || !address->IsString() || label == nullptr ||
      !label->IsString()) {
    error = "bad journal line: missing or mistyped field";
    return false;
  }
  out.txid = txid->AsString();
  out.category = category->AsString();
  out.amount = amount->AsInt();
  out.fee = fee->AsInt();
  out.time = time->AsInt();
  out.blockheight = static_cast<int>(blockheight->AsInt());
  out.address = address->AsString();
  out.label = label->AsString();
  return true;
}

Json RecordToJson(const WalletTxRecord& r) {
  Json j = Json::Object();
  j.Set("txid", r.txid);
  j.Set("category", r.category);
  j.Set("amount", r.amount);
  j.Set("fee", r.fee);
  j.Set("time", r.time);
  j.Set("blockheight", static_cast<int64_t>(r.blockheight));
  j.Set("address", r.address);
  j.Set("label", r.label);
  return j;
}

// OOC-003 (Milestone 20), same fix as the wallet file: the journal
// holds transaction history (txids, addresses, amounts, labels) —
// no secrets, but privacy-sensitive — so it gets the same
// owner-only (0600) treatment. open() with mode 0600 is umask-proof
// for a new file, but does not change a pre-existing file's mode,
// so fchmod() explicitly, verify with fstat(), and fail (leaving
// any previous contents intact) rather than write into a file with
// wider permissions. The mode is fixed BEFORE any byte is written,
// and the old contents are truncated only after the mode is
// verified. Platform scope: POSIX only (see walletfile.cpp).
bool OpenPrivateFile(const std::string& path, FILE*& fileOut) {
  fileOut = nullptr;
  // No O_TRUNC here on purpose: truncate only after the mode is
  // verified (below), so a chmod failure leaves the old file intact
  // instead of destroying it.
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT, 0600);
  if (fd < 0) return false;
  if (::fchmod(fd, 0600) != 0) {
    ::close(fd);
    return false;
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || (st.st_mode & 0777) != 0600) {
    ::close(fd);
    return false;
  }
  if (::ftruncate(fd, 0) != 0) {
    ::close(fd);
    return false;
  }
  FILE* f = ::fdopen(fd, "wb");
  if (f == nullptr) {
    ::close(fd);
    return false;
  }
  fileOut = f;
  return true;
}
}  // namespace

bool LoadTxJournal(const std::string& path,
                   std::vector<WalletTxRecord>& out, std::string& error) {
  out.clear();
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    if (ec) {
      error = "cannot access journal file: " + ec.message();
      return false;
    }
    return true;  // missing sidecar: empty journal, not an error
  }
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    error = "cannot read journal file";
    return false;
  }
  // Keyed by txid, latest wins: a later line for the same txid
  // replaces the earlier one in place, never duplicating it.
  std::string line;
  char buffer[4096];
  std::size_t n = 0;
  bool ok = true;
  while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) {
    for (std::size_t i = 0; i < n; ++i) {
      if (buffer[i] == '\n') {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) {
          WalletTxRecord rec;
          if (!ParseRecordLine(line, rec, error)) {
            ok = false;
            break;
          }
          bool replaced = false;
          for (WalletTxRecord& existing : out) {
            if (existing.txid == rec.txid) {
              existing = rec;
              replaced = true;
              break;
            }
          }
          if (!replaced) out.push_back(rec);
        }
        line.clear();
      } else {
        line.push_back(buffer[i]);
      }
    }
    if (!ok) break;
  }
  if (ok && std::ferror(f) != 0) {
    error = "cannot read journal file";
    ok = false;
  }
  if (ok && !line.empty()) {
    // A final line without a trailing newline: still a record.
    if (line.back() == '\r') line.pop_back();
    WalletTxRecord rec;
    if (!ParseRecordLine(line, rec, error)) {
      ok = false;
    } else {
      bool replaced = false;
      for (WalletTxRecord& existing : out) {
        if (existing.txid == rec.txid) {
          existing = rec;
          replaced = true;
          break;
        }
      }
      if (!replaced) out.push_back(rec);
    }
  }
  std::fclose(f);
  if (!ok) out.clear();
  return ok;
}

bool SaveTxJournal(const std::string& path,
                   const std::vector<WalletTxRecord>& records,
                   std::string& error) {
  const std::string tmpPath = path + ".tmp";
  FILE* f = nullptr;
  if (!OpenPrivateFile(tmpPath, f)) {
    error = "cannot write journal file";
    return false;
  }
  bool ok = true;
  for (const WalletTxRecord& r : records) {
    const std::string line = RecordToJson(r).Dump() + "\n";
    if (std::fwrite(line.data(), 1, line.size(), f) != line.size()) {
      ok = false;
      break;
    }
  }
  if (ok && std::fflush(f) != 0) ok = false;
  // fsync the tmp file before renaming, so the rename only ever
  // publishes fully-durable contents.
  if (ok && ::fsync(::fileno(f)) != 0) ok = false;
  if (std::fclose(f) != 0) ok = false;
  if (!ok) {
    error = "cannot write journal file";
    std::error_code ec;
    std::filesystem::remove(tmpPath, ec);
    return false;
  }
  std::error_code ec;
  std::filesystem::rename(tmpPath, path, ec);
  if (ec) {
    error = "cannot replace journal file: " + ec.message();
    std::filesystem::remove(tmpPath, ec);
    return false;
  }
  // The rename carries the tmp file's inode (and its verified 0600
  // mode) onto the final path — verify anyway rather than assume it.
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0 ||
      (st.st_mode & 0777) != 0600) {
    error = "journal file permissions are not 0600 after save";
    return false;
  }
  return true;
}

}  // namespace oeeoo
