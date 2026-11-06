// SPDX-License-Identifier: MIT
// Oeeoo Coin — JSON-RPC server (Milestone 10).
// Bitcoin-style JSON-RPC 2.0 over HTTP on 127.0.0.1 (default port
// 64776; P2P is 64775). Single requests and batches. Authentication
// is HTTP Basic with a cookie file (`<dataDir>/.cookie`, content
// `__cookie__:<random hex>`), exactly like Bitcoin's cookie auth.
// The server runs an accept thread; connections are handled by a
// fixed worker-thread pool fed from a bounded work queue (Milestone
// 16, Bitcoin Core-style: `-rpcthreads` default 16, `-rpcworkqueue`
// default 64). Core has NO per-client rate limiting, and neither
// does Oeeoo — the controls are the thread count and the queue
// depth. A connection arriving when the queue is full is answered
// HTTP 500 with a JSON-RPC -32603 "RPC work queue is full" error
// and closed; the server keeps running. Every method call is
// serialized through `nodeMutex` when one is given, because Node
// is not thread-safe. The server never owns the node or the wallet.
// The RPC surface is a documented subset of Bitcoin's (see
// docs/MILESTONE10.md for the method list and the differences).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

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
  // RPC load control (Milestone 16). Both setters clamp to >= 1.
  // SetMaxThreads takes effect at the next Start() (the pool is
  // sized when the server starts); SetWorkQueueSize takes effect
  // immediately for connections accepted after the call. Defaults
  // match Bitcoin Core: 16 threads, 64 queued connections.
  void SetMaxThreads(int threads) {
    maxThreads_.store(threads < 1 ? 1 : threads);
  }
  void SetWorkQueueSize(int size) {
    workQueueSize_.store(size < 1 ? 1 : size);
  }
  int MaxThreads() const { return maxThreads_.load(); }
  int WorkQueueSize() const { return workQueueSize_.load(); }
  // Connections queued but not yet picked up by a worker.
  int WorkQueueDepth() const;
  // Connections currently being handled by a worker.
  int ActiveConnections() const { return activeConnections_.load(); }
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
  void WorkerLoop();
  // Enqueue an accepted connection for a worker. False when the
  // queue is full or the server is stopping (caller answers HTTP
  // 500 and closes the connection).
  bool TryEnqueueConnection(int fd);
  // HTTP 500 + JSON-RPC -32603 "RPC work queue is full", sent to a
  // connection the full queue refused. Best-effort: the socket is
  // closed by the caller either way.
  void SendQueueFullResponse(int fd);
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
  // Worker pool + bounded work queue (Milestone 16). The queue is
  // bounded by workQueueSize_: it can never grow past that depth,
  // and TryEnqueueConnection refuses (HTTP 500) instead of
  // growing it. queueShutdown_ is guarded by queueMutex_.
  std::atomic<int> maxThreads_{16};
  std::atomic<int> workQueueSize_{64};
  std::vector<std::thread> workers_;
  mutable std::mutex queueMutex_;
  std::condition_variable queueCond_;
  std::queue<int> workQueue_;
  bool queueShutdown_ = false;
  // Sockets currently inside HandleConnection, so Stop() can
  // shutdown() a slow client's socket and join the workers
  // instead of hanging on a blocked recv. Guarded by activeMutex_;
  // lock order is queueMutex_ -> activeMutex_ (never the reverse).
  std::mutex activeMutex_;
  std::unordered_set<int> activeFds_;
  // Concurrent connection count (connections being handled by a
  // worker). The old thread-per-connection cap (kMaxConnections =
  // 32) is gone: the pool size + the bounded queue bound the
  // server's work instead.
  std::atomic<int> activeConnections_{0};
};

}  // namespace oeeoo
