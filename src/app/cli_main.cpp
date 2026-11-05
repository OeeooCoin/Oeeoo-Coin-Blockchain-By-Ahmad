// SPDX-License-Identifier: MIT
// Oeeoo Coin — oeeoo-cli: call the local node's JSON-RPC server.
// Usage: oeeoo-cli [--datadir <path>] [--rpcport <n>] <method> [params-json]
#include <iostream>

#include "app/cli.h"
#include "rpc/json.h"

int main(int argc, char** argv) {
  using namespace oeeoo;
  std::string dataDir = "oeeoo-data";
  uint16_t rpcPort = 64776;
  std::string method;
  Json params = Json::Array();
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--datadir") {
      if (i + 1 >= argc) {
        std::cerr << "oeeoo-cli: --datadir needs a path\n";
        return 1;
      }
      dataDir = argv[++i];
    } else if (arg == "--rpcport") {
      if (i + 1 >= argc) {
        std::cerr << "oeeoo-cli: --rpcport needs a number\n";
        return 1;
      }
      try {
        rpcPort = static_cast<uint16_t>(std::stoul(argv[++i]));
      } catch (...) {
        std::cerr << "oeeoo-cli: invalid --rpcport\n";
        return 1;
      }
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: oeeoo-cli [--datadir <path>] [--rpcport <n>] "
                   "<method> [params-json]\n";
      return 0;
    } else if (method.empty()) {
      method = arg;
    } else {
      std::string parseError;
      if (!Json::Parse(arg, params, parseError)) {
        std::cerr << "oeeoo-cli: params are not valid JSON: " << parseError
                  << "\n";
        return 1;
      }
    }
  }
  if (method.empty()) {
    std::cerr << "Usage: oeeoo-cli [--datadir <path>] [--rpcport <n>] "
                 "<method> [params-json]\n";
    return 1;
  }
  Json response;
  std::string error;
  if (!CliCall(dataDir, rpcPort, method, params, response, error)) {
    std::cerr << "oeeoo-cli: " << error << "\n";
    return 1;
  }
  std::cout << response.Dump() << "\n";
  const Json* err = response.Get("error");
  if (err != nullptr && !err->IsNull()) {
    return 1;
  }
  return 0;
}
