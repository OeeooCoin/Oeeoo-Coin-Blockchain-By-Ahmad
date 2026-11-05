// Tests: CompactSize (VarInt) — exact encodings from the spec table,
// round-trips, and rejection of non-minimal / truncated encodings.
#include <cstdint>
#include <string>

#include "serialize/varint.h"
#include "test_harness.h"

using namespace oeeoo;

namespace {
std::string Hex(const std::vector<unsigned char>& v) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string s;
  for (unsigned char b : v) {
    s.push_back(kHex[b >> 4]);
    s.push_back(kHex[b & 0x0f]);
  }
  return s;
}

bool Decode(const std::vector<unsigned char>& bytes, uint64_t& value) {
  std::size_t off = 0;
  return ReadVarInt(bytes.data(), bytes.size(), off, value) &&
         off == bytes.size();
}
}  // namespace

void TestVarInt() {
  test::BeginSection("varint encodings");
  CHECK(Hex(WriteVarInt(0)) == "00");
  CHECK(Hex(WriteVarInt(252)) == "fc");
  CHECK(Hex(WriteVarInt(253)) == "fdfd00");
  CHECK(Hex(WriteVarInt(65535)) == "fdffff");
  CHECK(Hex(WriteVarInt(65536)) == "fe00000100");
  CHECK(Hex(WriteVarInt(4294967295ULL)) == "feffffffff");
  CHECK(Hex(WriteVarInt(4294967296ULL)) == "ff0000000001000000");
  CHECK(VarIntSize(0) == 1);
  CHECK(VarIntSize(252) == 1);
  CHECK(VarIntSize(253) == 3);
  CHECK(VarIntSize(65536) == 5);
  CHECK(VarIntSize(4294967296ULL) == 9);

  test::BeginSection("varint round-trips + rejections");
  const uint64_t values[] = {0, 1, 252, 253, 254, 65535, 65536,
                             4294967295ULL, 4294967296ULL, 0xffffffffffffffffULL};
  for (uint64_t v : values) {
    uint64_t back = 0;
    CHECK(Decode(WriteVarInt(v), back) && back == v);
  }
  // Non-minimal encodings must be rejected.
  uint64_t v = 0;
  CHECK(!Decode({0xfd, 0xfc, 0x00}, v));  // 252 as 3 bytes
  CHECK(!Decode({0xfe, 0xff, 0xff, 0x00, 0x00}, v));  // 65535 as 5 bytes
  CHECK(!Decode({0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00},
                v));  // 0xffffffff as 9 bytes (fits in 5)
  // Truncated encodings must be rejected.
  CHECK(!Decode({0xfd}, v));
  CHECK(!Decode({0xfd, 0x00}, v));
  CHECK(!Decode({0xfe, 0x00, 0x00}, v));
  CHECK(!Decode({}, v));
}
