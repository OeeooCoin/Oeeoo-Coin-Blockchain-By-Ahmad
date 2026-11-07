// SPDX-License-Identifier: MIT
// Oeeoo Coin — JSON-RPC server implementation.
#include "rpc/rpcserver.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/rand.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "blockchain/merkle.h"
#include "consensus/difficulty.h"
#include "consensus/subsidy.h"
#include "crypto/address.h"
#include "mining/miner.h"
#include "network/protocol.h"
#include "rpc/rpcutil.h"
#include "script/script.h"

namespace oeeoo {

namespace {
// ---- small helpers ----
std::string HashHex(const Hash256& h) {
  return BytesToHexStr(std::vector<unsigned char>(h.begin(), h.end()));
}

bool HexToHash(const std::string& hex, Hash256& out) {
  std::vector<unsigned char> bytes;
  if (!HexToBytesStr(hex, bytes) || bytes.size() != 32) return false;
  std::memcpy(out.data(), bytes.data(), 32);
  return true;
}

// Approximate a 256-bit target as a long double, using the top 64
// significant bits (plenty for a difficulty ratio).
long double TargetApprox(const Target& target) {
  int first = 0;
  while (first < 32 && target[first] == 0) ++first;
  if (first == 32) return 0.0L;
  uint64_t mantissa = 0;
  for (int i = 0; i < 8; ++i) {
    const int idx = first + i;
    mantissa <<= 8;
    if (idx < 32) mantissa |= target[idx];
  }
  // value = mantissa * 256^(24 - first) (mantissa covers 8 bytes).
  const int exponent = 8 * (24 - first);
  return static_cast<long double>(mantissa) * std::pow(2.0L, exponent);
}

double AmountToOeeoo(Amount sats) {
  return static_cast<double>(sats) / 100000000.0;
}

// Parse an amount from a JSON number (OEEOO) or a decimal string.
bool ParseAmount(const Json& j, Amount& out, std::string& error) {
  if (j.IsString()) {
    const std::string& s = j.AsString();
    std::size_t dot = s.find('.');
    std::string whole = (dot == std::string::npos) ? s : s.substr(0, dot);
    std::string frac = (dot == std::string::npos) ? "" : s.substr(dot + 1);
    if (whole.empty() || frac.size() > 8) {
      error = "bad amount";
      return false;
    }
    for (char c : whole) {
      if (c < '0' || c > '9') {
        error = "bad amount";
        return false;
      }
    }
    for (char c : frac) {
      if (c < '0' || c > '9') {
        error = "bad amount";
        return false;
      }
    }
    while (frac.size() < 8) frac.push_back('0');
    try {
      const int64_t w = std::stoll(whole);
      const int64_t f = frac.empty() ? 0 : std::stoll(frac);
      __int128 total = static_cast<__int128>(w) * COIN + f;
      if (total > MAX_MONEY || total < 0) {
        error = "amount out of range";
        return false;
      }
      out = static_cast<Amount>(total);
      return true;
    } catch (...) {
      error = "bad amount";
      return false;
    }
  }
  if (j.IsNumber()) {
    const double d = j.AsDouble();
    if (d < 0 || d > 21000000.0) {
      error = "amount out of range";
      return false;
    }
    out = static_cast<Amount>(std::llround(d * 100000000.0));
    return true;
  }
  error = "amount must be a number or string";
  return false;
}

Json TxToJson(const Transaction& tx) {
  Json j = Json::Object();
  j.Set("txid", HashHex(tx.GetTxId()));
  j.Set("hash", HashHex(tx.GetTxId()));
  j.Set("version", tx.version);
  j.Set("locktime", static_cast<int64_t>(tx.lockTime));
  Json vin = Json::Array();
  for (const TxIn& in : tx.vin) {
    Json item = Json::Object();
    item.Set("txid", HashHex(in.prevTxHash));
    item.Set("vout", static_cast<int64_t>(in.prevOutIndex));
    Json scriptSig = Json::Object();
    scriptSig.Set("hex", BytesToHexStr(in.scriptSig));
    item.Set("scriptSig", scriptSig);
    item.Set("sequence", static_cast<int64_t>(in.sequence));
    vin.Push(item);
  }
  j.Set("vin", vin);
  Json vout = Json::Array();
  for (std::size_t i = 0; i < tx.vout.size(); ++i) {
    Json item = Json::Object();
    item.Set("value", AmountToOeeoo(tx.vout[i].value));
    item.Set("n", static_cast<int64_t>(i));
    Json script = Json::Object();
    script.Set("hex", BytesToHexStr(tx.vout[i].scriptPubKey));
    item.Set("scriptPubKey", script);
    vout.Push(item);
  }
  j.Set("vout", vout);
  return j;
}

bool SendAll(int fd, const std::string& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
    if (n <= 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}
}  // namespace

RpcServer::RpcServer(Node& node, uint16_t port, const std::string& dataDir,
                     std::mutex* nodeMutex)
    : node_(node),
      port_(port),
      dataDir_(dataDir),
      nodeMutex_(nodeMutex) {}

RpcServer::~RpcServer() { Stop(); }

std::string RpcServer::CookieFile() const {
  if (dataDir_.empty()) return "";
  return dataDir_ + "/.cookie";
}

bool RpcServer::Start(std::string& error) {
  if (running_.load()) {
    error = "RPC server already running";
    return false;
  }
  // Random cookie for Basic auth.
  unsigned char randomBytes[32];
  if (RAND_bytes(randomBytes, sizeof(randomBytes)) != 1) {
    error = "randomness failure";
    return false;
  }
  cookie_ = BytesToHexStr(
      std::vector<unsigned char>(randomBytes, randomBytes + 32));
  if (!dataDir_.empty()) {
    const std::string path = CookieFile();
    FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) {
      error = "cannot write cookie file";
      return false;
    }
    std::fprintf(f, "__cookie__:%s\n", cookie_.c_str());
    std::fclose(f);
    ::chmod(path.c_str(), 0600);
  }
  listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listenFd_ < 0) {
    error = "cannot create socket";
    return false;
  }
  int reuse = 1;
  ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port_);
  if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) != 0) {
    error = "cannot bind RPC port";
    ::close(listenFd_);
    listenFd_ = -1;
    return false;
  }
  if (port_ == 0) {
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&bound),
                      &len) == 0) {
      port_ = ntohs(bound.sin_port);
    }
  }
  if (::listen(listenFd_, 16) != 0) {
    error = "cannot listen on RPC port";
    ::close(listenFd_);
    listenFd_ = -1;
    return false;
  }
  running_.store(true);
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    queueShutdown_ = false;
  }
  for (int i = 0; i < maxThreads_.load(); ++i) {
    workers_.emplace_back(&RpcServer::WorkerLoop, this);
  }
  acceptThread_ = std::thread(&RpcServer::AcceptLoop, this);
  return true;
}

void RpcServer::Stop() {
  if (!running_.load()) return;
  running_.store(false);
  if (listenFd_ >= 0) {
    ::shutdown(listenFd_, SHUT_RDWR);
    ::close(listenFd_);
    listenFd_ = -1;
  }
  if (acceptThread_.joinable()) acceptThread_.join();
  // Stop queueing and drop connections still waiting in the queue
  // (their sockets are closed here — no worker will ever see them).
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    queueShutdown_ = true;
    while (!workQueue_.empty()) {
      ::close(workQueue_.front());
      workQueue_.pop();
    }
  }
  queueCond_.notify_all();
  // Unblock any worker waiting on a slow client: shutdown() makes
  // its blocked recv return, so the join below cannot hang. Only
  // shutdown() here — the worker itself closes the socket. The
  // sweep cannot miss a fd: a worker registers its fd and checks
  // queueShutdown_ (set above) under the queue lock before
  // handling it, so any fd in flight either was registered before
  // this sweep or is abandoned by the worker instead of handled.
  {
    std::lock_guard<std::mutex> lock(activeMutex_);
    for (int fd : activeFds_) ::shutdown(fd, SHUT_RDWR);
  }
  for (std::thread& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
  workers_.clear();
}

int RpcServer::WorkQueueDepth() const {
  std::lock_guard<std::mutex> lock(queueMutex_);
  return static_cast<int>(workQueue_.size());
}

bool RpcServer::TryEnqueueConnection(int fd) {
  std::lock_guard<std::mutex> lock(queueMutex_);
  if (queueShutdown_) return false;
  if (static_cast<int>(workQueue_.size()) >= workQueueSize_.load()) {
    return false;
  }
  workQueue_.push(fd);
  queueCond_.notify_one();
  return true;
}

