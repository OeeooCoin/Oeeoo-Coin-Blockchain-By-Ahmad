// SPDX-License-Identifier: MIT
// Oeeoo Coin — wallet transaction journal, sidecar file (Milestone 16).
//
// The wallet's transaction history lives OUTSIDE the encrypted wallet
// file, in a sibling sidecar `<wallet-path>.txjournal`. The wallet
// file format (PBKDF2-HMAC-SHA512 200k + AES-256-GCM, payload v1) is
// UNCHANGED: migrating it to a payload v2 with a tx-history section
// was rejected as too risky (a format migration on the file that
// holds the mnemonic could strand funds), and the sidecar keeps the
// wallet file untouched.
//
// Format: one JSON object per line (JSON-lines). Each line is a
// complete WalletTxRecord:
//   {"txid":"<64 hex>","category":"send"|"receive","amount":<sats, signed>,
//    "fee":<sats, sends only, 0 for receives>,"time":<unix>,
//    "blockheight":<int, -1 when unconfirmed>,
//    "address":"<counterparty / first mine address>","label":""}
// Amount follows the Bitcoin convention: negative for sends
// (excluding the fee, which is reported separately), positive for
// receives. `confirmations` is NOT stored — it is computed at query
// time from the tip height. `label` is always "" in v0.1.0 (no
// labels). The journal holds no secrets (txids, amounts, addresses
// only), so it is readable while the wallet is locked.
//
// Write discipline: the whole file is rewritten on every save via a
// temporary file + rename (`<path>.tmp` -> `<path>`), the same atomic
// pattern as the wallet file, so a crash mid-save can never leave a
// half-written journal. A single record is never appended
// half-written: there is no append path at all.
//
// Keying / merging: records are keyed by txid. Loading merges
// duplicates with latest-wins (the last line for a txid wins), so a
// record is never duplicated. A re-scan that re-discovers a receive
// overwrites the same txid with the same values.
//
// Migration rule: a wallet file with NO sidecar starts with an EMPTY
// journal — that is not an error. History is rebuilt from ScanUtxo
// (receives) on the next rescan, because every still-unspent receive
// is re-discovered. Sends made before the journal existed are NOT
// recoverable: a send's only trace was the spent inputs, which a
// UTXO scan cannot reconstruct. This limitation is honest and
// permanent for pre-journal sends.
//
// Load strictness: a malformed non-blank line fails the load
// (fail-closed: silently dropping history would be worse than
// refusing to start with a corrupt journal). Blank lines are
// skipped. A missing file is an empty journal, not an error.
#pragma once

#include <string>
#include <vector>

#include "wallet/wallet.h"

namespace oeeoo {

// The sidecar path derives from the wallet path — never a separate
// config value.
std::string TxJournalPathFor(const std::string& walletPath);

// Load the journal at `path` into `out` (replacing its contents).
// A missing file loads as empty and returns true. False + `error`
// on a malformed line or an I/O error.
bool LoadTxJournal(const std::string& path,
                   std::vector<WalletTxRecord>& out, std::string& error);
// Save `records` to `path` atomically (tmp file + rename).
// False + `error` on I/O failure; the old file is left intact.
bool SaveTxJournal(const std::string& path,
                   const std::vector<WalletTxRecord>& records,
                   std::string& error);

}  // namespace oeeoo
