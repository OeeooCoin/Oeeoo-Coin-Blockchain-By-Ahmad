// SHA-256 / SHA-256d via OpenSSL EVP.
// No hand-rolled cryptography: OpenSSL is a well-reviewed library.
// SHA256d(data) = SHA256(SHA256(data)) — used for block and tx ids
// and proof-of-work hashing where the protocol specifies it.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oeeoo {

using Hash256 = std::array<unsigned char, 32>;

// FNV-1a hash for Hash256, for unordered_map/unordered_set keys.
struct Hash256Hash {
  std::size_t operator()(const Hash256& h) const {
    std::size_t x = 1469598103934665603ULL;
    for (unsigned char b : h) {
      x ^= b;
      x *= 1099511628211ULL;
    }
    return x;
  }
};

// SHA-256 of `data`. Throws std::runtime_error only if OpenSSL fails.
Hash256 Sha256(const unsigned char* data, std::size_t len);
Hash256 Sha256(const std::vector<unsigned char>& data);
Hash256 Sha256(const std::string& data);

// Double SHA-256: SHA256(SHA256(data)).
Hash256 Sha256d(const unsigned char* data, std::size_t len);
Hash256 Sha256d(const std::vector<unsigned char>& data);
Hash256 Sha256d(const std::string& data);

// Lowercase hex encoding of a 32-byte hash, in byte order produced.
std::string HexStr(const Hash256& hash);

}  // namespace oeeoo
