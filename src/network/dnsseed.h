// SPDX-License-Identifier: MIT
// Oeeoo Coin — DNS seed resolution (launch support, post-Milestone 12).
// A DNS seed is a hostname that answers with the IP addresses of
// live nodes; the seed itself is NOT a peer. The seed list starts
// EMPTY — no seed hostnames are invented here (spec item 15). The
// operator adds real seeds in oeeoo.conf (`dnsseed = ...`) or with
// `--dns-seed`. Only IPv4 is returned (the AddrEntry format).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "network/protocol.h"

namespace oeeoo {

// Resolve `hostname` into peer addresses on `port`. At most 100
// addresses are returned. False + `error` when the name does not
// resolve or returns no IPv4 address.
bool ResolveSeed(const std::string& hostname, uint16_t port,
                 std::vector<AddrEntry>& out, std::string& error);

}  // namespace oeeoo