void RpcServer::SendQueueFullResponse(int fd) {
  Json response = Json::Object();
  response.Set("jsonrpc", "2.0");
  response.Set("id", Json());
  Json err = Json::Object();
  err.Set("code", -32603);
  err.Set("message", "RPC work queue is full");
  response.Set("error", err);
  const std::string body = response.Dump();
  const std::string http = "HTTP/1.1 500 Internal Server Error\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: " +
                           std::to_string(body.size()) +
                           "\r\nConnection: close\r\n\r\n" + body;
  SendAll(fd, http);
}

void RpcServer::WorkerLoop() {
  while (true) {
    int fd = -1;
    {
      std::unique_lock<std::mutex> lock(queueMutex_);
      queueCond_.wait(lock, [this] {
        return queueShutdown_ || !workQueue_.empty();
      });
      if (workQueue_.empty()) return;  // shutdown (queue drained)
      fd = workQueue_.front();
      workQueue_.pop();
      // Register the fd as active while still holding the queue
      // lock, and check the shutdown flag here: if Stop() has
      // already set it, this fd is abandoned (closed below)
      // instead of handled — so Stop()'s shutdown sweep cannot
      // miss a fd a worker is about to block on. (If the flag is
      // not set yet, Stop() has not swept either, so the sweep
      // will see this fd.)
      {
        std::lock_guard<std::mutex> activeLock(activeMutex_);
        activeFds_.insert(fd);
      }
      if (queueShutdown_) {
        std::lock_guard<std::mutex> activeLock(activeMutex_);
        activeFds_.erase(fd);
        ::close(fd);
        return;
      }
    }  // queueMutex_ released BEFORE HandleConnection: a slow
    // client must not block the queue or Stop().
    activeConnections_.fetch_add(1);
    HandleConnection(fd);
    ::close(fd);
    activeConnections_.fetch_sub(1);
    {
      std::lock_guard<std::mutex> activeLock(activeMutex_);
      activeFds_.erase(fd);
    }
  }
}

void RpcServer::AcceptLoop() {
  while (running_.load()) {
    pollfd pfd{};
    pfd.fd = listenFd_;
    pfd.events = POLLIN;
    const int ready = ::poll(&pfd, 1, 200);
    if (ready <= 0) continue;
    if (!running_.load()) break;
    sockaddr_in client{};
    socklen_t len = sizeof(client);
    const int fd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&client),
                            &len);
    if (fd < 0) continue;
    // Fixed worker pool + bounded queue (Milestone 16): the
    // connection is queued for a worker instead of spawning a
    // thread. When the queue is full the connection is answered
    // HTTP 500 (JSON-RPC -32603 "RPC work queue is full") and
    // closed immediately; the server keeps running.
    if (!TryEnqueueConnection(fd)) {
      SendQueueFullResponse(fd);
      ::close(fd);
      continue;
    }
  }
}

bool RpcServer::Authorized(const std::string& authHeader) const {
  if (!authEnabled_) return true;
  const std::string prefix = "Basic ";
  if (authHeader.rfind(prefix, 0) != 0) return false;
  std::string decoded;
  if (!Base64DecodeStr(authHeader.substr(prefix.size()), decoded)) {
    return false;
  }
  return ConstantTimeEqual(decoded, "__cookie__:" + cookie_);
}

void RpcServer::HandleConnection(int fd) {
  // Read the HTTP header (cap 64 KB).
  std::string header;
  char buffer[4096];
  std::size_t headerEnd = std::string::npos;
  while (header.size() < 65536) {
    const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0) return;
    header.append(buffer, static_cast<std::size_t>(n));
    headerEnd = header.find("\r\n\r\n");
    if (headerEnd != std::string::npos) break;
  }
  if (headerEnd == std::string::npos) return;
  std::string body = header.substr(headerEnd + 4);
  header.resize(headerEnd);
  // Parse the request line + headers.
  std::string method;
  std::string authHeader;
  std::size_t contentLength = 0;
  std::size_t lineStart = 0;
  bool firstLine = true;
  while (lineStart < header.size()) {
    std::size_t lineEnd = header.find("\r\n", lineStart);
    if (lineEnd == std::string::npos) lineEnd = header.size();
    const std::string line = header.substr(lineStart, lineEnd - lineStart);
    if (firstLine) {
      firstLine = false;
      const std::size_t space = line.find(' ');
      if (space != std::string::npos) method = line.substr(0, space);
    } else {
      const std::size_t colon = line.find(':');
      if (colon != std::string::npos) {
        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        while (!value.empty() && value[0] == ' ') value.erase(0, 1);
        for (char& c : name) c = static_cast<char>(std::tolower(c));
        if (name == "authorization") authHeader = value;
        if (name == "content-length") {
          // Milestone 18 (fuzzing find M18B-R1): std::stoul throws
          // std::invalid_argument / std::out_of_range on a
          // non-numeric or out-of-range value, and the throw
          // escaped HandleConnection — WorkerLoop has no catch,
          // so one bad header aborted the whole node process
          // (std::terminate) before the auth check. Parse
          // defensively instead: digits only, saturating at the
          // 4 MB body cap (the read loop below stops there
          // anyway), and answer 400 on anything else.
          std::size_t parsedLength = 0;
          bool lengthOk = !value.empty();
          for (char c : value) {
            if (c < '0' || c > '9') {
              lengthOk = false;
              break;
            }
            parsedLength = parsedLength * 10 +
                           static_cast<std::size_t>(c - '0');
            if (parsedLength > 4 * 1024 * 1024) {
              parsedLength = 4 * 1024 * 1024 + 1;
            }
          }
          if (!lengthOk) {
            const std::string response =
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: 22\r\n"
                "Connection: close\r\n\r\n"
                "invalid Content-Length";
            SendAll(fd, response);
            return;
          }
          contentLength = parsedLength;
        }
      }
    }
    if (lineEnd == header.size()) break;
    lineStart = lineEnd + 2;
  }
  // Read the rest of the body.
  while (body.size() < contentLength && body.size() < 4 * 1024 * 1024) {
    const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0) return;
    body.append(buffer, static_cast<std::size_t>(n));
  }
  std::string status = "HTTP/1.1 200 OK\r\n";
  std::string responseBody;
  if (method != "POST") {
    status = "HTTP/1.1 405 Method Not Allowed\r\n";
    responseBody = "JSON-RPC requires POST";
  } else if (!Authorized(authHeader)) {
    status = "HTTP/1.1 401 Unauthorized\r\n"
             "WWW-Authenticate: Basic realm=\"OeeooCoin RPC\"\r\n";
    responseBody = "unauthorized";
  } else {
    Json request;
    std::string parseError;
    if (!Json::Parse(body, request, parseError)) {
      Json response = Json::Object();
      response.Set("jsonrpc", "2.0");
      response.Set("id", Json());
      Json err = Json::Object();
      err.Set("code", -32700);
      err.Set("message", "Parse error: " + parseError);
      response.Set("error", err);
      responseBody = response.Dump();
    } else {
      Json response = HandleRequest(request);
      if (!response.IsNull()) responseBody = response.Dump();
    }
  }
  std::string response = status + "Content-Type: application/json\r\n" +
                         "Content-Length: " +
                         std::to_string(responseBody.size()) + "\r\n" +
                         "Connection: close\r\n\r\n" + responseBody;
  SendAll(fd, response);
}

Json RpcServer::HandleRequest(const Json& request) {
  if (request.IsArray()) {
    // Batch: one response per request (notifications get none).
    Json responses = Json::Array();
    for (const Json& item : request.AsArray()) {
      Json single = HandleRequest(item);
      if (!single.IsNull()) responses.Push(single);
    }
    return responses;
  }
  if (!request.IsObject()) {
    Json response = Json::Object();
    response.Set("jsonrpc", "2.0");
    response.Set("id", Json());
    Json err = Json::Object();
    err.Set("code", -32600);
    err.Set("message", "Invalid Request");
    response.Set("error", err);
    return response;
  }
  const Json* id = request.Get("id");
  const Json* methodJson = request.Get("method");
  Json response = Json::Object();
  response.Set("jsonrpc", "2.0");
  if (id != nullptr) {
    response.Set("id", *id);
  } else {
    response.Set("id", Json());
  }
  if (methodJson == nullptr || !methodJson->IsString()) {
    Json err = Json::Object();
    err.Set("code", -32600);
    err.Set("message", "Invalid Request: missing method");
    response.Set("error", err);
    return response;
  }
  const Json* params = request.Get("params");
  Json emptyParams = Json::Array();
  int code = 0;
  std::string message;
  Json result = CallMethod(methodJson->AsString(),
                           params != nullptr ? *params : emptyParams, code,
                           message);
  if (code != 0) {
    Json err = Json::Object();
    err.Set("code", code);
    err.Set("message", message);
    response.Set("error", err);
  } else {
    response.Set("result", result);
  }
  // JSON-RPC notification (no id): no response.
  if (id == nullptr) return Json();
  return response;
}

