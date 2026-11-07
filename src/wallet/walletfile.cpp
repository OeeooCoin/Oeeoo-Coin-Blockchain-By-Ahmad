// SPDX-License-Identifier: MIT
// Oeeoo Coin — encrypted wallet file implementation (AES-256-GCM).
#include "wallet/walletfile.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "serialize/endian.h"

namespace oeeoo {

namespace {
const char* kMagic = "OEEOOWALLET1";  // 12 bytes
constexpr std::size_t kSaltLen = 16;
constexpr std::size_t kNonceLen = 12;
constexpr std::size_t kTagLen = 16;
constexpr int kPbkdf2Iterations = 200000;
constexpr uint32_t kPayloadVersion = 1;

bool ReadFile(const std::string& path, std::vector<unsigned char>& data) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  unsigned char buffer[4096];
  std::size_t n = 0;
  while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) {
    data.insert(data.end(), buffer, buffer + n);
  }
  const bool ok = std::ferror(f) == 0;
  std::fclose(f);
  return ok;
}

// OOC-003 (Milestone 20): the wallet file holds the encrypted
// mnemonic, so it must be owner-only (0600) however it is created.
// open() with mode 0600 is umask-proof for a NEW file (a umask can
// only clear bits, and 0600 has no group/other bits to begin with),
// but it does NOT change the mode of a PRE-EXISTING file — so
// fchmod() explicitly afterwards, then verify with fstat() and fail
// (leaving any previous contents intact) rather than write secrets
// into a file with wider permissions. The mode is fixed BEFORE any
// byte is written, and the old contents are truncated only after
// the mode is verified, so sensitive data never sits in a
// world-readable file, even briefly.
// Platform scope: POSIX only. Windows ACLs are out of scope — this
// project targets POSIX here; do not add Windows-only code.
bool OpenPrivateFile(const std::string& path, FILE*& fileOut) {
  fileOut = nullptr;
  // No O_TRUNC here on purpose: truncate only after the mode is
  // verified (below), so a chmod failure leaves the old file intact
  // instead of destroying it.
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT, 0600);
  if (fd < 0) return false;
  if (::fchmod(fd, 0600) != 0) {
    ::close(fd);
    return false;
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || (st.st_mode & 0777) != 0600) {
    ::close(fd);
    return false;
  }
  if (::ftruncate(fd, 0) != 0) {
    ::close(fd);
    return false;
  }
  FILE* f = ::fdopen(fd, "wb");
  if (f == nullptr) {
    ::close(fd);
    return false;
  }
  fileOut = f;
  return true;
}

bool WriteFile(const std::string& path,
               const std::vector<unsigned char>& data) {
  FILE* f = nullptr;
  if (!OpenPrivateFile(path, f)) return false;
  const bool ok =
      std::fwrite(data.data(), 1, data.size(), f) == data.size();
  if (std::fclose(f) != 0) return false;
  return ok;
}

void DeriveKey(const std::string& passphrase,
               const unsigned char* salt, std::size_t saltLen,
               unsigned char* keyOut) {
  PKCS5_PBKDF2_HMAC(passphrase.c_str(), static_cast<int>(passphrase.size()),
                    salt, static_cast<int>(saltLen), kPbkdf2Iterations,
                    EVP_sha512(), 32, keyOut);
}

std::vector<unsigned char> SerializePayload(const WalletFileData& data) {
  std::vector<unsigned char> out;
  AppendLE32(out, kPayloadVersion);
  AppendLE32(out, static_cast<uint32_t>(data.mnemonic.size()));
  out.insert(out.end(), data.mnemonic.begin(), data.mnemonic.end());
  AppendLE32(out, data.nextExternal);
  AppendLE32(out, data.nextChange);
  return out;
}

bool ParsePayload(const std::vector<unsigned char>& bytes,
                  WalletFileData& data) {
  if (bytes.size() < 16) return false;
  const uint32_t version = ReadLE32(bytes.data());
  if (version != kPayloadVersion) return false;
  const uint32_t mnemonicLen = ReadLE32(bytes.data() + 4);
  // Milestone 18 (fuzzing find M18B-R2): `16 + mnemonicLen` was
  // computed in uint32 arithmetic, so mnemonicLen = 0xFFFFFFF0
  // wrapped it to 0 and the bounds check passed — then
  // std::string::assign read ~4 GB out of the small plaintext
  // buffer (ASan heap-buffer-overflow). Compare in size_t so the
  // length can never wrap: the payload is version(4) + len(4) +
  // mnemonic + nextExternal(4) + nextChange(4).
  if (static_cast<std::size_t>(mnemonicLen) > bytes.size() - 16) {
    return false;
  }
  data.mnemonic.assign(
      reinterpret_cast<const char*>(bytes.data() + 8), mnemonicLen);
  data.nextExternal = ReadLE32(bytes.data() + 8 + mnemonicLen);
  data.nextChange = ReadLE32(bytes.data() + 12 + mnemonicLen);
  return true;
}
}  // namespace

