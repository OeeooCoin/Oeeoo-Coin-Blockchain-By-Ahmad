// SPDX-License-Identifier: MIT
// Oeeoo Coin — ban manager implementation.
#include "network/peer.h"

namespace oeeoo {

void BanManager::Ban(const std::string& address, uint32_t nowSeconds) {
  BanInfo& info = bans_[address];
  info.offences += 1;
  uint64_t duration = kBanDurationFirstSeconds;
  for (int i = 1; i < info.offences; ++i) {
    duration *= 2;
    if (duration >= kBanDurationMaxSeconds) {
      duration = kBanDurationMaxSeconds;
      break;
    }
  }
  info.until = nowSeconds + static_cast<uint32_t>(duration);
}

bool BanManager::IsBanned(const std::string& address,
                          uint32_t nowSeconds) const {
  auto it = bans_.find(address);
  if (it == bans_.end()) return false;
  return nowSeconds < it->second.until;
}

}  // namespace oeeoo
