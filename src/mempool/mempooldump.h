// SPDX-License-Identifier: MIT
// Oeeoo Coin — mempool persistence (Milestone 17, Bitcoin's
// `mempool.dat` model).
//
// The mempool (transactions + fee + added time, NOT the UTXO set)
// is saved to `<dataDir>/mempool.dat` on clean shutdown and on
// demand via the `savemempool` RPC, and reloaded on start. The
// reload re-validates every transaction against the current UTXO
// set and drops anything invalid or expired.
//
// Format (Oeeoo's own — this is NOT Bitcoin Core's binary
// `mempool.dat` format, which stores a serialized dump with
// Core-specific fields; the model is the same, the bytes are
// ours): JSON-lines, one object per line, UTF-8.
//
//   Line 1 (header, required):
//     {"format":"oeeoo-mempool","version":1}
//   One line per entry, highest fee rate first:
//     {"txid":"<64 hex>","tx":"<hex of the serialized transaction>",
//      "fee":<satoshis, integer>,"time":<unix seconds, addedTime>}
//
// The stored fee is checked against the fee recomputed during
// re-validation; a mismatch drops the entry (a tampered file must
// not inject a transaction with a wrong fee). The stored txid must
// match the deserialized transaction's txid. `time` is the
// entry's ORIGINAL added time, so a reloaded entry keeps its
// remaining lifetime (it does not get a fresh 14 days).
//
// Write discipline: the whole file is rewritten on every save via
// a temporary file + fsync + rename (`<path>.new` -> `<path>`),
// the same atomic pattern as the M16 wallet journal sidecar (which
// uses `<path>.tmp`). A crash mid-save can never leave a
// half-written mempool file; at worst the previous good file
// survives. The directory itself is not fsynced (same as the
// journal): on a power loss immediately after the rename, some
// filesystems could lose the new file — the node then starts with
// an empty mempool, which is safe, never corrupt.
//
// Load discipline (best-effort, never fatal — losing the mempool
// loses no funds, so a bad file must never refuse startup):
//   - a missing file loads as empty (not an error);
//   - a malformed header fails the whole load (false + error),
//     the mempool stays as it was;
//   - a malformed entry line drops just that entry and the load
//     continues;
//   - an entry older than 14 days (Mempool::kExpirySeconds) is
//     dropped as expired;
//   - every other entry is re-validated against the current UTXO
//     set (fee, dust, script, conflicts, RBF rules — exactly the
//     same checks as a fresh mempool admission) and dropped when
//     it fails. Entries are added in topological order (a parent
//     before any child spending its outputs), computed from the
//     file itself, so a chain of unconfirmed transactions reloads
//     whole; a cycle in the file is dropped as invalid.
// The caller learns what happened from MempoolLoadStats.
//
// Size bound (Oeeoo's own choice): a save writes at most
// kMempoolDumpMaxEntries entries and at most kMempoolDumpMaxBytes
// of file, highest fee rate first — the most valuable
// transactions survive the cap. The file is never unbounded: even
// a full 300 MB mempool produces at most a 64 MiB file.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "consensus/vesting.h"
#include "mempool/mempool.h"
#include "utxo/coins.h"

namespace oeeoo {

// Save cap (Oeeoo's own choice, see the header comment): at most
// this many entries, at most this many file bytes, highest fee
// rate first. Hex encoding roughly doubles the raw transaction
// size, so 64 MiB of file holds ~29 MB of raw transactions.
constexpr std::size_t kMempoolDumpMaxEntries = 100000;
constexpr std::size_t kMempoolDumpMaxBytes = 64ULL * 1024 * 1024;

// What a load did. `loaded` + the drop counts always add up to
// the number of entry lines in the file (a header-only file loads
// zero entries with zero drops).
struct MempoolLoadStats {
  std::size_t loaded = 0;
  std::size_t droppedExpired = 0;
  std::size_t droppedInvalid = 0;
};

// Save `mempool` to `path` atomically (tmp file + fsync +
// rename). False + `error` on I/O failure; the old file is left
// intact. An empty mempool writes a header-only file.
bool SaveMempoolFile(const std::string& path, const Mempool& mempool,
                     std::string& error);

// Load the mempool file at `path` into `mempool`, re-validating
// every entry against `utxo` at `height` / `medianTimePast` with
// `nowSeconds` as the validation clock (see the header comment
// for the exact rules). A missing file loads as empty and returns
// true. False + `error` only when the file cannot be read at all
// or its header is malformed; per-entry problems are counted in
// `stats`, never fatal. `stats` is cleared on entry.
bool LoadMempoolFile(const std::string& path, Mempool& mempool,
                     const CoinsView& utxo, int height,
                     uint32_t medianTimePast, uint32_t nowSeconds,
                     MempoolLoadStats& stats, std::string& error,
                     const VestingParams& vesting = VestingParams{});

}  // namespace oeeoo
