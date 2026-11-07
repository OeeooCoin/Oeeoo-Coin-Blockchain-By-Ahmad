// SPDX-License-Identifier: MIT
// Oeeoo Coin — node configuration implementation.
#include "app/config.h"

#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

#include "network/protocol.h"

namespace oeeoo {

uint16_t AppConfig::EffectivePort() const {
  if (portExplicit) return port;
  return NetworkIdentityFor(testnet ? NetworkId::kTestnet
                                    : NetworkId::kMainnet)
      .p2pPort;
}

uint16_t AppConfig::EffectiveRpcPort() const {
  if (rpcPortExplicit) return rpcPort;
  return NetworkIdentityFor(testnet ? NetworkId::kTestnet
                                    : NetworkId::kMainnet)
      .rpcPort;
}

uint16_t AppConfig::EffectiveStratumPort() const {
  if (stratumPortExplicit) return stratumPort;
  return NetworkIdentityFor(testnet ? NetworkId::kTestnet
                                    : NetworkId::kMainnet)
      .stratumPort;
}

namespace {
std::string Trim(const std::string& s) {
  std::size_t begin = 0;
  while (begin < s.size() &&
         std::isspace(static_cast<unsigned char>(s[begin]))) {
    ++begin;
  }
  std::size_t end = s.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    --end;
  }
  return s.substr(begin, end - begin);
}

bool ParseUint(const std::string& s, uint64_t& out) {
  if (s.empty()) return false;
  for (char c : s) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  }
  try {
    std::size_t used = 0;
    out = std::stoull(s, &used);
    return used == s.size();
  } catch (...) {
    return false;
  }
}

bool ParsePort(const std::string& s, uint16_t& out, std::string& error) {
  uint64_t v = 0;
  if (!ParseUint(s, v) || v > 65535) {
    error = "invalid port: " + s;
    return false;
  }
  out = static_cast<uint16_t>(v);
  return true;
}

bool ParseBool(const std::string& s, bool& out, std::string& error) {
  if (s == "true" || s == "1" || s == "yes") {
    out = true;
    return true;
  }
  if (s == "false" || s == "0" || s == "no") {
    out = false;
    return true;
  }
  error = "invalid boolean (expected true/false): " + s;
  return false;
}

bool SetKey(AppConfig& config, const std::string& key,
            const std::string& value, std::string& error) {
  if (key == "datadir") {
    config.dataDir = value;
  } else if (key == "network") {
    // Milestone 20: `network=testnet` selects the test-network
    // identity; `network=mainnet` (or an absent key) is mainnet.
    // Anything else is rejected loudly — silently falling back
    // to mainnet would hide a typo that puts a test node on
    // Mainnet (or vice versa).
    if (value == "mainnet") {
      config.testnet = false;
    } else if (value == "testnet") {
      config.testnet = true;
    } else {
      error = "invalid network (expected mainnet/testnet): " + value;
      return false;
    }
  } else if (key == "port") {
    if (!ParsePort(value, config.port, error)) return false;
    config.portExplicit = true;
  } else if (key == "rpcport") {
    if (!ParsePort(value, config.rpcPort, error)) return false;
    config.rpcPortExplicit = true;
  } else if (key == "stratumport") {
    if (!ParsePort(value, config.stratumPort, error)) return false;
    config.stratumPortExplicit = true;
  } else if (key == "mineraddress") {
    config.minerAddress = value;
  } else if (key == "genesisfile") {
    config.genesisFile = value;
  } else if (key == "wallet") {
    config.walletFile = value;
  } else if (key == "genesischeckpoint") {
    if (value.size() != 64 ||
        value.find_first_not_of("0123456789abcdefABCDEF") !=
            std::string::npos) {
      error = "invalid genesischeckpoint (64 hex chars): " + value;
      return false;
    }
    config.genesisCheckpoint = value;
  } else if (key == "maxmempool") {
    uint64_t v = 0;
    if (!ParseUint(value, v) || v == 0 || v > 100000) {
      error = "invalid maxmempool (MB, 1..100000): " + value;
      return false;
    }
    config.maxMempoolBytes = static_cast<std::size_t>(v) * 1024 * 1024;
  } else if (key == "founderscript") {
    config.founderScript = value;
  } else if (key == "dnsseed") {
    if (value.empty()) {
      error = "dnsseed must not be empty";
      return false;
    }
    config.dnsSeeds.push_back(value);
  } else if (key == "addnode") {
    if (value.empty()) {
      error = "addnode must not be empty";
      return false;
    }
    config.addNodes.push_back(value);
  } else if (key == "deepverify") {
    if (!ParseBool(value, config.deepVerify, error)) return false;
  } else if (key == "rpcthreads") {
    uint64_t v = 0;
    if (!ParseUint(value, v) || v < 1 || v > 1024) {
      error = "invalid rpcthreads (1..1024): " + value;
      return false;
    }
    config.rpcThreads = static_cast<int>(v);
  } else if (key == "rpcworkqueue") {
    uint64_t v = 0;
    if (!ParseUint(value, v) || v < 1 || v > 100000) {
      error = "invalid rpcworkqueue (1..100000): " + value;
      return false;
    }
    config.rpcWorkQueueSize = static_cast<int>(v);
  } else if (key == "persistmempool") {
    if (!ParseBool(value, config.persistMempool, error)) return false;
  } else if (key == "maxpeers") {
    uint64_t v = 0;
    if (!ParseUint(value, v) || v > 1000000) {
      error = "invalid maxpeers: " + value;
      return false;
    }
    config.maxPeers = static_cast<int>(v);
  } else {
    error = "unknown config key: " + key;
    return false;
  }
  return true;
}
}  // namespace

