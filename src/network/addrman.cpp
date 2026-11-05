// SPDX-License-Identifier: MIT
// Oeeoo Coin — address manager implementation.
#include "network/addrman.h"

#include <algorithm>
#include <array>
#include <random>

namespace oeeoo {

bool AddressManager::Add(const AddrEntry& entry) {
  if (entry.ip.empty() || entry.port == 0) return false;
  // Only real dotted-quad IPv4 addresses are stored; anything else
  // (hostnames, junk) is rejected at the door.
  std::array<unsigned char, 4> ipv4{};
  if (!ParseIPv4(entry.ip, ipv4)) return false;
  if (Contains(entry.ip, entry.port)) return false;
  if (addrs_.size() >= kMaxAddresses) return false;
  addrs_.push_back(entry);
  return true;
}

bool AddressManager::Remove(const std::string& ip, uint16_t port) {
  auto it = std::find_if(addrs_.begin(), addrs_.end(), [&](const AddrEntry& a) {
    return a.ip == ip && a.port == port;
  });
  if (it == addrs_.end()) return false;
  addrs_.erase(it);
  return true;
}

std::vector<AddrEntry> AddressManager::GetAddr(std::size_t count) const {
  // Advertise a random sample, not the first N in insertion order:
  // always handing out the same oldest addresses would let one
  // peer's addresses dominate the network's view.
  std::vector<AddrEntry> shuffled = addrs_;
  std::random_device rd;
  std::mt19937 rng(rd());
  std::shuffle(shuffled.begin(), shuffled.end(), rng);
  std::vector<AddrEntry> out;
  for (std::size_t i = 0; i < shuffled.size() && out.size() < count; ++i) {
    out.push_back(shuffled[i]);
  }
  return out;
}

std::vector<AddrEntry> AddressManager::GetCandidates(
    std::size_t count) const {
  std::vector<AddrEntry> out;
  for (auto it = addrs_.rbegin(); it != addrs_.rend() && out.size() < count;
       ++it) {
    out.push_back(*it);
  }
  return out;
}

bool AddressManager::Contains(const std::string& ip, uint16_t port) const {
  return std::any_of(addrs_.begin(), addrs_.end(), [&](const AddrEntry& a) {
    return a.ip == ip && a.port == port;
  });
}

}  // namespace oeeoo
