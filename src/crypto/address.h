// P2PKH address encoding foundations (Milestone 2).
// Launch addresses are legacy P2PKH only: Base58Check of
// version byte 0x0f + 20-byte HASH160 of the public key.
// Full wallet derivation (BIP32/BIP39) is Milestone 8; this file
// only covers the encoding, so any key can be turned into an
// address and any address can be validated.
#pragma once

#include <string>
#include <vector>

#include "crypto/hash160.h"

namespace oeeoo {

// P2PKH address for a 20-byte public-key hash.
std::string PubKeyHashToAddress(const Hash160& hash160);

// P2PKH address for a public key (compressed or uncompressed bytes).
std::string PubKeyToAddress(const std::vector<unsigned char>& pubkey);

// Validate a P2PKH address: correct Base58Check, version byte 0x0f,
// 20-byte payload. On success sets `hash160` and returns true.
bool DecodeP2PKHAddress(const std::string& address, Hash160& hash160);

}  // namespace oeeoo
