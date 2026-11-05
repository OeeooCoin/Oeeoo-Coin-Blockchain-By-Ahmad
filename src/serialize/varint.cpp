#include "serialize/varint.h"

#include "serialize/endian.h"

namespace oeeoo {

std::size_t VarIntSize(uint64_t value) {
  if (value < 0xfd) return 1;
  if (value <= 0xffff) return 3;
  if (value <= 0xffffffffULL) return 5;
  return 9;
}

std::vector<unsigned char> WriteVarInt(uint64_t value) {
  std::vector<unsigned char> out;
  if (value < 0xfd) {
    out.push_back(static_cast<unsigned char>(value));
  } else if (value <= 0xffff) {
    out.push_back(0xfd);
    AppendLE16(out, static_cast<uint16_t>(value));
  } else if (value <= 0xffffffffULL) {
    out.push_back(0xfe);
    AppendLE32(out, static_cast<uint32_t>(value));
  } else {
    out.push_back(0xff);
    AppendLE64(out, value);
  }
  return out;
}

bool ReadVarInt(const unsigned char* data, std::size_t len, std::size_t& offset,
                uint64_t& value) {
  if (offset >= len) return false;
  const unsigned char prefix = data[offset++];
  if (prefix < 0xfd) {
    value = prefix;
    return true;
  }
  const std::size_t need = (prefix == 0xfd) ? 2 : (prefix == 0xfe) ? 4 : 8;
  if (len - offset < need) return false;
  uint64_t v = 0;
  if (prefix == 0xfd) {
    v = ReadLE16(data + offset);
    if (v < 0xfd) return false;  // non-minimal encoding
  } else if (prefix == 0xfe) {
    v = ReadLE32(data + offset);
    if (v <= 0xffff) return false;  // non-minimal encoding
  } else {
    v = ReadLE64(data + offset);
    if (v <= 0xffffffffULL) return false;  // non-minimal encoding
  }
  offset += need;
  value = v;
  return true;
}

}  // namespace oeeoo
