#include "crypto/base58.h"

#include "crypto/sha256.h"

namespace oeeoo {

namespace {
const char* kAlphabet =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

int CharValue(char c) {
  // Base58 alphabet lookup; -1 for characters outside it.
  const char* p = kAlphabet;
  for (int i = 0; p[i] != '\0'; ++i) {
    if (p[i] == c) return i;
  }
  return -1;
}
}  // namespace

std::string Base58Encode(const unsigned char* data, std::size_t len) {
  // Count leading zero bytes: each becomes a leading '1'.
  std::size_t zeros = 0;
  while (zeros < len && data[zeros] == 0) ++zeros;
  // Convert the rest as a big-endian base-256 number to base-58.
  // Work in base 2^32 limbs for speed; input here is at most 25 bytes.
  std::vector<unsigned char> digits(1, 0);  // base-58 digits, little-endian
  for (std::size_t i = zeros; i < len; ++i) {
    int carry = data[i];
    for (std::size_t j = 0; j < digits.size(); ++j) {
      carry += 256 * digits[j];
      digits[j] = static_cast<unsigned char>(carry % 58);
      carry /= 58;
    }
    while (carry > 0) {
      digits.push_back(static_cast<unsigned char>(carry % 58));
      carry /= 58;
    }
  }
  std::string out(zeros, '1');
  for (std::size_t i = digits.size(); i-- > 0;) {
    if (digits.size() == 1 && digits[0] == 0 && zeros == len) break;
    out.push_back(kAlphabet[digits[i]]);
  }
  // All-zero input: digits are a single 0; output is just the '1's.
  if (zeros == len) return std::string(zeros, '1');
  return out;
}

std::string Base58Encode(const std::vector<unsigned char>& data) {
  return Base58Encode(data.data(), data.size());
}

bool Base58Decode(const std::string& str, std::vector<unsigned char>& out) {
  std::size_t zeros = 0;
  while (zeros < str.size() && str[zeros] == '1') ++zeros;
  std::vector<unsigned char> bytes(1, 0);  // base-256, little-endian
  for (std::size_t i = zeros; i < str.size(); ++i) {
    const int v = CharValue(str[i]);
    if (v < 0) return false;
    int carry = v;
    for (std::size_t j = 0; j < bytes.size(); ++j) {
      carry += 58 * bytes[j];
      bytes[j] = static_cast<unsigned char>(carry % 256);
      carry /= 256;
    }
    while (carry > 0) {
      bytes.push_back(static_cast<unsigned char>(carry % 256));
      carry /= 256;
    }
  }
  out.assign(zeros, 0);
  for (std::size_t i = bytes.size(); i-- > 0;) {
    if (bytes.size() == 1 && bytes[0] == 0 && zeros == str.size()) break;
    out.push_back(bytes[i]);
  }
  if (zeros == str.size()) {
    out.assign(zeros, 0);  // all-'1' input decodes to zero bytes
  }
  return true;
}

std::string Base58CheckEncode(unsigned char version,
                              const std::vector<unsigned char>& payload) {
  std::vector<unsigned char> data;
  data.reserve(1 + payload.size() + 4);
  data.push_back(version);
  data.insert(data.end(), payload.begin(), payload.end());
  const Hash256 chk = Sha256d(data);
  data.insert(data.end(), chk.begin(), chk.begin() + 4);
  return Base58Encode(data);
}

bool Base58CheckDecode(const std::string& str, unsigned char& version,
                       std::vector<unsigned char>& payload) {
  std::vector<unsigned char> data;
  if (!Base58Decode(str, data)) return false;
  if (data.size() < 5) return false;  // version + checksum minimum
  const Hash256 chk = Sha256d(
      std::vector<unsigned char>(data.begin(), data.end() - 4));
  for (int i = 0; i < 4; ++i) {
    if (chk[i] != data[data.size() - 4 + i]) return false;
  }
  version = data[0];
  payload.assign(data.begin() + 1, data.end() - 4);
  return true;
}

}  // namespace oeeoo