bool LoadConfigFile(const std::string& path, AppConfig& config,
                    std::string& error) {
  std::ifstream in(path);
  if (!in) {
    error = "cannot open config file: " + path;
    return false;
  }
  std::string line;
  int lineNo = 0;
  while (std::getline(in, line)) {
    ++lineNo;
    std::string text = Trim(line);
    if (text.empty() || text[0] == '#') continue;
    const std::size_t eq = text.find('=');
    if (eq == std::string::npos) {
      error = "bad config line " + std::to_string(lineNo) +
              " (expected key = value)";
      return false;
    }
    const std::string key = Trim(text.substr(0, eq));
    std::string value = Trim(text.substr(eq + 1));
    // Strip a matching pair of quotes.
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
    }
    if (!SetKey(config, key, value, error)) {
      error = "config line " + std::to_string(lineNo) + ": " + error;
      return false;
    }
  }
  return true;
}

bool ParseArgs(int argc, char** argv, AppConfig& config,
               std::string& error) {
  // A --config file is loaded first; later flags override it.
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config") {
      if (i + 1 >= argc) {
        error = "--config needs a path";
        return false;
      }
      if (!LoadConfigFile(argv[++i], config, error)) return false;
    }
  }
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto needValue = [&](const char* name, std::string& out) {
      if (i + 1 >= argc) {
        error = std::string(name) + " needs a value";
        return false;
      }
      out = argv[++i];
      return true;
    };
    if (arg == "--config") {
      ++i;  // already loaded above
    } else if (arg == "--help" || arg == "-h") {
      config.help = true;
    } else if (arg == "--version" || arg == "-V") {
      config.version = true;
    } else if (arg == "--datadir") {
      if (!needValue("--datadir", config.dataDir)) return false;
    } else if (arg == "--testnet") {
      // Milestone 20: select the test-network identity
      // (testnet magic, default ports 64779/64780/64781,
      // "(testnet)" user agent). Default (no flag) is mainnet.
      config.testnet = true;
    } else if (arg == "--network") {
      std::string value;
      if (!needValue("--network", value)) return false;
      if (value == "mainnet") {
        config.testnet = false;
      } else if (value == "testnet") {
        config.testnet = true;
      } else {
        error = "invalid --network (expected mainnet/testnet): " + value;
        return false;
      }
    } else if (arg == "--port") {
      std::string value;
      if (!needValue("--port", value)) return false;
      if (!ParsePort(value, config.port, error)) return false;
      config.portExplicit = true;
    } else if (arg == "--rpcport") {
      std::string value;
      if (!needValue("--rpcport", value)) return false;
      if (!ParsePort(value, config.rpcPort, error)) return false;
      config.rpcPortExplicit = true;
    } else if (arg == "--stratumport") {
      std::string value;
      if (!needValue("--stratumport", value)) return false;
      if (!ParsePort(value, config.stratumPort, error)) return false;
      config.stratumPortExplicit = true;
    } else if (arg == "--miner-address") {
      if (!needValue("--miner-address", config.minerAddress)) return false;
    } else if (arg == "--genesis-file") {
      if (!needValue("--genesis-file", config.genesisFile)) return false;
    } else if (arg == "--wallet") {
      if (!needValue("--wallet", config.walletFile)) return false;
    } else if (arg == "--genesis-checkpoint") {
      std::string value;
      if (!needValue("--genesis-checkpoint", value)) return false;
      if (value.size() != 64 ||
          value.find_first_not_of("0123456789abcdefABCDEF") !=
              std::string::npos) {
        error = "invalid --genesis-checkpoint (64 hex chars): " + value;
        return false;
      }
      config.genesisCheckpoint = value;
    } else if (arg == "--maxmempool") {
      std::string value;
      if (!needValue("--maxmempool", value)) return false;
      uint64_t v = 0;
      if (!ParseUint(value, v) || v == 0 || v > 100000) {
        error = "invalid --maxmempool (MB, 1..100000): " + value;
        return false;
      }
      config.maxMempoolBytes = static_cast<std::size_t>(v) * 1024 * 1024;
    } else if (arg == "--founder-script") {
      if (!needValue("--founder-script", config.founderScript)) return false;
    } else if (arg == "--dns-seed") {
      std::string value;
      if (!needValue("--dns-seed", value)) return false;
      config.dnsSeeds.push_back(value);
    } else if (arg == "--addnode") {
      std::string value;
      if (!needValue("--addnode", value)) return false;
      config.addNodes.push_back(value);
    } else if (arg == "--pow-limit-bits") {
      std::string value;
      if (!needValue("--pow-limit-bits", value)) return false;
      try {
        config.powLimitBits =
            static_cast<uint32_t>(std::stoul(value, nullptr, 16));
      } catch (...) {
        error = "invalid --pow-limit-bits (hex): " + value;
        return false;
      }
    } else if (arg == "--deep-verify") {
      config.deepVerify = true;
    } else if (arg == "--no-deep-verify") {
      config.deepVerify = false;
    } else if (arg == "--persistmempool") {
      config.persistMempool = true;
    } else if (arg == "--no-persistmempool") {
      config.persistMempool = false;
    } else if (arg.rfind("--persistmempool=", 0) == 0) {
      // Core-style `-persistmempool=0` spelling (double-dash
      // form): an explicit 0/1 value after the '='.
      const std::string value = arg.substr(strlen("--persistmempool="));
      if (!ParseBool(value, config.persistMempool, error)) return false;
    } else if (arg == "--rpcthreads") {
      std::string value;
      if (!needValue("--rpcthreads", value)) return false;
      uint64_t v = 0;
      if (!ParseUint(value, v) || v < 1 || v > 1024) {
        error = "invalid --rpcthreads (1..1024): " + value;
        return false;
      }
      config.rpcThreads = static_cast<int>(v);
    } else if (arg == "--rpcworkqueue") {
      std::string value;
      if (!needValue("--rpcworkqueue", value)) return false;
      uint64_t v = 0;
      if (!ParseUint(value, v) || v < 1 || v > 100000) {
        error = "invalid --rpcworkqueue (1..100000): " + value;
        return false;
      }
      config.rpcWorkQueueSize = static_cast<int>(v);
    } else if (arg == "--maxpeers") {
      std::string value;
      if (!needValue("--maxpeers", value)) return false;
      uint64_t v = 0;
      if (!ParseUint(value, v) || v > 1000000) {
        error = "invalid maxpeers: " + value;
        return false;
      }
      config.maxPeers = static_cast<int>(v);
    } else {
      error = "unknown argument: " + arg;
      return false;
    }
  }
  return true;
}

