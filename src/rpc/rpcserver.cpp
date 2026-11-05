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
    // One thread per connection: a slow client must not block all
    // RPC. Cap the count so a connection flood cannot grow threads
    // without bound; excess connections are refused immediately.
    if (activeConnections_.load() >= kMaxConnections) {
      ::close(fd);
      continue;
    }
    activeConnections_.fetch_add(1);
    std::thread([this, fd] {
      HandleConnection(fd);
      ::close(fd);
      activeConnections_.fetch_sub(1);
    }).detach();
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
          contentLength = static_cast<std::size_t>(std::stoul(value));
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
                          "getdifficulty",
                          "gettxout",
                          "gettxoutsetinfo",
                          "getmempoolinfo",
                          "estimatesmartfee",
                          "getrawmempool",
                          "getnetworkinfo",
                          "getpeerinfo",
                          "getconnectioncount",
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
                          "createwallet",
                          "walletpassphrase",
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

  if (method == "getnetworkinfo") {
    Json j = Json::Object();
    j.Set("version", "0.1.0");
    j.Set("subversion", "/OeeooCoin:0.1.0/");
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
      item.Set("fee", 0);
      item.Set("sigops", 0);
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
    w->ScanUtxo(node_.Utxo());
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
    w->ScanUtxo(node_.Utxo());
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
    return Json(HashHex(tx.GetTxId()));
  }

  if (method == "getwalletinfo") {
    Wallet* w = activeWallet();
    if (w == nullptr) return Json();
    w->ScanUtxo(node_.Utxo());
    std::string saveError;
    saveWallet(saveError);  // best-effort
    Json j = Json::Object();
    j.Set("walletname", "default");
    j.Set("walletversion", 1);
    j.Set("balance", AmountToOeeoo(w->GetBalance()));
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
