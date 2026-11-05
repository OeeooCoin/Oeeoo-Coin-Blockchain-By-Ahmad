// SPDX-License-Identifier: MIT
// Oeeoo Coin — ECDSA on secp256k1 (Milestone 4).
// Thin wrapper over OpenSSL's EC routines. Keys are 32-byte private
// scalars; public keys are 33-byte compressed points (02/03 prefix).
// Signatures are DER-encoded ECDSA signatures. This module never
// invents cryptography: curve, hashing and DER rules are OpenSSL's.
#pragma once

#include <cstdint>
#include <vector>

#include "crypto/hash160.h"
#include "crypto/sha256.h"

namespace oeeoo {

// A key pair: 32-byte private key, 33-byte compressed public key.
struct KeyPair {
  std::vector<unsigned char> privkey;  // 32 bytes
  std::vector<unsigned char> pubkey;   // 33 bytes, compressed
};

// Generate a fresh random key pair (OS randomness via OpenSSL).
// Returns false only if the OS RNG fails.
bool GenerateKeyPair(KeyPair& out);

// Compressed public key for a 32-byte private key. Empty on bad input.
std::vector<unsigned char> PubKeyFromPrivKey(
    const std::vector<unsigned char>& privkey);

// DER-encode an ECDSA signature of a 32-byte hash with `privkey`.
// Empty on failure.
std::vector<unsigned char> SignHash(
    const Hash256& hash, const std::vector<unsigned char>& privkey);

// Verify a DER signature of a 32-byte hash against a public key.
// `pubkey` must be 33 bytes (compressed) or 65 bytes (uncompressed).
bool VerifySignature(const std::vector<unsigned char>& pubkey,
                     const std::vector<unsigned char>& derSig,
                     const Hash256& hash);

// HASH160 of a public key (used for P2PKH addresses).
Hash160 PubKeyHash160(const std::vector<unsigned char>& pubkey);

}  // namespace oeeoo
