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
//
// Network identity (Milestone 20, audit OOC-SA-001 §3.3): mainnet
// and the test network have DISTINCT identities (magic, default
// ports, user agent) so a node on one network can never handshake
// with a node on the other — the magic check drops cross-network
// bytes. Protocol version is the SAME on both networks (70015);
// the magic mismatch is what separates them. The active identity
// is process-wide and defaults to mainnet; node_main selects it
// once at startup from --testnet / `network=testnet`.
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
constexpr std::array<unsigned char, 4> kMainnetMagic = {0xe2, 0xd8, 0x04,
                                                        0xd6};
// Historical name for the mainnet magic. Kept so existing code
// keeps compiling; new code must use ActiveNetworkMagic(), which
// follows the selected network. (M20: Node::OnBytes now compares
// against ActiveNetworkMagic().)
constexpr std::array<unsigned char, 4> kNetworkMagic = kMainnetMagic;
// Oeeoo Coin test-network magic (Milestone 20): first 4 bytes of
// SHA-256 of "Oeeoo Coin Testnet Network Magic", hash order — the
// SAME derivation as mainnet (verified: the mainnet string hashes
// to e2 d8 04 d6). Computed with python3 hashlib (see
// tests/test_milestone20d.cpp): d9 73 08 c2.
constexpr std::array<unsigned char, 4> kTestnetMagic = {0xd9, 0x73, 0x08,
                                                        0xc2};
constexpr uint16_t kMainnetPort = 64775;
constexpr uint16_t kMainnetRpcPort = 64776;
constexpr uint16_t kMainnetStratumPort = 64777;
// Test-network default ports: the mainnet triple shifted by +4
// (64779/64780/64781), so a mainnet node and a testnet node can
// run side by side on one host with no listener colliding on any
// of P2P, RPC, or Stratum. None of 64779–64781 collides with the
// mainnet ports 64775–64777. An operator-set port always wins
// over these defaults (AppConfig::Effective*()).
constexpr uint16_t kTestnetPort = 64779;
constexpr uint16_t kTestnetRpcPort = 64780;
constexpr uint16_t kTestnetStratumPort = 64781;
// User agents. The testnet marker "(testnet)" is what a peer (and
// the operator reading a log) sees in the version handshake.
constexpr const char* kMainnetUserAgent = "/OeeooCoin:0.1.0/";
constexpr const char* kTestnetUserAgent = "/OeeooCoin:0.1.0(testnet)/";

// One network's full wire identity. Protocol version is NOT part
// of it: kProtocolVersion (70015, below) is the same on both
// networks by design — the magic mismatch separates them.
struct NetworkIdentity {
  std::array<unsigned char, 4> magic;
  uint16_t p2pPort;      // default P2P listen port
  uint16_t rpcPort;      // default JSON-RPC port (127.0.0.1 only)
  uint16_t stratumPort;  // default Stratum port (127.0.0.1 only)
  const char* userAgent;
  const char* name;  // "mainnet" / "testnet"
};

constexpr NetworkIdentity kMainnetIdentity = {
    kMainnetMagic, kMainnetPort, kMainnetRpcPort, kMainnetStratumPort,
    kMainnetUserAgent, "mainnet"};
constexpr NetworkIdentity kTestnetIdentity = {
    kTestnetMagic, kTestnetPort, kTestnetRpcPort, kTestnetStratumPort,
    kTestnetUserAgent, "testnet"};

enum class NetworkId { kMainnet, kTestnet };

// Identity for a network id (no selection involved).
const NetworkIdentity& NetworkIdentityFor(NetworkId id);
// Select the process-wide active network (default: mainnet).
// node_main calls this once at startup from --testnet /
// `network=testnet`. SerializeMessage / ParseMessage use the
// active magic, so one process speaks exactly one network.
void SelectNetwork(NetworkId id);
NetworkId ActiveNetwork();
const NetworkIdentity& ActiveNetworkIdentity();
// Magic / user agent of the active network.
std::array<unsigned char, 4> ActiveNetworkMagic();
std::string ActiveUserAgent();
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
  // Connection-failure count, maintained by AddressManager
  // (RecordFailure / RecordSuccess). NOT part of the wire format:
  // SerializeAddr / ParseAddr carry only time/services/ip/port, so
  // this field never crosses the network and is always 0 for an
  // entry freshly parsed from an `addr` message.
  int failures = 0;
};

// Parse a dotted-quad IPv4 address ("1.2.3.4"). Returns false for
// anything else (empty string, bad octets, trailing junk). Shared
// with the address manager so it only stores valid addresses.
bool ParseIPv4(const std::string& ip, std::array<unsigned char, 4>& out);
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
