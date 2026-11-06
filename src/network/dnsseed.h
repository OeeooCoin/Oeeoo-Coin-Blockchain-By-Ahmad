// SPDX-License-Identifier: MIT
// Oeeoo Coin — DNS seed resolution (launch support, post-Milestone 12).
// A DNS seed is a hostname that answers with the IP addresses of
// live nodes; the seed itself is NOT a peer. The seed list starts
// EMPTY — no seed hostnames are invented here (spec item 15). The
// operator adds real seeds in oeeoo.conf (`dnsseed = ...`) or with
// `--dns-seed`. Only IPv4 is returned (the AddrEntry format).
//
// Timeout (Milestone 15): the lookup runs on a worker thread with a
// deadline, Bitcoin Core-style (Core runs DNS off the main thread so
// a hanging resolver cannot block startup). The deadline itself is
// Oeeoo's own choice — no Core lookup-timeout constant exists to
// copy. On timeout ResolveSeed returns false with an error
// containing "timed out". The worker thread cannot be cancelled
// (getaddrinfo is not cancellable), so it is detached and its late
// result is discarded; it writes only to its own locals, never to
// the caller's `out`.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "network/protocol.h"

namespace oeeoo {

// DNS lookup deadline in milliseconds. Oeeoo's own choice (NOT a
// Bitcoin Core constant): 10 seconds is long enough for a slow but
// live resolver and short enough that a hanging resolver cannot
// stall node startup for long. ResolveSeed's default.
constexpr int kDnsSeedTimeoutMs = 10000;

// Result of one (untimed) seed lookup, as produced by a resolver.
struct SeedLookupResult {
  bool ok = false;                  // false: `error` explains why
  std::vector<std::string> ips;     // dotted-quad IPv4 addresses
  std::string error;                // set when ok == false
};

// A resolver: hostname in, lookup result out. The default resolver
// is the real getaddrinfo call; tests inject a fake (e.g. one that
// sleeps) to exercise the deadline without a real slow DNS server.
using SeedResolver = std::function<SeedLookupResult(const std::string& hostname)>;

// Testable core of ResolveSeed: runs `resolver` on a worker thread
// and waits at most `timeoutMs` for it. Returns false + `error`
// when the hostname is empty, the resolver fails, it returns no
// IPv4 address, or the deadline expires ("timed out"). At most 100
// addresses are returned, IPv4 only. A timed-out worker thread is
// detached and its late result discarded.
bool ResolveSeedWithResolver(const std::string& hostname, uint16_t port,
                             std::vector<AddrEntry>& out, std::string& error,
                             int timeoutMs, SeedResolver resolver);

// Resolve `hostname` into peer addresses on `port`. At most 100
// addresses are returned. False + `error` when the name does not
// resolve, returns no IPv4 address, or the lookup does not finish
// within `timeoutMs` (default kDnsSeedTimeoutMs, so existing
// callers get the timeout with zero changes).
bool ResolveSeed(const std::string& hostname, uint16_t port,
                 std::vector<AddrEntry>& out, std::string& error,
                 int timeoutMs = kDnsSeedTimeoutMs);

}  // namespace oeeoo
