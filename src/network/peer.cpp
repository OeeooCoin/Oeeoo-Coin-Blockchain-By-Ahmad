// SPDX-License-Identifier: MIT
// Oeeoo Coin — ban manager implementation.
#include "network/peer.h"

#include <array>

#include "network/protocol.h"

namespace oeeoo {

std::string BanManager::BanKey(const std::string& address) {
  // Loopback test identifiers ("loopback:<name>") have no port
  // and are stable across reconnects: they already are the key.
  if (address.rfind("loopback:", 0) == 0) return address;
  const std::size_t colon = address.rfind(':');
  if (colon == std::string::npos) return address;  // no port: as-is
  // Only a valid IPv4 "ip:port" is stripped. Anything else —
  // an IPv4-mapped form, a malformed string, a non-numeric or
  // out-of-range port — is left untouched, so BanKey never
  // invents a key from an address it does not understand.
  std::array<unsigned char, 4> octets{};
  if (!ParseIPv4(address.substr(0, colon), octets)) return address;
  int parsed = 0;
  try {
    parsed = std::stoi(address.substr(colon + 1));
  } catch (...) {
    return address;
  }
  if (parsed <= 0 || parsed > 65535) return address;
  return address.substr(0, colon);
}

void BanManager::Ban(const std::string& address, uint32_t nowSeconds) {
  BanInfo& info = bans_[BanKey(address)];
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
  auto it = bans_.find(BanKey(address));
  if (it == bans_.end()) return false;
  return nowSeconds < it->second.until;
}

}  // namespace oeeoo
