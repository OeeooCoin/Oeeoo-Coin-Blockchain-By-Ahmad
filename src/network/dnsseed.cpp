// SPDX-License-Identifier: MIT
// Oeeoo Coin — DNS seed resolution implementation (getaddrinfo,
// on a worker thread with a deadline since Milestone 15).
#include "network/dnsseed.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <future>
#include <memory>
#include <thread>
#include <utility>

namespace oeeoo {
namespace {

// The real resolver: getaddrinfo, IPv4 only. Runs on the worker
// thread; its result is handed back through a promise, never by
// writing into the caller's output.
SeedLookupResult RealResolve(const std::string& hostname) {
  SeedLookupResult result;
  addrinfo hints{};
  hints.ai_family = AF_INET;  // IPv4 only
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* resolved = nullptr;
  const int rc = ::getaddrinfo(hostname.c_str(), nullptr, &hints, &resolved);
  if (rc != 0) {
    result.error =
        "cannot resolve seed " + hostname + ": " + gai_strerror(rc);
    return result;
  }
  for (addrinfo* ai = resolved; ai != nullptr; ai = ai->ai_next) {
    const auto* addr = reinterpret_cast<sockaddr_in*>(ai->ai_addr);
    char ip[INET_ADDRSTRLEN];
    if (::inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip)) == nullptr) {
      continue;
    }
    result.ips.emplace_back(ip);
  }
  ::freeaddrinfo(resolved);
  result.ok = true;
  return result;
}

}  // namespace

bool ResolveSeedWithResolver(const std::string& hostname, uint16_t port,
                             std::vector<AddrEntry>& out, std::string& error,
                             int timeoutMs, SeedResolver resolver) {
  out.clear();
  if (hostname.empty()) {
    error = "empty seed hostname";
    return false;
  }
  // Run the resolver on a worker thread: getaddrinfo has no timeout
  // of its own, so without this a hanging resolver would block the
  // caller (node startup) indefinitely. The promise is shared with
  // the detached thread, so if the deadline expires the thread can
  // still finish in the background and set its (now unread) result
  // without touching anything the caller owns.
  auto promise =
      std::make_shared<std::promise<SeedLookupResult>>();
  std::future<SeedLookupResult> future = promise->get_future();
  std::thread([promise, resolver = std::move(resolver), hostname]() {
    promise->set_value(resolver(hostname));
  }).detach();

  if (future.wait_for(std::chrono::milliseconds(timeoutMs)) ==
      std::future_status::timeout) {
    // Deadline expired: report the timeout and return. The worker
    // thread keeps running until getaddrinfo returns; its late
    // result is discarded.
    error = "seed " + hostname + " timed out after " +
            std::to_string(timeoutMs) + "ms";
    return false;
  }
  SeedLookupResult result = future.get();
  if (!result.ok) {
    error = result.error;
    return false;
  }
  const uint32_t now = static_cast<uint32_t>(std::time(nullptr));
  for (const std::string& ip : result.ips) {
    if (out.size() >= 100) break;
    AddrEntry entry;
    entry.time = now;
    entry.services = 1;
    entry.ip = ip;
    entry.port = port;
    out.push_back(entry);
  }
  if (out.empty()) {
    error = "seed " + hostname + " returned no IPv4 address";
    return false;
  }
  return true;
}

bool ResolveSeed(const std::string& hostname, uint16_t port,
                 std::vector<AddrEntry>& out, std::string& error,
                 int timeoutMs) {
  return ResolveSeedWithResolver(hostname, port, out, error, timeoutMs,
                                 RealResolve);
}

}  // namespace oeeoo
