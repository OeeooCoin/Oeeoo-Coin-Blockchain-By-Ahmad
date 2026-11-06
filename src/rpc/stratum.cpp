// SPDX-License-Identifier: MIT
// Oeeoo Coin — Stratum v1 mining server implementation.
#include "rpc/stratum.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/rand.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "blockchain/merkle.h"
#include "consensus/difficulty.h"
#include "crypto/address.h"
#include "crypto/sha256.h"
#include "mining/miner.h"
#include "rpc/rpcutil.h"
#include "script/script.h"

namespace oeeoo {

namespace {
std::string HashHex(const Hash256& h) {
  return BytesToHexStr(std::vector<unsigned char>(h.begin(), h.end()));
}

bool SendAllFd(int fd, const std::string& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
    if (n <= 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

// target = (2^256 - 1) / difficulty, computed exactly with the
// diff-1 target scaled by 1e9 (so fractional difficulties work).
bool DifficultyToTarget(double difficulty, Target& out) {
  if (!(difficulty > 0) || !std::isfinite(difficulty)) return false;
  Target diff1;
  if (!BitsToTarget(0x1d00ffff, diff1)) return false;
  Uint256 num = Uint256::FromBigEndian(diff1);
  num.MultiplyUint32(1000000000);
  const uint64_t denInt =
      static_cast<uint64_t>(std::llround(difficulty * 1000000000.0));
  if (denInt == 0) return false;
  Uint256 den;
  den.d[0] = static_cast<uint32_t>(denInt);
  den.d[1] = static_cast<uint32_t>(denInt >> 32);
  Uint256 q = Uint256::Divide(num, den);
  out = q.ToBigEndian();
  return true;
}

std::string Hex32(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned>(v));
  return buf;
}

// Serializes node access when a shared mutex was provided
// (nullptr = the caller guarantees single-threaded node use).
struct NodeLock {
  explicit NodeLock(std::mutex* m) : mutex_(m) {
    if (mutex_ != nullptr) mutex_->lock();
  }
  ~NodeLock() {
    if (mutex_ != nullptr) mutex_->unlock();
  }
  std::mutex* mutex_;
};
}  // namespace

StratumServer::StratumServer(Node& node, uint16_t port,
                             std::mutex* nodeMutex)
    : node_(node), port_(port), nodeMutex_(nodeMutex) {}

StratumServer::~StratumServer() { Stop(); }

std::string StratumServer::CurrentJobId() const {
  std::lock_guard<std::mutex> lock(jobMutex_);
  return hasJob_ ? currentJob_.id : "";
}

bool StratumServer::Start(std::string& error) {
  if (running_.load()) {
    error = "Stratum server already running";
    return false;
  }
  if (minerAddress_.empty()) {
    error = "miner address not set";
    return false;
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
    error = "cannot bind Stratum port";
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
    error = "cannot listen on Stratum port";
    ::close(listenFd_);
    listenFd_ = -1;
    return false;
  }
  running_.store(true);
  acceptThread_ = std::thread(&StratumServer::AcceptLoop, this);
  watchThread_ = std::thread(&StratumServer::WatchLoop, this);
  return true;
}

void StratumServer::Stop() {
  if (!running_.load()) return;
  running_.store(false);
  if (listenFd_ >= 0) {
    ::shutdown(listenFd_, SHUT_RDWR);
    ::close(listenFd_);
    listenFd_ = -1;
  }
  {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    for (const auto& session : sessions_) {
      if (session->fd >= 0) ::shutdown(session->fd, SHUT_RDWR);
    }
  }
  if (acceptThread_.joinable()) acceptThread_.join();
  if (watchThread_.joinable()) watchThread_.join();
  std::vector<std::thread> threads;
  {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    threads.swap(clientThreads_);
  }
  for (auto& t : threads) {
    if (t.joinable()) t.join();
  }
}

void StratumServer::AcceptLoop() {
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
    auto session = std::make_shared<Session>();
    session->fd = fd;
    session->difficulty = initialDifficulty_;
    session->windowStart = node_.Now();
    unsigned char randomBytes[4];
    RAND_bytes(randomBytes, sizeof(randomBytes));
    session->extranonce1 = BytesToHexStr(
        std::vector<unsigned char>(randomBytes, randomBytes + 4));
    {
      std::lock_guard<std::mutex> lock(sessionsMutex_);
      sessions_.push_back(session);
      clientThreads_.emplace_back(&StratumServer::ClientLoop, this, fd);
    }
  }
}

void StratumServer::RemoveSession(int fd) {
  std::lock_guard<std::mutex> lock(sessionsMutex_);
  for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
    if ((*it)->fd == fd) {
      sessions_.erase(it);
      break;
    }
  }
}

