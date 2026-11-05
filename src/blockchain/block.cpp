#include "blockchain/block.h"

#include <algorithm>
#include <set>

#include "blockchain/merkle.h"
#include "consensus/consensus.h"
#include "serialize/varint.h"

namespace oeeoo {

namespace {
void AppendLE32(std::vector<unsigned char>& out, uint32_t v) {
  out.push_back(static_cast<unsigned char>(v & 0xff));
  out.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
  out.push_back(static_cast<unsigned char>((v >> 16) & 0xff));
  out.push_back(static_cast<unsigned char>((v >> 24) & 0xff));
}

uint32_t ReadLE32(const std::vector<unsigned char>& in, std::size_t off) {
  return static_cast<uint32_t>(in[off]) |
         (static_cast<uint32_t>(in[off + 1]) << 8) |
         (static_cast<uint32_t>(in[off + 2]) << 16) |
         (static_cast<uint32_t>(in[off + 3]) << 24);
}
}  // namespace

std::vector<unsigned char> BlockHeader::Serialize() const {
  std::vector<unsigned char> out;
  out.reserve(80);
  AppendLE32(out, static_cast<uint32_t>(nVersion));
  out.insert(out.end(), hashPrevBlock.begin(), hashPrevBlock.end());
  out.insert(out.end(), hashMerkleRoot.begin(), hashMerkleRoot.end());
  AppendLE32(out, nTime);
  AppendLE32(out, nBits);
  AppendLE32(out, nNonce);
  return out;  // exactly 80 bytes
}

bool BlockHeader::Deserialize(const std::vector<unsigned char>& bytes,
                              BlockHeader& out) {
  if (bytes.size() != 80) return false;
  out.nVersion = static_cast<int32_t>(ReadLE32(bytes, 0));
  std::copy(bytes.begin() + 4, bytes.begin() + 36, out.hashPrevBlock.begin());
  std::copy(bytes.begin() + 36, bytes.begin() + 68, out.hashMerkleRoot.begin());
  out.nTime = ReadLE32(bytes, 68);
  out.nBits = ReadLE32(bytes, 72);
  out.nNonce = ReadLE32(bytes, 76);
  return true;
}

Hash256 BlockHeader::GetHash() const { return Sha256d(Serialize()); }

std::string BlockHeader::GetHashHex() const { return HexStr(GetHash()); }

// ---- Full block ----

std::vector<unsigned char> Block::Serialize() const {
  std::vector<unsigned char> out = header.Serialize();
  const std::vector<unsigned char> countBytes = WriteVarInt(vtx.size());
  out.insert(out.end(), countBytes.begin(), countBytes.end());
  for (const Transaction& tx : vtx) {
    std::vector<unsigned char> txBytes = tx.Serialize();
    out.insert(out.end(), txBytes.begin(), txBytes.end());
  }
  return out;
}

bool Block::Deserialize(const std::vector<unsigned char>& bytes, Block& out) {
  if (bytes.size() < 80) return false;
  std::vector<unsigned char> headerBytes(bytes.begin(), bytes.begin() + 80);
  if (!BlockHeader::Deserialize(headerBytes, out.header)) return false;
  std::size_t offset = 80;
  uint64_t count = 0;
  if (!ReadVarInt(bytes.data(), bytes.size(), offset, count)) return false;
  if (count > 1000000) return false;  // sanity cap before allocating
  out.vtx.clear();
  out.vtx.reserve(static_cast<std::size_t>(count));
  for (uint64_t i = 0; i < count; ++i) {
    Transaction tx;
    std::size_t consumed = 0;
    if (!Transaction::Deserialize(bytes, offset, tx, consumed)) return false;
    offset += consumed;
    out.vtx.push_back(std::move(tx));
  }
  if (offset != bytes.size()) return false;  // trailing garbage
  return true;
}

Hash256 Block::ComputeMerkleRoot() const {
  std::vector<Hash256> txids;
  txids.reserve(vtx.size());
  for (const Transaction& tx : vtx) txids.push_back(tx.GetTxId());
  // Qualified: the unqualified name would resolve to this member.
  return ::oeeoo::ComputeMerkleRoot(txids);
}

bool Block::CheckBasic(std::string& error) const {
  std::vector<unsigned char> bytes = Serialize();
  if (bytes.size() > static_cast<std::size_t>(MAX_BLOCK_SIZE)) {
    error = "block exceeds maximum block size";
    return false;
  }
  if (vtx.empty()) {
    error = "block has no transactions";
    return false;
  }
  for (const Transaction& tx : vtx) {
    if (!tx.CheckBasic()) {
      error = "invalid transaction in block";
      return false;
    }
  }
  if (ComputeMerkleRoot() != header.hashMerkleRoot) {
    error = "merkle root mismatch";
    return false;
  }
  std::set<Hash256> seen;
  for (const Transaction& tx : vtx) {
    if (!seen.insert(tx.GetTxId()).second) {
      error = "duplicate transaction in block";
      return false;
    }
  }
  return true;
}

}  // namespace oeeoo
