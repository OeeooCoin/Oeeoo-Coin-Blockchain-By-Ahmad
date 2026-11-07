// SPDX-License-Identifier: MIT
// Oeeoo Coin — wire protocol implementation.
#include "network/protocol.h"

#include <cstring>

#include "crypto/hash160.h"
#include "serialize/endian.h"
#include "serialize/varint.h"

namespace oeeoo {

namespace {
// Active network identity (Milestone 20). Process-wide, mainnet
// by default; node_main selects it once at startup. Tests switch
// it only around the code under test and restore mainnet after.
NetworkId g_activeNetwork = NetworkId::kMainnet;
}  // namespace

const NetworkIdentity& NetworkIdentityFor(NetworkId id) {
  return id == NetworkId::kTestnet ? kTestnetIdentity : kMainnetIdentity;
}

void SelectNetwork(NetworkId id) { g_activeNetwork = id; }

NetworkId ActiveNetwork() { return g_activeNetwork; }

const NetworkIdentity& ActiveNetworkIdentity() {
  return NetworkIdentityFor(g_activeNetwork);
}

std::array<unsigned char, 4> ActiveNetworkMagic() {
  return ActiveNetworkIdentity().magic;
}

std::string ActiveUserAgent() {
  return ActiveNetworkIdentity().userAgent;
}

namespace {
// Bounds-checked reader over a payload.
struct Reader {
  const unsigned char* data;
  std::size_t len;
  std::size_t off = 0;
  bool Need(std::size_t n) const { return len - off >= n; }
  bool U8(uint8_t& v) {
    if (!Need(1)) return false;
    v = data[off++];
    return true;
  }
  bool U16LE(uint16_t& v) {
    if (!Need(2)) return false;
    v = static_cast<uint16_t>(data[off]) |
        (static_cast<uint16_t>(data[off + 1]) << 8);
    off += 2;
    return true;
  }
  bool U32LE(uint32_t& v) {
    if (!Need(4)) return false;
    v = ReadLE32(data + off);
    off += 4;
    return true;
  }
  bool I32LE(int32_t& v) {
    uint32_t u = 0;
    if (!U32LE(u)) return false;
    v = static_cast<int32_t>(u);
    return true;
  }
  bool U64LE(uint64_t& v) {
    if (!Need(8)) return false;
    v = ReadLE64(data + off);
    off += 8;
    return true;
  }
  bool I64LE(int64_t& v) {
    uint64_t u = 0;
    if (!U64LE(u)) return false;
    v = static_cast<int64_t>(u);
    return true;
  }
  bool Bytes(unsigned char* dst, std::size_t n) {
    if (!Need(n)) return false;
    std::memcpy(dst, data + off, n);
    off += n;
    return true;
  }
  bool VarInt(uint64_t& v) { return ReadVarInt(data, len, off, v); }
  bool Str(std::string& s) {
    uint64_t n = 0;
    if (!VarInt(n)) return false;
    if (n > len - off) return false;
    s.assign(reinterpret_cast<const char*>(data + off),
             static_cast<std::size_t>(n));
    off += static_cast<std::size_t>(n);
    return true;
  }
};

void AppendVarInt(std::vector<unsigned char>& out, uint64_t v) {
  const std::vector<unsigned char> enc = WriteVarInt(v);
  out.insert(out.end(), enc.begin(), enc.end());
}
void AppendStr(std::vector<unsigned char>& out, const std::string& s) {
  AppendVarInt(out, s.size());
  out.insert(out.end(), s.begin(), s.end());
}
}  // namespace

std::vector<unsigned char> SerializeMessage(const Message& msg) {
  std::vector<unsigned char> out;
  out.reserve(kMessageHeaderSize + msg.payload.size());
  // Milestone 20: the wire magic is the ACTIVE network's magic,
  // so a testnet process frames every message with the testnet
  // magic and a mainnet process with the mainnet magic.
  const std::array<unsigned char, 4> magic = ActiveNetworkMagic();
  out.insert(out.end(), magic.begin(), magic.end());
  std::string cmd = msg.command.substr(0, 12);
  out.insert(out.end(), cmd.begin(), cmd.end());
  out.resize(out.size() + (12 - cmd.size()), 0);
  AppendLE32(out, static_cast<uint32_t>(msg.payload.size()));
  Hash256 checksum = Sha256d(msg.payload);
  out.insert(out.end(), checksum.begin(), checksum.begin() + 4);
  out.insert(out.end(), msg.payload.begin(), msg.payload.end());
  return out;
}

bool ParseMessage(const std::vector<unsigned char>& bytes, std::size_t offset,
                  Message& msg, std::size_t& bytesConsumed) {
  bytesConsumed = 0;
  if (bytes.size() - offset < kMessageHeaderSize) return false;
  const unsigned char* h = bytes.data() + offset;
  // Milestone 20: the magic check is against the ACTIVE
  // network's magic — bytes framed for the other network are
  // rejected here, before any dispatch. (Node::OnBytes repeats
  // this check against the active magic, too.)
  const std::array<unsigned char, 4> magic = ActiveNetworkMagic();
  if (std::memcmp(h, magic.data(), 4) != 0) return false;
  std::string command(reinterpret_cast<const char*>(h + 4), 12);
  const std::size_t nul = command.find('\0');
  if (nul != std::string::npos) command.resize(nul);
  if (command.empty()) return false;
  const uint32_t length = ReadLE32(h + 16);
  if (length > kMaxMessageLength) return false;
  if (bytes.size() - offset - kMessageHeaderSize < length) return false;
  const unsigned char* payload = h + kMessageHeaderSize;
  Hash256 checksum = Sha256d(
      std::vector<unsigned char>(payload, payload + length));
  if (std::memcmp(checksum.data(), h + 20, 4) != 0) return false;
  msg.command = command;
  msg.payload.assign(payload, payload + length);
  bytesConsumed = kMessageHeaderSize + length;
  return true;
}

std::vector<unsigned char> SerializeVersion(const VersionPayload& v) {
  std::vector<unsigned char> out;
  AppendLE32(out, static_cast<uint32_t>(v.version));
  AppendLE64(out, v.services);
  AppendLE64(out, static_cast<uint64_t>(v.timestamp));
  AppendLE64(out, v.nonce);
  AppendStr(out, v.userAgent);
  AppendLE32(out, static_cast<uint32_t>(v.startHeight));
  out.push_back(v.relay ? 1 : 0);
  return out;
}

bool ParseVersion(const std::vector<unsigned char>& payload,
                  VersionPayload& out) {
  Reader r{payload.data(), payload.size(), 0};
  if (!r.I32LE(out.version)) return false;
  if (!r.U64LE(out.services)) return false;
  if (!r.I64LE(out.timestamp)) return false;
  if (!r.U64LE(out.nonce)) return false;
  if (!r.Str(out.userAgent)) return false;
  if (!r.I32LE(out.startHeight)) return false;
  uint8_t relay = 0;
  if (!r.U8(relay)) return false;
  out.relay = relay != 0;
  return true;
}

std::vector<unsigned char> SerializeInventory(
    const std::vector<InventoryItem>& items) {
  std::vector<unsigned char> out;
  AppendVarInt(out, items.size());
  for (const InventoryItem& item : items) {
    AppendLE32(out, item.type);
    out.insert(out.end(), item.hash.begin(), item.hash.end());
  }
  return out;
}

bool ParseInventory(const std::vector<unsigned char>& payload,
                    std::vector<InventoryItem>& out) {
  Reader r{payload.data(), payload.size(), 0};
  uint64_t count = 0;
  if (!r.VarInt(count)) return false;
  if (count > 50000) return false;
  out.clear();
  for (uint64_t i = 0; i < count; ++i) {
    InventoryItem item;
    if (!r.U32LE(item.type)) return false;
    if (!r.Bytes(item.hash.data(), 32)) return false;
    out.push_back(item);
  }
  return true;
}

std::vector<unsigned char> SerializePing(uint64_t nonce) {
  std::vector<unsigned char> out;
  AppendLE64(out, nonce);
  return out;
}

bool ParsePing(const std::vector<unsigned char>& payload, uint64_t& nonce) {
  if (payload.size() != 8) return false;
  Reader r{payload.data(), payload.size(), 0};
  return r.U64LE(nonce);
}

std::vector<unsigned char> SerializeGetHeaders(
    int32_t version, const std::vector<Hash256>& locator,
    const Hash256& hashStop) {
  std::vector<unsigned char> out;
  AppendLE32(out, static_cast<uint32_t>(version));
  AppendVarInt(out, locator.size());
  for (const Hash256& h : locator) {
    out.insert(out.end(), h.begin(), h.end());
  }
  out.insert(out.end(), hashStop.begin(), hashStop.end());
  return out;
}

bool ParseGetHeaders(const std::vector<unsigned char>& payload,
                     int32_t& version, std::vector<Hash256>& locator,
                     Hash256& hashStop) {
  Reader r{payload.data(), payload.size(), 0};
  if (!r.I32LE(version)) return false;
  uint64_t count = 0;
  if (!r.VarInt(count)) return false;
  if (count > 2000) return false;
  locator.clear();
  for (uint64_t i = 0; i < count; ++i) {
    Hash256 h;
    if (!r.Bytes(h.data(), 32)) return false;
    locator.push_back(h);
  }
  if (!r.Bytes(hashStop.data(), 32)) return false;
  return true;
}

std::vector<unsigned char> SerializeHeaders(
    const std::vector<BlockHeader>& headers) {
  std::vector<unsigned char> out;
  AppendVarInt(out, headers.size());
  for (const BlockHeader& h : headers) {
    std::vector<unsigned char> bytes = h.Serialize();
    out.insert(out.end(), bytes.begin(), bytes.end());
  }
  return out;
}

bool ParseHeaders(const std::vector<unsigned char>& payload,
                  std::vector<BlockHeader>& out) {
  Reader r{payload.data(), payload.size(), 0};
  uint64_t count = 0;
  if (!r.VarInt(count)) return false;
  if (count > 2000) return false;
  out.clear();
  for (uint64_t i = 0; i < count; ++i) {
    std::vector<unsigned char> bytes(80);
    if (!r.Bytes(bytes.data(), 80)) return false;
    BlockHeader h;
    if (!BlockHeader::Deserialize(bytes, h)) return false;
    out.push_back(h);
  }
  return true;
}

bool ParseIPv4(const std::string& ip, std::array<unsigned char, 4>& out) {
  std::size_t start = 0;
  for (int i = 0; i < 4; ++i) {
    std::size_t dot = ip.find('.', start);
    std::string part = ip.substr(start, dot == std::string::npos
                                            ? std::string::npos
                                            : dot - start);
    if (part.empty() || part.size() > 3) return false;
    int value = 0;
    for (char c : part) {
      if (c < '0' || c > '9') return false;
      value = value * 10 + (c - '0');
    }
    if (value > 255) return false;
    out[i] = static_cast<unsigned char>(value);
    if (dot == std::string::npos) {
      if (i != 3) return false;
      break;
    }
    start = dot + 1;
  }
  return true;
}

namespace {
std::string FormatIPv4(const unsigned char* bytes) {
  return std::to_string(bytes[0]) + "." + std::to_string(bytes[1]) + "." +
         std::to_string(bytes[2]) + "." + std::to_string(bytes[3]);
}
}  // namespace

std::vector<unsigned char> SerializeAddr(const std::vector<AddrEntry>& addrs) {
  // Serialize first, then write the count: the count must equal the
  // number of entries actually written (unparseable IPs are
  // skipped), or a peer would read past the end of the payload.
  std::vector<unsigned char> body;
  std::size_t count = 0;
  for (const AddrEntry& a : addrs) {
    std::array<unsigned char, 4> ipv4{};
    if (!ParseIPv4(a.ip, ipv4)) continue;  // skip unparseable entries
    ++count;
    AppendLE32(body, a.time);
    AppendLE64(body, a.services);
    // IPv4-mapped IPv6: 10 zero bytes, 0xffff, 4 IPv4 bytes.
    body.insert(body.end(), 10, 0);
    body.push_back(0xff);
    body.push_back(0xff);
    body.insert(body.end(), ipv4.begin(), ipv4.end());
    body.push_back(static_cast<unsigned char>((a.port >> 8) & 0xff));
    body.push_back(static_cast<unsigned char>(a.port & 0xff));
  }
  std::vector<unsigned char> out;
  AppendVarInt(out, count);
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

bool ParseAddr(const std::vector<unsigned char>& payload,
               std::vector<AddrEntry>& out) {
  Reader r{payload.data(), payload.size(), 0};
  uint64_t count = 0;
  if (!r.VarInt(count)) return false;
  if (count > 1000) return false;
  out.clear();
  for (uint64_t i = 0; i < count; ++i) {
    AddrEntry a;
    if (!r.U32LE(a.time)) return false;
    if (!r.U64LE(a.services)) return false;
    unsigned char ipBytes[16];
    if (!r.Bytes(ipBytes, 16)) return false;
    // Accept only IPv4-mapped addresses (10 zero bytes, 0xffff);
    // anything else is malformed and rejects the whole message.
    for (int i = 0; i < 10; ++i) {
      if (ipBytes[i] != 0) return false;
    }
    if (ipBytes[10] != 0xff || ipBytes[11] != 0xff) return false;
    a.ip = FormatIPv4(ipBytes + 12);
    uint8_t hi = 0, lo = 0;
    if (!r.U8(hi)) return false;
    if (!r.U8(lo)) return false;
    a.port = static_cast<uint16_t>((hi << 8) | lo);
    out.push_back(a);
  }
  return true;
}

std::vector<unsigned char> SerializeSendCmpct(bool announce, uint64_t version) {
  std::vector<unsigned char> out;
  out.push_back(announce ? 1 : 0);
  AppendLE64(out, version);
  return out;
}

bool ParseSendCmpct(const std::vector<unsigned char>& payload, bool& announce,
                    uint64_t& version) {
  if (payload.size() != 9) return false;
  Reader r{payload.data(), payload.size(), 0};
  uint8_t a = 0;
  if (!r.U8(a)) return false;
  announce = a != 0;
  return r.U64LE(version);
}

std::vector<unsigned char> SerializeReject(const std::string& command,
                                           unsigned char code,
                                           const std::string& reason) {
  std::vector<unsigned char> out;
  AppendStr(out, command);
  out.push_back(code);
  AppendStr(out, reason);
  return out;
}

bool ParseReject(const std::vector<unsigned char>& payload,
                 std::string& command, unsigned char& code,
                 std::string& reason) {
  Reader r{payload.data(), payload.size(), 0};
  if (!r.Str(command)) return false;
  if (!r.U8(code)) return false;
  return r.Str(reason);
}

}  // namespace oeeoo
