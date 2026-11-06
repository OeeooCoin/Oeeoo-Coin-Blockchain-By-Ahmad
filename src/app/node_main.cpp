// SPDX-License-Identifier: MIT
// Oeeoo Coin — the runnable node (Milestone 12).
// Loads the chain store, starts P2P + JSON-RPC + Stratum, and runs
// until SIGINT/SIGTERM or the RPC `stop` method.
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>

#include "app/config.h"
#include "app/genesis.h"
#include "consensus/consensus.h"
#include "consensus/vesting.h"
#include "db/chainstore.h"
#include "network/node.h"
#include "network/transport.h"
#include "rpc/rpcserver.h"
#include "rpc/rpcutil.h"
#include "rpc/stratum.h"
#include "wallet/nodewallet.h"

namespace {
std::atomic<bool> g_stop{false};

void OnSignal(int) { g_stop.store(true); }

std::string HashHex(const oeeoo::Hash256& h) {
  return oeeoo::BytesToHexStr(
      std::vector<unsigned char>(h.begin(), h.end()));
}



// Derive the vesting parameters from the stored genesis block, so a
// restart validates founder spends against the same rules as the
// chain was built with. Returns false only on a database error.
bool DeriveVestingFromStore(oeeoo::ChainStore& store,
                            oeeoo::VestingParams& vesting,
                            std::string& error) {
  oeeoo::Hash256 tip;
  std::string tipError;
  if (!store.LoadTip(tip, tipError)) {
    return true;  // fresh store: nothing to derive
  }
  std::vector<std::tuple<int, oeeoo::BlockHeader, oeeoo::Target>> headers;
  if (!store.GetAllHeaders(headers, error)) return false;
  if (headers.empty()) return true;
  oeeoo::Block genesis;
  if (!store.GetBlock(std::get<1>(headers[0]).GetHash(), genesis, error)) {
    return false;
  }
  if (genesis.vtx.size() >= 1 && genesis.vtx[0].vout.size() == 1 &&
      genesis.vtx[0].vout[0].value == oeeoo::FOUNDER_ALLOCATION) {
    vesting.enabled = true;
    vesting.launchTime = genesis.header.nTime;
    vesting.founderScript = genesis.vtx[0].vout[0].scriptPubKey;
  }
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  using namespace oeeoo;
  AppConfig config;
  std::string error;
  if (!ParseArgs(argc, argv, config, error)) {
    std::cerr << "oeeoo-node: " << error << "\n" << UsageText(argv[0]);
    return 1;
  }
  if (config.help) {
    std::cout << UsageText(argv[0]);
    return 0;
  }
  if (config.version) {
    std::cout << "oeeoo-node 0.1.0\n";
    return 0;
  }
  std::error_code ec;
  std::filesystem::create_directories(config.dataDir, ec);
  if (ec) {
    std::cerr << "oeeoo-node: cannot create data directory " << config.dataDir
              << "\n";
    return 1;
  }
  ChainStore store;
  if (!store.Open(config.dataDir + "/chain.db", error)) {
    std::cerr << "oeeoo-node: " << error << "\n";
    return 1;
  }
  // Vesting parameters, before the chain is loaded: an explicit
  // genesis file wins, then the config script, then the stored
  // genesis block.
  VestingParams vesting;
  Block genesis;
  bool haveGenesisFile = false;
  if (!config.genesisFile.empty()) {
    std::vector<unsigned char> founderScript;
    if (!ReadGenesisFile(config.genesisFile, genesis, founderScript,
                         error)) {
      std::cerr << "oeeoo-node: " << error << "\n";
      return 1;
    }
    vesting.enabled = true;
    vesting.launchTime = genesis.header.nTime;
    vesting.founderScript = founderScript;
    haveGenesisFile = true;
  } else if (!config.founderScript.empty()) {
    if (!HexToBytesStr(config.founderScript, vesting.founderScript)) {
      std::cerr << "oeeoo-node: founder script is not valid hex\n";
      return 1;
    }
    vesting.enabled = true;
    // launchTime 0: adopted from the stored genesis on load.
  } else if (!DeriveVestingFromStore(store, vesting, error)) {
    std::cerr << "oeeoo-node: " << error << "\n";
    return 1;
  }
  Node node(config.powLimitBits);
  node.SetVestingParams(vesting);
  node.SetMaxPeers(config.maxPeers);
  node.SetMaxMempoolBytes(config.maxMempoolBytes);
  node.AttachStore(&store);
  if (!node.LoadFromStore(store, error, config.deepVerify)) {
    std::cerr << "oeeoo-node: " << error << "\n";
    return 1;
  }
  // Genesis identity. A --genesis-file must match the genesis block
  // already in the data directory: a different genesis is refused,
  // never silently ignored. A --genesis-checkpoint must match the
  // genesis that is actually loaded (from the store, or from the
  // file on a fresh directory).
  if (!CheckGenesisIdentity(node, genesis, haveGenesisFile,
                            config.genesisCheckpoint, error)) {
    std::cerr << "oeeoo-node: " << error << "\n";
    return 1;
  }
  if (node.TipHeight() < 0) {
    if (!haveGenesisFile) {
      std::cerr << "oeeoo-node: fresh data directory and no genesis file.\n"
                   "Generate one with oeeoo-genesis and pass "
                   "--genesis-file <path>.\n";
      return 1;
    }
    if (!node.BroadcastBlock(genesis, error)) {
      std::cerr << "oeeoo-node: genesis rejected: " << error << "\n";
      return 1;
    }
    std::cout << "Genesis accepted: " << HashHex(genesis.GetHash()) << "\n";
  }
  // Node wallet (Milestone 14). A relative path resolves against the
  // data directory. An existing file loads LOCKED; a missing file is
  // remembered so the operator can create it with the createwallet
  // RPC. A corrupt file refuses startup loudly.
  NodeWallet nodeWallet;
  bool walletConfigured = false;
  std::string walletPathStr;
  if (!config.walletFile.empty()) {
    std::filesystem::path walletPath(config.walletFile);
    if (walletPath.is_relative()) {
      walletPath = std::filesystem::path(config.dataDir) / walletPath;
    }
    walletPathStr = walletPath.string();
    if (!nodeWallet.LoadFile(walletPathStr, error)) {
      std::cerr << "oeeoo-node: " << error << "\n";
      return 1;
    }
    walletConfigured = true;
  }
  for (const std::string& seed : config.dnsSeeds) {
    node.AddDnsSeed(seed);
  }
  if (!config.dnsSeeds.empty() && node.AddrMan().Size() == 0) {
    std::string seedError;
    const std::size_t added = node.QueryDnsSeeds(seedError);
    std::cout << "DNS seeds: " << added << " peer address(es) found";
    if (!seedError.empty()) std::cout << " (" << seedError << ")";
    std::cout << "\n";
  }
  auto transport = std::make_shared<TcpTransport>();
  node.SetTransport(transport);
  if (!node.Listen(config.port)) {
    std::cerr << "oeeoo-node: cannot listen on P2P port " << config.port
              << "\n";
    return 1;
  }
  std::mutex nodeMutex;
  RpcServer rpc(node, config.rpcPort, config.dataDir, &nodeMutex);
  if (walletConfigured) rpc.AttachNodeWallet(&nodeWallet);
  // M16: RPC load control (Core-style thread count + bounded work
  // queue). Defaults are 16 threads / 64 queued when unconfigured.
  rpc.SetMaxThreads(config.rpcThreads);
  rpc.SetWorkQueueSize(config.rpcWorkQueueSize);
  if (!rpc.Start(error)) {
    std::cerr << "oeeoo-node: RPC failed to start: " << error << "\n";
    return 1;
  }
  StratumServer stratum(node, config.stratumPort, &nodeMutex);
  bool stratumOn = false;
  if (!config.minerAddress.empty()) {
    stratum.SetMinerAddress(config.minerAddress);
    if (!stratum.Start(error)) {
      std::cerr << "oeeoo-node: Stratum failed to start: " << error << "\n";
      return 1;
    }
    stratumOn = true;
  }
  std::cout << "Oeeoo Coin node 0.1.0\n"
            << "  data dir: " << config.dataDir << "\n"
            << "  height: " << node.TipHeight()
            << "  tip: " << HashHex(node.TipHash()) << "\n"
            << "  P2P: " << config.port << "  RPC: 127.0.0.1:"
            << config.rpcPort << "  Stratum: "
            << (stratumOn ? std::to_string(config.stratumPort) : "off")
            << "\n"
            << "  vesting: " << (vesting.enabled ? "enabled" : "disabled")
            << "\n"
            << "  wallet: "
            << (!walletConfigured
                    ? "none"
                    : (nodeWallet.HasWallet()
                           ? walletPathStr + " (locked)"
                           : walletPathStr +
                                 " (not created yet — use createwallet RPC)"))
            << "\n";
  if (config.powLimitBits != 0x1d00ffff) {
    // A non-mainnet proof-of-work limit means this node is on a
    // test chain, not Mainnet. Say so loudly: an operator who
    // passes --pow-limit-bits on a mainnet data directory would
    // otherwise silently run a different chain.
    std::cout << "  *** WARNING: TESTING MODE — proof-of-work limit is "
                 "0x"
              << std::hex << config.powLimitBits << std::dec
              << ", not the mainnet limit 0x1d00ffff. This node is "
                 "NOT on Mainnet. ***\n";
  }
  std::cout << std::flush;
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  while (!g_stop.load() && !rpc.StopRequested()) {
    node.Poll(100);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  std::cout << "Shutting down...\n";
  if (stratumOn) stratum.Stop();
  rpc.Stop();
  if (walletConfigured) {
    // Persist the indices, then wipe the keys from memory. Lock()
    // saves again best-effort; the explicit save here surfaces an
    // I/O failure as a warning instead of swallowing it.
    std::string walletError;
    if (!nodeWallet.SaveIfDirty(walletError)) {
      std::cerr << "oeeoo-node: warning: cannot save wallet: " << walletError
                << "\n";
    }
    nodeWallet.Lock();
  }
  store.Close();
  return 0;
}
