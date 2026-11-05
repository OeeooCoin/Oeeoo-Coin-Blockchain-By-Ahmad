// SPDX-License-Identifier: MIT
// Oeeoo Coin — address manager implementation.
#include "network/addrman.h"

#include <algorithm>

namespace oeeoo {

bool AddressManager::Add(const AddrEntry& entry) {
  if (entry.ip.empty() || entry.port == 0) return false;
  if (Contains(entry.ip, entry.port)) return false;
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
  std::vector<AddrEntry> out;
  for (std::size_t i = 0; i < addrs_.size() && out.size() < count; ++i) {
    out.push_back(addrs_[i]);
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
