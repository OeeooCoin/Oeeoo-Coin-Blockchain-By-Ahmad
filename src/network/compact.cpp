// SPDX-License-Identifier: MIT
// Oeeoo Coin — compact block implementation.
#include "network/compact.h"

#include <cstring>
#include <unordered_map>

#include "crypto/sha256.h"
#include "network/siphash.h"
#include "serialize/endian.h"
#include "serialize/varint.h"

namespace oeeoo {

namespace {
void AppendVarInt(std::vector<unsigned char>& out, uint64_t v) {
  const std::vector<unsigned char> enc = WriteVarInt(v);
  out.insert(out.end(), enc.begin(), enc.end());
}

struct Reader {
  const unsigned char* data;
  std::size_t len;
  std::size_t off = 0;
  bool Need(std::size_t n) const { return len - off >= n; }
  bool Bytes(unsigned char* dst, std::size_t n) {
    if (!Need(n)) return false;
    std::memcpy(dst, data + off, n);
    off += n;
    return true;
  }
  bool U64LE(uint64_t& v) {
    if (!Need(8)) return false;
    v = ReadLE64(data + off);
    off += 8;
    return true;
  }
  bool U32LE(uint32_t& v) {
    if (!Need(4)) return false;
    v = ReadLE32(data + off);
    off += 4;
    return true;
  }
  bool VarInt(uint64_t& v) { return ReadVarInt(data, len, off, v); }
};

void SipKeys(const BlockHeader& header, uint64_t nonce, uint64_t& k0,
             uint64_t& k1) {
  std::vector<unsigned char> bytes = header.Serialize();
  AppendLE64(bytes, nonce);
  Hash256 keyMaterial = Sha256d(bytes);
  std::memcpy(&k0, keyMaterial.data(), 8);
  std::memcpy(&k1, keyMaterial.data() + 8, 8);
}
}  // namespace

std::array<unsigned char, 6> ShortTxId(const BlockHeader& header,
                                       uint64_t nonce, const Hash256& txid) {
  uint64_t k0 = 0, k1 = 0;
  SipKeys(header, nonce, k0, k1);
  std::vector<unsigned char> data(txid.begin(), txid.end());
  const uint64_t hash = SipHash24(k0, k1, data);
  std::array<unsigned char, 6> out{};
  for (int i = 0; i < 6; ++i) {
    out[i] = static_cast<unsigned char>((hash >> (8 * i)) & 0xff);
  }
  return out;
}

CompactBlock MakeCompactBlock(const Block& block, uint64_t nonce) {
  CompactBlock cb;
  cb.header = block.header;
  cb.nonce = nonce;
  cb.txCount = static_cast<uint32_t>(block.vtx.size());
  cb.prefilled.push_back({0, block.vtx[0]});  // coinbase always prefilled
  for (std::size_t i = 1; i < block.vtx.size(); ++i) {
    cb.shortIds.push_back(
        ShortTxId(block.header, nonce, block.vtx[i].GetTxId()));
  }
  return cb;
}

std::vector<unsigned char> SerializeCompactBlock(const CompactBlock& cb) {
  std::vector<unsigned char> out;
  std::vector<unsigned char> headerBytes = cb.header.Serialize();
  out.insert(out.end(), headerBytes.begin(), headerBytes.end());
  AppendLE64(out, cb.nonce);
  AppendVarInt(out, cb.txCount);
  AppendVarInt(out, cb.shortIds.size());
  for (const auto& id : cb.shortIds) {
    out.insert(out.end(), id.begin(), id.end());
  }
  AppendVarInt(out, cb.prefilled.size());
  for (const auto& entry : cb.prefilled) {
    AppendVarInt(out, entry.first);
    std::vector<unsigned char> txBytes = entry.second.Serialize();
    out.insert(out.end(), txBytes.begin(), txBytes.end());
  }
  return out;
}

bool ParseCompactBlock(const std::vector<unsigned char>& payload,
                       CompactBlock& out) {
  Reader r{payload.data(), payload.size(), 0};
  std::vector<unsigned char> headerBytes(80);
  if (!r.Bytes(headerBytes.data(), 80)) return false;
  if (!BlockHeader::Deserialize(headerBytes, out.header)) return false;
  if (!r.U64LE(out.nonce)) return false;
  uint64_t txCount = 0;
  if (!r.VarInt(txCount)) return false;
  if (txCount == 0 || txCount > 100000) return false;
  out.txCount = static_cast<uint32_t>(txCount);
  uint64_t count = 0;
  if (!r.VarInt(count)) return false;
  if (count > 100000) return false;
  out.shortIds.clear();
  for (uint64_t i = 0; i < count; ++i) {
    std::array<unsigned char, 6> id{};
    if (!r.Bytes(id.data(), 6)) return false;
    out.shortIds.push_back(id);
  }
  if (!r.VarInt(count)) return false;
  if (count > 100000) return false;
  out.prefilled.clear();
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t index = 0;
    if (!r.VarInt(index)) return false;
    Transaction tx;
    std::size_t consumed = 0;
    if (!Transaction::Deserialize(payload, r.off, tx, consumed)) return false;
    r.off += consumed;
    out.prefilled.push_back(
        {static_cast<uint32_t>(index), std::move(tx)});
  }
  return r.off == r.len;
}

