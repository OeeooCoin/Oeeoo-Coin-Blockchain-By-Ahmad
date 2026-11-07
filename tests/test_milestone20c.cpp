// Tests: Milestone 20C — OOC-003 wallet file permissions (0600).
// The wallet file holds the encrypted mnemonic, so every save path
// must create it owner-only (0600) regardless of the process umask,
// including when a pre-existing file had wider permissions, and the
// atomic tmp + rename path must end 0600 with no .tmp left behind.
// The tx journal (same vulnerability class, wallet-adjacent) is
// covered in the last section.
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "test_harness.h"
#include "wallet/nodewallet.h"
#include "wallet/txjournal.h"
#include "wallet/wallet.h"
#include "wallet/walletfile.h"

using namespace oeeoo;

namespace {
constexpr const char* kPassphrase = "correct horse battery staple";
constexpr const char* kNewPassphrase = "new passphrase for m20c";

std::string ZeroMnemonic() {
  std::string m;
  for (int i = 0; i < 23; ++i) m += "abandon ";
  m += "art";
  return m;
}

std::string TestDir() {
  return (std::filesystem::temp_directory_path() /
          ("oeeoo-test-m20c-" +
           std::to_string(static_cast<long long>(::getpid()))))
      .string();
}

// Restores the previous umask on scope exit, even when a CHECK in
// the scope fails (CHECK never aborts, but the guard makes the
// restore unconditional anyway).
struct UmaskGuard {
  explicit UmaskGuard(mode_t newMask) : old_(::umask(newMask)) {}
  ~UmaskGuard() { ::umask(old_); }
  UmaskGuard(const UmaskGuard&) = delete;
  UmaskGuard& operator=(const UmaskGuard&) = delete;
  mode_t old_;
};

mode_t FileMode(const std::string& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return static_cast<mode_t>(-1);
  return st.st_mode & 0777;
}

bool FileExists(const std::string& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

// Create a file at `path` with exactly `mode` permissions, so the
// save-over-it cases start from a known wide-permission state.
void CreateFileWithMode(const std::string& path, mode_t mode) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return;
  std::fwrite("placeholder", 1, 11, f);
  std::fclose(f);
  ::chmod(path.c_str(), mode);
}

WalletFileData ZeroWalletData() {
  WalletFileData data;
  data.mnemonic = ZeroMnemonic();
  data.nextExternal = 7;
  data.nextChange = 3;
  return data;
}

std::vector<WalletTxRecord> SampleRecords() {
  WalletTxRecord r;
  r.txid = std::string(64, 'a');
  r.category = "receive";
  r.amount = 50LL * 100000000LL;
  r.fee = 0;
  r.time = 1700000000;
  r.blockheight = 100;
  r.address = "7L88G1Not1Gv8nmci9mchhoN9XgbXTXqX5";
  r.label = "";
  return {r};
}
}  // namespace

