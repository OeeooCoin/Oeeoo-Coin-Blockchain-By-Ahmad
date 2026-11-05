// SPDX-License-Identifier: MIT
// Oeeoo Coin — transports: loopback (tests) and TCP (real sockets).
#include "network/transport.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace oeeoo {

// ---- Loopback transport ----

namespace {
std::mutex g_loopbackMutex;
std::vector<std::pair<std::string, LoopbackTransport*>> g_loopbackRegistry;
}  // namespace

LoopbackTransport::LoopbackTransport(const std::string& address)
    : address_(address) {
  std::lock_guard<std::mutex> lock(g_loopbackMutex);
  g_loopbackRegistry.push_back({address_, this});
}

LoopbackTransport::~LoopbackTransport() {
  CloseLoopback();
  std::lock_guard<std::mutex> lock(g_loopbackMutex);
  g_loopbackRegistry.erase(
      std::remove_if(g_loopbackRegistry.begin(), g_loopbackRegistry.end(),
                     [&](const auto& e) { return e.second == this; }),
      g_loopbackRegistry.end());
}

bool LoopbackTransport::Connect(const std::string& address,
                                std::string& peerId) {
  LoopbackTransport* target = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_loopbackMutex);
    for (const auto& e : g_loopbackRegistry) {
      if (e.first == address) target = e.second;
    }
  }
  if (!target || target == this) return false;
  peerId = "loopback:" + address_ + "->" + address + "#" +
           std::to_string(nextPeer_++);
  const std::string remotePeerId = "loopback:" + address + "->" + address_ +
                                   "#" + std::to_string(target->nextPeer_++);
  Link link;
  link.remote = target;
  link.remotePeerId = remotePeerId;
  links_.push_back({peerId, link});
  Link reverse;
  reverse.remote = this;
  reverse.remotePeerId = peerId;
  target->links_.push_back({remotePeerId, reverse});
  return true;
}

bool LoopbackTransport::Send(const std::string& peerId,
                             const std::vector<unsigned char>& bytes) {
  for (const auto& entry : links_) {
    if (entry.first == peerId) {
      TransportMessage msg;
      msg.peerId = entry.second.remotePeerId;
      msg.bytes = bytes;
      entry.second.remote->inbound_.push_back(std::move(msg));
      return true;
    }
  }
  return false;
}

bool LoopbackTransport::Poll(TransportMessage& msg, int timeoutMs) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (true) {
    if (!inbound_.empty()) {
      msg = std::move(inbound_.front());
      inbound_.erase(inbound_.begin());
      return true;
    }
    if (timeoutMs <= 0 ||
        std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void LoopbackTransport::Disconnect(const std::string& peerId) {
  links_.erase(std::remove_if(links_.begin(), links_.end(),
                              [&](const auto& e) { return e.first == peerId; }),
               links_.end());
}

void LoopbackTransport::Close() { CloseLoopback(); }

void LoopbackTransport::CloseLoopback() {
  inbound_.clear();
  links_.clear();
}

// ---- TCP transport ----

TcpTransport::TcpTransport() = default;
TcpTransport::~TcpTransport() { CloseTcp(); }

bool TcpTransport::FindPeer(const std::string& peerId, int& fd) const {
  for (const auto& entry : peers_) {
    if (entry.first == peerId) {
      fd = entry.second;
      return true;
    }
  }
  return false;
}

bool TcpTransport::Connect(const std::string& address, std::string& peerId) {
  const std::size_t colon = address.rfind(':');
  if (colon == std::string::npos) return false;
  const std::string ip = address.substr(0, colon);
  const int port = std::stoi(address.substr(colon + 1));
  if (port <= 0 || port > 65535) return false;
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
    close(fd);
    return false;
  }
  // 5-second connect timeout.
  timeval timeout{};
  timeout.tv_sec = 5;
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return false;
  }
  peerId = "tcp:" + std::to_string(nextPeer_++);
  peers_.push_back({peerId, fd});
  return true;
}

bool TcpTransport::Listen(uint16_t port) {
  if (listenFd_ >= 0) return true;
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return false;
  }
  if (listen(fd, 16) != 0) {
    close(fd);
    return false;
  }
  listenFd_ = fd;
  return true;
}

bool TcpTransport::Send(const std::string& peerId,
                        const std::vector<unsigned char>& bytes) {
  int fd = -1;
  if (!FindPeer(peerId, fd)) return false;
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const ssize_t n =
        send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
    if (n <= 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

bool TcpTransport::Poll(TransportMessage& msg, int timeoutMs) {
  fd_set readSet;
  FD_ZERO(&readSet);
  int maxFd = -1;
  if (listenFd_ >= 0) {
    FD_SET(listenFd_, &readSet);
    maxFd = listenFd_;
  }
  for (const auto& entry : peers_) {
    FD_SET(entry.second, &readSet);
    if (entry.second > maxFd) maxFd = entry.second;
  }
  if (maxFd < 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
    return false;
  }
  timeval timeout{};
  timeout.tv_sec = timeoutMs / 1000;
  timeout.tv_usec = (timeoutMs % 1000) * 1000;
  const int ready = select(maxFd + 1, &readSet, nullptr, nullptr, &timeout);
  if (ready <= 0) return false;
  if (listenFd_ >= 0 && FD_ISSET(listenFd_, &readSet)) {
    const int fd = accept(listenFd_, nullptr, nullptr);
    if (fd >= 0) {
      const std::string peerId = "tcp:" + std::to_string(nextPeer_++);
      peers_.push_back({peerId, fd});
    }
    return false;  // no data yet; the next Poll will read from the peer
  }
  for (const auto& entry : peers_) {
    if (!FD_ISSET(entry.second, &readSet)) continue;
    unsigned char buffer[65536];
    const ssize_t n = recv(entry.second, buffer, sizeof(buffer), 0);
    if (n <= 0) {
      // Peer closed or error: drop the connection.
      close(entry.second);
      peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
                                  [&](const auto& e) {
                                    return e.first == entry.first;
                                  }),
                   peers_.end());
      return false;
    }
    msg.peerId = entry.first;
    msg.bytes.assign(buffer, buffer + n);
    return true;
  }
  return false;
}

void TcpTransport::Disconnect(const std::string& peerId) {
  int fd = -1;
  if (!FindPeer(peerId, fd)) return;
  close(fd);
  peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
                              [&](const auto& e) {
                                return e.first == peerId;
                              }),
               peers_.end());
}

void TcpTransport::Close() { CloseTcp(); }

void TcpTransport::CloseTcp() {
  for (const auto& entry : peers_) close(entry.second);
  peers_.clear();
  if (listenFd_ >= 0) {
    close(listenFd_);
    listenFd_ = -1;
  }
}

}  // namespace oeeoo
