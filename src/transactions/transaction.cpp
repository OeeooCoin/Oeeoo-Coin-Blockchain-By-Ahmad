#include "transactions/transaction.h"

#include <set>

#include "consensus/consensus.h"
#include "serialize/endian.h"
#include "serialize/varint.h"

namespace oeeoo {

namespace {

// Bounds-checked cursor over a byte buffer.
struct Cursor {
  const unsigned char* data;
  std::size_t len;
  std::size_t off = 0;

  bool Need(std::size_t n) const { return len - off >= n; }
  bool ReadBytes(unsigned char* dst, std::size_t n) {
    if (!Need(n)) return false;
    for (std::size_t i = 0; i < n; ++i) dst[i] = data[off + i];
    off += n;
    return true;
  }
  bool ReadU32(uint32_t& v) {
    if (!Need(4)) return false;
    v = ReadLE32(data + off);
    off += 4;
    return true;
  }
  bool ReadI32(int32_t& v) {
    uint32_t u = 0;
    if (!ReadU32(u)) return false;
    v = static_cast<int32_t>(u);
    return true;
  }
  bool ReadI64(int64_t& v) {
    if (!Need(8)) return false;
    v = static_cast<int64_t>(ReadLE64(data + off));
    off += 8;
    return true;
  }
  bool ReadVarInt(uint64_t& v) { return oeeoo::ReadVarInt(data, len, off, v); }
  bool ReadScript(std::vector<unsigned char>& script) {
    uint64_t n = 0;
    if (!ReadVarInt(n)) return false;
    if (n > len - off) return false;  // script longer than input left
    script.assign(data + off, data + off + static_cast<std::size_t>(n));
    off += static_cast<std::size_t>(n);
    return true;
  }
};

void AppendVarInt(std::vector<unsigned char>& out, uint64_t v) {
  const std::vector<unsigned char> enc = WriteVarInt(v);
  out.insert(out.end(), enc.begin(), enc.end());
}

}  // namespace

std::vector<unsigned char> Transaction::Serialize() const {
  std::vector<unsigned char> out;
  AppendLE32(out, static_cast<uint32_t>(version));
  AppendVarInt(out, vin.size());
  for (const TxIn& in : vin) {
    out.insert(out.end(), in.prevTxHash.begin(), in.prevTxHash.end());
    AppendLE32(out, in.prevOutIndex);
    AppendVarInt(out, in.scriptSig.size());
    out.insert(out.end(), in.scriptSig.begin(), in.scriptSig.end());
    AppendLE32(out, in.sequence);
  }
  AppendVarInt(out, vout.size());
  for (const TxOut& o : vout) {
    AppendLE64(out, static_cast<uint64_t>(o.value));
    AppendVarInt(out, o.scriptPubKey.size());
    out.insert(out.end(), o.scriptPubKey.begin(), o.scriptPubKey.end());
  }
  AppendLE32(out, lockTime);
  return out;
}

bool Transaction::Deserialize(const std::vector<unsigned char>& bytes,
                              Transaction& out) {
  if (bytes.empty() || bytes.size() > static_cast<std::size_t>(MAX_BLOCK_SIZE)) {
    return false;
  }
  std::size_t consumed = 0;
  if (!Deserialize(bytes, 0, out, consumed)) return false;
  if (consumed != bytes.size()) return false;  // trailing bytes
  return true;
}

bool Transaction::Deserialize(const std::vector<unsigned char>& bytes,
                              std::size_t offset, Transaction& out,
                              std::size_t& bytesConsumed) {
  if (offset >= bytes.size()) return false;
  Cursor c{bytes.data(), bytes.size(), offset};
  Transaction tx;
  if (!c.ReadI32(tx.version)) return false;
  uint64_t nIn = 0;
  if (!c.ReadVarInt(nIn)) return false;
  // Each input is at least 41 bytes; reject absurd counts up front.
  if (nIn > (bytes.size() / 41) + 1) return false;
  for (uint64_t i = 0; i < nIn; ++i) {
    TxIn in;
    if (!c.ReadBytes(in.prevTxHash.data(), 32)) return false;
    if (!c.ReadU32(in.prevOutIndex)) return false;
    if (!c.ReadScript(in.scriptSig)) return false;
    if (!c.ReadU32(in.sequence)) return false;
    tx.vin.push_back(std::move(in));
  }
  uint64_t nOut = 0;
  if (!c.ReadVarInt(nOut)) return false;
  if (nOut > (bytes.size() / 9) + 1) return false;
  for (uint64_t i = 0; i < nOut; ++i) {
    TxOut o;
    if (!c.ReadI64(o.value)) return false;
    if (!c.ReadScript(o.scriptPubKey)) return false;
    tx.vout.push_back(std::move(o));
  }
  if (!c.ReadU32(tx.lockTime)) return false;
  bytesConsumed = c.off - offset;
  out = std::move(tx);
  return true;
}

bool IsCoinbase(const Transaction& tx) {
  return tx.vin.size() == 1 && tx.vin[0].prevTxHash == Hash256{} &&
         tx.vin[0].prevOutIndex == 0xffffffff;
}

Hash256 Transaction::GetTxId() const { return Sha256d(Serialize()); }

std::string Transaction::GetTxIdHex() const { return HexStr(GetTxId()); }

bool Transaction::CheckBasic() const {
  if (vin.empty() || vout.empty()) return false;
  if (Serialize().size() > static_cast<std::size_t>(MAX_BLOCK_SIZE)) {
    return false;
  }
  __int128 totalOut = 0;
  for (const TxOut& o : vout) {
    if (!MoneyRange(o.value)) return false;
    totalOut += o.value;
    if (totalOut > MAX_MONEY) return false;
  }
  std::set<std::pair<Hash256, uint32_t>> seen;
  for (const TxIn& in : vin) {
    if (!seen.insert({in.prevTxHash, in.prevOutIndex}).second) {
      return false;  // duplicate input
    }
  }
  return true;
}

}  // namespace oeeoo
