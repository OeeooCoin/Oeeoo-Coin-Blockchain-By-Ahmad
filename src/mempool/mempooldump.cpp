// SPDX-License-Identifier: MIT
// Oeeoo Coin — mempool persistence implementation (Milestone 17).
#include "mempool/mempooldump.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include <unistd.h>

#include "rpc/json.h"
#include "rpc/rpcutil.h"

namespace oeeoo {

namespace {

// One parsed entry line (a transaction still to be admitted).
struct DumpRecord {
  Hash256 txid;
  Transaction tx;
  Amount fee = 0;
  uint32_t addedTime = 0;
};

bool ParseHeaderLine(const std::string& line, std::string& error) {
  Json j;
  if (!Json::Parse(line, j, error)) {
    error = "bad mempool file header: " + error;
    return false;
  }
  const Json* format = j.Get("format");
  const Json* version = j.Get("version");
  if (!j.IsObject() || format == nullptr || !format->IsString() ||
      format->AsString() != "oeeoo-mempool" || version == nullptr ||
      !version->IsInt() || version->AsInt() != 1) {
    error = "bad mempool file header: not an oeeoo-mempool version 1 file";
    return false;
  }
  return true;
}

// Parse one entry line. False when the line is not a well-formed
// record (caller counts it as dropped-invalid and continues).
bool ParseRecordLine(const std::string& line, DumpRecord& out,
                     std::string& error) {
  Json j;
  if (!Json::Parse(line, j, error)) {
    error = "bad mempool entry: " + error;
    return false;
  }
  if (!j.IsObject()) {
    error = "bad mempool entry: not an object";
    return false;
  }
  const Json* txid = j.Get("txid");
  const Json* txHex = j.Get("tx");
  const Json* fee = j.Get("fee");
  const Json* time = j.Get("time");
  if (txid == nullptr || !txid->IsString() ||
      txid->AsString().size() != 64 || txHex == nullptr ||
      !txHex->IsString() || txHex->AsString().empty() || fee == nullptr ||
      !fee->IsInt() || fee->AsInt() < 0 || time == nullptr ||
      !time->IsInt() || time->AsInt() < 0 ||
      time->AsInt() > 0xffffffffLL) {
    error = "bad mempool entry: missing or mistyped field";
    return false;
  }
  std::vector<unsigned char> bytes;
  if (!HexToBytesStr(txHex->AsString(), bytes)) {
    error = "bad mempool entry: tx is not valid hex";
    return false;
  }
  Transaction tx;
  if (!Transaction::Deserialize(bytes, tx)) {
    error = "bad mempool entry: tx does not deserialize";
    return false;
  }
  std::vector<unsigned char> idBytes;
  if (!HexToBytesStr(txid->AsString(), idBytes) || idBytes.size() != 32) {
    error = "bad mempool entry: txid is not 32 bytes of hex";
    return false;
  }
  Hash256 id;
  std::copy(idBytes.begin(), idBytes.end(), id.begin());
  if (tx.GetTxId() != id) {
    error = "bad mempool entry: txid does not match the transaction";
    return false;
  }
  out.txid = id;
  out.tx = std::move(tx);
  out.fee = static_cast<Amount>(fee->AsInt());
  out.addedTime = static_cast<uint32_t>(time->AsInt());
  return true;
}

// Read the whole file into lines (without trailing newlines).
// False + `error` on an I/O error.
bool ReadLines(const std::string& path, std::vector<std::string>& lines,
               std::string& error) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    error = "cannot read mempool file";
    return false;
  }
  std::string line;
  char buffer[8192];
  std::size_t n = 0;
  bool ok = true;
  while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) {
    for (std::size_t i = 0; i < n; ++i) {
      if (buffer[i] == '\n') {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        line.clear();
      } else {
        line.push_back(buffer[i]);
      }
    }
  }
  if (std::ferror(f) != 0) {
    error = "cannot read mempool file";
    ok = false;
  }
  if (ok && !line.empty()) {
    if (line.back() == '\r') line.pop_back();
    lines.push_back(line);
  }
  std::fclose(f);
  return ok;
}

}  // namespace

bool SaveMempoolFile(const std::string& path, const Mempool& mempool,
                     std::string& error) {
  // Highest fee rate first, so the caps below keep the most
  // valuable transactions.
  const std::vector<Mempool::Entry> entries = mempool.GetByFeeRate();
  const std::string tmpPath = path + ".new";
  FILE* f = std::fopen(tmpPath.c_str(), "wb");
  if (f == nullptr) {
    error = "cannot write mempool file";
    return false;
  }
  bool ok = true;
  std::size_t writtenBytes = 0;
  const auto writeLine = [&](const std::string& line) {
    if (!ok) return;
    if (std::fwrite(line.data(), 1, line.size(), f) != line.size()) {
      ok = false;
      return;
    }
    writtenBytes += line.size();
  };
  Json header = Json::Object();
  header.Set("format", "oeeoo-mempool");
  header.Set("version", static_cast<int64_t>(1));
  writeLine(header.Dump() + "\n");
  std::size_t writtenEntries = 0;
  for (const Mempool::Entry& e : entries) {
    if (!ok) break;
    if (writtenEntries >= kMempoolDumpMaxEntries) break;
    Json j = Json::Object();
    const Hash256 txid = e.tx.GetTxId();
    j.Set("txid", BytesToHexStr(std::vector<unsigned char>(txid.begin(),
                                                           txid.end())));
    j.Set("tx", BytesToHexStr(e.tx.Serialize()));
    j.Set("fee", static_cast<int64_t>(e.fee));
    j.Set("time", static_cast<int64_t>(e.addedTime));
    const std::string line = j.Dump() + "\n";
    if (writtenBytes + line.size() > kMempoolDumpMaxBytes) break;
    writeLine(line);
    ++writtenEntries;
  }
  if (ok && std::fflush(f) != 0) ok = false;
  // fsync the tmp file before renaming, so the rename only ever
  // publishes fully-durable contents.
  if (ok && ::fsync(::fileno(f)) != 0) ok = false;
  if (std::fclose(f) != 0) ok = false;
  if (!ok) {
    error = "cannot write mempool file";
    std::error_code ec;
    std::filesystem::remove(tmpPath, ec);
    return false;
  }
  std::error_code ec;
  std::filesystem::rename(tmpPath, path, ec);
  if (ec) {
    error = "cannot replace mempool file: " + ec.message();
    std::filesystem::remove(tmpPath, ec);
    return false;
  }
  return true;
}

