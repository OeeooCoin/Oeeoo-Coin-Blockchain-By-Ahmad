// SPDX-License-Identifier: MIT
// Oeeoo Coin — BIP32 hierarchical deterministic keys (Milestone 8).
// Derivation is exactly BIP32 (HMAC-SHA512, hardened indexes >=
// 2^31, neutered public derivation). Two Oeeoo choices:
//  - SLIP-0044 coin type is 65535 (provisional, NOT registered —
//    spec item; the code uses it, nothing claims it is assigned).
//  - Extended keys use Oeeoo version bytes (NOT Bitcoin's xprv/xpub),
//    so an Oeeoo key cannot be mistaken for a Bitcoin key:
//    private 0x0488C3E4, public 0x0488B7E4. The resulting prefixes
//    are recorded in docs/MILESTONE8.md after the first test run.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace oeeoo {

constexpr uint32_t kHardenedBit = 0x80000000;
constexpr uint32_t kBip44Purpose = 44;
constexpr uint32_t kOeeooCoinType = 65535;  // provisional, unregistered
constexpr uint32_t kXprvVersion = 0x0488C3E4;
constexpr uint32_t kXpubVersion = 0x0488B7E4;

// One extended key: either a private key (with its public key) or a
// public-only (neutered) key.
struct ExtendedKey {
  uint8_t depth = 0;
  uint32_t parentFingerprint = 0;
  uint32_t childNumber = 0;
  std::array<unsigned char, 32> chainCode{};
  bool isPrivate = false;
  // Private key bytes (32); all zero when public-only.
  std::array<unsigned char, 32> privkey{};
  // Compressed public key (33 bytes); always set.
  std::array<unsigned char, 33> pubkey{};

  bool IsValid() const;
  // First 4 bytes of HASH160(pubkey).
  uint32_t Fingerprint() const;
};

// Master key from a BIP39 seed: HMAC-SHA512(key="Bitcoin seed").
bool MasterKeyFromSeed(const std::vector<unsigned char>& seed,
                       ExtendedKey& out);
// Derive one child. Hardened indexes (i >= 2^31) require a private
// parent. False on invalid keys (e.g. child scalar out of range).
bool DeriveChild(const ExtendedKey& parent, uint32_t index, ExtendedKey& out);
// Derive a path like "m/44'/65535'/0'/0/0" (also accepts "h").
// The path must start with "m" (or "M"). False on bad syntax.
bool DerivePath(const ExtendedKey& master, const std::string& path,
                ExtendedKey& out);
// Public-only copy (neutered): same chain code and pubkey, no privkey.
ExtendedKey Neuter(const ExtendedKey& key);

// Serialize to Base58Check extended-key strings (Oeeoo versions).
std::string SerializePrivate(const ExtendedKey& key);
std::string SerializePublic(const ExtendedKey& key);
// Parse an extended-key string (either version). False on bad
// checksum, wrong version, or inconsistent key material.
bool ParseExtendedKey(const std::string& encoded, ExtendedKey& out);

}  // namespace oeeoo
