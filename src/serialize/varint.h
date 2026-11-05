// Bitcoin-style CompactSize (VarInt) encoding.
//   value < 0xfd            : 1 byte
//   value <= 0xffff         : 0xfd + 2 bytes LE
//   value <= 0xffffffff     : 0xfe + 4 bytes LE
//   otherwise               : 0xff + 8 bytes LE
// Decoding REJECTS non-minimal encodings (e.g. 252 written as fd fc 00)
// and truncated input, so every node decodes the same bytes the same way.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace oeeoo {

// Minimal encoding of `value`.
std::vector<unsigned char> WriteVarInt(uint64_t value);

// Decode a VarInt starting at `data[offset]` (at most `len` bytes total).
// On success sets `value`, advances `offset` past the encoding and
// returns true. Returns false on truncation, non-minimal encoding,
// or a value that would overflow uint64_t.
bool ReadVarInt(const unsigned char* data, std::size_t len, std::size_t& offset,
                uint64_t& value);

// Encoded size of `value` (1, 3, 5 or 9 bytes).
std::size_t VarIntSize(uint64_t value);

}  // namespace oeeoo