bool SaveWalletFile(const std::string& path, const WalletFileData& data,
                    const std::string& passphrase, std::string& error) {
  unsigned char salt[kSaltLen];
  unsigned char nonce[kNonceLen];
  if (RAND_bytes(salt, sizeof(salt)) != 1 ||
      RAND_bytes(nonce, sizeof(nonce)) != 1) {
    error = "randomness failure";
    return false;
  }
  unsigned char key[32];
  DeriveKey(passphrase, salt, sizeof(salt), key);
  const std::vector<unsigned char> plaintext = SerializePayload(data);
  std::vector<unsigned char> ciphertext(plaintext.size());
  unsigned char tag[kTagLen];
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (ctx == nullptr) {
    error = "cipher context failure";
    return false;
  }
  bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr,
                               nullptr) == 1;
  if (ok) {
    ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceLen,
                             nullptr) == 1;
  }
  if (ok) {
    ok = EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, nonce) == 1;
  }
  int outLen = 0;
  if (ok) {
    ok = EVP_EncryptUpdate(ctx, ciphertext.data(), &outLen,
                           plaintext.data(),
                           static_cast<int>(plaintext.size())) == 1;
  }
  if (ok) {
    ok = EVP_EncryptFinal_ex(ctx, ciphertext.data() + outLen, &outLen) == 1;
  }
  if (ok) {
    ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagLen, tag) == 1;
  }
  EVP_CIPHER_CTX_free(ctx);
  OPENSSL_cleanse(key, sizeof(key));
  if (!ok) {
    error = "encryption failure";
    return false;
  }
  std::vector<unsigned char> file;
  file.insert(file.end(), kMagic, kMagic + 12);
  file.insert(file.end(), salt, salt + kSaltLen);
  file.insert(file.end(), nonce, nonce + kNonceLen);
  file.insert(file.end(), ciphertext.begin(), ciphertext.end());
  file.insert(file.end(), tag, tag + kTagLen);
  if (!WriteFile(path, file)) {
    error = "cannot write wallet file";
    return false;
  }
  return true;
}

bool LoadWalletFile(const std::string& path, const std::string& passphrase,
                    WalletFileData& data, std::string& error) {
  std::vector<unsigned char> file;
  if (!ReadFile(path, file)) {
    error = "cannot read wallet file";
    return false;
  }
  if (file.size() < 12 + kSaltLen + kNonceLen + kTagLen) {
    error = "wallet file is too short";
    return false;
  }
  if (std::memcmp(file.data(), kMagic, 12) != 0) {
    error = "not an Oeeoo wallet file";
    return false;
  }
  const unsigned char* salt = file.data() + 12;
  const unsigned char* nonce = salt + kSaltLen;
  const unsigned char* ciphertext = nonce + kNonceLen;
  const std::size_t ciphertextLen =
      file.size() - 12 - kSaltLen - kNonceLen - kTagLen;
  const unsigned char* tag = ciphertext + ciphertextLen;
  unsigned char key[32];
  DeriveKey(passphrase, salt, kSaltLen, key);
  std::vector<unsigned char> plaintext(ciphertextLen);
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (ctx == nullptr) {
    error = "cipher context failure";
    return false;
  }
  bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr,
                               nullptr) == 1;
  if (ok) {
    ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceLen,
                             nullptr) == 1;
  }
  if (ok) {
    ok = EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, nonce) == 1;
  }
  int outLen = 0;
  if (ok) {
    ok = EVP_DecryptUpdate(ctx, plaintext.data(), &outLen, ciphertext,
                           static_cast<int>(ciphertextLen)) == 1;
  }
  if (ok) {
    ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagLen,
                             const_cast<unsigned char*>(tag)) == 1;
  }
  if (ok) {
    // Final verifies the tag: wrong passphrase or tampered file
    // fails here.
    ok = EVP_DecryptFinal_ex(ctx, plaintext.data() + outLen, &outLen) == 1;
  }
  EVP_CIPHER_CTX_free(ctx);
  OPENSSL_cleanse(key, sizeof(key));
  if (!ok) {
    error = "decryption failed (wrong passphrase or tampered file)";
    return false;
  }
  if (!ParsePayload(plaintext, data)) {
    error = "wallet payload is corrupt";
    return false;
  }
  return true;
}

}  // namespace oeeoo
