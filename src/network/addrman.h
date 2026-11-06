// SPDX-License-Identifier: MIT
// Oeeoo Coin — address manager (Milestone 6, hardened in Milestone 15).
// Keeps the peer addresses a node knows about: addresses learned from
// `addr` messages and manually added peers. Hands out addresses for
// `addr` replies and for outbound connection attempts.
// DNS seeds do NOT exist yet (spec section 11.4, unresolved item 15):
// no seed hostnames are invented here — the seed list starts empty
// and is filled only by real operator configuration. Hardcoded
// fallback peers are likewise unresolved (item 16).
//
// AS-map awareness is NOT implemented: it needs an external AS-mapping
// data file plus an operator decision about which file to trust, and
// neither exists yet. Netgroup (/16) bucketing below is the anti-fill
// defence Oeeoo ships instead; AS-map support is a future decision,
// not a silent omission. (Milestone 16 decision, ACCEPTED: /16
// bucketing is the permanent defence for v0.1.0; AS-map is deferred
// post-launch.)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "network/protocol.h"

namespace oeeoo {

class AddressManager {
 public:
  // Hard cap on stored addresses: a peer can send unlimited `addr`
  // messages, so without a cap the table would grow without bound.
  static constexpr std::size_t kMaxAddresses = 1000;
  // Per-netgroup cap: at most this many entries may share one /16
  // netgroup. Oeeoo's own choice (NOT a Bitcoin Core constant):
  // 64 entries is ~6% of the 1,000-entry table, so a single network
  // can never fill the table on its own, while a legitimate /16
  // (e.g. a hosting provider with many nodes) still has ample room.
  // This is the anti-fill defence: without it, one hostile network
  // could crowd every honest address out of the table.
  static constexpr std::size_t kMaxPerNetgroup = 64;
  // Entry expiry horizon: 30 days (Bitcoin Core's horizon). An
  // address not re-advertised within this long is stale and is
  // dropped by RemoveExpired.
  static constexpr uint32_t kAddrExpirySeconds = 30 * 24 * 3600;
  // Consecutive failures before an entry is dropped as dead.
  // Oeeoo's own choice (NOT a Bitcoin Core constant): 3 gives a
  // flapping peer a chance to recover without letting a dead
  // address linger in the table.
  static constexpr int kMaxFailures = 3;

  // Netgroup of an IPv4 address: its /16 (first two octets), e.g.
  // "10.20" for "10.20.3.4". Returns an empty string for an invalid
  // IP (Add already rejects invalid IPs via ParseIPv4, so callers
  // of Add never see the empty group).
  static std::string NetgroupOf(const std::string& ip);

  // Add an address (from an `addr` message or manual config).
  // Returns false if the address is already known, invalid, the
  // table is full, or its netgroup is full.
  bool Add(const AddrEntry& entry);
  // Remove an address.
  bool Remove(const std::string& ip, uint16_t port);
  // Drop entries older than kAddrExpirySeconds as of `nowSeconds`
  // (seconds since the Unix epoch). The node should call this on
  // its 60-second sweep (Node::Poll); the manager runs no timer
  // of its own.
  void RemoveExpired(uint32_t nowSeconds);
  // Record a failed connection attempt to an address. Increments
  // the entry's failure count; when it reaches kMaxFailures the
  // entry is REMOVED from the table (a peer that fails 3 times is
  // treated as dead). Returns false if the address is unknown.
  // The node wires this to its connection-failure path.
  bool RecordFailure(const std::string& ip, uint16_t port);
  // Record a successful connection: resets the entry's failure
  // count to 0. Returns false if the address is unknown.
  // The node wires this to its connection-success path.
  bool RecordSuccess(const std::string& ip, uint16_t port);
  // Up to `count` addresses to advertise in an `addr` message.
  std::vector<AddrEntry> GetAddr(std::size_t count) const;
  // Addresses to try connecting to (newest first). Note: entries
  // with failures 1..kMaxFailures-1 are still returned — only an
  // entry that reached kMaxFailures (and was therefore removed)
  // stops being offered. No filtering is applied here.
  std::vector<AddrEntry> GetCandidates(std::size_t count) const;
  bool Contains(const std::string& ip, uint16_t port) const;
  std::size_t Size() const { return addrs_.size(); }

 private:
  std::size_t CountNetgroup(const std::string& netgroup) const;
  std::vector<AddrEntry> addrs_;
};

}  // namespace oeeoo