bool LoadMempoolFile(const std::string& path, Mempool& mempool,
                     const CoinsView& utxo, int height,
                     uint32_t medianTimePast, uint32_t nowSeconds,
                     MempoolLoadStats& stats, std::string& error,
                     const VestingParams& vesting) {
  stats = MempoolLoadStats{};
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    if (ec) {
      error = "cannot access mempool file: " + ec.message();
      return false;
    }
    return true;  // missing file: empty mempool, not an error
  }
  std::vector<std::string> lines;
  if (!ReadLines(path, lines, error)) return false;
  // The first non-blank line is the header. A missing or
  // malformed header fails the whole load: without a version we
  // cannot trust any line that follows.
  std::size_t first = 0;
  while (first < lines.size() && lines[first].empty()) ++first;
  if (first >= lines.size() ||
      !ParseHeaderLine(lines[first], error)) {
    return false;
  }
  std::vector<DumpRecord> records;
  for (std::size_t i = first + 1; i < lines.size(); ++i) {
    if (lines[i].empty()) continue;
    DumpRecord rec;
    std::string lineError;
    if (!ParseRecordLine(lines[i], rec, lineError)) {
      ++stats.droppedInvalid;
      continue;
    }
    // Expired entries never reach validation: their remaining
    // lifetime is already over (same 14-day rule as the live
    // mempool's expiry sweep).
    if (nowSeconds >= rec.addedTime + Mempool::kExpirySeconds) {
      ++stats.droppedExpired;
      continue;
    }
    records.push_back(std::move(rec));
  }
  if (records.empty()) return true;

  // Topological order: a parent before any child spending its
  // outputs, so a chain of unconfirmed transactions reloads
  // whole in a single pass. Kahn's algorithm over the file's own
  // txids; records left unordered form a cycle and are invalid.
  std::unordered_map<Hash256, std::size_t, Hash256Hash> index;
  index.reserve(records.size());
  for (std::size_t i = 0; i < records.size(); ++i) {
    index.emplace(records[i].txid, i);
  }
  std::vector<std::vector<std::size_t>> children(records.size());
  std::vector<std::size_t> inDegree(records.size(), 0);
  for (std::size_t i = 0; i < records.size(); ++i) {
    std::unordered_set<Hash256, Hash256Hash> parents;
    for (const TxIn& in : records[i].tx.vin) {
      const auto it = index.find(in.prevTxHash);
      if (it == index.end() || it->second == i) continue;
      if (parents.insert(in.prevTxHash).second) {
        children[it->second].push_back(i);
        ++inDegree[i];
      }
    }
  }
  std::queue<std::size_t> ready;
  for (std::size_t i = 0; i < records.size(); ++i) {
    if (inDegree[i] == 0) ready.push(i);
  }
  std::vector<std::size_t> order;
  order.reserve(records.size());
  while (!ready.empty()) {
    const std::size_t i = ready.front();
    ready.pop();
    order.push_back(i);
    for (const std::size_t child : children[i]) {
      if (--inDegree[child] == 0) ready.push(child);
    }
  }
  if (order.size() != records.size()) {
    // A cycle: every record not ordered depends (transitively)
    // on itself. They can never validate, so they are invalid.
    std::unordered_set<std::size_t> ordered(order.begin(), order.end());
    for (std::size_t i = 0; i < records.size(); ++i) {
      if (ordered.count(i) == 0) ++stats.droppedInvalid;
    }
  }
  for (const std::size_t i : order) {
    const DumpRecord& rec = records[i];
    std::string addError;
    if (!mempool.AddLoadedTransaction(rec.tx, utxo, height,
                                      medianTimePast, rec.addedTime,
                                      nowSeconds, addError, vesting)) {
      // The parent may itself have been dropped above (invalid
      // or fee-mismatched), so a "missing input" here is just
      // the child of a dropped parent — still a drop either way.
      ++stats.droppedInvalid;
      continue;
    }
    // The stored fee must match the fee re-validation computed:
    // a tampered file must not inject a transaction with a wrong
    // fee. The fee is recomputed from the UTXO set, so any
    // mismatch means the file does not describe this chain.
    Mempool::Entry entry;
    if (!mempool.GetEntry(rec.txid, entry) || entry.fee != rec.fee) {
      mempool.Remove(rec.txid);
      ++stats.droppedInvalid;
      continue;
    }
    ++stats.loaded;
  }
  return true;
}

}  // namespace oeeoo