void StratumServer::ClientLoop(int fd) {
  std::shared_ptr<Session> session;
  {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    for (const auto& s : sessions_) {
      if (s->fd == fd) {
        session = s;
        break;
      }
    }
  }
  if (session == nullptr) {
    ::close(fd);
    return;
  }
  char buffer[4096];
  while (running_.load()) {
    const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
    if (n <= 0) break;
    session->buffer.append(buffer, static_cast<std::size_t>(n));
    std::size_t newline = 0;
    while ((newline = session->buffer.find('\n')) != std::string::npos) {
      std::string line = session->buffer.substr(0, newline);
      session->buffer.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty() || line.size() > 65536) continue;
      Json request;
      std::string parseError;
      if (!Json::Parse(line, request, parseError)) continue;
      Json response = HandleMessage(*session, request);
      if (!response.IsNull()) {
        if (!SendAllFd(fd, response.Dump() + "\n")) {
          ::close(fd);
          RemoveSession(fd);
          return;
        }
      }
    }
    // Milestone 18 (fuzzing find M18B-R3): session->buffer only
    // shrinks when a '\n' arrives, so a client sending megabytes
    // with no newline grew it without bound (20 MB held for a
    // 20 MB no-newline stream). The cap must sit ABOVE the
    // longest skipped line (65536 bytes): a long but properly
    // terminated line is skipped on arrival, so dropping the
    // connection at 65536 would kill legitimate long lines
    // mid-stream. At 1 MB a tail can never become a valid line
    // either — it is 16x the line cap — and per-session memory
    // stays bounded.
    if (session->buffer.size() > 1024 * 1024) {
      ::close(fd);
      RemoveSession(fd);
      return;
    }
  }
  ::close(fd);
  RemoveSession(fd);
}

void StratumServer::SendToSession(Session& session, const Json& message) {
  if (session.fd < 0) return;
  SendAllFd(session.fd, message.Dump() + "\n");
}

void StratumServer::BroadcastJob(const Job& job, bool cleanJobs) {
  Json notify = Json::Object();
  notify.Set("id", Json());
  notify.Set("method", "mining.notify");
  Json params = Json::Array();
  params.Push(job.id);
  params.Push(HashHex(job.prevHash));
  params.Push(job.coinbase1);
  params.Push(job.coinbase2);
  Json branches = Json::Array();
  for (const std::string& branch : job.merkleBranches) {
    branches.Push(branch);
  }
  params.Push(branches);
  params.Push(Hex32(job.version));
  params.Push(Hex32(job.nbits));
  params.Push(Hex32(job.ntime));
  params.Push(cleanJobs);
  notify.Set("params", params);
  std::vector<std::shared_ptr<Session>> sessions;
  {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    sessions = sessions_;
  }
  for (const auto& session : sessions) {
    if (session->subscribed) SendToSession(*session, notify);
  }
}

