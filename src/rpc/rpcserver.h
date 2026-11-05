// SPDX-License-Identifier: MIT
// Oeeoo Coin — JSON-RPC server (Milestone 10).
// Bitcoin-style JSON-RPC 2.0 over HTTP on 127.0.0.1 (default port
// 64776; P2P is 64775). Single requests and batches. Authentication
// is HTTP Basic with a cookie file (`<dataDir>/.cookie`, content
// `__cookie__:<random hex>`), exactly like Bitcoin's cookie auth.
// The server runs an accept thread; every method call is serialized
// through `nodeMutex` when one is given, because Node is not
// thread-safe. The server never owns the node or the wallet.
// The RPC surface is a documented subset of Bitcoin's (see
// docs/MILESTONE10.md for the method list and the differences).
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "network/node.h"
#include "rpc/json.h"
#include "wallet/nodewallet.h"
#include "wallet/wallet.h"

namespace oeeoo {

constexpr uint16_t kRpcPort = 64776;

class RpcServer {
 public:
  // `dataDir` holds the .cookie file (empty = no cookie file is
  // written, auth still uses the in-memory cookie). `nodeMutex`
  // serializes node access across servers/threads (may be null
  // when only one thread ever touches the node).
  RpcServer(Node& node, uint16_t port = kRpcPort,
            const std::string& dataDir = "",
            std::mutex* nodeMutex = nullptr);
  ~RpcServer();

  void AttachWallet(Wallet* wallet) { wallet_ = wallet; }
  // Attach the node wallet (Milestone 14). When one is attached it
  // takes precedence over a plain wallet: the wallet methods go
  // through its lock state. The plain-wallet attach stays for tests
  // that drive the RPC server without a node wallet.
  void AttachNodeWallet(NodeWallet* wallet) { nodeWallet_ = wallet; }
  void SetAuthEnabled(bool enabled) { authEnabled_ = enabled; }
  bool Start(std::string& error);
  void Stop();
  bool IsRunning() const { return running_.load(); }
  uint16_t Port() const { return port_; }
  std::string CookieFile() const;
  const std::string& Cookie() const { return cookie_; }
  bool StopRequested() const { return stopRequested_.load(); }

 private:
  struct NodeLock {
    explicit NodeLock(std::mutex* m) : mutex_(m) {
      if (mutex_ != nullptr) mutex_->lock();
    }
    ~NodeLock() {
      if (mutex_ != nullptr) mutex_->unlock();
    }
    std::mutex* mutex_;
  };

  Json HandleRequest(const Json& request);
  // Returns the result, or Null + sets `code`/`message` on error.
  Json CallMethod(const std::string& method, const Json& params, int& code,
                  std::string& message);
  void AcceptLoop();
  void HandleConnection(int fd);
  bool Authorized(const std::string& authHeader) const;

  Node& node_;
  Wallet* wallet_ = nullptr;
  NodeWallet* nodeWallet_ = nullptr;
  uint16_t port_;
  std::string dataDir_;
  std::mutex* nodeMutex_;
  bool authEnabled_ = true;
  std::string cookie_;
  int listenFd_ = -1;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopRequested_{false};
  std::thread acceptThread_;
  // Concurrent connection count. Each connection runs on its own
  // thread (a slow client must not block all RPC), capped so a
  // flood of connections cannot grow threads without bound.
  std::atomic<int> activeConnections_{0};
  static constexpr int kMaxConnections = 32;
};

}  // namespace oeeoo
