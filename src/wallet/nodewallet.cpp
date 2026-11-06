// SPDX-License-Identifier: MIT
// Oeeoo Coin — the node wallet implementation (Milestone 14).
#include "wallet/nodewallet.h"

#include <algorithm>
#include <ctime>
#include <filesystem>

#include <openssl/crypto.h>

#include "wallet/txjournal.h"
#include "wallet/walletfile.h"

namespace oeeoo {

namespace {
// Save through a temporary file + rename, so a crash mid-save
// cannot leave a half-written wallet file. The format is identical
// to SaveWalletFile; only the write path is atomic.
bool SaveWalletFileAtomic(const std::string& path,
                          const WalletFileData& data,
                          const std::string& passphrase,
                          std::string& error) {
  const std::string tmpPath = path + ".tmp";
  if (!SaveWalletFile(tmpPath, data, passphrase, error)) return false;
  std::error_code ec;
  std::filesystem::rename(tmpPath, path, ec);
  if (ec) {
    error = "cannot replace wallet file: " + ec.message();
    std::filesystem::remove(tmpPath, ec);
    return false;
  }
  return true;
}
}  // namespace

uint64_t NodeWallet::NowSeconds() {
  return static_cast<uint64_t>(std::time(nullptr));
}

void NodeWallet::RelockIfExpired() {
  if (!hasWallet_ || locked_) return;
  if (unlockUntil_ == UINT64_MAX) return;  // until walletlock
  if (NowSeconds() >= unlockUntil_) Lock();
}

bool NodeWallet::IsLocked() {
  RelockIfExpired();
  return !hasWallet_ || locked_;
}

uint64_t NodeWallet::UnlockedUntil() {
  RelockIfExpired();
  if (!hasWallet_ || locked_) return 0;
  return unlockUntil_;
}

Wallet* NodeWallet::GetWallet() {
  RelockIfExpired();
  if (!hasWallet_) return nullptr;
  return &wallet_;
}

std::string NodeWallet::JournalPath() const {
  return TxJournalPathFor(path_);
}

bool NodeWallet::LoadJournal(std::string& error) {
  std::vector<WalletTxRecord> records;
  if (!LoadTxJournal(JournalPath(), records, error)) return false;
  wallet_.SetTxRecords(records);
  return true;
}

bool NodeWallet::SaveJournalIfDirty(std::string& error) {
  if (!hasWallet_) return true;
  if (!wallet_.TxRecordsDirty()) return true;
  if (!SaveTxJournal(JournalPath(), wallet_.ListTxRecords(), error)) {
    return false;
  }
  wallet_.ClearTxRecordsDirty();
  return true;
}

bool NodeWallet::ChangePassphrase(const std::string& oldPassphrase,
                                  const std::string& newPassphrase,
                                  std::string& error) {
  if (!hasWallet_) {
    error = "no wallet file loaded";
    return false;
  }
  if (locked_) {
    error = "wallet is locked";
    return false;
  }
  if (newPassphrase.empty()) {
    error = "passphrase must not be empty";
    return false;
  }
  // Verify the old passphrase by decrypting. A failure says
  // "incorrect passphrase" and never distinguishes a wrong
  // passphrase from a corrupt file, same as Unlock.
  WalletFileData data;
  if (!LoadWalletFile(path_, oldPassphrase, data, error)) {
    error = "incorrect passphrase";
    return false;
  }
  // The live wallet's indices may be newer than the file's (a
  // rescan can advance them); persist the max, as Unlock does.
  data.nextExternal =
      std::max(wallet_.GetNextExternal(), data.nextExternal);
  data.nextChange = std::max(wallet_.GetNextChange(), data.nextChange);
  if (!SaveWalletFileAtomic(path_, data, newPassphrase, error)) {
    OPENSSL_cleanse(data.mnemonic.data(), data.mnemonic.size());
    return false;
  }
  OPENSSL_cleanse(data.mnemonic.data(), data.mnemonic.size());
  filePassphrase_ = newPassphrase;
  savedExternal_ = data.nextExternal;
  savedChange_ = data.nextChange;
  return true;
}

bool NodeWallet::LoadFile(const std::string& path, std::string& error) {
  if (hasWallet_) {
    error = "a wallet is already loaded";
    return false;
  }
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    // Existing file: verify it below.
  } else if (ec) {
    error = "cannot access wallet file: " + ec.message();
    return false;
  } else {
    // No file yet: remember the path and report success. HasWallet()
    // stays false until CreateFile() creates it, so the node can
    // start with --wallet <new-path> and the operator can create the
    // wallet with the createwallet RPC.
    path_ = path;
    return true;
  }
  // Verify the file parses as a wallet file WITHOUT the passphrase:
  // a decryption failure means "valid file, wrong passphrase" (the
  // file is fine, just locked); a format error means the file is
  // not a wallet file at all and the node must refuse it now,
  // not at the first unlock attempt.
  WalletFileData probe;
  if (!LoadWalletFile(path, "", probe, error)) {
    if (error.find("decryption failed") == std::string::npos) {
      return false;  // "too short" / "not an Oeeoo wallet file" / I/O
    }
  }
  path_ = path;
  hasWallet_ = true;
  locked_ = true;
  unlockUntil_ = 0;
  savedExternal_ = 0;
  savedChange_ = 0;
  // The sidecar journal loads alongside the wallet file. A missing
  // sidecar is an empty journal (migration rule, see txjournal.h);
  // a corrupt one fails the load — silently dropping history would
  // be worse than refusing to start.
  if (!LoadJournal(error)) {
    hasWallet_ = false;
    path_.clear();
    return false;
  }
  return true;
}

