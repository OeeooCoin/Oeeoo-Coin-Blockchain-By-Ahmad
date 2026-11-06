// SPDX-License-Identifier: MIT
// Oeeoo Coin — peer state, misbehavior scoring and bans (Milestone 6).
// Each connected peer tracks its handshake state, protocol version,
// advertised height, compact-block support, ping state, recently seen
// inventory (to avoid re-requesting), and a misbehavior score.
// Score >= 100 bans the peer: progressive ban durations (1st ban 24h,
// then doubling up to 30 days). Bans are keyed by IP ALONE
// (Milestone 16): BanManager::BanKey strips the ephemeral TCP port
// from "ip:port" before the key is stored or looked up, so a ban
// survives a reconnect from a new ephemeral port. Peer.address
// keeps the full "ip:port" for display/diagnostics — it is NOT the
// ban key. Bans are enforced by BanManager.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "crypto/sha256.h"
#include "network/protocol.h"

namespace oeeoo {

constexpr int kBanScoreThreshold = 100;
constexpr uint32_t kBanDurationFirstSeconds = 24 * 60 * 60;  // 24h
constexpr uint32_t kBanDurationMaxSeconds = 30 * 24 * 60 * 60;  // 30 days

class BanManager {
 public:
  // The ban key for an address (Milestone 16, the ONE place this
  // rule lives, so it cannot be bypassed by a caller passing a
  // raw address): "1.2.3.4:56789" -> "1.2.3.4" (the TCP port is
  // ephemeral — keying by "ip:port" let a banned peer reconnect
  // from a fresh port, M15 §C.2.6); "loopback:<name>" stays as-is
  // (stable test identifiers, no port); an address with no port
  // stays as-is; anything that is not a valid IPv4 "ip:port"
  // (including IPv4-mapped forms) stays as-is — no new parsing,
  // only the existing IPv4 check plus a numeric-port check.
  static std::string BanKey(const std::string& address);
  // Record a ban starting at `nowSeconds`. Durations double per
  // repeat offence, capped at 30 days. The key is BanKey(address).
  void Ban(const std::string& address, uint32_t nowSeconds);
  // True when BanKey(address) is banned at `nowSeconds`.
  bool IsBanned(const std::string& address, uint32_t nowSeconds) const;
  void Unban(const std::string& address) {
    bans_.erase(BanKey(address));
  }
  std::size_t Size() const { return bans_.size(); }

 private:
  struct BanInfo {
    uint32_t until = 0;
    int offences = 0;
  };
  std::unordered_map<std::string, BanInfo> bans_;
};

// One connected peer.
struct Peer {
  std::string id;       // transport-level id (opaque to the protocol)
  std::string address;  // "ip:port" for display/diagnostics. The BAN
  // key is the IP alone (BanManager::BanKey) — the port is
  // ephemeral and must never key a ban (Milestone 16).
  bool outbound = false;

  // Handshake state.
  bool versionReceived = false;
  bool verackReceived = false;
  bool handshakeComplete = false;
  int32_t version = 0;
  int32_t startHeight = 0;
  uint64_t nonce = 0;

  // Compact blocks (sendcmpct): peer wants announcements, and which
  // compact-block version it speaks.
  bool compactAnnounce = false;
  uint64_t compactVersion = 0;

  // Misbehavior.
  int misbehavior = 0;
  // Recently seen inventory (tx/block hashes) — do not re-request.
  std::unordered_set<Hash256, Hash256Hash> knownInventory;

  uint64_t lastPingNonce = 0;
  uint32_t lastPingTime = 0;
  // Peer-eviction data (Milestone 15): when the node is full, the
  // worst inbound peer is evicted. connectedAt is set at
  // registration (ConnectTo / OnBytes); lastTxTime / lastBlockTime
  // update on receipt of a valid tx / accepted block from the peer;
  // pingRttMs is the round-trip of the last answered ping
  // (UINT32_MAX = unknown); lastPingSentMs is steady-clock
  // milliseconds of the last ping sent (NOT wall clock — the wall
  // clock can jump, the steady clock cannot).
  uint32_t connectedAt = 0;
  uint32_t lastTxTime = 0;
  uint32_t lastBlockTime = 0;
  uint32_t lastPongTime = 0;
  uint32_t pingRttMs = UINT32_MAX;
  uint64_t lastPingSentMs = 0;
  // Block-stalling data (Milestone 15): the block this peer was
  // most recently asked for (getdata / getblocktxn) and when the
  // request went out. Empty hash = no outstanding request. The
  // explicit {} initializer matters: without it the array holds
  // indeterminate bytes and the stall sweep in Poll would read
  // garbage and disconnect every peer on its first call.
  Hash256 awaitingBlock{};
  uint32_t blockRequestAt = 0;
  // Buffered bytes from the transport (stream reassembly).
  std::vector<unsigned char> recvBuffer;

  // Add `score` misbehavior. Returns true when the peer crosses the
  // ban threshold with this addition.
  bool AddMisbehavior(int score) {
    misbehavior += score;
    return misbehavior >= kBanScoreThreshold;
  }
};

}  // namespace oeeoo
