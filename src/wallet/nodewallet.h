// SPDX-License-Identifier: MIT
// Oeeoo Coin — the node wallet (Milestone 14).
// A wallet file owned by oeeoo-node: locked by default, unlocked
// only through the walletpassphrase RPC.
//
// Security model (Bitcoin-style):
// - The wallet file on disk is encrypted (PBKDF2-HMAC-SHA512, 200,000
//   iterations + AES-256-GCM; see walletfile.h). The node NEVER reads
//   the file passphrase from a CLI flag or a config file: it would be
//   visible in `ps` / on disk. The passphrase travels only over
//   localhost RPC (walletpassphrase, cookie-authenticated), lives in
//   memory only while the wallet is unlocked, and is wiped on lock.
// - At rest (locked) the wallet is watch-only: addresses and balances
//   stay visible (getbalance / listunspent / getwalletinfo work),
//   but nothing can be spent and no new address is derived — deriving
//   one would advance the file's indices without a passphrase in
//   memory to persist them. getnewaddress / sendtoaddress refuse
//   while locked. (Before the wallet has been unlocked at least once
//   since the node started, the locked view is empty: the public
//   keys live inside the encrypted file.)
// - Unlocking decrypts the file, restores the full wallet, and sets
//   an unlock deadline (0 = until walletlock). When the deadline
//   passes the wallet re-locks itself lazily on next use.
// - The BIP39 passphrase of a node wallet is always empty: the file
//   passphrase is the only secret the node handles. A wallet created
//   elsewhere with a non-empty BIP39 passphrase cannot be used by
//   the node (unlock would derive the wrong keys). The node wallet
//   always uses BIP44 account 0 — the file format does not store an
//   account index.
// - Saves are atomic: the file is written to `<path>.tmp` and then
//   renamed over `<path>`, so a crash mid-save cannot leave a
//   half-written wallet file. The format itself is unchanged.
#pragma once

#include <cstdint>
#include <string>

#include "wallet/wallet.h"

namespace oeeoo {

class NodeWallet {
 public:
  NodeWallet() = default;
  // No copying: the object holds secrets.
  NodeWallet(const NodeWallet&) = delete;
  NodeWallet& operator=(const NodeWallet&) = delete;

  // Register a wallet file path (absolute; the caller resolves it).
  // If the file exists, verify it parses as a wallet file and load
  // it LOCKED. If the file does not exist, the path is remembered
  // and HasWallet() stays false until CreateFile() creates it.
  bool LoadFile(const std::string& path, std::string& error);
  // Create a brand-new wallet file: fresh 24-word mnemonic, saved
  // encrypted with `passphrase`. Fails if the file already exists.
  // Returns the mnemonic ONCE in mnemonicOut — the caller must show
  // it to the operator immediately and never store it. The new
  // wallet is left LOCKED.
  bool CreateFile(const std::string& path, const std::string& passphrase,
                  std::string& mnemonicOut, std::string& error);

  bool HasWallet() const { return hasWallet_; }
  // True when locked, or when the unlock deadline has passed (the
  // re-lock happens lazily here). Non-const on purpose.
  bool IsLocked();
  // Unix time the unlock expires: 0 = locked; UINT64_MAX = unlocked
  // until walletlock. Non-const: it re-locks first when expired.
  uint64_t UnlockedUntil();
  // The live wallet: full wallet when unlocked, watch-only view when
  // locked. nullptr when no wallet file is loaded/created yet.
  // Non-const: it re-locks first when the deadline passed, so a
  // caller never gets a full wallet past its deadline.
  Wallet* GetWallet();
  std::string Path() const { return path_; }

  // Unlock with the file passphrase. timeoutSeconds == 0 means
  // "until walletlock". False + error on a wrong passphrase
  // ("incorrect passphrase"), a missing wallet, or an empty
  // passphrase. The file never reveals whether the passphrase or
  // the file is bad; the error says "incorrect passphrase" for
  // both, matching LoadWalletFile.
  bool Unlock(const std::string& passphrase, uint32_t timeoutSeconds,
              std::string& error);
  // Lock now: persist the indices first, then wipe the keys and the
  // file passphrase from memory. The watch-only view stays.
  void Lock();
  // Re-encrypt the file with the current indices, when unlocked and
  // the indices moved since the last save. No-op when locked or when
  // nothing changed. False + error on I/O failure.
  bool SaveIfDirty(std::string& error);
  // Change the file passphrase (Milestone 16). The wallet must be
  // UNLOCKED (Bitcoin requires this too: the new passphrase can
  // only be applied while the mnemonic is in memory). Verifies
  // `oldPassphrase` by decrypting — a wrong old passphrase fails
  // with "incorrect passphrase", the same message as Unlock (the
  // file never reveals whether the passphrase or the file is bad).
  // Re-encrypts with `newPassphrase` and saves atomically (tmp +
  // rename), then updates the in-memory file passphrase. An empty
  // new passphrase is refused. The wallet file format is unchanged.
  bool ChangePassphrase(const std::string& oldPassphrase,
                        const std::string& newPassphrase,
                        std::string& error);
  // The sidecar journal path (<wallet-path>.txjournal). Derives
  // from the wallet path — never a separate config value.
  std::string JournalPath() const;
  // Persist the tx journal when it changed. Works while locked too:
  // the journal holds no secrets, and receives discovered by a
  // locked watch-only rescan must survive a restart. No-op when
  // there is no wallet or nothing changed. False + error on I/O
  // failure.
  bool SaveJournalIfDirty(std::string& error);

 private:
  void RelockIfExpired();  // lazy auto-relock on deadline
  // Load the sidecar journal from disk into the live wallet
  // (missing sidecar = empty journal, not an error). False + error
  // on a corrupt journal.
  bool LoadJournal(std::string& error);
  static uint64_t NowSeconds();

  std::string path_;
  bool hasWallet_ = false;
  Wallet wallet_;
  bool locked_ = true;
  // 0 = locked; UINT64_MAX = unlocked until walletlock; otherwise
  // the unix time the unlock expires.
  uint64_t unlockUntil_ = 0;
  std::string filePassphrase_;  // in memory ONLY while unlocked
  uint32_t savedExternal_ = 0;
  uint32_t savedChange_ = 0;
};

}  // namespace oeeoo
