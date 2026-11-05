// SPDX-License-Identifier: MIT
// Oeeoo Coin — encrypted wallet file (Milestone 8).
// Format (single file, all binary):
//   magic      12 bytes  "OEEOOWALLET1"
//   salt       16 bytes  random (PBKDF2 salt)
//   nonce      12 bytes  random (AES-GCM nonce)
//   ciphertext variable  AES-256-GCM over the payload
//   tag        16 bytes  GCM authentication tag
// Key: PBKDF2-HMAC-SHA512(passphrase, salt, 200,000 iterations,
// 32 bytes). Payload (version 1): uint32 LE version, uint32 LE
// mnemonic length, mnemonic bytes, uint32 LE next external index,
// uint32 LE next change index.
// A wrong passphrase fails GCM tag verification — the file never
// reveals whether the passphrase or the file is bad, only that
// decryption failed. The file holds the MNEMONIC: whoever holds
// the file AND the passphrase owns the funds. Never share either.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oeeoo {

struct WalletFileData {
  std::string mnemonic;
  uint32_t nextExternal = 0;
  uint32_t nextChange = 0;
};

// Encrypt + write a wallet file. False + `error` on failure.
bool SaveWalletFile(const std::string& path, const WalletFileData& data,
                    const std::string& passphrase, std::string& error);
// Read + decrypt a wallet file. False on a wrong passphrase, a
// tampered file, or an I/O error (`error` says which, without
// distinguishing wrong-passphrase from tampering in the message).
bool LoadWalletFile(const std::string& path, const std::string& passphrase,
                    WalletFileData& data, std::string& error);

}  // namespace oeeoo
