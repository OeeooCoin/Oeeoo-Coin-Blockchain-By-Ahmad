// HASH160 = RIPEMD-160(SHA-256(data)).
// Used for P2PKH addresses: the 20-byte hash of a public key.
// Implemented with OpenSSL EVP (SHA-256) and the OpenSSL legacy
// provider for RIPEMD-160 (deprecated in OpenSSL 3 but still the
// reviewed implementation; no hand-rolled RIPEMD-160).
#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace oeeoo {

using Hash160 = std::array<unsigned char, 20>;

Hash160 ComputeHash160(const unsigned char* data, std::size_t len);
Hash160 ComputeHash160(const std::vector<unsigned char>& data);
Hash160 ComputeHash160(const std::string& data);

std::string HexStr(const Hash160& hash);

}  // namespace oeeoo