std::string UsageText(const char* program) {
  std::ostringstream out;
  out << "Usage: " << program << " [options]\n"
      << "\n"
      << "  --config <path>       config file (oeeoo.conf format)\n"
      << "  --datadir <path>      data directory (default: oeeoo-data)\n"
      << "  --testnet             test-network identity: testnet magic\n"
      << "                         (d9 73 08 c2), default ports 64779 "
         "(P2P) / 64780 (RPC) /\n"
      << "                         64781 (Stratum), user agent "
         "\"/OeeooCoin:0.1.0(testnet)/\".\n"
      << "                         Default (no flag) is mainnet; a "
         "testnet node can never\n"
      << "                         peer with a mainnet node.\n"
      << "  --network <name>      mainnet (default) or testnet; same "
         "as --testnet, and the\n"
      << "                         CLI form of the `network` config key\n"
      << "  --port <n>            P2P port (default: 64775 mainnet, "
         "64779 testnet)\n"
      << "  --rpcport <n>         JSON-RPC port, 127.0.0.1 only (default: 64776 mainnet, 64780 testnet)\n"
      << "  --rpcthreads <n>      RPC worker threads (default: 16)\n"
      << "  --rpcworkqueue <n>    RPC work-queue depth; queue-full -> HTTP 500\n"
      << "                         (default: 64)\n"
      << "  --stratumport <n>     Stratum port, 127.0.0.1 only (default: 64777 mainnet, 64781 testnet)\n"
      << "  --miner-address <a>   P2PKH address for Stratum coinbase (enables Stratum)\n"
      << "  --genesis-file <path> genesis file from oeeoo-genesis (fresh chain only)\n"
      << "  --wallet <path>       wallet file for the node wallet (loads LOCKED;\n"
      << "                         unlock with the walletpassphrase RPC)\n"
      << "  --genesis-checkpoint <hex> expected mainnet genesis hash (64 hex)\n"
      << "  --maxmempool <MB>     mempool size cap in MB (default: 300)\n"
      << "  --persistmempool[=0|1] mempool persistence on/off "
         "(default: on); --no-persistmempool disables the\n"
      << "                         automatic save on shutdown and load on "
         "start (savemempool RPC still works)\n"
      << "  --founder-script <hex> founder 2-of-3 multisig script (vesting)\n"
      << "  --dns-seed <host>     DNS seed hostname (repeatable; none by default)\n"
      << "  --addnode <ip:port>   manually added peer (repeatable); the dial loop\n"
      << "                         dials it at startup and keeps it in the address book\n"
      << "  --pow-limit-bits <hex> TESTING ONLY: easiest difficulty bits\n"
      << "                         (default: 1d00ffff = mainnet)\n"
      << "  --deep-verify         fully re-validate the stored chain on load\n"
      << "  --no-deep-verify      skip deep verification on load (default is on)\n"
      << "  --maxpeers <n>        max peers (default: 125)\n"
      << "  --help, -h            this text\n"
      << "  --version, -V         print the version and exit\n";
  return out.str();
}

}  // namespace oeeoo
