#include "crypto/sha256.h"

#include <openssl/evp.h>

#include <stdexcept>

namespace oeeoo {

namespace {
Hash256 EvpSha256(const unsigned char* data, std::size_t len) {
  Hash256 out{};
  unsigned int out_len = 0;
  // EVP_Digest is the one-shot OpenSSL digest API.
  if (EVP_Digest(data, len, out.data(), &out_len, EVP_sha256(), nullptr) != 1 ||
      out_len != out.size()) {
    throw std::runtime_error("OpenSSL SHA-256 failed");
  }
  return out;
}
}  // namespace

Hash256 Sha256(const unsigned char* data, std::size_t len) {
  return EvpSha256(data, len);
}

Hash256 Sha256(const std::vector<unsigned char>& data) {
  return EvpSha256(data.data(), data.size());
}

Hash256 Sha256(const std::string& data) {
  return EvpSha256(reinterpret_cast<const unsigned char*>(data.data()),
                   data.size());
}

Hash256 Sha256d(const unsigned char* data, std::size_t len) {
  const Hash256 first = EvpSha256(data, len);
  return EvpSha256(first.data(), first.size());
}

Hash256 Sha256d(const std::vector<unsigned char>& data) {
  return Sha256d(data.data(), data.size());
}

Hash256 Sha256d(const std::string& data) {
  return Sha256d(reinterpret_cast<const unsigned char*>(data.data()),
                 data.size());
}

std::string HexStr(const Hash256& hash) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (unsigned char b : hash) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0f]);
  }
  return out;
}

}  // namespace oeeoo
