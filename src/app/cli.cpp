// SPDX-License-Identifier: MIT
// Oeeoo Coin — oeeoo-cli implementation (plain POSIX sockets).
#include "app/cli.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>

#include "rpc/rpcutil.h"

namespace oeeoo {

bool CliCall(const std::string& dataDir, uint16_t rpcPort,
             const std::string& method, const Json& params, Json& response,
             std::string& error) {
  // Cookie auth (same file the server writes).
  std::string cookie;
  {
    std::ifstream in(dataDir + "/.cookie");
    if (!in) {
      error = "cannot read the RPC cookie (" + dataDir +
              "/.cookie): is the node running?";
      return false;
    }
    std::string line;
    if (!std::getline(in, line) || line.rfind("__cookie__:", 0) != 0) {
      error = "the RPC cookie file is malformed";
      return false;
    }
    cookie = line.substr(std::string("__cookie__:").size());
  }
  Json request = Json::Object();
  request.Set("jsonrpc", "2.0");
  request.Set("id", 1);
  request.Set("method", method);
  request.Set("params", params);
  const std::string body = request.Dump();
  const std::string http =
      "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: "
      "application/json\r\nContent-Length: " +
      std::to_string(body.size()) +
      "\r\nAuthorization: Basic " +
      Base64EncodeStr("__cookie__:" + cookie) +
      "\r\nConnection: close\r\n\r\n" + body;
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    error = "cannot create a socket";
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(rpcPort);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    error = "cannot connect to the RPC server at 127.0.0.1:" +
            std::to_string(rpcPort) + " (is the node running?)";
    return false;
  }
  std::size_t sent = 0;
  while (sent < http.size()) {
    const ssize_t n =
        ::send(fd, http.data() + sent, http.size() - sent, 0);
    if (n <= 0) {
      ::close(fd);
      error = "failed to send the RPC request";
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  std::string reply;
  char buffer[4096];
  for (;;) {
    const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n < 0) {
      ::close(fd);
      error = "failed to read the RPC reply";
      return false;
    }
    if (n == 0) break;
    reply.append(buffer, static_cast<std::size_t>(n));
  }
  ::close(fd);
  const std::size_t split = reply.find("\r\n\r\n");
  if (split == std::string::npos) {
    error = "the RPC reply is not valid HTTP";
    return false;
  }
  std::string parseError;
  if (!Json::Parse(reply.substr(split + 4), response, parseError)) {
    error = "the RPC reply is not valid JSON: " + parseError;
    return false;
  }
  return true;
}

}  // namespace oeeoo
