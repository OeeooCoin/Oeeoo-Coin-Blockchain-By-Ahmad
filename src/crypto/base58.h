// Base58 and Base58Check encoding (Bitcoin-style).
// Base58Check = version byte (1) + payload + 4-byte checksum,
// where checksum = first 4 bytes of SHA256d(version + payload).
// Decode verifies the checksum and rejects invalid characters,
// short input, and bad checksums.
#pragma once

#include <string>
#include <vector>

namespace oeeoo {

// Plain Base58 encode (leading zero bytes become '1's).
std::string Base58Encode(const std::vector<unsigned char>& data);
std::string Base58Encode(const unsigned char* data, std::size_t len);

// Plain Base58 decode. Returns false on an invalid character.
bool Base58Decode(const std::string& str, std::vector<unsigned char>& out);

// Base58Check encode: version + payload + checksum.
std::string Base58CheckEncode(unsigned char version,
                              const std::vector<unsigned char>& payload);

// Base58Check decode: verifies the checksum. On success sets
// `version` and `payload` and returns true.
bool Base58CheckDecode(const std::string& str, unsigned char& version,
                       std::vector<unsigned char>& payload);

}  // namespace oeeoo
