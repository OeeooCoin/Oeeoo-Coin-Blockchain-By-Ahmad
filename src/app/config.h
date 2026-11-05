// SPDX-License-Identifier: MIT
// Oeeoo Coin — node configuration (Milestone 12).
// Two sources, in increasing priority: a config file
// (`oeeoo.conf`, key = value) and command-line arguments.
// The CLI overrides the file; the file overrides the defaults.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oeeoo {

struct AppConfig {
  std::string dataDir = "oeeoo-data";
  uint16_t port = 64775;         // P2P
  uint16_t rpcPort = 64776;      // JSON-RPC (127.0.0.1 only)
  uint16_t stratumPort = 64777;  // Stratum v1 (127.0.0.1 only)
  std::string minerAddress;      // empty = Stratum disabled
  std::string genesisFile;       // empty = an existing chain is required
  // Wallet file for the node wallet (Milestone 14). Empty = no
  // wallet (RPC wallet methods return -13, as before). A relative
  // path is resolved against the data directory by node_main.
  // The wallet loads LOCKED; unlock it with the walletpassphrase
  // RPC. The passphrase is NEVER a CLI flag or a config value.
  std::string walletFile;
  // Expected mainnet genesis hash (64 hex chars). Empty = no
  // checkpoint. When set, the node refuses to start unless the
  // genesis it loads (from file or from the store) hashes to this
  // value. The mainnet genesis hash is NOT yet decided, so this is
  // empty by default; it must be set by the operator once the
  // launch genesis is approved and published.
  std::string genesisCheckpoint;
  std::string founderScript;     // hex, optional (derived otherwise)
  uint32_t powLimitBits = 0x1d00ffff;  // testing only; mainnet = default
  std::vector<std::string> dnsSeeds;  // empty by default (none invented)
  // Default true since Milestone 13: a normal (light) load trusts
  // the stored UTXO table and headers without re-validating the
  // chain, so a tampered store would pass a restart. Deep
  // verification replays the chain from genesis and compares the
  // UTXO set entry by entry; it is fast on a young chain and is
  // the safe default. Use --no-deep-verify to skip it (e.g. on a
  // very tall chain where a full replay is slow).
  bool deepVerify = true;
  int maxPeers = 125;
  std::size_t maxMempoolBytes = 300ULL * 1024 * 1024;  // 300 MB default
  bool help = false;
  bool version = false;
};

// Parse a config file (key = value, '#' comments, blank lines
// allowed). Unknown keys are an error. False + `error` on failure.
bool LoadConfigFile(const std::string& path, AppConfig& config,
                    std::string& error);
// Parse argv (program name in argv[0]). `--config <path>` loads a
// config file first; every other flag overrides it. False + `error`
// on an unknown flag or a bad value.
bool ParseArgs(int argc, char** argv, AppConfig& config,
               std::string& error);
// Usage text for --help.
std::string UsageText(const char* program);

}  // namespace oeeoo