std::vector<unsigned char> SerializeGetBlockTxn(
    const Hash256& blockHash, const std::vector<uint32_t>& indexes) {
  std::vector<unsigned char> out;
  out.insert(out.end(), blockHash.begin(), blockHash.end());
  AppendVarInt(out, indexes.size());
  for (uint32_t index : indexes) AppendVarInt(out, index);
  return out;
}

bool ParseGetBlockTxn(const std::vector<unsigned char>& payload,
                      Hash256& blockHash, std::vector<uint32_t>& indexes) {
  Reader r{payload.data(), payload.size(), 0};
  if (!r.Bytes(blockHash.data(), 32)) return false;
  uint64_t count = 0;
  if (!r.VarInt(count)) return false;
  if (count > 100000) return false;
  indexes.clear();
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t index = 0;
    if (!r.VarInt(index)) return false;
    indexes.push_back(static_cast<uint32_t>(index));
  }
  return r.off == r.len;
}

std::vector<unsigned char> SerializeBlockTxn(
    const Hash256& blockHash,
    const std::vector<std::pair<uint32_t, Transaction>>& txs) {
  std::vector<unsigned char> out;
  out.insert(out.end(), blockHash.begin(), blockHash.end());
  AppendVarInt(out, txs.size());
  for (const auto& entry : txs) {
    AppendVarInt(out, entry.first);
    std::vector<unsigned char> txBytes = entry.second.Serialize();
    out.insert(out.end(), txBytes.begin(), txBytes.end());
  }
  return out;
}

bool ParseBlockTxn(const std::vector<unsigned char>& payload,
                   Hash256& blockHash,
                   std::vector<std::pair<uint32_t, Transaction>>& txs) {
  Reader r{payload.data(), payload.size(), 0};
  if (!r.Bytes(blockHash.data(), 32)) return false;
  uint64_t count = 0;
  if (!r.VarInt(count)) return false;
  if (count > 100000) return false;
  txs.clear();
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t index = 0;
    if (!r.VarInt(index)) return false;
    Transaction tx;
    std::size_t consumed = 0;
    if (!Transaction::Deserialize(payload, r.off, tx, consumed)) return false;
    r.off += consumed;
    txs.push_back({static_cast<uint32_t>(index), std::move(tx)});
  }
  return r.off == r.len;
}

bool ReconstructBlock(const CompactBlock& cb, const Mempool& mempool,
                      Block& out, std::vector<uint32_t>& missing) {
  // Map short IDs to mempool transactions.
  std::unordered_map<std::string, Transaction> byShortId;
  for (const Mempool::Entry& entry : mempool.GetByFeeRate()) {
    std::array<unsigned char, 6> id =
        ShortTxId(cb.header, cb.nonce, entry.tx.GetTxId());
    byShortId[std::string(reinterpret_cast<const char*>(id.data()),
                          id.size())] = entry.tx;
  }
  const std::size_t txCount = cb.txCount;
  if (txCount == 0) return false;
  // Note: shortIds + prefilled may EXCEED txCount after the receiver
  // merges blocktxn answers into prefilled (a slot is then both
  // prefilled and short-id'd). The loop below treats prefilled as
  // authoritative; the header's Merkle root (checked by the caller)
  // catches a wrong transaction in a merged slot.
  if (cb.shortIds.size() > txCount) return false;
  if (cb.prefilled.size() > txCount) return false;
  std::vector<Transaction> txs(txCount);
  std::vector<bool> filled(txCount, false);
  for (const auto& entry : cb.prefilled) {
    if (entry.first >= txCount) return false;
    txs[entry.first] = entry.second;
    filled[entry.first] = true;
  }
  missing.clear();
  std::size_t shortIdx = 0;
  for (std::size_t i = 0; i < txCount; ++i) {
    if (filled[i]) continue;
    if (shortIdx >= cb.shortIds.size()) return false;
    const std::array<unsigned char, 6>& id = cb.shortIds[shortIdx++];
    auto it = byShortId.find(
        std::string(reinterpret_cast<const char*>(id.data()), id.size()));
    if (it == byShortId.end()) {
      missing.push_back(static_cast<uint32_t>(i));
    } else {
      txs[i] = it->second;
      filled[i] = true;
    }
  }
  if (!missing.empty()) return false;
  out.header = cb.header;
  out.vtx = std::move(txs);
  return true;
}

}  // namespace oeeoo
