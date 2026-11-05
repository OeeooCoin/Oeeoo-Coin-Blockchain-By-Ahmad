// Tests: Base58Check + P2PKH addresses against independently
// computed vectors (Python). Version byte 0x0f, 34-char addresses.
// The "75Pk" vector proves the requested prefix is achievable:
// hash160 197a166292b7c3d067d0aed78e74c4a883d1465c encodes to
// 75PkvD7AKStxvaDSyQH9gtC9kTStqPnPxV.
#include <string>

#include "crypto/address.h"
#include "crypto/base58.h"
#include "crypto/hash160.h"
#include "test_harness.h"

using namespace oeeoo;

namespace {
Hash160 FromHex20(const std::string& hex) {
  Hash160 h{};
  for (std::size_t i = 0; i < 20; ++i) {
    const auto nib = [](char c) -> unsigned char {
      if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
      return static_cast<unsigned char>(c - 'a' + 10);
    };
    h[i] = static_cast<unsigned char>((nib(hex[2 * i]) << 4) | nib(hex[2 * i + 1]));
  }
  return h;
}
}  // namespace

void TestAddress() {
  test::BeginSection("base58check + p2pkh address vectors");
  // hash160 0x11*20 -> 74dHhhkwZiebrbdmmb5wEtZvMGUwwWBGpT (34 chars).
  const std::string a1 =
      PubKeyHashToAddress(FromHex20("1111111111111111111111111111111111111111"));
  CHECK(a1 == "74dHhhkwZiebrbdmmb5wEtZvMGUwwWBGpT");
  CHECK(a1.size() == 34);
  // hash160 all zero -> 7353mdUKehx9GW6JNHznGt5oSZs9mBsnYx.
  CHECK(PubKeyHashToAddress(Hash160{}) ==
        "7353mdUKehx9GW6JNHznGt5oSZs9mBsnYx");
  // The requested "75Pk" prefix, achieved with version byte 0x0f.
  const std::string a75 =
      PubKeyHashToAddress(FromHex20("197a166292b7c3d067d0aed78e74c4a883d1465c"));
  CHECK(a75 == "75PkvD7AKStxvaDSyQH9gtC9kTStqPnPxV");
  CHECK(a75.rfind("75Pk", 0) == 0);

  // HASH160 of a compressed public key (02 + 0x22*32), then its address.
  std::vector<unsigned char> pubkey2;
  pubkey2.push_back(0x02);
  pubkey2.insert(pubkey2.end(), 32, 0x22);
  CHECK(HexStr(ComputeHash160(pubkey2)) ==
        "4377a5acd66dc5cb67148a24818d1e51fa183bd2");
  CHECK(PubKeyToAddress(pubkey2) == "79DnNiKicRvs6S7DakR4Bp3xsNa9LF46J6");

  test::BeginSection("address decode + rejections");
  Hash160 h{};
  CHECK(DecodeP2PKHAddress(a75, h));
  CHECK(h == FromHex20("197a166292b7c3d067d0aed78e74c4a883d1465c"));
  CHECK(DecodeP2PKHAddress(a1, h));
  CHECK(h == FromHex20("1111111111111111111111111111111111111111"));
  // Bad checksum (last character changed) rejected.
  std::string bad = a75;
  bad.back() = (bad.back() == 'V' ? 'W' : 'V');
  CHECK(!DecodeP2PKHAddress(bad, h));
  // Invalid Base58 character ('0' is not in the alphabet) rejected.
  CHECK(!DecodeP2PKHAddress("75PkvD7AKStxvaDSyQH9gtC9kTStqPnPxV0", h));
  // Empty / garbage rejected.
  CHECK(!DecodeP2PKHAddress("", h));
  CHECK(!DecodeP2PKHAddress("not-an-address", h));
  // Plain Base58 round-trip.
  const std::vector<unsigned char> raw = {0x00, 0x00, 0x01, 0x02, 0xff};
  const std::string enc = Base58Encode(raw);
  std::vector<unsigned char> dec;
  CHECK(Base58Decode(enc, dec) && dec == raw);
  CHECK(Base58Encode(std::vector<unsigned char>{}) == "");
  CHECK(Base58Encode(std::vector<unsigned char>{0x00}) == "1");
}
