// SPDX-License-Identifier: MIT
// Oeeoo Coin — address manager (Milestone 6).
// Keeps the peer addresses a node knows about: addresses learned from
// `addr` messages and manually added peers. Hands out addresses for
// `addr` replies and for outbound connection attempts.
// DNS seeds do NOT exist yet (spec section 11.4, unresolved item 15):
// no seed hostnames are invented here — the seed list starts empty
// and is filled only by real operator configuration. Hardcoded
// fallback peers are likewise unresolved (item 16).
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
  // Add an address (from an `addr` message or manual config).
  // Returns false if the address is already known, invalid, or the
  // table is full.
  bool Add(const AddrEntry& entry);
  // Remove an address.
  bool Remove(const std::string& ip, uint16_t port);
  // Up to `count` addresses to advertise in an `addr` message.
  std::vector<AddrEntry> GetAddr(std::size_t count) const;
  // Addresses to try connecting to (newest first).
  std::vector<AddrEntry> GetCandidates(std::size_t count) const;
  bool Contains(const std::string& ip, uint16_t port) const;
  std::size_t Size() const { return addrs_.size(); }

 private:
  std::vector<AddrEntry> addrs_;
};

}  // namespace oeeoo
