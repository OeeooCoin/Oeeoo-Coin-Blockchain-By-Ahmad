// SPDX-License-Identifier: MIT
// Oeeoo Coin — ECDSA implementation (OpenSSL low-level EC API).
// OpenSSL 3.0 deprecates this API in favour of EVP, but the low-level
// calls are still correct and fully supported; the pragma below silences
// only the deprecation warnings for this file.
#include "crypto/ecdsa.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>

namespace oeeoo {

namespace {
EC_KEY* KeyFromPriv(const std::vector<unsigned char>& privkey) {
  if (privkey.size() != 32) return nullptr;
  EC_KEY* key = EC_KEY_new_by_curve_name(NID_secp256k1);
  if (!key) return nullptr;
  BIGNUM* bn = BN_bin2bn(privkey.data(), 32, nullptr);
  if (!bn) {
    EC_KEY_free(key);
    return nullptr;
  }
  if (EC_KEY_set_private_key(key, bn) != 1) {
    BN_free(bn);
    EC_KEY_free(key);
    return nullptr;
  }
  const EC_GROUP* group = EC_KEY_get0_group(key);
  EC_POINT* point = EC_POINT_new(group);
  if (!point) {
    BN_free(bn);
    EC_KEY_free(key);
    return nullptr;
  }
  if (EC_POINT_mul(group, point, bn, nullptr, nullptr, nullptr) != 1) {
    EC_POINT_free(point);
    BN_free(bn);
    EC_KEY_free(key);
    return nullptr;
  }
  if (EC_KEY_set_public_key(key, point) != 1) {
    EC_POINT_free(point);
    BN_free(bn);
    EC_KEY_free(key);
    return nullptr;
  }
  EC_POINT_free(point);
  BN_free(bn);
  return key;
}

std::vector<unsigned char> PointToCompressed(const EC_KEY* key) {
  const EC_GROUP* group = EC_KEY_get0_group(key);
  const EC_POINT* point = EC_KEY_get0_public_key(key);
  std::vector<unsigned char> out(33);
  size_t n = EC_POINT_point2oct(group, point, POINT_CONVERSION_COMPRESSED,
                                out.data(), out.size(), nullptr);
  if (n != 33) return {};
  return out;
}
}  // namespace

bool GenerateKeyPair(KeyPair& out) {
  EC_KEY* key = EC_KEY_new_by_curve_name(NID_secp256k1);
  if (!key) return false;
  if (EC_KEY_generate_key(key) != 1) {
    EC_KEY_free(key);
    return false;
  }
  const BIGNUM* priv = EC_KEY_get0_private_key(key);
  out.privkey.resize(32);
  if (BN_bn2binpad(priv, out.privkey.data(), 32) != 32) {
    EC_KEY_free(key);
    return false;
  }
  out.pubkey = PointToCompressed(key);
  EC_KEY_free(key);
  return out.pubkey.size() == 33;
}

std::vector<unsigned char> PubKeyFromPrivKey(
    const std::vector<unsigned char>& privkey) {
  EC_KEY* key = KeyFromPriv(privkey);
  if (!key) return {};
  std::vector<unsigned char> pubkey = PointToCompressed(key);
  EC_KEY_free(key);
  return pubkey;
}

namespace {
// BIP62 low-S: normalize a signature's S to the lower half of the
// group order (S > n/2 becomes n - S). Both forms are valid ECDSA
// signatures for the same hash; low-S is the canonical form the
// verifier accepts, which removes signature malleability.
bool LowSNormalize(ECDSA_SIG* sig, const EC_GROUP* group) {
  const BIGNUM* r = nullptr;
  const BIGNUM* s = nullptr;
  ECDSA_SIG_get0(sig, &r, &s);
  BIGNUM* order = BN_new();
  BIGNUM* half = BN_new();
  if (order == nullptr || half == nullptr) {
    BN_free(order);
    BN_free(half);
    return false;
  }
  if (EC_GROUP_get_order(group, order, nullptr) != 1) {
    BN_free(order);
    BN_free(half);
    return false;
  }
  BN_rshift1(half, order);  // n/2
  bool ok = true;
  if (BN_cmp(s, half) > 0) {
    BIGNUM* ns = BN_new();
    BIGNUM* rCopy = BN_dup(r);
    if (ns == nullptr || rCopy == nullptr) {
      BN_free(ns);
      BN_free(rCopy);
      ok = false;
    } else if (BN_sub(ns, order, s) != 1 ||
               ECDSA_SIG_set0(sig, rCopy, ns) != 1) {
      BN_free(ns);
      BN_free(rCopy);
      ok = false;
    }
  }
  BN_free(order);
  BN_free(half);
  return ok;
}
}  // namespace

std::vector<unsigned char> SignHash(
    const Hash256& hash, const std::vector<unsigned char>& privkey) {
  EC_KEY* key = KeyFromPriv(privkey);
  if (!key) return {};
  ECDSA_SIG* sig = ECDSA_do_sign(hash.data(), 32, key);
  if (!sig) {
    EC_KEY_free(key);
    return {};
  }
  // Always produce low-S signatures (BIP62).
  if (!LowSNormalize(sig, EC_KEY_get0_group(key))) {
    ECDSA_SIG_free(sig);
    EC_KEY_free(key);
    return {};
  }
  EC_KEY_free(key);
  int derLen = i2d_ECDSA_SIG(sig, nullptr);
  if (derLen <= 0) {
    ECDSA_SIG_free(sig);
    return {};
  }
  std::vector<unsigned char> der(static_cast<std::size_t>(derLen));
  unsigned char* p = der.data();
  if (i2d_ECDSA_SIG(sig, &p) != derLen) {
    ECDSA_SIG_free(sig);
    return {};
  }
  ECDSA_SIG_free(sig);
  return der;
}

bool VerifySignature(const std::vector<unsigned char>& pubkey,
                     const std::vector<unsigned char>& derSig,
                     const Hash256& hash) {
  if (pubkey.size() != 33 && pubkey.size() != 65) return false;
  if (derSig.empty() || derSig.size() > 72) return false;
  EC_KEY* key = EC_KEY_new_by_curve_name(NID_secp256k1);
  if (!key) return false;
  const EC_GROUP* group = EC_KEY_get0_group(key);
  EC_POINT* point = EC_POINT_new(group);
  if (!point) {
    EC_KEY_free(key);
    return false;
  }
  if (EC_POINT_oct2point(group, point, pubkey.data(), pubkey.size(),
                         nullptr) != 1) {
    EC_POINT_free(point);
    EC_KEY_free(key);
    return false;
  }
  if (EC_KEY_set_public_key(key, point) != 1) {
    EC_POINT_free(point);
    EC_KEY_free(key);
    return false;
  }
  EC_POINT_free(point);
  const unsigned char* p = derSig.data();
  ECDSA_SIG* sig = d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(derSig.size()));
  if (!sig) {
    EC_KEY_free(key);
    return false;
  }
  // BIP62 low-S rule: high-S signatures are rejected, so a
  // signature cannot be mutated into a different valid one.
  const BIGNUM* s = nullptr;
  ECDSA_SIG_get0(sig, nullptr, &s);
  BIGNUM* order = BN_new();
  BIGNUM* half = BN_new();
  bool lowS = false;
  if (order != nullptr && half != nullptr &&
      EC_GROUP_get_order(group, order, nullptr) == 1) {
    BN_rshift1(half, order);  // n/2
    lowS = (BN_cmp(s, half) <= 0);
  }
  BN_free(order);
  BN_free(half);
  if (!lowS) {
    ECDSA_SIG_free(sig);
    EC_KEY_free(key);
    return false;
  }
  int result = ECDSA_do_verify(hash.data(), 32, sig, key);
  ECDSA_SIG_free(sig);
  EC_KEY_free(key);
  return result == 1;
}

Hash160 PubKeyHash160(const std::vector<unsigned char>& pubkey) {
  return ComputeHash160(pubkey);
}

}  // namespace oeeoo

#pragma GCC diagnostic pop
