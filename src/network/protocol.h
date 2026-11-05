// SPDX-License-Identifier: MIT
// Oeeoo Coin — P2P wire protocol (Milestone 6).
// Message framing (Bitcoin-style):
//   magic     4 bytes  e2 d8 04 d6 (Oeeoo mainnet magic)
//   command   12 bytes ASCII, null-padded
//   length    uint32 LE (payload bytes)
//   checksum  4 bytes  first 4 bytes of SHA256d(payload)
//   payload   `length` bytes
// Maximum message: 2 MB. Every received message is checked for magic,
// length and checksum before dispatch. Unknown commands are ignored
// (a peer is not punished for a command we do not know yet).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "blockchain/block.h"
#include "crypto/sha256.h"

namespace oeeoo {

// Oeeoo Coin mainnet network magic (SHA-256 of
// "Oeeoo Coin Mainnet Network Magic", first 4 bytes, hash order).
constexpr std::array<unsigned char, 4> kNetworkMagic = {0xe2, 0xd8, 0x04, 0xd6};
constexpr uint16_t kMainnetPort = 64775;
constexpr std::size_t kMaxMessageLength = 2 * 1024 * 1024;
constexpr std::size_t kMessageHeaderSize = 24;
constexpr int32_t kProtocolVersion = 70015;

// Inventory types.
constexpr uint32_t kInvTx = 1;
constexpr uint32_t kInvBlock = 2;

struct Message {
  std::string command;              // e.g. "version", "tx", "block"
  std::vector<unsigned char> payload;
};

// Serialize a message (header + payload).
std::vector<unsigned char> SerializeMessage(const Message& msg);
// Parse one message from `bytes` starting at `offset`. On success
// sets `msg` and `bytesConsumed` (header + payload) and returns true.
// Returns false on truncation, bad magic, oversize length, or bad
// checksum. `bytesConsumed` is 0 on failure.
bool ParseMessage(const std::vector<unsigned char>& bytes, std::size_t offset,
                  Message& msg, std::size_t& bytesConsumed);

// ---- Payload builders / parsers (all little-endian unless noted) ----

struct VersionPayload {
  int32_t version = kProtocolVersion;
  uint64_t services = 0;
  int64_t timestamp = 0;
  uint64_t nonce = 0;
  std::string userAgent;  // e.g. "/OeeooCoin:0.1.0/"
  int32_t startHeight = 0;
  bool relay = true;
};

std::vector<unsigned char> SerializeVersion(const VersionPayload& v);
bool ParseVersion(const std::vector<unsigned char>& payload,
                  VersionPayload& out);

struct InventoryItem {
  uint32_t type = 0;  // kInvTx / kInvBlock
  Hash256 hash{};
};

// inv / getdata / notfound payload: VarInt count + items.
std::vector<unsigned char> SerializeInventory(
    const std::vector<InventoryItem>& items);
bool ParseInventory(const std::vector<unsigned char>& payload,
                    std::vector<InventoryItem>& out);

// ping / pong payload: 8-byte nonce LE.
std::vector<unsigned char> SerializePing(uint64_t nonce);
bool ParsePing(const std::vector<unsigned char>& payload, uint64_t& nonce);

// getheaders payload: version + locator hashes + hashStop.
std::vector<unsigned char> SerializeGetHeaders(
    int32_t version, const std::vector<Hash256>& locator,
    const Hash256& hashStop);
bool ParseGetHeaders(const std::vector<unsigned char>& payload,
                     int32_t& version, std::vector<Hash256>& locator,
                     Hash256& hashStop);

// headers payload: VarInt count + 80-byte headers (no tx count).
std::vector<unsigned char> SerializeHeaders(
    const std::vector<BlockHeader>& headers);
bool ParseHeaders(const std::vector<unsigned char>& payload,
                  std::vector<BlockHeader>& out);

// addr payload: VarInt count + entries (time u32, services u64,
// IPv4-mapped IPv6 16 bytes, port u16 big-endian).
struct AddrEntry {
  uint32_t time = 0;
  uint64_t services = 0;
  std::string ip;  // dotted IPv4, e.g. "1.2.3.4"
  uint16_t port = kMainnetPort;
};
std::vector<unsigned char> SerializeAddr(const std::vector<AddrEntry>& addrs);
bool ParseAddr(const std::vector<unsigned char>& payload,
               std::vector<AddrEntry>& out);

// sendcmpct payload: announce (bool byte) + version (uint64 LE).
std::vector<unsigned char> SerializeSendCmpct(bool announce, uint64_t version);
bool ParseSendCmpct(const std::vector<unsigned char>& payload, bool& announce,
                    uint64_t& version);

// reject payload: command (string) + code (byte) + reason (string).
std::vector<unsigned char> SerializeReject(const std::string& command,
                                           unsigned char code,
                                           const std::string& reason);
bool ParseReject(const std::vector<unsigned char>& payload,
                 std::string& command, unsigned char& code,
                 std::string& reason);

}  // namespace oeeoo
