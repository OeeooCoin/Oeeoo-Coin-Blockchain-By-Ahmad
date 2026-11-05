// Little-endian integer helpers — Oeeoo Coin serialization rules.
// All multi-byte integers in blocks and transactions are little-endian.
// These helpers are the single place that rule lives; nothing else
// hand-rolls byte order.
#pragma once

#include <cstdint>
#include <vector>

namespace oeeoo {

inline void AppendLE16(std::vector<unsigned char>& out, uint16_t v) {
  out.push_back(static_cast<unsigned char>(v & 0xff));
  out.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
}

inline void AppendLE32(std::vector<unsigned char>& out, uint32_t v) {
  out.push_back(static_cast<unsigned char>(v & 0xff));
  out.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
  out.push_back(static_cast<unsigned char>((v >> 16) & 0xff));
  out.push_back(static_cast<unsigned char>((v >> 24) & 0xff));
}

inline void AppendLE64(std::vector<unsigned char>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xff));
  }
}

inline uint16_t ReadLE16(const unsigned char* p) {
  return static_cast<uint16_t>(p[0]) |
         (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t ReadLE32(const unsigned char* p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t ReadLE64(const unsigned char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= (static_cast<uint64_t>(p[i]) << (8 * i));
  }
  return v;
}

}  // namespace oeeoo