void TestMilestone20C() {
  const std::string dir = TestDir();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::string walletPath = dir + "/wallet.oeeoo";
  const std::string journalPath = dir + "/journal.txjournal";

  test::BeginSection(
      "milestone20c: SaveWalletFile creates 0600 files (umask 022)");
  {
    // (a) Fresh save under the common permissive umask 022: the old
    // fopen("wb") code produced 0644 here. The mode must be exactly
    // 0600 — no group/other bits at all.
    UmaskGuard guard(022);
    std::string error;
    CHECK(SaveWalletFile(walletPath, ZeroWalletData(), kPassphrase,
                         error));
    CHECK(FileMode(walletPath) == 0600);
    // (e, part 1) The 0600 file still round-trips: save -> load ->
    // mnemonic matches, indices intact.
    WalletFileData loaded;
    CHECK(LoadWalletFile(walletPath, kPassphrase, loaded, error));
    CHECK(loaded.mnemonic == ZeroMnemonic());
    CHECK(loaded.nextExternal == 7);
    CHECK(loaded.nextChange == 3);
  }

  test::BeginSection(
      "milestone20c: SaveWalletFile over an existing file keeps 0600 "
      "(umask 000)");
  {
    // (b) Save again over the same path (the replacement path) under
    // the most permissive umask 000, where fopen("wb") would have
    // produced 0666 for a new file and left a pre-existing file's
    // 0644 mode untouched (open with O_TRUNC does not change the
    // mode of an existing file — hence the explicit fchmod).
    const std::string path2 = dir + "/wallet2.oeeoo";
    {
      UmaskGuard guard(000);
      std::string error;
      CHECK(SaveWalletFile(path2, ZeroWalletData(), kPassphrase, error));
      CHECK(FileMode(path2) == 0600);
      CHECK(SaveWalletFile(path2, ZeroWalletData(), kPassphrase, error));
      CHECK(FileMode(path2) == 0600);
    }
    // Direct-save replacement over a pre-existing 0644 file: the
    // result must be 0600, not the file's old 0644.
    const std::string path3 = dir + "/wallet3.oeeoo";
    CreateFileWithMode(path3, 0644);
    CHECK(FileMode(path3) == 0644);  // precondition: wide before save
    {
      UmaskGuard guard(022);
      std::string error;
      CHECK(SaveWalletFile(path3, ZeroWalletData(), kPassphrase, error));
    }
    CHECK(FileMode(path3) == 0600);
    WalletFileData loaded;
    std::string error;
    CHECK(LoadWalletFile(path3, kPassphrase, loaded, error));
    CHECK(loaded.mnemonic == ZeroMnemonic());
  }

  test::BeginSection(
      "milestone20c: atomic save over a pre-existing 0644 file ends "
      "0600 (umask 022)");
  {
    // (c) The NodeWallet atomic path (tmp + rename): create a real
    // wallet, widen its file to 0644 as if it pre-dated the fix,
    // then save over it through the atomic path and assert 0600.
    const std::string path = dir + "/atomic.oeeoo";
    std::string error;
    std::string mnemonic;
    {
      NodeWallet nw;
      CHECK(nw.CreateFile(path, kPassphrase, mnemonic, error));
      CHECK(FileMode(path) == 0600);
      CHECK(!FileExists(path + ".tmp"));  // (d) no tmp left behind
    }
    CHECK(::chmod(path.c_str(), 0644) == 0);
    CHECK(FileMode(path) == 0644);  // precondition: wide before save
    {
      UmaskGuard guard(022);
      NodeWallet nw;
      CHECK(nw.LoadFile(path, error));
      CHECK(nw.Unlock(kPassphrase, 0, error));
      // ChangePassphrase is the reachable atomic-save entry:
      // it re-encrypts and saves via SaveWalletFileAtomic.
      CHECK(nw.ChangePassphrase(kPassphrase, kNewPassphrase, error));
    }
    CHECK(FileMode(path) == 0600);
    CHECK(!FileExists(path + ".tmp"));  // (d) no tmp left behind
    // (e, part 2) The atomically replaced wallet still opens, with
    // the NEW passphrase (and no longer with the old one).
    {
      NodeWallet nw;
      CHECK(nw.LoadFile(path, error));
      CHECK(!nw.Unlock(kPassphrase, 0, error));
      CHECK(nw.Unlock(kNewPassphrase, 0, error));
      CHECK(nw.GetWallet() != nullptr);
      CHECK(nw.GetWallet()->GetMnemonic() == mnemonic);
    }
  }

  test::BeginSection(
      "milestone20c: atomic save leaves no .tmp behind (umask 022)");
  {
    // (d) Post-save absence of the tmp file on both atomic entry
    // points exercised here (create + passphrase change). What is
    // NOT directly observed: the tmp file's mode mid-save — the
    // save is synchronous and no test hook exists in the code, so
    // the tmp pathname cannot be stat'ed while it exists. What IS
    // directly observed: the tmp file is written by SaveWalletFile
    // itself, the exact function proven to create 0600 files in
    // the first two sections, so the tmp file's mode follows from
    // the same code path rather than from an assumption about the
    // rename. Post-save absence + final 0600 is asserted here.
    const std::string path = dir + "/notmp.oeeoo";
    std::string error;
    std::string mnemonic;
    NodeWallet nw;
    {
      UmaskGuard guard(022);
      CHECK(nw.CreateFile(path, kPassphrase, mnemonic, error));
    }
    CHECK(FileMode(path) == 0600);
    CHECK(!FileExists(path + ".tmp"));
    {
      UmaskGuard guard(022);
      CHECK(nw.Unlock(kPassphrase, 0, error));
      CHECK(nw.ChangePassphrase(kPassphrase, kNewPassphrase, error));
    }
    CHECK(FileMode(path) == 0600);
    CHECK(!FileExists(path + ".tmp"));
  }

  test::BeginSection(
      "milestone20c: tx journal saves 0600 (additional OOC-003 fix)");
  {
    // The journal holds transaction history (txids, addresses,
    // amounts) — no secrets, but privacy-sensitive — and its tmp
    // file had the same fopen("wb") flaw. Same assertions as the
    // wallet file: fresh save 0600 under umask 022, replacement
    // over a pre-existing 0644 file ends 0600, no .tmp remains,
    // and the journal still round-trips.
    {
      UmaskGuard guard(022);
      std::string error;
      CHECK(SaveTxJournal(journalPath, SampleRecords(), error));
    }
    CHECK(FileMode(journalPath) == 0600);
    CHECK(!FileExists(journalPath + ".tmp"));
    CHECK(::chmod(journalPath.c_str(), 0644) == 0);
    CHECK(FileMode(journalPath) == 0644);  // precondition
    {
      UmaskGuard guard(000);
      std::string error;
      CHECK(SaveTxJournal(journalPath, SampleRecords(), error));
    }
    CHECK(FileMode(journalPath) == 0600);
    CHECK(!FileExists(journalPath + ".tmp"));
    std::vector<WalletTxRecord> loaded;
    std::string error;
    CHECK(LoadTxJournal(journalPath, loaded, error));
    CHECK(loaded.size() == 1);
    if (loaded.size() == 1) {
      CHECK(loaded[0] == SampleRecords()[0]);
    }
  }

  std::filesystem::remove_all(dir, ec);
}
