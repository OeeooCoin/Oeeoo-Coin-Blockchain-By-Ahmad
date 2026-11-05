// SPDX-License-Identifier: MIT
// Oeeoo Coin — Stratum v1 mining server (Milestone 10).
// Line-delimited JSON-RPC over TCP (default port 64777; P2P is
// 64775, RPC is 64776). Speaks the standard Stratum v1 dialect:
// mining.subscribe / mining.authorize / mining.set_difficulty /
// mining.notify / mining.submit. The wire format follows the public
// Stratum convention: `prevhash` in mining.notify is the previous
// block hash in DISPLAY order (big-endian hex, as getblockhash
// returns it); the miner reverses it when building the header.
// A share that also meets the network difficulty is assembled into
// a full block and submitted to the chain. Difficulty starts at
// SetInitialDifficulty (default 1.0) with a basic vardiff: every
// 60 seconds a session's difficulty doubles above 12 shares/minute
// and halves below 2 shares/minute (floor 0.01).
// The server never owns the node. All node access is serialized
// through `nodeMutex` when one is given.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "network/node.h"
#include "rpc/json.h"

namespace oeeoo {

constexpr uint16_t kStratumPort = 64777;

class StratumServer {
 public:
  StratumServer(Node& node, uint16_t port = kStratumPort,
                std::mutex* nodeMutex = nullptr);
  ~StratumServer();

  // P2PKH address the coinbase pays (required before Start).
  void SetMinerAddress(const std::string& address) {
    minerAddress_ = address;
  }
  void SetInitialDifficulty(double difficulty) {
    initialDifficulty_ = difficulty;
  }
  bool Start(std::string& error);
  void Stop();
  bool IsRunning() const { return running_.load(); }
  uint16_t Port() const { return port_; }
  std::string CurrentJobId() const;

  uint64_t AcceptedShares() const { return acceptedShares_.load(); }
  uint64_t RejectedShares() const { return rejectedShares_.load(); }
  uint64_t BlocksFound() const { return blocksFound_.load(); }

 private:
  struct Session {
    int fd = -1;
    std::string extranonce1;  // 8 hex chars (4 bytes)
    double difficulty = 1.0;
    bool subscribed = false;
    bool authorized = false;
    std::string worker;
    uint64_t sharesAccepted = 0;
    uint64_t sharesRejected = 0;
    uint64_t sharesInWindow = 0;
    uint32_t windowStart = 0;
    std::string buffer;
  };

  struct Job {
    std::string id;
    Hash256 prevHash;  // internal byte order
    std::string coinbase1;  // hex, up to extranonce1
    std::string coinbase2;  // hex, after extranonce2
    std::vector<std::string> merkleBranches;  // hex
    uint32_t version = 1;
    uint32_t nbits = 0;
    uint32_t ntime = 0;
    int height = 0;
    Amount coinbaseValue = 0;
    std::vector<Transaction> otherTxs;  // mempool txs in the template
  };

  void AcceptLoop();
  void ClientLoop(int fd);
  void WatchLoop();  // rebuilds the job when the tip moves
  bool BuildJob(Job& job, std::string& error);
  void BroadcastJob(const Job& job, bool cleanJobs);
  void SendToSession(Session& session, const Json& message);
  Json HandleMessage(Session& session, const Json& request);
  bool ValidateShare(Session& session, const Job& job,
                     const std::string& extranonce2Hex, uint32_t ntime,
                     uint32_t nonce, std::string& error);
  void CloseSession(Session& session);
  void RemoveSession(int fd);

  Node& node_;
  uint16_t port_;
  std::mutex* nodeMutex_;
  std::string minerAddress_;
  double initialDifficulty_ = 1.0;
  int listenFd_ = -1;
  std::atomic<bool> running_{false};
  std::thread acceptThread_;
  std::thread watchThread_;

  std::mutex sessionsMutex_;
  std::vector<std::shared_ptr<Session>> sessions_;
  std::vector<std::thread> clientThreads_;

  mutable std::mutex jobMutex_;
  Job currentJob_;
  Job previousJob_;
  bool hasJob_ = false;
  uint64_t jobCounter_ = 0;

  std::atomic<uint64_t> acceptedShares_{0};
  std::atomic<uint64_t> rejectedShares_{0};
  std::atomic<uint64_t> blocksFound_{0};
};

}  // namespace oeeoo
