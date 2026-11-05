// SPDX-License-Identifier: MIT
// Oeeoo Coin — small shared helpers for the RPC / Stratum servers
// (Milestone 10): hex and Base64 encode/decode. Header-only.
#pragma once

#include <string>
#include <vector>

namespace oeeoo {

inline std::string BytesToHexStr(const std::vector<unsigned char>& bytes) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char b : bytes) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0f]);
  }
  return out;
}

inline bool HexToBytesStr(const std::string& hex,
                          std::vector<unsigned char>& out) {
  if (hex.size() % 2 != 0) return false;
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  out.clear();
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int hi = nibble(hex[i]);
    const int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.push_back(static_cast<unsigned char>((hi << 4) | lo));
  }
  return true;
}

inline std::string Base64EncodeStr(const std::string& input) {
  static const char* kTable =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t i = 0; i < input.size(); i += 3) {
    const int remaining = static_cast<int>(input.size() - i);
    const uint32_t a = static_cast<unsigned char>(input[i]);
    const uint32_t b =
        remaining > 1 ? static_cast<unsigned char>(input[i + 1]) : 0;
    const uint32_t c =
        remaining > 2 ? static_cast<unsigned char>(input[i + 2]) : 0;
    const uint32_t triple = (a << 16) | (b << 8) | c;
    out.push_back(kTable[(triple >> 18) & 0x3F]);
    out.push_back(kTable[(triple >> 12) & 0x3F]);
    out.push_back(remaining > 1 ? kTable[(triple >> 6) & 0x3F] : '=');
    out.push_back(remaining > 2 ? kTable[triple & 0x3F] : '=');
  }
  return out;
}

inline bool Base64DecodeStr(const std::string& input, std::string& out) {
  const auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  if (input.size() % 4 != 0) return false;
  out.clear();
  for (std::size_t i = 0; i < input.size(); i += 4) {
    int v[4];
    int padding = 0;
    for (int j = 0; j < 4; ++j) {
      if (input[i + j] == '=') {
        v[j] = 0;
        ++padding;
      } else {
        v[j] = value(input[i + j]);
        if (v[j] < 0) return false;
      }
    }
    if (padding > 2) return false;
    const uint32_t triple =
        (static_cast<uint32_t>(v[0]) << 18) |
        (static_cast<uint32_t>(v[1]) << 12) |
        (static_cast<uint32_t>(v[2]) << 6) |
        static_cast<uint32_t>(v[3]);
    out.push_back(static_cast<char>((triple >> 16) & 0xFF));
    if (padding < 2) out.push_back(static_cast<char>((triple >> 8) & 0xFF));
    if (padding < 1) out.push_back(static_cast<char>(triple & 0xFF));
  }
  return true;
}

// Constant-time equality (for credentials).
inline bool ConstantTimeEqual(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  }
  return diff == 0;
}

}  // namespace oeeoo
