#include "crypto/hash160.h"

#include <mutex>
#include <stdexcept>

#include <openssl/evp.h>
#include <openssl/provider.h>

#include "crypto/sha256.h"

namespace oeeoo {

namespace {
void EnsureLegacyProvider() {
  // RIPEMD-160 lives in OpenSSL 3's legacy provider; load it once.
  static std::once_flag flag;
  std::call_once(flag, [] {
    OSSL_PROVIDER_load(nullptr, "legacy");
    OSSL_PROVIDER_load(nullptr, "default");
  });
}
}  // namespace

Hash160 ComputeHash160(const unsigned char* data, std::size_t len) {
  EnsureLegacyProvider();
  const Hash256 sha = Sha256(data, len);
  Hash160 out{};
  unsigned int out_len = 0;
  if (EVP_Digest(sha.data(), sha.size(), out.data(), &out_len,
                 EVP_ripemd160(), nullptr) != 1 ||
      out_len != out.size()) {
    throw std::runtime_error("OpenSSL RIPEMD-160 failed");
  }
  return out;
}

Hash160 ComputeHash160(const std::vector<unsigned char>& data) {
  return ComputeHash160(data.data(), data.size());
}

Hash160 ComputeHash160(const std::string& data) {
  return ComputeHash160(reinterpret_cast<const unsigned char*>(data.data()),
                        data.size());
}

std::string HexStr(const Hash160& hash) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(40);
  for (unsigned char b : hash) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0f]);
  }
  return out;
}

}  // namespace oeeoo
