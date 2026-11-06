// SPDX-License-Identifier: MIT
// Oeeoo Coin — libFuzzer target: encrypted wallet file loader
// (Milestone 18). The input bytes are written to one scratch
// file (reused across runs, pid-unique so parallel fuzzers do
// not share it) and loaded with the corpus passphrase
// "fuzz-corpus-passphrase". LoadWalletFile checks the 12-byte
// magic BEFORE the 200,000-iteration PBKDF2, so random inputs
// are cheap; only inputs keeping the magic pay the KDF, which
// is why this target runs with far fewer iterations than the
// parser targets (see CMakeLists.txt). Seed corpus:
// tests/fuzz/corpus/wallet/ (a valid file produced by
// tests/fuzz/gen_corpus.cpp with the same passphrase).
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <unistd.h>

#include "wallet/walletfile.h"

using namespace oeeoo;

namespace {
std::string ScratchPath() {
  static const std::string path =
      "/tmp/oeeoo-fuzz-wallet-" +
      std::to_string(static_cast<long>(::getpid())) + ".oeeoo";
  return path;
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Wallet files are small; cap the input so a huge libFuzzer
  // mutation does not turn into a huge file write per run.
  if (size > 1024 * 1024) return 0;
  const std::string path = ScratchPath();
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return 0;
  if (size > 0) {
    std::fwrite(data, 1, size, f);
  }
  std::fclose(f);
  WalletFileData loaded;
  std::string error;
  if (LoadWalletFile(path, "fuzz-corpus-passphrase", loaded, error)) {
    // A successfully decrypted file must have a sane payload:
    // the mnemonic is printable words, indexes are small.
    if (loaded.mnemonic.empty() || loaded.mnemonic.size() > 1024) {
      __builtin_trap();
    }
  }
  return 0;
}