bool StratumServer::BuildJob(Job& job, std::string& error) {
  ChainState& chain = node_.Chain();
  const int height = chain.GetTipHeight() + 1;
  const uint32_t mtp = chain.GetMedianTimePast();
  std::string bitsError;
  const uint32_t bits = chain.GetNextBits(bitsError);
  if (!bitsError.empty()) {
    error = bitsError;
    return false;
  }
  const uint32_t blockTime = std::max(node_.Now(), mtp + 1);
  Hash160 minerHash;
  if (!DecodeP2PKHAddress(minerAddress_, minerHash)) {
    error = "bad miner address";
    return false;
  }
  std::vector<unsigned char> minerScript =
      CreateP2PKHScriptPubKey(minerHash);
  BlockTemplate templ =
      AssembleBlock(chain.GetTipHash(), height, blockTime, bits,
                    node_.MempoolRef(), node_.Utxo(), mtp, minerScript);
  // Rebuild the coinbase with the Stratum scriptSig:
  // height push + "/OeeooStratum" + extranonce1 (4B) + extranonce2 (4B).
  std::vector<unsigned char> scriptSig =
      PushData(EncodeScriptNum(height));
  const std::string marker = "/OeeooStratum";
  scriptSig.insert(scriptSig.end(), marker.begin(), marker.end());
  const std::size_t extranonceOffset = scriptSig.size();
  scriptSig.insert(scriptSig.end(), 8, 0);  // en1 + en2 placeholders
  Transaction coinbase;
  coinbase.version = 1;
  TxIn in;
  in.prevTxHash.fill(0);
  in.prevOutIndex = 0xffffffff;
  in.scriptSig = scriptSig;
  in.sequence = 0xffffffff;
  coinbase.vin.push_back(in);
  coinbase.vout.push_back(templ.block.vtx[0].vout[0]);
  coinbase.lockTime = 0;
  std::vector<unsigned char> serialized = coinbase.Serialize();
  // version(4) + vin count(1) + prev hash(32) + prev index(4) +
  // scriptSig length(1, always 1 byte: the scriptSig is < 253 bytes).
  const std::size_t scriptSigStart = 42;
  const std::size_t en1Start = scriptSigStart + extranonceOffset;
  job.coinbase1 = BytesToHexStr(std::vector<unsigned char>(
      serialized.begin(), serialized.begin() + en1Start));
  job.coinbase2 = BytesToHexStr(std::vector<unsigned char>(
      serialized.begin() + en1Start + 8, serialized.end()));
  std::vector<Hash256> txids;
  txids.push_back(coinbase.GetTxId());
  for (std::size_t i = 1; i < templ.block.vtx.size(); ++i) {
    txids.push_back(templ.block.vtx[i].GetTxId());
  }
  job.merkleBranches.clear();
  for (const Hash256& branch : MerkleBranch(txids, 0)) {
    job.merkleBranches.push_back(HashHex(branch));
  }
  job.prevHash = chain.GetTipHash();
  job.version = 1;
  job.nbits = bits;
  job.ntime = blockTime;
  job.height = height;
  job.coinbaseValue = templ.block.vtx[0].vout[0].value;
  job.otherTxs.assign(templ.block.vtx.begin() + 1, templ.block.vtx.end());
  {
    std::lock_guard<std::mutex> lock(jobMutex_);
    job.id = Hex32(static_cast<uint32_t>(++jobCounter_));
  }
  return true;
}

void StratumServer::WatchLoop() {
  int lastTipHeight = -2;
  uint32_t lastJobTime = 0;
  while (running_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (!running_.load()) break;
    if (minerAddress_.empty()) continue;
    const uint32_t now = node_.Now();
    bool rebuild = false;
    {
      NodeLock nodeLock(nodeMutex_);
      const int tipHeight = node_.Chain().GetTipHeight();
      if (tipHeight != lastTipHeight) rebuild = true;
      if (now - lastJobTime >= 60) rebuild = true;
      if (rebuild) {
        Job job;
        std::string error;
        if (BuildJob(job, error)) {
          {
            std::lock_guard<std::mutex> jobLock(jobMutex_);
            previousJob_ = currentJob_;
            currentJob_ = job;
            hasJob_ = true;
          }
          lastTipHeight = tipHeight;
          lastJobTime = now;
          BroadcastJob(job, true);
        }
      }
    }
  }
}

