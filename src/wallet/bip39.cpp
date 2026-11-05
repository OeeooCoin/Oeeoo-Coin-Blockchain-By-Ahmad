// SPDX-License-Identifier: MIT
// Oeeoo Coin — BIP39 implementation.
#include "wallet/bip39.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstring>

#include "crypto/sha256.h"
#include "wallet/bip39_words.h"

namespace oeeoo {

namespace {
int WordIndex(const std::string& word) {
  // The list is alphabetical; binary search it.
  int lo = 0, hi = 2047;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const int cmp = word.compare(kBip39Words[mid]);
    if (cmp == 0) return mid;
    if (cmp < 0) {
      hi = mid - 1;
    } else {
      lo = mid + 1;
    }
  }
  return -1;
}

std::vector<std::string> SplitWords(const std::string& mnemonic) {
  std::vector<std::string> words;
  std::string current;
  for (char c : mnemonic) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (!current.empty()) {
        words.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) words.push_back(current);
  return words;
}
}  // namespace

std::string GenerateMnemonic(int entropyBits) {
  if (entropyBits != 128 && entropyBits != 160 && entropyBits != 192 &&
      entropyBits != 224 && entropyBits != 256) {
    return {};
  }
  std::vector<unsigned char> entropy(entropyBits / 8);
  if (RAND_bytes(entropy.data(), static_cast<int>(entropy.size())) != 1) {
    return {};
  }
  return MnemonicFromEntropy(entropy);
}

std::string MnemonicFromEntropy(const std::vector<unsigned char>& entropy) {
  const std::size_t bits = entropy.size() * 8;
  if (bits != 128 && bits != 160 && bits != 192 && bits != 224 &&
      bits != 256) {
    return {};
  }
  const Hash256 hash = Sha256(entropy);
  const std::size_t checksumBits = bits / 32;
  // Bit stream: entropy bits + first `checksumBits` of SHA256(entropy).
  std::string mnemonic;
  std::size_t bitPos = 0;
  const std::size_t totalBits = bits + checksumBits;
  for (std::size_t i = 0; i < totalBits / 11; ++i) {
    int index = 0;
    for (int j = 0; j < 11; ++j, ++bitPos) {
      int bit = 0;
      if (bitPos < bits) {
        bit = (entropy[bitPos / 8] >> (7 - (bitPos % 8))) & 1;
      } else {
        const std::size_t csPos = bitPos - bits;
        bit = (hash[csPos / 8] >> (7 - (csPos % 8))) & 1;
      }
      index = (index << 1) | bit;
    }
    if (!mnemonic.empty()) mnemonic.push_back(' ');
    mnemonic += kBip39Words[index];
  }
  return mnemonic;
}

bool EntropyFromMnemonic(const std::string& mnemonic,
                         std::vector<unsigned char>& entropy) {
  const std::vector<std::string> words = SplitWords(mnemonic);
  const std::size_t count = words.size();
  if (count != 12 && count != 15 && count != 18 && count != 21 &&
      count != 24) {
    return false;
  }
  const std::size_t totalBits = count * 11;
  const std::size_t checksumBits = totalBits / 33;
  const std::size_t entropyBits = totalBits - checksumBits;
  entropy.assign(entropyBits / 8, 0);
  std::vector<unsigned char> checksum(checksumBits / 8 + 1, 0);
  for (std::size_t i = 0; i < count; ++i) {
    const int index = WordIndex(words[i]);
    if (index < 0) return false;
    for (int j = 0; j < 11; ++j) {
      const std::size_t bitPos = i * 11 + j;
      const int bit = (index >> (10 - j)) & 1;
      if (bitPos < entropyBits) {
        if (bit) entropy[bitPos / 8] |= (1 << (7 - (bitPos % 8)));
      } else {
        const std::size_t csPos = bitPos - entropyBits;
        if (bit) checksum[csPos / 8] |= (1 << (7 - (csPos % 8)));
      }
    }
  }
  const Hash256 hash = Sha256(entropy);
  for (std::size_t i = 0; i < checksumBits; ++i) {
    const int expected = (hash[i / 8] >> (7 - (i % 8))) & 1;
    const int got = (checksum[i / 8] >> (7 - (i % 8))) & 1;
    if (expected != got) return false;
  }
  return true;
}

std::vector<unsigned char> MnemonicToSeed(
    const std::string& mnemonic, const std::string& passphrase) {
  const std::string salt = "mnemonic" + passphrase;
  std::vector<unsigned char> seed(64);
  PKCS5_PBKDF2_HMAC(mnemonic.c_str(), static_cast<int>(mnemonic.size()),
                    reinterpret_cast<const unsigned char*>(salt.c_str()),
                    static_cast<int>(salt.size()), 2048, EVP_sha512(),
                    64, seed.data());
  return seed;
}

bool ValidateMnemonic(const std::string& mnemonic) {
  std::vector<unsigned char> entropy;
  return EntropyFromMnemonic(mnemonic, entropy);
}

}  // namespace oeeoo
