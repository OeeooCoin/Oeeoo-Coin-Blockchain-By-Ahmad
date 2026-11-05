// SPDX-License-Identifier: MIT
// Oeeoo Coin — DNS seed resolution implementation (getaddrinfo).
#include "network/dnsseed.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <cstring>
#include <ctime>

namespace oeeoo {

bool ResolveSeed(const std::string& hostname, uint16_t port,
                 std::vector<AddrEntry>& out, std::string& error) {
  out.clear();
  if (hostname.empty()) {
    error = "empty seed hostname";
    return false;
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;  // IPv4 only
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  const int rc = ::getaddrinfo(hostname.c_str(), nullptr, &hints, &result);
  if (rc != 0) {
    error = "cannot resolve seed " + hostname + ": " + gai_strerror(rc);
    return false;
  }
  const uint32_t now = static_cast<uint32_t>(std::time(nullptr));
  for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
    if (out.size() >= 100) break;
    const auto* addr =
        reinterpret_cast<sockaddr_in*>(ai->ai_addr);
    char ip[INET_ADDRSTRLEN];
    if (::inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip)) == nullptr) {
      continue;
    }
    AddrEntry entry;
    entry.time = now;
    entry.services = 1;
    entry.ip = ip;
    entry.port = port;
    out.push_back(entry);
  }
  ::freeaddrinfo(result);
  if (out.empty()) {
    error = "seed " + hostname + " returned no IPv4 address";
    return false;
  }
  return true;
}

}  // namespace oeeoo