Json StratumServer::HandleMessage(Session& session, const Json& request) {
  Json response = Json::Object();
  const Json* id = request.Get("id");
  response.Set("id", id != nullptr ? *id : Json());
  const Json* methodJson = request.Get("method");
  if (methodJson == nullptr || !methodJson->IsString()) {
    Json err = Json::Object();
    err.Set("code", -32600);
    err.Set("message", "Invalid Request");
    response.Set("error", err);
    response.Set("result", Json());
    return response;
  }
  const std::string method = methodJson->AsString();
  const Json* paramsJson = request.Get("params");
  Json emptyParams = Json::Array();
  const Json& params = paramsJson != nullptr ? *paramsJson : emptyParams;

  auto fail = [&](int code, const std::string& message) {
    Json err = Json::Object();
    err.Set("code", code);
    err.Set("message", message);
    response.Set("error", err);
    response.Set("result", Json());
    return response;
  };
  auto ok = [&](const Json& result) {
    response.Set("error", Json());
    response.Set("result", result);
    return response;
  };

  if (method == "mining.subscribe") {
    session.subscribed = true;
    unsigned char randomBytes[4];
    RAND_bytes(randomBytes, sizeof(randomBytes));
    const std::string subId = BytesToHexStr(std::vector<unsigned char>(
        randomBytes, randomBytes + 4));
    Json result = Json::Array();
    Json methods = Json::Array();
    Json notifyMethod = Json::Array();
    notifyMethod.Push("mining.notify");
    notifyMethod.Push(subId);
    Json diffMethod = Json::Array();
    diffMethod.Push("mining.set_difficulty");
    diffMethod.Push(subId);
    methods.Push(notifyMethod);
    methods.Push(diffMethod);
    result.Push(methods);
    result.Push(session.extranonce1);
    result.Push(4);  // extranonce2 size
    // Tell the client its difficulty and current job.
    Json setDiff = Json::Object();
    setDiff.Set("id", Json());
    setDiff.Set("method", "mining.set_difficulty");
    Json diffParams = Json::Array();
    diffParams.Push(session.difficulty);
    setDiff.Set("params", diffParams);
    SendToSession(session, setDiff);
    {
      std::lock_guard<std::mutex> lock(jobMutex_);
      if (hasJob_) BroadcastJob(currentJob_, true);
    }
    return ok(result);
  }

  if (method == "mining.authorize") {
    const Json* user = params.IsArray() && !params.AsArray().empty()
                           ? &params.AsArray()[0]
                           : nullptr;
    session.authorized = true;
    if (user != nullptr && user->IsString()) session.worker = user->AsString();
    return ok(Json(true));
  }

  if (method == "mining.extranonce.subscribe") {
    return fail(-32601, "extranonce subscription is not supported");
  }

  if (method == "mining.submit") {
    if (!params.IsArray() || params.AsArray().size() < 5) {
      return fail(-32602, "mining.submit needs 5 params");
    }
    const auto& arr = params.AsArray();
    const std::string jobId =
        arr[1].IsString() ? arr[1].AsString() : "";
    const std::string extranonce2 =
        arr[2].IsString() ? arr[2].AsString() : "";
    const std::string ntimeHex =
        arr[3].IsString() ? arr[3].AsString() : "";
    const std::string nonceHex =
        arr[4].IsString() ? arr[4].AsString() : "";
    uint32_t ntime = 0, nonce = 0;
    try {
      ntime = static_cast<uint32_t>(std::stoul(ntimeHex, nullptr, 16));
      nonce = static_cast<uint32_t>(std::stoul(nonceHex, nullptr, 16));
    } catch (...) {
      return fail(-32602, "bad ntime/nonce hex");
    }
    Job job;
    bool found = false;
    {
      std::lock_guard<std::mutex> lock(jobMutex_);
      if (hasJob_ && currentJob_.id == jobId) {
        job = currentJob_;
        found = true;
      } else if (!previousJob_.id.empty() && previousJob_.id == jobId) {
        job = previousJob_;
        found = true;
      }
    }
    if (!found) {
      rejectedShares_.fetch_add(1);
      return fail(-1, "unknown job");
    }
    std::string error;
    if (!ValidateShare(session, job, extranonce2, ntime, nonce, error)) {
      rejectedShares_.fetch_add(1);
      session.sharesRejected++;
      return fail(-1, error);
    }
    acceptedShares_.fetch_add(1);
    session.sharesAccepted++;
    session.sharesInWindow++;
    // Basic vardiff: adjust every 60 seconds.
    const uint32_t now = node_.Now();
    if (now - session.windowStart >= 60) {
      if (session.sharesInWindow > 12) {
        session.difficulty *= 2.0;
      } else if (session.sharesInWindow < 2 && session.difficulty > 0.01) {
        session.difficulty /= 2.0;
      }
      session.sharesInWindow = 0;
      session.windowStart = now;
      Json setDiff = Json::Object();
      setDiff.Set("id", Json());
      setDiff.Set("method", "mining.set_difficulty");
      Json diffParams = Json::Array();
      diffParams.Push(session.difficulty);
      setDiff.Set("params", diffParams);
      SendToSession(session, setDiff);
    }
    return ok(Json(true));
  }

  return fail(-32601, "Method not found");
}

