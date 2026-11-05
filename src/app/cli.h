// SPDX-License-Identifier: MIT
// Oeeoo Coin — oeeoo-cli: a small JSON-RPC client (Milestone 12).
// Reads the cookie from <dataDir>/.cookie, POSTs to the local RPC
// server, and returns the parsed JSON response.
#pragma once

#include <cstdint>
#include <string>

#include "rpc/json.h"

namespace oeeoo {

// Call `method` with `params` on the RPC server at 127.0.0.1:rpcPort.
// The cookie is read from <dataDir>/.cookie. On success `response`
// holds the full JSON-RPC response object (check its "error"
// field). False + `error` when the server cannot be reached, the
// cookie is missing, or the reply is not valid JSON.
bool CliCall(const std::string& dataDir, uint16_t rpcPort,
             const std::string& method, const Json& params, Json& response,
             std::string& error);

}  // namespace oeeoo
