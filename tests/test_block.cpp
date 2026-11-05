// Tests: block header serialization (exactly 80 bytes, little-endian,
// deterministic) and header hash against an independently computed
// SHA-256d vector.
#include <string>

#include "blockchain/block.h"
#include "test_harness.h"

using namespace oeeoo;

void TestBlock() {
  test::BeginSection("block header serialization + hash");
  BlockHeader h;
  h.nVersion = 1;
  h.hashPrevBlock.fill(0);
  h.hashMerkleRoot.fill(0);
  h.nTime = 0;
  h.nBits = 0x1d00ffff;  // genesis-difficulty candidate from the spec
  h.nNonce = 0;

  const std::vector<unsigned char> ser = h.Serialize();
  CHECK(ser.size() == 80);
  // Exact expected bytes: 01000000 || 32x00 || 32x00 || 00000000 ||
  // ffff001d || 00000000 (nBits little-endian).
  const std::string expected_hex =
      "01000000"
      "0000000000000000000000000000000000000000000000000000000000000000"
      "0000000000000000000000000000000000000000000000000000000000000000"
      "00000000"
      "ffff001d"
      "00000000";
  std::string got_hex;
  static constexpr char kHex[] = "0123456789abcdef";
  for (unsigned char b : ser) {
    got_hex.push_back(kHex[b >> 4]);
    got_hex.push_back(kHex[b & 0x0f]);
  }
  CHECK(got_hex == expected_hex);

  // Round-trip: deserialize the serialization, get the same header.
  BlockHeader h2;
  CHECK(BlockHeader::Deserialize(ser, h2));
  CHECK(h2.nVersion == 1);
  CHECK(h2.nTime == 0);
  CHECK(h2.nBits == 0x1d00ffff);
  CHECK(h2.nNonce == 0);
  CHECK(h2.hashPrevBlock == h.hashPrevBlock);
  CHECK(h2.hashMerkleRoot == h.hashMerkleRoot);
  CHECK(h2.Serialize() == ser);

  // Wrong size must be rejected.
  BlockHeader h3;
  CHECK(!BlockHeader::Deserialize(std::vector<unsigned char>(79, 0), h3));
  CHECK(!BlockHeader::Deserialize(std::vector<unsigned char>(81, 0), h3));

  // Header hash: SHA256d of the 80 bytes above, computed independently
  // with Python hashlib before this test was written.
  CHECK(h.GetHashHex() ==
        "6afe9b465e17364b4148fa0ecd30e8ccd1c0bb69a79c98852f08f99f36b4060c");
  // Deterministic: hashing twice gives the same result.
  CHECK(h.GetHash() == h.GetHash());
}
