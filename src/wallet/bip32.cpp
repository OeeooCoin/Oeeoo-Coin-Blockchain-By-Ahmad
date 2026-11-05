// SPDX-License-Identifier: MIT
// Oeeoo Coin — BIP32 implementation (OpenSSL EC/BN for the curve math).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include "wallet/bip32.h"

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/obj_mac.h>

#include <cstring>

#include "crypto/address.h"
#include "crypto/base58.h"
#include "crypto/ecdsa.h"
#include "crypto/hash160.h"
#include "crypto/sha256.h"

namespace oeeoo {

namespace {
// secp256k1 order.
BIGNUM* CurveOrder() {
  static BIGNUM* order = nullptr;
  if (order == nullptr) {
    order = BN_new();
    BN_hex2bn(&order,
              "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141");
  }
  return order;
}

bool ScalarValid(const unsigned char* bytes, std::size_t len) {
  BIGNUM* bn = BN_bin2bn(bytes, static_cast<int>(len), nullptr);
  const bool ok = bn != nullptr && !BN_is_zero(bn) &&
                  BN_cmp(bn, CurveOrder()) < 0;
  BN_free(bn);
  return ok;
}

// (a + b) mod n, 32 bytes big-endian. False if the result is 0.
bool AddScalars(const std::array<unsigned char, 32>& a,
                const std::array<unsigned char, 32>& b,
                std::array<unsigned char, 32>& out) {
  BIGNUM* bnA = BN_bin2bn(a.data(), 32, nullptr);
  BIGNUM* bnB = BN_bin2bn(b.data(), 32, nullptr);
  BIGNUM* sum = BN_new();
  BN_CTX* ctx = BN_CTX_new();
  bool ok = bnA != nullptr && bnB != nullptr && sum != nullptr &&
            ctx != nullptr;
  if (ok) {
    ok = BN_mod_add(sum, bnA, bnB, CurveOrder(), ctx) == 1;
    ok = ok && !BN_is_zero(sum);
    if (ok) {
      ok = BN_bn2binpad(sum, out.data(), 32) == 32;
    }
  }
  BN_free(bnA);
  BN_free(bnB);
  BN_free(sum);
  BN_CTX_free(ctx);
  return ok;
}

// Compressed public key for a 32-byte private scalar.
bool PubkeyFromScalar(const std::array<unsigned char, 32>& priv,
                      std::array<unsigned char, 33>& pub) {
  std::vector<unsigned char> privVec(priv.begin(), priv.end());
  std::vector<unsigned char> pubVec = PubKeyFromPrivKey(privVec);
  if (pubVec.size() != 33) return false;
  std::memcpy(pub.data(), pubVec.data(), 33);
  return true;
}

// Public child key: point(IL) + parentPub, compressed. False when
// the result is the point at infinity.
bool TweakPubkey(const std::array<unsigned char, 33>& parentPub,
                 const std::array<unsigned char, 32>& tweak,
                 std::array<unsigned char, 33>& out) {
  EC_GROUP* group = EC_GROUP_new_by_curve_name(NID_secp256k1);
  if (group == nullptr) return false;
  BN_CTX* ctx = BN_CTX_new();
  EC_POINT* parent = EC_POINT_new(group);
  EC_POINT* tweakPoint = EC_POINT_new(group);
  EC_POINT* child = EC_POINT_new(group);
  BIGNUM* bn = BN_bin2bn(tweak.data(), 32, nullptr);
  bool ok = ctx != nullptr && parent != nullptr && tweakPoint != nullptr &&
            child != nullptr && bn != nullptr;
  if (ok) {
    ok = EC_POINT_oct2point(group, parent, parentPub.data(), 33, ctx) == 1;
  }
  if (ok) {
    ok = EC_POINT_mul(group, tweakPoint, bn, nullptr, nullptr, ctx) == 1;
  }
  if (ok) {
    ok = EC_POINT_add(group, child, parent, tweakPoint, ctx) == 1;
  }
  if (ok) {
    ok = EC_POINT_point2oct(group, child, POINT_CONVERSION_COMPRESSED,
                            out.data(), 33, ctx) == 33;
  }
  EC_POINT_free(parent);
  EC_POINT_free(tweakPoint);
  EC_POINT_free(child);
  BN_free(bn);
  BN_CTX_free(ctx);
  EC_GROUP_free(group);
  return ok;
}

std::array<unsigned char, 32> HmacSha512Half(
    const std::array<unsigned char, 32>& key,
    const std::vector<unsigned char>& data, bool firstHalf) {
  unsigned char out[64];
  unsigned int outLen = 0;
  HMAC(EVP_sha512(), key.data(), 32, data.data(), data.size(), out, &outLen);
  std::array<unsigned char, 32> half{};
  std::memcpy(half.data(), out + (firstHalf ? 0 : 32), 32);
  return half;
}

void AppendBE32(std::vector<unsigned char>& out, uint32_t v) {
  out.push_back(static_cast<unsigned char>((v >> 24) & 0xff));
  out.push_back(static_cast<unsigned char>((v >> 16) & 0xff));
  out.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
  out.push_back(static_cast<unsigned char>(v & 0xff));
}

// Raw Base58Check (4-byte version): the base58 module only supports
// 1-byte versions, and extended keys need 4.
std::string EncodeBase58CheckRaw(const std::vector<unsigned char>& data) {
  const Hash256 checksum = Sha256d(data);
  std::vector<unsigned char> full = data;
  full.insert(full.end(), checksum.begin(), checksum.begin() + 4);
  return Base58Encode(full);
}

bool DecodeBase58CheckRaw(const std::string& str,
                          std::vector<unsigned char>& data) {
  std::vector<unsigned char> full;
  if (!Base58Decode(str, full)) return false;
  if (full.size() < 4) return false;
  std::vector<unsigned char> payload(full.begin(), full.end() - 4);
  const Hash256 checksum = Sha256d(payload);
  if (std::memcmp(checksum.data(), full.data() + full.size() - 4, 4) != 0) {
    return false;
  }
  data = std::move(payload);
  return true;
}
}  // namespace

bool ExtendedKey::IsValid() const {
  if (pubkey[0] != 0x02 && pubkey[0] != 0x03) return false;
  if (isPrivate) {
    if (!ScalarValid(privkey.data(), 32)) return false;
    std::array<unsigned char, 33> check{};
    if (!PubkeyFromScalar(privkey, check)) return false;
    return check == pubkey;
  }
  return true;
}

uint32_t ExtendedKey::Fingerprint() const {
  const Hash160 hash = ComputeHash160(
      std::vector<unsigned char>(pubkey.begin(), pubkey.end()));
  return (static_cast<uint32_t>(hash[0]) << 24) |
         (static_cast<uint32_t>(hash[1]) << 16) |
         (static_cast<uint32_t>(hash[2]) << 8) |
         static_cast<uint32_t>(hash[3]);
}

bool MasterKeyFromSeed(const std::vector<unsigned char>& seed,
                       ExtendedKey& out) {
  if (seed.size() < 16 || seed.size() > 64) return false;
  unsigned char digest[64];
  unsigned int digestLen = 0;
  const std::string keyMaterial = "Bitcoin seed";
  HMAC(EVP_sha512(), keyMaterial.c_str(), keyMaterial.size(), seed.data(),
       seed.size(), digest, &digestLen);
  if (digestLen != 64) return false;
  if (!ScalarValid(digest, 32)) return false;
  out = ExtendedKey{};
  out.depth = 0;
  out.parentFingerprint = 0;
  out.childNumber = 0;
  std::memcpy(out.chainCode.data(), digest + 32, 32);
  out.isPrivate = true;
  std::memcpy(out.privkey.data(), digest, 32);
  return PubkeyFromScalar(out.privkey, out.pubkey);
}

bool DeriveChild(const ExtendedKey& parent, uint32_t index,
                 ExtendedKey& out) {
  const bool hardened = (index & kHardenedBit) != 0;
  if (hardened && !parent.isPrivate) return false;
  std::vector<unsigned char> data;
  if (hardened) {
    data.push_back(0x00);
    data.insert(data.end(), parent.privkey.begin(), parent.privkey.end());
  } else {
    data.insert(data.end(), parent.pubkey.begin(), parent.pubkey.end());
  }
  AppendBE32(data, index);
  const std::array<unsigned char, 32> il =
      HmacSha512Half(parent.chainCode, data, true);
  const std::array<unsigned char, 32> ir =
      HmacSha512Half(parent.chainCode, data, false);
  if (!ScalarValid(il.data(), 32)) return false;
  out = ExtendedKey{};
  out.depth = static_cast<uint8_t>(parent.depth + 1);
  out.parentFingerprint = parent.Fingerprint();
  out.childNumber = index;
  out.chainCode = ir;
  if (parent.isPrivate) {
    out.isPrivate = true;
    if (!AddScalars(parent.privkey, il, out.privkey)) return false;
    if (!PubkeyFromScalar(out.privkey, out.pubkey)) return false;
  } else {
    out.isPrivate = false;
    if (!TweakPubkey(parent.pubkey, il, out.pubkey)) return false;
  }
  return true;
}

bool DerivePath(const ExtendedKey& master, const std::string& path,
                ExtendedKey& out) {
  if (path.empty() || (path[0] != 'm' && path[0] != 'M')) return false;
  ExtendedKey current = master;
  std::size_t pos = 1;
  while (pos < path.size()) {
    if (path[pos] != '/') return false;
    ++pos;
    if (pos >= path.size()) return false;
    uint32_t index = 0;
    bool hardened = false;
    bool anyDigit = false;
    while (pos < path.size() && path[pos] >= '0' && path[pos] <= '9') {
      index = index * 10 + static_cast<uint32_t>(path[pos] - '0');
      if (index >= kHardenedBit) return false;  // overflow guard
      ++pos;
      anyDigit = true;
    }
    if (!anyDigit) return false;
    if (pos < path.size() && (path[pos] == '\'' || path[pos] == 'h' ||
                              path[pos] == 'H')) {
      hardened = true;
      ++pos;
    }
    if (hardened) index |= kHardenedBit;
    ExtendedKey child;
    if (!DeriveChild(current, index, child)) return false;
    current = child;
  }
  out = current;
  return true;
}

ExtendedKey Neuter(const ExtendedKey& key) {
  ExtendedKey pub = key;
  pub.isPrivate = false;
  pub.privkey.fill(0);
  return pub;
}

namespace {
std::string SerializeExtended(const ExtendedKey& key, bool asPrivate) {
  std::vector<unsigned char> bytes;
  const uint32_t version = asPrivate ? kXprvVersion : kXpubVersion;
  bytes.push_back(static_cast<unsigned char>((version >> 24) & 0xff));
  bytes.push_back(static_cast<unsigned char>((version >> 16) & 0xff));
  bytes.push_back(static_cast<unsigned char>((version >> 8) & 0xff));
  bytes.push_back(static_cast<unsigned char>(version & 0xff));
  bytes.push_back(key.depth);
  bytes.push_back(static_cast<unsigned char>((key.parentFingerprint >> 24) & 0xff));
  bytes.push_back(static_cast<unsigned char>((key.parentFingerprint >> 16) & 0xff));
  bytes.push_back(static_cast<unsigned char>((key.parentFingerprint >> 8) & 0xff));
  bytes.push_back(static_cast<unsigned char>(key.parentFingerprint & 0xff));
  AppendBE32(bytes, key.childNumber);
  bytes.insert(bytes.end(), key.chainCode.begin(), key.chainCode.end());
  if (asPrivate) {
    bytes.push_back(0x00);
    bytes.insert(bytes.end(), key.privkey.begin(), key.privkey.end());
  } else {
    bytes.insert(bytes.end(), key.pubkey.begin(), key.pubkey.end());
  }
  return EncodeBase58CheckRaw(bytes);
}
}  // namespace

std::string SerializePrivate(const ExtendedKey& key) {
  if (!key.isPrivate) return {};
  return SerializeExtended(key, true);
}

std::string SerializePublic(const ExtendedKey& key) {
  return SerializeExtended(key, false);
}

bool ParseExtendedKey(const std::string& encoded, ExtendedKey& out) {
  std::vector<unsigned char> bytes;
  if (!DecodeBase58CheckRaw(encoded, bytes)) return false;
  if (bytes.size() != 78) return false;
  const uint32_t version =
      (static_cast<uint32_t>(bytes[0]) << 24) |
      (static_cast<uint32_t>(bytes[1]) << 16) |
      (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
  bool asPrivate = false;
  if (version == kXprvVersion) {
    asPrivate = true;
  } else if (version == kXpubVersion) {
    asPrivate = false;
  } else {
    return false;
  }
  ExtendedKey key;
  key.depth = bytes[4];
  key.parentFingerprint =
      (static_cast<uint32_t>(bytes[5]) << 24) |
      (static_cast<uint32_t>(bytes[6]) << 16) |
      (static_cast<uint32_t>(bytes[7]) << 8) | bytes[8];
  key.childNumber =
      (static_cast<uint32_t>(bytes[9]) << 24) |
      (static_cast<uint32_t>(bytes[10]) << 16) |
      (static_cast<uint32_t>(bytes[11]) << 8) | bytes[12];
  std::memcpy(key.chainCode.data(), bytes.data() + 13, 32);
  if (asPrivate) {
    if (bytes[45] != 0x00) return false;
    key.isPrivate = true;
    std::memcpy(key.privkey.data(), bytes.data() + 46, 32);
    if (!PubkeyFromScalar(key.privkey, key.pubkey)) return false;
  } else {
    key.isPrivate = false;
    std::memcpy(key.pubkey.data(), bytes.data() + 45, 33);
    if (key.pubkey[0] != 0x02 && key.pubkey[0] != 0x03) return false;
  }
  if (!key.IsValid()) return false;
  out = key;
  return true;
}

}  // namespace oeeoo
#pragma GCC diagnostic pop
