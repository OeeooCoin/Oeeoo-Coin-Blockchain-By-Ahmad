// SPDX-License-Identifier: MIT
// Oeeoo Coin — network transport abstraction (Milestone 6).
// The node speaks protocol Messages; the transport moves raw bytes.
// Two transports ship: LoopbackTransport (in-process, deterministic,
// used by tests) and TcpTransport (real POSIX sockets). Peers are
// identified by an opaque string id assigned by the transport.
// The transport does NOT frame messages — the node reassembles the
// byte stream per peer (see Node::OnBytes).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oeeoo {

struct TransportMessage {
  std::string peerId;
  std::vector<unsigned char> bytes;
  // Address of the remote peer as "ip:port" (TCP) or
  // "loopback:<sender>" (loopback). Empty when unknown. The node
  // uses it as the peer's address so bans are keyed by IP and
  // survive a reconnect (Milestone 15).
  std::string remoteAddress;
};

class ITransport {
 public:
  virtual ~ITransport() = default;
  // Connect to `address` ("ip:port" for TCP, "loopback:name" for the
  // loopback transport). On success sets `peerId`. False on failure.
  virtual bool Connect(const std::string& address, std::string& peerId) = 0;
  // Listen for inbound connections on `port`. False on failure.
  virtual bool Listen(uint16_t port) = 0;
  virtual bool Send(const std::string& peerId,
                    const std::vector<unsigned char>& bytes) = 0;
  // Receive one chunk of bytes. False on timeout / nothing to read.
  virtual bool Poll(TransportMessage& msg, int timeoutMs) = 0;
  virtual void Disconnect(const std::string& peerId) = 0;
  virtual void Close() = 0;
};

// In-process transport for tests: transports are linked by address
// ("loopback:<name>") through a process-wide registry. No sockets.
class LoopbackTransport : public ITransport {
 public:
  explicit LoopbackTransport(const std::string& address);
  ~LoopbackTransport() override;
  bool Connect(const std::string& address, std::string& peerId) override;
  bool Listen(uint16_t /*port*/) override { return true; }  // no-op
  bool Send(const std::string& peerId,
            const std::vector<unsigned char>& bytes) override;
  bool Poll(TransportMessage& msg, int timeoutMs) override;
  void Disconnect(const std::string& peerId) override;
  void Close() override;

 private:
  // Non-virtual cleanup. The destructor calls this directly: a
  // virtual call from a destructor would dispatch to this class
  // only (the object is already being destroyed), and cppcheck
  // flags it. Routing both the destructor and Close() through the
  // same non-virtual function keeps the cleanup identical and the
  // dispatch question moot.
  void CloseLoopback();

  struct Link {
    LoopbackTransport* remote = nullptr;
    std::string remotePeerId;  // how the remote end knows this peer
  };
  std::string address_;
  std::vector<TransportMessage> inbound_;
  std::vector<std::pair<std::string, Link>> links_;  // by local peerId
  int nextPeer_ = 0;
};

// Real TCP transport (POSIX sockets). `address` is "ip:port".
// Each connection is a non-blocking socket driven by select() in Poll.
class TcpTransport : public ITransport {
 public:
  TcpTransport();
  ~TcpTransport() override;
  bool Connect(const std::string& address, std::string& peerId) override;
  bool Listen(uint16_t port) override;
  bool Send(const std::string& peerId,
            const std::vector<unsigned char>& bytes) override;
  bool Poll(TransportMessage& msg, int timeoutMs) override;
  void Disconnect(const std::string& peerId) override;
  void Close() override;

 private:
  // Non-virtual cleanup; see CloseLoopback above.
  void CloseTcp();

  int listenFd_ = -1;
  // One entry per connected peer: transport id, socket fd, and the
  // remote "ip:port" recorded at accept()/Connect time (Milestone
  // 15 — the node needs the real IP so bans survive reconnects).
  struct TcpPeer {
    std::string peerId;
    int fd = -1;
    std::string address;  // "ip:port"; empty if it could not be read
  };
  std::vector<TcpPeer> peers_;
  int nextPeer_ = 0;
  bool FindPeer(const std::string& peerId, int& fd) const;
};

}  // namespace oeeoo
