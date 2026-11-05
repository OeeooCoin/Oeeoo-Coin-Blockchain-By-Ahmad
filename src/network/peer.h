// SPDX-License-Identifier: MIT
// Oeeoo Coin — peer state, misbehavior scoring and bans (Milestone 6).
// Each connected peer tracks its handshake state, protocol version,
// advertised height, compact-block support, ping state, recently seen
// inventory (to avoid re-requesting), and a misbehavior score.
// Score >= 100 bans the peer: progressive ban durations (1st ban 24h,
// then doubling up to 30 days). Bans are keyed by the peer's address
// string and enforced by BanManager.
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
  // Record a ban starting at `nowSeconds`. Durations double per
  // repeat offence, capped at 30 days.
  void Ban(const std::string& address, uint32_t nowSeconds);
  bool IsBanned(const std::string& address, uint32_t nowSeconds) const;
  void Unban(const std::string& address) { bans_.erase(address); }
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
  std::string address;  // "ip:port" — the ban key
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