namespace {
// Parameter helpers for CallMethod.
const Json* ParamAt(const Json& params, std::size_t index) {
  if (!params.IsArray() || index >= params.AsArray().size()) return nullptr;
  return &params.AsArray()[index];
}
const Json* ParamNamed(const Json& params, const std::string& name) {
  if (!params.IsObject()) return nullptr;
  return params.Get(name);
}
const Json* Param(const Json& params, std::size_t index,
                  const std::string& name) {
  const Json* p = ParamAt(params, index);
  if (p != nullptr) return p;
  return ParamNamed(params, name);
}
}  // namespace

Json RpcServer::CallMethod(const std::string& method, const Json& params,
                           int& code, std::string& message) {
  auto fail = [&](int c, const std::string& m) {
    code = c;
    message = m;
    return Json();
  };
  NodeLock lock(nodeMutex_);
  ChainState& chain = node_.Chain();

  // The wallet the RPC methods use: the node wallet when one is
  // attached (it owns the lock state), else the plain test wallet.
  // Returns nullptr + sets code/message when no wallet is loaded.
  auto activeWallet = [&]() -> Wallet* {
    if (nodeWallet_ != nullptr && nodeWallet_->HasWallet()) {
      return nodeWallet_->GetWallet();
    }
    if (wallet_ != nullptr) return wallet_;
    code = -13;
    message = "wallet not loaded";
    return nullptr;
  };
  // True when the active wallet is a locked node wallet.
  auto walletLocked = [&]() -> bool {
    return nodeWallet_ != nullptr && nodeWallet_->HasWallet() &&
           nodeWallet_->IsLocked();
  };
  // Persist the node wallet's indices after an operation that may
  // have advanced them. No-op for the plain test wallet. Returns
  // false + sets `error` on I/O failure.
  auto saveWallet = [&](std::string& error) -> bool {
    if (nodeWallet_ == nullptr || !nodeWallet_->HasWallet()) return true;
    return nodeWallet_->SaveIfDirty(error);
  };

  if (method == "help") {
    Json methods = Json::Array();
    for (const char* m : {"help",
                          "stop",
                          "getblockchaininfo",
                          "getblockcount",
                          "getbestblockhash",
                          "getblockhash",
                          "getblock",
                          "getblockheader",
                          "getchaintips",
                          "getblockstats",
                          "getchaintxstats",
                          "getdifficulty",
                          "gettxout",
                          "gettxoutsetinfo",
                          "getmempoolinfo",
                          "savemempool",
                          "importmempool",
                          "estimatesmartfee",
                          "getrawmempool",
                          "getnetworkinfo",
                          "getpeerinfo",
                          "getconnectioncount",
                          "addnode",
                          "getmininginfo",
                          "getblocktemplate",
                          "submitblock",
                          "sendrawtransaction",
                          "getrawtransaction",
                          "decoderawtransaction",
                          "getbalance",
                          "getnewaddress",
                          "listunspent",
                          "sendtoaddress",
                          "getwalletinfo",
                          "listtransactions",
                          "gettransaction",
                          "createwallet",
                          "walletpassphrase",
                          "walletpassphrasechange",
                          "walletlock"}) {
      methods.Push(std::string(m));
    }
    return methods;
  }

  if (method == "stop") {
    stopRequested_.store(true);
    return Json("Oeeoo Coin server stopping");
  }

  if (method == "getblockchaininfo") {
    Json info = Json::Object();
    info.Set("chain", "main");
    info.Set("blocks", chain.GetTipHeight());
    info.Set("headers", chain.GetTipHeight());
    info.Set("bestblockhash", HashHex(chain.GetTipHash()));
    std::string bitsError;
    const uint32_t bits = chain.GetNextBits(bitsError);
    Target tipTarget;
    BitsToTarget(bits, tipTarget);
    Target diff1;
    BitsToTarget(0x1d00ffff, diff1);
    const long double diff =
        TargetApprox(diff1) / TargetApprox(tipTarget);
    info.Set("difficulty", static_cast<double>(diff));
    info.Set("mediantime", static_cast<int64_t>(chain.GetMedianTimePast()));
    info.Set("verificationprogress", 1.0);
    info.Set("chainwork", chain.GetTipWorkHex());
    info.Set("pruned", false);
    return info;
  }

  if (method == "getblockcount") {
    return Json(static_cast<int64_t>(chain.GetTipHeight()));
  }

  if (method == "getbestblockhash") {
    return Json(HashHex(chain.GetTipHash()));
  }

  if (method == "getblockhash") {
    const Json* p = Param(params, 0, "height");
    if (p == nullptr || !p->IsNumber()) {
      return fail(-32602, "getblockhash needs a height");
    }
    Hash256 hash;
    if (!chain.GetBlockHashAtHeight(static_cast<int>(p->AsInt()), hash)) {
      return fail(-8, "Block height out of range");
    }
    return Json(HashHex(hash));
  }

  if (method == "getblock" || method == "getblockheader") {
    const Json* p = Param(params, 0, "blockhash");
    if (p == nullptr || !p->IsString()) {
      return fail(-32602, "a block hash is required");
    }
    Hash256 hash;
    if (!HexToHash(p->AsString(), hash)) {
      return fail(-8, "invalid block hash");
    }
    BlockHeader header;
    if (!chain.GetBlockHeader(hash, header)) {
      return fail(-5, "Block not found");
    }
    const int height = chain.GetHeight(hash);
    uint32_t mtp = 0;
    chain.GetMedianTimePastAtHeight(height, mtp);
    Target blockTarget, diff1;
    BitsToTarget(header.nBits, blockTarget);
    BitsToTarget(0x1d00ffff, diff1);
    const double difficulty = static_cast<double>(
        TargetApprox(diff1) / TargetApprox(blockTarget));
    char hexBuf[16];
    std::snprintf(hexBuf, sizeof(hexBuf), "%08x",
                  static_cast<unsigned>(header.nVersion));
    const std::string versionHex(hexBuf);
    std::snprintf(hexBuf, sizeof(hexBuf), "%08x",
                  static_cast<unsigned>(header.nBits));
    const std::string bitsHex(hexBuf);
    Json j = Json::Object();
    j.Set("hash", HashHex(hash));
    j.Set("confirmations",
          static_cast<int64_t>(chain.GetTipHeight() - height + 1));
    j.Set("height", static_cast<int64_t>(height));
    j.Set("version", static_cast<int64_t>(header.nVersion));
    j.Set("versionHex", versionHex);
    j.Set("merkleroot", HashHex(header.hashMerkleRoot));
    j.Set("time", static_cast<int64_t>(header.nTime));
    j.Set("mediantime", static_cast<int64_t>(mtp));
    j.Set("nonce", static_cast<int64_t>(header.nNonce));
    j.Set("bits", bitsHex);
    j.Set("difficulty", difficulty);
    j.Set("chainwork", chain.GetTipWorkHex());
    j.Set("nTx", 0);
    j.Set("previousblockhash", HashHex(header.hashPrevBlock));
    if (method == "getblockheader") {
      return j;
    }
    // getblock: verbosity 1 = txids, 2 = full transactions.
    const Json* verbosity = Param(params, 1, "verbosity");
    const int64_t verbose = verbosity != nullptr ? verbosity->AsInt(1) : 1;
    Block block;
    if (!node_.GetBlock(hash, block)) {
      return fail(-5, "Block not found (pruned or not stored)");
    }
    j.Set("nTx", static_cast<int64_t>(block.vtx.size()));
    Json txs = Json::Array();
    for (const Transaction& tx : block.vtx) {
      if (verbose >= 2) {
        txs.Push(TxToJson(tx));
      } else {
        txs.Push(HashHex(tx.GetTxId()));
      }
    }
    j.Set("tx", txs);
    return j;
  }

  if (method == "getdifficulty") {
    std::string bitsError;
    const uint32_t bits = chain.GetNextBits(bitsError);
    Target tipTarget, diff1;
    BitsToTarget(bits, tipTarget);
    BitsToTarget(0x1d00ffff, diff1);
    return Json(static_cast<double>(TargetApprox(diff1) /
                                    TargetApprox(tipTarget)));
  }

  if (method == "gettxout") {
    const Json* txidP = Param(params, 0, "txid");
    const Json* voutP = Param(params, 1, "n");
    if (txidP == nullptr || !txidP->IsString() || voutP == nullptr ||
        !voutP->IsNumber()) {
      return fail(-32602, "gettxout needs txid and output index");
    }
    Hash256 txid;
    if (!HexToHash(txidP->AsString(), txid)) {
      return fail(-8, "invalid txid");
    }
    Coin coin;
    if (!node_.Utxo().GetCoin({txid, static_cast<uint32_t>(voutP->AsInt())},
                              coin)) {
      return Json();  // null: no such unspent output
    }
    Json j = Json::Object();
    j.Set("bestblock", HashHex(chain.GetTipHash()));
    j.Set("confirmations", static_cast<int64_t>(chain.GetTipHeight() -
                                                 coin.height + 1));
    j.Set("value", AmountToOeeoo(coin.out.value));
    Json script = Json::Object();
    script.Set("hex", BytesToHexStr(coin.out.scriptPubKey));
    script.Set("type", "pubkeyhash");
    j.Set("scriptPubKey", script);
    j.Set("coinbase", coin.isCoinbase);
    return j;
  }

  if (method == "gettxoutsetinfo") {
    std::vector<std::pair<OutPoint, Coin>> coins;
    node_.Utxo().GetAllCoins(coins);
    __int128 total = 0;
    for (const auto& entry : coins) total += entry.second.out.value;
    Json j = Json::Object();
    j.Set("height", static_cast<int64_t>(chain.GetTipHeight()));
    j.Set("bestblock", HashHex(chain.GetTipHash()));
    j.Set("txouts", static_cast<int64_t>(coins.size()));
    j.Set("total_amount",
          AmountToOeeoo(static_cast<Amount>(total)));
    return j;
  }

  if (method == "getmempoolinfo") {
    Json j = Json::Object();
    j.Set("size", static_cast<int64_t>(node_.MempoolRef().Size()));
    j.Set("bytes", static_cast<int64_t>(node_.MempoolRef().Bytes()));
    j.Set("usage", static_cast<int64_t>(node_.MempoolRef().Bytes()));
    j.Set("maxmempool",
          static_cast<int64_t>(node_.MempoolRef().MaxBytes()));
    j.Set("mempoolminfee", 0.00000001);  // 1 sat/vB, minimum relay fee
    // Milestone 17: true once the startup mempool-load attempt
    // has finished — including when no mempool.dat existed. False
    // when persistence is disabled (the load was skipped) or no
    // load has been attempted yet.
    j.Set("loaded", node_.MempoolLoaded());
    return j;
  }

  if (method == "estimatesmartfee") {
    // Median fee rate (sat/vB) over the last 24 blocks of
    // confirmed transactions; 1 sat/vB when there is no data.
    Json j = Json::Object();
    j.Set("feerate", AmountToOeeoo(node_.EstimateFeeRate()));
    j.Set("blocks", static_cast<int64_t>(
                        node_.FeeEstimatorRef().BlocksRecorded()));
    return j;
  }

  if (method == "getrawmempool") {
    const Json* verboseP = Param(params, 0, "verbose");
    const bool verbose =
        verboseP != nullptr ? verboseP->AsBool(false) : false;
    std::vector<Mempool::Entry> entries = node_.MempoolRef().GetByFeeRate();
    if (!verbose) {
      Json arr = Json::Array();
      for (const Mempool::Entry& e : entries) {
        arr.Push(HashHex(e.tx.GetTxId()));
      }
      return arr;
    }
    Json obj = Json::Object();
    for (const Mempool::Entry& e : entries) {
      Json item = Json::Object();
      item.Set("fee", AmountToOeeoo(e.fee));
      item.Set("size", static_cast<int64_t>(e.size));
      item.Set("time", static_cast<int64_t>(e.addedTime));
      item.Set("height", static_cast<int64_t>(chain.GetTipHeight() + 1));
      obj.Set(HashHex(e.tx.GetTxId()), item);
    }
    return obj;
  }

  if (method == "savemempool") {
    // Explicit on-demand dump (Milestone 17): writes the mempool
    // file even when persistmempool=0 — the operator asked for a
    // dump. Returns null on success, like Bitcoin Core.
    std::string error;
    if (!node_.DumpMempool(error)) {
      return fail(-32603, error);
    }
    return Json();
  }

  if (method == "importmempool") {
    // Import a mempool file at an explicit path (Milestone 17):
    // the same load rules as the startup load (re-validate
    // against the current UTXO set, drop invalid/expired),
    // merged into the current mempool. Returns the load counts
    // (an Oeeoo choice — Core returns null).
    const Json* p = Param(params, 0, "filename");
    if (p == nullptr || !p->IsString() || p->AsString().empty()) {
      return fail(-32602, "importmempool needs a file path");
    }
    MempoolLoadStats stats;
    std::string error;
    if (!node_.ImportMempoolFile(p->AsString(), stats, error)) {
      return fail(-32603, error);
    }
    Json j = Json::Object();
    j.Set("loaded", static_cast<int64_t>(stats.loaded));
    j.Set("dropped_expired", static_cast<int64_t>(stats.droppedExpired));
    j.Set("dropped_invalid", static_cast<int64_t>(stats.droppedInvalid));
    return j;
  }

  if (method == "getnetworkinfo") {
    Json j = Json::Object();
    j.Set("version", "0.1.0");
    j.Set("subversion", ActiveUserAgent());  // M20: follows the
    // selected network (testnet advertises the testnet marker)
    j.Set("protocolversion", 70015);
    j.Set("localservices", "0000000000000001");
    j.Set("timeoffset", 0);
    j.Set("connections", static_cast<int64_t>(node_.PeerCount()));
    j.Set("relayfee", AmountToOeeoo(1));
    j.Set("warnings", "");
    return j;
  }

  if (method == "getpeerinfo") {
    Json arr = Json::Array();
    for (const Peer& peer : node_.GetPeers()) {
      Json item = Json::Object();
      item.Set("id", peer.id);
      item.Set("addr", peer.address);
      item.Set("version", static_cast<int64_t>(peer.version));
      item.Set("subver", "");
      item.Set("inbound", !peer.outbound);
      item.Set("startingheight", static_cast<int64_t>(peer.startHeight));
      item.Set("banscore", static_cast<int64_t>(peer.misbehavior));
      item.Set("synced_headers", static_cast<int64_t>(chain.GetTipHeight()));
      item.Set("synced_blocks", static_cast<int64_t>(chain.GetTipHeight()));
      arr.Push(item);
    }
    return arr;
  }

  if (method == "getconnectioncount") {
    return Json(static_cast<int64_t>(node_.PeerCount()));
  }

  // Milestone 18: `addnode` — Bitcoin-shaped: addnode <node>
  // <command>, where <node> is "ip:port" and <command> is
  // add / remove / onetry. Error codes follow Bitcoin Core's
  // rpc/protocol.h: -23 node already added, -24 node not added,
  // -29 node not connected (onetry could not connect). Returns
  // null on success, like Core.
  if (method == "addnode") {
    const Json* pNode = Param(params, 0, "node");
    const Json* pCommand = Param(params, 1, "command");
    if (pNode == nullptr || !pNode->IsString() || pCommand == nullptr ||
        !pCommand->IsString()) {
      return fail(-32602,
                  "addnode needs a node address and a command "
                  "(add/remove/onetry)");
    }
    const std::string address = pNode->AsString();
    const std::string command = pCommand->AsString();
    if (command == "add") {
      std::string error;
      if (!node_.AddManualPeer(address, error)) {
        // Already in the address book -> Core's -23; anything
        // else (malformed address, full book) is a bad parameter.
        if (node_.IsKnownPeer(address)) {
          return fail(-23, "Node already added");
        }
        return fail(-32602, error);
      }
      return Json();
    }
    if (command == "remove") {
      if (!node_.IsKnownPeer(address)) {
        return fail(-24, "Node has not been added");
      }
      node_.RemoveManualPeer(address);
      return Json();
    }
    if (command == "onetry") {
      // One immediate dial attempt, without adding the address
      // to the book. The dial loop keeps it out of the book too:
      // a onetry peer that disconnects is not re-dialed.
      // Refused when already connected: ConnectTo itself always
      // attempts (a stale entry must not block a re-dial), so the
      // duplicate-connection guard lives here, at the RPC.
      if (node_.IsConnectedTo(address)) {
        return fail(-32602, "Node is already connected");
      }
      if (!node_.ConnectTo(address)) {
        return fail(-29, "Unable to connect to node: " + address);
      }
      return Json();
    }
    return fail(-32602, "addnode: unknown command (add/remove/onetry)");
  }

  if (method == "getmininginfo") {
    Json j = Json::Object();
    j.Set("blocks", static_cast<int64_t>(chain.GetTipHeight()));
    j.Set("currentblocksize", 0);
    j.Set("currentblocktx", 0);
    std::string bitsError;
    const uint32_t bits = chain.GetNextBits(bitsError);
    Target tipTarget, diff1;
    BitsToTarget(bits, tipTarget);
    BitsToTarget(0x1d00ffff, diff1);
    j.Set("difficulty", static_cast<double>(TargetApprox(diff1) /
                                            TargetApprox(tipTarget)));
    // Network hash rate estimate over the last 120 blocks:
    // (work difference) / (time span).
    double hashps = 0.0;
    const int tipHeight = chain.GetTipHeight();
    if (tipHeight >= 1) {
      const int startHeight = std::max(0, tipHeight - 120);
      Hash256 startHash, tipHash;
      BlockHeader startHeader, tipHeader;
      Target startWork, tipWork;
      if (chain.GetBlockHashAtHeight(startHeight, startHash) &&
          chain.GetBlockHashAtHeight(tipHeight, tipHash) &&
          chain.GetBlockHeader(startHash, startHeader) &&
          chain.GetBlockHeader(tipHash, tipHeader) &&
          chain.GetChainWork(startHash, startWork) &&
          chain.GetChainWork(tipHash, tipWork)) {
        const int64_t span = static_cast<int64_t>(tipHeader.nTime) -
                             static_cast<int64_t>(startHeader.nTime);
        if (span > 0) {
          Uint256 workDiff = Uint256::FromBigEndian(tipWork);
          workDiff.Subtract(Uint256::FromBigEndian(startWork));
          // work is a 256-bit count of hashes; approximate as double.
          std::array<unsigned char, 32> be = workDiff.ToBigEndian();
          Target approx{};
          std::memcpy(approx.data(), be.data(), 32);
          hashps = static_cast<double>(TargetApprox(approx)) /
                   static_cast<double>(span);
        }
      }
    }
    j.Set("networkhashps", hashps);
    j.Set("pooledtx", static_cast<int64_t>(node_.MempoolRef().Size()));
    return j;
  }

  // ---- Milestone 16: chain tips, block stats, chain tx stats ----
  if (method == "getchaintips") {
    Json arr = Json::Array();
    for (const ChainTip& tip : chain.GetChainTips()) {
      Json item = Json::Object();
      item.Set("height", static_cast<int64_t>(tip.height));
      item.Set("hash", HashHex(tip.hash));
      item.Set("branchlen", static_cast<int64_t>(tip.branchlen));
      item.Set("status", tip.status);
      arr.Push(item);
    }
    return arr;
  }

  if (method == "getblockstats") {
    const Json* p = Param(params, 0, "blockhash");
    if (p == nullptr || !p->IsString()) {
      return fail(-32602, "getblockstats needs a block hash");
    }
    Hash256 hash;
    if (!HexToHash(p->AsString(), hash)) {
      return fail(-8, "invalid block hash");
    }
    BlockHeader header;
    if (!chain.GetBlockHeader(hash, header)) {
      return fail(-5, "Block not found");
    }
    Block block;
    if (!node_.GetBlock(hash, block)) {
      return fail(-5, "Block not found (pruned or not stored)");
    }
    const int height = chain.GetHeight(hash);
    // Fees: recomputed by replaying the block's own ancestry from
    // the genesis block through stored blocks, applying each
    // transaction's inputs/outputs to a fresh UTXO view. The node's
    // current UTXO view is the TIP's view and cannot price older
    // blocks (their inputs are already spent), so it is deliberately
    // NOT used here. Blocks were fully validated at accept time, so
    // a missing input during replay is an internal inconsistency,
    // not a client error. O(chain length) per call — acceptable for
    // a v0.1.0 node; a fee index at accept time would be the
    // optimisation if this ever gets slow. No SegWit fields: there
    // is no SegWit.
    std::vector<Hash256> ancestry;
    {
      Hash256 cur = hash;
      // Defensive bound: the ancestry cannot be longer than the
      // index itself.
      for (std::size_t guard = 0; guard <= chain.Size(); ++guard) {
        ancestry.push_back(cur);
        Hash256 prev;
        if (!chain.GetPrevHash(cur, prev)) {
          return fail(-32603, "cannot recompute fees: broken ancestry");
        }
        if (prev == Hash256{}) break;  // genesis reached
        cur = prev;
      }
      // The walk must end at the genesis block (height 0); anything
      // else means the ancestry is broken or the guard tripped.
      if (ancestry.empty() || chain.GetHeight(ancestry.back()) != 0) {
        return fail(-32603, "cannot recompute fees: broken ancestry");
      }
    }
    std::reverse(ancestry.begin(), ancestry.end());
    CoinsView view;
    Amount targetFee = 0;
    std::size_t targetNonCoinbaseBytes = 0;
    for (const Hash256& ancHash : ancestry) {
      Block ancBlock;
      if (!node_.GetBlock(ancHash, ancBlock)) {
        return fail(-5, "Block not found (pruned or not stored)");
      }
      const int ancHeight = chain.GetHeight(ancHash);
      const bool isTarget = (ancHash == hash);
      for (const Transaction& tx : ancBlock.vtx) {
        if (IsCoinbase(tx)) {
          for (std::size_t i = 0; i < tx.vout.size(); ++i) {
            Coin coin;
            coin.out = tx.vout[i];
            coin.height = ancHeight;
            coin.isCoinbase = true;
            view.AddCoin({tx.GetTxId(), static_cast<uint32_t>(i)}, coin);
          }
          continue;
        }
        __int128 inSum = 0;
        __int128 outSum = 0;
        for (const TxIn& in : tx.vin) {
          Coin coin;
          if (!view.GetCoin({in.prevTxHash, in.prevOutIndex}, coin)) {
            return fail(-32603,
                        "cannot recompute fees: input not in replayed "
                        "UTXO view");
          }
          inSum += coin.out.value;
        }
        for (const TxOut& out : tx.vout) outSum += out.value;
        const Amount fee = static_cast<Amount>(inSum - outSum);
        if (isTarget) {
          targetFee += fee;
          targetNonCoinbaseBytes += tx.Serialize().size();
        }
        for (const TxIn& in : tx.vin) {
          view.SpendCoin({in.prevTxHash, in.prevOutIndex});
        }
        for (std::size_t i = 0; i < tx.vout.size(); ++i) {
          Coin coin;
          coin.out = tx.vout[i];
          coin.height = ancHeight;
          coin.isCoinbase = false;
          view.AddCoin({tx.GetTxId(), static_cast<uint32_t>(i)}, coin);
        }
      }
    }
    // Median-Time-Past of this block: median of the last 11 block
    // times on ITS OWN ancestry (not the active chain's), so side
    // branches report their own mediantime.
    uint32_t mediantime = 0;
    {
      std::vector<uint32_t> times;
      Hash256 cur = hash;
      for (int i = 0; i < 11; ++i) {
        BlockHeader h;
        if (!chain.GetBlockHeader(cur, h)) break;
        times.push_back(h.nTime);
        Hash256 prev;
        if (!chain.GetPrevHash(cur, prev) || prev == Hash256{}) break;
        cur = prev;
      }
      std::sort(times.begin(), times.end());
      if (!times.empty()) mediantime = times[times.size() / 2];
    }
    Json j = Json::Object();
    j.Set("height", static_cast<int64_t>(height));
    j.Set("hash", HashHex(hash));
    j.Set("txs", static_cast<int64_t>(block.vtx.size()));
    j.Set("size", static_cast<int64_t>(block.Serialize().size()));
    j.Set("totalfee", AmountToOeeoo(targetFee));
    const double avgFeeRate =
        targetNonCoinbaseBytes == 0
            ? 0.0
            : static_cast<double>(targetFee) /
                  static_cast<double>(targetNonCoinbaseBytes);
    j.Set("avgfeerate", avgFeeRate);
    j.Set("time", static_cast<int64_t>(header.nTime));
    j.Set("mediantime", static_cast<int64_t>(mediantime));
    return j;
  }

  if (method == "getchaintxstats") {
    // Params: [nblocks] — default 2016 (Bitcoin Core's default).
    // The window is the last min(nblocks, tipHeight + 1) blocks.
    // `txcount` is the total tx count of the whole chain up to the
    // window's final block (Core's definition), while
    // `window_tx_count` counts only the window. Stored blocks only;
    // no UTXO walk is needed.
    const Json* nP = Param(params, 0, "nblocks");
    int64_t nblocks = 2016;
    if (nP != nullptr) {
      if (!nP->IsNumber()) {
        return fail(-32602, "getchaintxstats needs a numeric nblocks");
      }
      nblocks = nP->AsInt();
      if (nblocks < 1) {
        return fail(-8, "nblocks must be at least 1");
      }
    }
    const int tipHeight = chain.GetTipHeight();
    if (tipHeight < 0) {
      return fail(-8, "no blocks in chain");
    }
    const int64_t window =
        std::min<int64_t>(nblocks, static_cast<int64_t>(tipHeight) + 1);
    const int firstHeight =
        static_cast<int>(static_cast<int64_t>(tipHeight) - window + 1);
    int64_t totalTx = 0;
    int64_t windowTx = 0;
    int64_t firstTime = 0;
    int64_t lastTime = 0;
    for (int h = 0; h <= tipHeight; ++h) {
      Hash256 blockHash;
      Block block;
      if (!chain.GetBlockHashAtHeight(h, blockHash) ||
          !node_.GetBlock(blockHash, block)) {
        return fail(-5, "Block not found (pruned or not stored)");
      }
      totalTx += static_cast<int64_t>(block.vtx.size());
      if (h >= firstHeight) {
        windowTx += static_cast<int64_t>(block.vtx.size());
        if (h == firstHeight) firstTime = block.header.nTime;
        if (h == tipHeight) lastTime = block.header.nTime;
      }
    }
    const int64_t interval =
        std::max<int64_t>(0, lastTime - firstTime);
    Json j = Json::Object();
    j.Set("time", lastTime);
    j.Set("txcount", totalTx);
    j.Set("window_block_count", window);
    j.Set("window_final_block_height", static_cast<int64_t>(tipHeight));
    j.Set("window_tx_count", windowTx);
    j.Set("window_interval", interval);
    j.Set("txrate", interval == 0
                        ? 0.0
                        : static_cast<double>(windowTx) /
                              static_cast<double>(interval));
    return j;
  }

  if (method == "getblocktemplate") {
    // Resolve the miner script: param coinbaseaddress, else wallet.
    std::vector<unsigned char> minerScript;
    const Json* addrP = Param(params, 0, "coinbaseaddress");
    if (addrP != nullptr && addrP->IsString()) {
      Hash160 hash;
      if (!DecodeP2PKHAddress(addrP->AsString(), hash)) {
        return fail(-5, "invalid coinbase address");
      }
      minerScript = CreateP2PKHScriptPubKey(hash);
    } else {
      // No coinbaseaddress param: derive one from the wallet. A
      // locked node wallet cannot derive one: deriving would
      // advance the file's indices without a passphrase in memory
      // to persist them.
      if (walletLocked()) {
        return fail(-13, "wallet is locked");
      }
      Wallet* w = activeWallet();
      if (w == nullptr) {
        return fail(-13, "getblocktemplate needs a coinbaseaddress or a "
                         "loaded wallet");
      }
      std::string error;
      WalletAddress addr = w->NewAddress(error);
      if (addr.address.empty()) {
        return fail(-13, "cannot derive miner address: " + error);
      }
      if (!saveWallet(error)) {
        return fail(-13, "cannot persist wallet: " + error);
      }
      minerScript = addr.scriptPubKey;
    }
    const int height = chain.GetTipHeight() + 1;
    const uint32_t mtp = chain.GetMedianTimePast();
    std::string bitsError;
    const uint32_t bits = chain.GetNextBits(bitsError);
    if (!bitsError.empty()) {
      return fail(-8, "cannot compute next difficulty: " + bitsError);
    }
    const uint32_t blockTime =
        std::max(node_.Now(), mtp + 1);
    BlockTemplate templ = AssembleBlock(
        chain.GetTipHash(), height, blockTime, bits, node_.MempoolRef(),
        node_.Utxo(), mtp, minerScript);
    Json j = Json::Object();
    j.Set("version", static_cast<int64_t>(templ.block.header.nVersion));
    j.Set("previousblockhash", HashHex(templ.block.header.hashPrevBlock));
    Json txs = Json::Array();
    for (std::size_t i = 1; i < templ.block.vtx.size(); ++i) {
      const Transaction& tx = templ.block.vtx[i];
      Json item = Json::Object();
      item.Set("data", BytesToHexStr(tx.Serialize()));
      item.Set("txid", HashHex(tx.GetTxId()));
      item.Set("hash", HashHex(tx.GetTxId()));
      // `fee` is integer satoshis (Bitcoin's unit here, NOT OEEOO),
      // recorded by AssembleBlock from validation — never
      // recomputed in the RPC layer.
      const std::size_t feeIdx = i - 1;
      item.Set("fee", feeIdx < templ.txFees.size()
                          ? static_cast<int64_t>(templ.txFees[feeIdx])
                          : 0);
      item.Set("sigops", static_cast<int64_t>(CountSigOps(tx)));
      item.Set("weight", static_cast<int64_t>(tx.Serialize().size() * 4));
      txs.Push(item);
    }
    j.Set("transactions", txs);
    Json aux = Json::Object();
    aux.Set("flags", "");
    j.Set("coinbaseaux", aux);
    j.Set("coinbasevalue",
          static_cast<int64_t>(templ.block.vtx[0].vout.empty()
                                   ? 0
                                   : templ.block.vtx[0].vout[0].value));
    Target target;
    BitsToTarget(bits, target);
    j.Set("target", BytesToHexStr(std::vector<unsigned char>(target.begin(),
                                                             target.end())));
    j.Set("mintime", static_cast<int64_t>(mtp + 1));
    Json mutable_ = Json::Array();
    mutable_.Push("time");
    mutable_.Push("transactions");
    mutable_.Push("prevblock");
    j.Set("mutable", mutable_);
    j.Set("noncerange", "00000000ffffffff");
    // sigoplimit 20000 is a POLICY limit for templates only (NOT a
    // consensus limit, NOT Core's 80,000 — a weight-era SegWit
    // number): Oeeoo has no consensus sigop limit. M16 decision,
    // recorded here and in docs/ROADMAP_BITCOIN_LEVEL.md.
    j.Set("sigoplimit", 20000);
    j.Set("sizelimit", 1000000);
    j.Set("curtime", static_cast<int64_t>(blockTime));
    char bitsHex[16];
    std::snprintf(bitsHex, sizeof(bitsHex), "%08x", bits);
    j.Set("bits", std::string(bitsHex));
    j.Set("height", static_cast<int64_t>(height));
    return j;
  }

  if (method == "submitblock") {
    const Json* p = Param(params, 0, "hexdata");
    if (p == nullptr || !p->IsString()) {
      return fail(-32602, "submitblock needs a hex block");
    }
    std::vector<unsigned char> bytes;
    if (!HexToBytesStr(p->AsString(), bytes)) {
      return fail(-22, "invalid hex");
    }
    Block block;
    if (!Block::Deserialize(bytes, block)) {
      return fail(-22, "block does not deserialize");
    }
    std::string error;
    if (!node_.BroadcastBlock(block, error)) {
      return Json("rejected: " + error);
    }
    return Json();
  }

  if (method == "sendrawtransaction") {
    const Json* p = Param(params, 0, "hexstring");
    if (p == nullptr || !p->IsString()) {
      return fail(-32602, "sendrawtransaction needs a hex transaction");
    }
    std::vector<unsigned char> bytes;
    if (!HexToBytesStr(p->AsString(), bytes)) {
      return fail(-22, "invalid hex");
    }
    Transaction tx;
    if (!Transaction::Deserialize(bytes, tx)) {
      return fail(-22, "transaction does not deserialize");
    }
    std::string error;
    if (!node_.BroadcastTx(tx, error)) {
      return fail(-26, "transaction rejected: " + error);
    }
    return Json(HashHex(tx.GetTxId()));
  }

  if (method == "decoderawtransaction" || method == "getrawtransaction") {
    const Json* p = Param(params, 0, "hexstring");
    Transaction tx;
    if (method == "decoderawtransaction") {
      if (p == nullptr || !p->IsString()) {
        return fail(-32602, "decoderawtransaction needs a hex transaction");
      }
      std::vector<unsigned char> bytes;
      if (!HexToBytesStr(p->AsString(), bytes) ||
          !Transaction::Deserialize(bytes, tx)) {
        return fail(-22, "transaction does not deserialize");
      }
      return TxToJson(tx);
    }
    // getrawtransaction: [txid, verbose=false]
    if (p == nullptr || !p->IsString()) {
      return fail(-32602, "getrawtransaction needs a txid");
    }
    Hash256 txid;
    if (!HexToHash(p->AsString(), txid)) {
      return fail(-8, "invalid txid");
    }
    // Search the mempool first, then the chain's blocks.
    for (const Mempool::Entry& e : node_.MempoolRef().GetByFeeRate()) {
      if (e.tx.GetTxId() == txid) {
        tx = e.tx;
        const Json* verboseP = Param(params, 1, "verbose");
        if (verboseP != nullptr && verboseP->AsBool(false)) {
          return TxToJson(tx);
        }
        return Json(BytesToHexStr(tx.Serialize()));
      }
    }
    for (int h = 0; h <= chain.GetTipHeight(); ++h) {
      Hash256 blockHash;
      Block block;
      if (!chain.GetBlockHashAtHeight(h, blockHash) ||
          !node_.GetBlock(blockHash, block)) {
        continue;
      }
      for (const Transaction& btx : block.vtx) {
        if (btx.GetTxId() == txid) {
          const Json* verboseP = Param(params, 1, "verbose");
          if (verboseP != nullptr && verboseP->AsBool(false)) {
            return TxToJson(btx);
          }
          return Json(BytesToHexStr(btx.Serialize()));
        }
      }
    }
    return fail(-5, "No such mempool or blockchain transaction");
  }

  // ---- wallet methods ----
  // getbalance / listunspent / getwalletinfo work while the node
  // wallet is locked (watch-only view). getnewaddress /
  // sendtoaddress refuse while locked: deriving an address or
  // spending advances the file's indices, and they can only be
  // persisted while the passphrase is in memory. After a scan the
  // lookahead indices may have advanced, so read methods persist
  // best-effort (a no-op when locked, or when nothing moved).

  if (method == "getbalance") {
    Wallet* w = activeWallet();
    if (w == nullptr) return Json();
    w->ScanUtxo(node_.Utxo(), chain.GetTipHeight() + 1);
    std::string saveError;
    saveWallet(saveError);  // best-effort; a read must not fail on I/O
    return Json(AmountToOeeoo(w->GetBalance()));
  }

  if (method == "getnewaddress") {
    if (walletLocked()) {
      return fail(-13, "wallet is locked");
    }
    Wallet* w = activeWallet();
    if (w == nullptr) return Json();
    std::string error;
    WalletAddress addr = w->NewAddress(error);
    if (addr.address.empty()) {
      return fail(-13, "cannot derive address: " + error);
    }
    if (!saveWallet(error)) {
      return fail(-13, "cannot persist wallet: " + error);
    }
    return Json(addr.address);
  }

  if (method == "listunspent") {
    Wallet* w = activeWallet();
    if (w == nullptr) return Json();
    w->ScanUtxo(node_.Utxo(), chain.GetTipHeight() + 1);
    std::string saveError;
    saveWallet(saveError);  // best-effort
    const bool spendable = !walletLocked();
    Json arr = Json::Array();
    for (const WalletUtxo& u : w->ListUnspent()) {
      Json item = Json::Object();
      item.Set("txid", HashHex(u.outpoint.txid));
      item.Set("vout", static_cast<int64_t>(u.outpoint.index));
      item.Set("address", u.address);
      item.Set("amount", AmountToOeeoo(u.value));
      item.Set("confirmations", static_cast<int64_t>(1));
      item.Set("spendable", spendable);
      arr.Push(item);
    }
    return arr;
  }

  if (method == "sendtoaddress") {
    if (walletLocked()) {
      return fail(-13, "wallet is locked");
    }
    Wallet* w = activeWallet();
    if (w == nullptr) return Json();
    const Json* addrP = Param(params, 0, "address");
    const Json* amountP = Param(params, 1, "amount");
    if (addrP == nullptr || !addrP->IsString() || amountP == nullptr) {
      return fail(-32602, "sendtoaddress needs address and amount");
    }
    Amount amount = 0;
    std::string amountError;
    if (!ParseAmount(*amountP, amount, amountError)) {
      return fail(-8, amountError);
    }
    Transaction tx;
    Amount fee = 0;
    std::string error;
    if (!w->CreateTransaction(addrP->AsString(), amount, node_.Utxo(),
                              chain.GetTipHeight() + 1,
                              chain.GetMedianTimePast(), tx, fee, error, 1)) {
      return fail(-13, "cannot build transaction: " + error);
    }
    // Persist the (possibly advanced) change index BEFORE the
    // transaction leaves: if the save fails, nothing was broadcast
    // and the operator can retry without risking address reuse.
    if (!saveWallet(error)) {
      return fail(-13, "cannot persist wallet: " + error);
    }
    if (!node_.BroadcastTx(tx, error)) {
      return fail(-26, "transaction rejected: " + error);
    }
    // Record the send in the wallet tx journal (Milestone 16):
    // amount is negative excluding the fee (Bitcoin convention),
    // unconfirmed (blockheight -1), time = first seen (now).
    // The journal persists best-effort here: the transaction is
    // already broadcast, so a journal I/O failure must not turn a
    // successful send into an error.
    WalletTxRecord sendRec;
    sendRec.txid = HashHex(tx.GetTxId());
    sendRec.category = "send";
    sendRec.amount = -amount;
    sendRec.fee = fee;
    sendRec.time = static_cast<int64_t>(node_.Now());
    sendRec.blockheight = -1;
    sendRec.address = addrP->AsString();
    w->RecordTx(sendRec);
    std::string journalError;
    saveWallet(journalError);  // best-effort (see above)
    return Json(HashHex(tx.GetTxId()));
  }

  if (method == "walletpassphrasechange") {
    const Json* oldP = Param(params, 0, "oldpassphrase");
    const Json* newP = Param(params, 1, "newpassphrase");
    if (oldP == nullptr || !oldP->IsString() || newP == nullptr ||
        !newP->IsString()) {
      return fail(-32602,
                  "walletpassphrasechange needs an old and a new "
                  "passphrase");
    }
    if (newP->AsString().empty()) {
      return fail(-8, "new passphrase must not be empty");
    }
    if (nodeWallet_ == nullptr || !nodeWallet_->HasWallet()) {
      return fail(-13, "wallet not loaded");
    }
    if (nodeWallet_->IsLocked()) {
      // Bitcoin requires the wallet to be unlocked to change the
      // passphrase; so does Oeeoo.
      return fail(-13, "wallet is locked");
    }
    std::string error;
    if (!nodeWallet_->ChangePassphrase(oldP->AsString(),
                                       newP->AsString(), error)) {
      // A wrong old passphrase is -14, the same code as
      // walletpassphrase; the message never distinguishes a wrong
      // passphrase from a corrupt file.
      if (error == "incorrect passphrase") return fail(-14, error);
      return fail(-13, error);
    }
    return Json("passphrase changed");
  }

  // ---- wallet transaction history (Milestone 16) ----
  // listtransactions / gettransaction read the sidecar journal
  // (txjournal.h) through the wallet's in-memory tx records. The
  // journal holds no secrets, so both methods work while the wallet
  // is locked (watch-only view): receives AND sends are history,
  // not spending. Confirmations are computed from the tip height
  // at call time, never stored. blockindex is always -1: the
  // journal does not track a transaction's index within its block.
  // A confirmed record's blockhash is looked up by height on the
  // CURRENT main chain, so after a reorg the height may point at a
  // different block — the journal does not track reorgs (a
  // documented limitation; Oeeoo has no abandon-transaction
  // handling in v0.1.0).

  if (method == "listtransactions" || method == "gettransaction") {
    Wallet* w = activeWallet();
    if (w == nullptr) return Json();
    // A rescan first: it (re)discovers receives, so a receive
    // confirmed since the last call is listed immediately.
    w->ScanUtxo(node_.Utxo(), chain.GetTipHeight() + 1);
    std::string saveError;
    saveWallet(saveError);  // best-effort
    auto txInfo = [&](const WalletTxRecord& rec, int64_t& confirmations,
                      std::string& blockhash, int64_t& blocktime) {
      confirmations = 0;
      blockhash = "";
      blocktime = 0;
      if (rec.blockheight < 0) return;
      const int64_t conf = static_cast<int64_t>(chain.GetTipHeight()) -
                           rec.blockheight + 1;
      if (conf <= 0) return;
      Hash256 h;
      if (!chain.GetBlockHashAtHeight(rec.blockheight, h)) return;
      BlockHeader hdr;
      if (!chain.GetBlockHeader(h, hdr)) return;
      confirmations = conf;
      blockhash = HashHex(h);
      blocktime = static_cast<int64_t>(hdr.nTime);
    };
    auto recordToJson = [&](const WalletTxRecord& rec) {
      int64_t confirmations = 0;
      std::string blockhash;
      int64_t blocktime = 0;
      txInfo(rec, confirmations, blockhash, blocktime);
      Json item = Json::Object();
      item.Set("account", "");
      item.Set("address", rec.address);
      item.Set("category", rec.category);
      item.Set("amount", AmountToOeeoo(rec.amount));
      if (rec.category == "send") {
        item.Set("fee", AmountToOeeoo(rec.fee));
      }
      item.Set("confirmations", confirmations);
      item.Set("blockhash", blockhash);
      item.Set("blockindex", static_cast<int64_t>(-1));
      item.Set("blocktime", blocktime);
      item.Set("txid", rec.txid);
      item.Set("time", rec.time);
      item.Set("timereceived", rec.time);
      return item;
    };

    if (method == "listtransactions") {
      const Json* accountP = Param(params, 0, "account");
      if (accountP != nullptr) {
        if (!accountP->IsString()) {
          return fail(-8, "listtransactions account must be a string");
        }
        const std::string account = accountP->AsString();
        if (account != "*" && !account.empty()) {
          return fail(-8, "invalid account (only \"*\" is supported)");
        }
      }
      const Json* countP = Param(params, 1, "count");
      int64_t count = 10;
      if (countP != nullptr) {
        if (!countP->IsNumber()) {
          return fail(-8, "listtransactions count must be a number");
        }
        count = countP->AsInt();
        if (count < 0) return fail(-8, "count must not be negative");
      }
      const Json* skipP = Param(params, 2, "skip");
      int64_t skip = 0;
      if (skipP != nullptr) {
        if (!skipP->IsNumber()) {
          return fail(-8, "listtransactions skip must be a number");
        }
        skip = skipP->AsInt();
        if (skip < 0) return fail(-8, "skip must not be negative");
      }
      // include_watchonly (param 3) is accepted and ignored: there
      // is no watch-only distinction in the journal — every record
      // belongs to this wallet.
      std::vector<WalletTxRecord> records = w->ListTxRecords();
      // Newest first (stable: equal times keep journal order).
      std::stable_sort(records.begin(), records.end(),
                       [](const WalletTxRecord& a,
                          const WalletTxRecord& b) {
                         return a.time > b.time;
                       });
      Json arr = Json::Array();
      int64_t shown = 0;
      for (std::size_t i = 0; i < records.size(); ++i) {
        if (static_cast<int64_t>(i) < skip) continue;
        if (shown >= count) break;
        arr.Push(recordToJson(records[i]));
        ++shown;
      }
      return arr;
    }

    // gettransaction
    const Json* txidP = Param(params, 0, "txid");
    if (txidP == nullptr || !txidP->IsString()) {
      return fail(-32602, "gettransaction needs a txid");
    }
    Hash256 txid;
    if (!HexToHash(txidP->AsString(), txid)) {
      return fail(-5, "Invalid or non-wallet transaction id");
    }
    const std::string want = HashHex(txid);  // normalised lowercase
    // Keep the records in a local vector: ListTxRecords() returns
    // by value, and a pointer into that temporary would dangle
    // once the loop ends.
    const std::vector<WalletTxRecord> records = w->ListTxRecords();
    const WalletTxRecord* found = nullptr;
    for (const WalletTxRecord& rec : records) {
      if (rec.txid == want) {
        found = &rec;
        break;
      }
    }
    if (found == nullptr) {
      return fail(-5, "Invalid or non-wallet transaction id");
    }
    int64_t confirmations = 0;
    std::string blockhash;
    int64_t blocktime = 0;
    txInfo(*found, confirmations, blockhash, blocktime);
    Json j = Json::Object();
    j.Set("amount", AmountToOeeoo(found->amount));
    if (found->category == "send") {
      j.Set("fee", AmountToOeeoo(found->fee));
    }
    j.Set("confirmations", confirmations);
    j.Set("blockhash", blockhash);
    j.Set("blockindex", static_cast<int64_t>(-1));
    j.Set("blocktime", blocktime);
    j.Set("txid", found->txid);
    j.Set("time", found->time);
    j.Set("timereceived", found->time);
    Json details = Json::Array();
    details.Push(recordToJson(*found));
    j.Set("details", details);
    return j;
  }

  if (method == "getwalletinfo") {
    Wallet* w = activeWallet();
    if (w == nullptr) return Json();
    w->ScanUtxo(node_.Utxo(), chain.GetTipHeight() + 1);
    std::string saveError;
    saveWallet(saveError);  // best-effort
    Json j = Json::Object();
    j.Set("walletname", "default");
    j.Set("walletversion", 1);
    j.Set("balance", AmountToOeeoo(w->GetBalance()));
    j.Set("immature_balance", AmountToOeeoo(w->GetImmatureBalance()));
    j.Set("txcount", static_cast<int64_t>(w->ListUnspent().size()));
    j.Set("keypoolsize", kWalletGapLimit);
    uint64_t unlockedUntil = 0;
    if (nodeWallet_ != nullptr && nodeWallet_->HasWallet()) {
      unlockedUntil = nodeWallet_->UnlockedUntil();
    }
    j.Set("unlocked_until", static_cast<int64_t>(unlockedUntil));
    j.Set("paytxfee", AmountToOeeoo(0));
    return j;
  }

  // ---- node-wallet management (Milestone 14) ----
  // createwallet needs a --wallet path registered with the node:
  // without one there is nowhere to write the new file.

  if (method == "createwallet") {
    const Json* passP = Param(params, 0, "passphrase");
    if (passP == nullptr || !passP->IsString() ||
        passP->AsString().empty()) {
      return fail(-8, "createwallet needs a non-empty passphrase");
    }
    if (nodeWallet_ == nullptr) {
      return fail(-13,
                  "no wallet file configured (start the node with "
                  "--wallet <path>)");
    }
    if (nodeWallet_->HasWallet()) {
      return fail(-13, "wallet already loaded");
    }
    std::string mnemonic;
    std::string error;
    if (!nodeWallet_->CreateFile(nodeWallet_->Path(),
                                 passP->AsString(), mnemonic, error)) {
      return fail(-13, "cannot create wallet: " + error);
    }
    Json j = Json::Object();
    j.Set("mnemonic", mnemonic);
    j.Set("warning",
          "Write down this recovery phrase now. It is shown only "
          "once — the node never stores it.");
    return j;
  }

  if (method == "walletpassphrase") {
    const Json* passP = Param(params, 0, "passphrase");
    const Json* timeoutP = Param(params, 1, "timeout");
    if (passP == nullptr || !passP->IsString() ||
        passP->AsString().empty() || timeoutP == nullptr ||
        !timeoutP->IsInt()) {
      return fail(-32602,
                  "walletpassphrase needs a passphrase and a timeout "
                  "in seconds");
    }
    const int64_t timeout = timeoutP->AsInt();
    if (timeout < 0 || timeout > 4294967295LL) {
      return fail(-32602, "walletpassphrase timeout out of range");
    }
    if (nodeWallet_ == nullptr || !nodeWallet_->HasWallet()) {
      return fail(-13, "wallet not loaded");
    }
    std::string error;
    if (!nodeWallet_->Unlock(passP->AsString(),
                             static_cast<uint32_t>(timeout), error)) {
      // -14 is Bitcoin's "wallet passphrase incorrect" code; it was
      // unused in this codebase.
      return fail(-14, error);
    }
    return Json("wallet unlocked");
  }

  if (method == "walletlock") {
    if (nodeWallet_ == nullptr || !nodeWallet_->HasWallet()) {
      return fail(-13, "wallet not loaded");
    }
    nodeWallet_->Lock();
    return Json("wallet locked");
  }

  return fail(-32601, "Method not found");
}

}  // namespace oeeoo