bool NodeWallet::CreateFile(const std::string& path,
                            const std::string& passphrase,
                            std::string& mnemonicOut, std::string& error) {
  if (hasWallet_) {
    error = "a wallet is already loaded";
    return false;
  }
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    error = "wallet file already exists: " + path;
    return false;
  }
  if (passphrase.empty()) {
    error = "passphrase must not be empty";
    return false;
  }
  Wallet fresh;
  if (!fresh.CreateNew(error)) return false;
  WalletFileData data;
  data.mnemonic = fresh.GetMnemonic();
  data.nextExternal = 0;
  data.nextChange = 0;
  if (!SaveWalletFileAtomic(path, data, passphrase, error)) return false;
  mnemonicOut = data.mnemonic;
  // The local copy is no longer needed once the caller has its own.
  OPENSSL_cleanse(data.mnemonic.data(), data.mnemonic.size());
  path_ = path;
  hasWallet_ = true;
  locked_ = true;
  unlockUntil_ = 0;
  savedExternal_ = 0;
  savedChange_ = 0;
  return true;
}

bool NodeWallet::Unlock(const std::string& passphrase,
                        uint32_t timeoutSeconds, std::string& error) {
  if (!hasWallet_) {
    error = "no wallet file loaded";
    return false;
  }
  if (passphrase.empty()) {
    error = "passphrase must not be empty";
    return false;
  }
  WalletFileData data;
  if (!LoadWalletFile(path_, passphrase, data, error)) {
    error = "incorrect passphrase";
    return false;
  }
  // Preserve the indices the live wallet already has: the
  // watch-only view may have advanced them past the file's values
  // (ScanUtxo moves the window past used addresses). Take the max.
  const uint32_t ext =
      std::max(wallet_.GetNextExternal(), data.nextExternal);
  const uint32_t chg = std::max(wallet_.GetNextChange(), data.nextChange);
  if (!wallet_.RestoreFromMnemonic(data.mnemonic, "", error)) {
    OPENSSL_cleanse(data.mnemonic.data(), data.mnemonic.size());
    return false;
  }
  OPENSSL_cleanse(data.mnemonic.data(), data.mnemonic.size());
  wallet_.SetNextIndices(ext, chg);
  // Reload the journal: the unlock restored a fresh wallet, and
  // the on-disk journal is canonical (a locked rescan may have
  // added receives to it since the last unlock).
  if (!LoadJournal(error)) {
    wallet_.WipeSecrets();  // never leave keys in a "locked" wallet
    filePassphrase_.clear();
    locked_ = true;
    unlockUntil_ = 0;
    return false;
  }
  filePassphrase_ = passphrase;
  locked_ = false;
  unlockUntil_ =
      (timeoutSeconds == 0) ? UINT64_MAX : NowSeconds() + timeoutSeconds;
  savedExternal_ = ext;
  savedChange_ = chg;
  return true;
}

void NodeWallet::Lock() {
  if (!hasWallet_) return;
  // Persist the journal first (it holds no secrets, so it saves
  // even though the wallet is about to lock), then the indices:
  // saving needs the mnemonic and the passphrase, both still in
  // memory. Best effort — the indices are also saved after every
  // mutating RPC, so this is a belt-and-braces save, not the only
  // one.
  std::string error;
  SaveJournalIfDirty(error);
  SaveIfDirty(error);
  wallet_.WipeSecrets();
  if (!filePassphrase_.empty()) {
    OPENSSL_cleanse(filePassphrase_.data(), filePassphrase_.size());
    filePassphrase_.clear();
  }
  locked_ = true;
  unlockUntil_ = 0;
}

bool NodeWallet::SaveIfDirty(std::string& error) {
  if (!hasWallet_) return true;
  // The journal saves even while locked (no secrets in it, and a
  // locked rescan may have discovered receives that must survive
  // a restart). A journal I/O failure is reported, not swallowed.
  if (!SaveJournalIfDirty(error)) return false;
  if (locked_ || filePassphrase_.empty()) return true;
  const uint32_t ext = wallet_.GetNextExternal();
  const uint32_t chg = wallet_.GetNextChange();
  if (ext == savedExternal_ && chg == savedChange_) return true;
  WalletFileData data;
  data.mnemonic = wallet_.GetMnemonic();
  data.nextExternal = ext;
  data.nextChange = chg;
  if (!SaveWalletFileAtomic(path_, data, filePassphrase_, error)) {
    return false;
  }
  OPENSSL_cleanse(data.mnemonic.data(), data.mnemonic.size());
  savedExternal_ = ext;
  savedChange_ = chg;
  return true;
}

}  // namespace oeeoo