bool StratumServer::ValidateShare(Session& session, const Job& job,
                                  const std::string& extranonce2Hex,
                                  uint32_t ntime, uint32_t nonce,
                                  std::string& error) {
  if (extranonce2Hex.size() != 8) {
    error = "extranonce2 must be 4 bytes (8 hex chars)";
    return false;
  }
  if (ntime < job.ntime) {
    error = "ntime is before the job time";
    return false;
  }
  if (ntime > job.ntime + 7200) {
    error = "ntime is too far in the future";
    return false;
  }
  std::vector<unsigned char> coinbaseBytes;
  if (!HexToBytesStr(job.coinbase1 + session.extranonce1 + extranonce2Hex +
                         job.coinbase2,
                     coinbaseBytes)) {
    error = "bad coinbase hex";
    return false;
  }
  Hash256 root = Sha256d(coinbaseBytes);
  for (const std::string& branchHex : job.merkleBranches) {
    std::vector<unsigned char> branchBytes;
    if (!HexToBytesStr(branchHex, branchBytes) || branchBytes.size() != 32) {
      error = "bad merkle branch";
      return false;
    }
    std::vector<unsigned char> buf;
    buf.insert(buf.end(), root.begin(), root.end());
    buf.insert(buf.end(), branchBytes.begin(), branchBytes.end());
    root = Sha256d(buf);
  }
  BlockHeader header;
  header.nVersion = static_cast<int32_t>(job.version);
  header.hashPrevBlock = job.prevHash;
  header.hashMerkleRoot = root;
  header.nTime = ntime;
  header.nBits = job.nbits;
  header.nNonce = nonce;
  const Hash256 hash = header.GetHash();
  Target sessionTarget;
  if (!DifficultyToTarget(session.difficulty, sessionTarget)) {
    error = "bad difficulty";
    return false;
  }
  if (std::memcmp(hash.data(), sessionTarget.data(), 32) > 0) {
    error = "low difficulty share";
    return false;
  }
  // The share is good. If it also meets the network target, it is a
  // real block: assemble it and submit it to the chain.
  if (CheckProofOfWork(hash, job.nbits)) {
    Transaction coinbase;
    if (Transaction::Deserialize(coinbaseBytes, coinbase)) {
      Block block;
      block.header = header;
      block.vtx.push_back(coinbase);
      for (const Transaction& tx : job.otherTxs) block.vtx.push_back(tx);
      block.header.hashMerkleRoot = block.ComputeMerkleRoot();
      std::string blockError;
      {
        NodeLock nodeLock(nodeMutex_);
        if (node_.BroadcastBlock(block, blockError)) {
          blocksFound_.fetch_add(1);
        }
      }
    }
  }
  return true;
}

}  // namespace oeeoo
