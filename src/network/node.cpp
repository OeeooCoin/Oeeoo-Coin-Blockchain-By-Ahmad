// SPDX-License-Identifier: MIT
// Oeeoo Coin — P2P node implementation.
#include "network/node.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <random>

#include "blockchain/verify.h"
#include "consensus/consensus.h"
#include "consensus/difficulty.h"
#include "network/dnsseed.h"
#include "mining/miner.h"
#include "serialize/endian.h"
#include "transactions/validation.h"

namespace oeeoo {

namespace {
constexpr uint32_t kPingIntervalSeconds = 60;
// A compact block that never completes (peer never answers
// blocktxn, or the answer still does not reconstruct) is dropped
// after this long, so a peer cannot pin memory here.
constexpr uint32_t kPendingCompactTimeoutSeconds = 120;
constexpr std::size_t kMaxHeadersPerMessage = 2000;
// Compact relay limits (Milestone 15). Both are Oeeoo's starting
// values, not Bitcoin Core constants: Core's getblocktxn depth is
// 10 and Oeeoo starts at the same depth; the per-peer in-flight
// limit of 5 pending compact blocks is Oeeoo's own choice —
// enough for a healthy peer relaying several blocks at once, small
// enough that one peer cannot pin much reconstruction state.
constexpr std::size_t kMaxCompactInFlightPerPeer = 5;
constexpr std::size_t kMaxBlockTxnIndexes = 10;
// Block-stalling disconnect (Milestone 15): a peer asked for a
// block (getdata) or block transactions (getblocktxn) that has not
// answered within this long is disconnected. Bitcoin Core's
// default is 2 seconds, adopted here.
constexpr uint32_t kBlockStallTimeoutSeconds = 2;
const char* kUserAgent = "/OeeooCoin:0.1.0/";

// Steady-clock milliseconds (monotonic — the wall clock can jump).
// Used for ping round-trips, never for consensus or expiry.
uint64_t SteadyMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// Parse an "ip:port" address string into its parts. Returns false
// for anything else. Loopback names ("loopback:...") have no port
// and are test-only addresses, so they are never recorded in the
// address manager — only real IPv4 addresses with a port are.
bool ParseIpPort(const std::string& address, std::string& ip,
                 uint16_t& port) {
  const std::size_t colon = address.rfind(':');
  if (colon == std::string::npos) return false;
  ip = address.substr(0, colon);
  std::array<unsigned char, 4> octets{};
  if (!ParseIPv4(ip, octets)) return false;
  // std::stoi throws on a non-numeric port; a malformed address
  // must fail cleanly, never crash the node.
  int parsed = 0;
  try {
    parsed = std::stoi(address.substr(colon + 1));
  } catch (...) {
    return false;
  }
  if (parsed <= 0 || parsed > 65535) return false;
  port = static_cast<uint16_t>(parsed);
  return true;
}
}  // namespace

Node::Node(uint32_t powLimitBits) : chain_(powLimitBits) {
  // A random 64-bit nonce identifies this node in the handshake.
  // It must NOT be time-derived: two nodes started in the same
  // second would otherwise share a nonce and mistake each other
  // for a self-connection.
  static std::atomic<uint64_t> nonceSalt{0};
  const uint64_t randomPart =
      (static_cast<uint64_t>(std::random_device{}()) << 32) |
      static_cast<uint64_t>(std::random_device{}());
  nodeNonce_ = randomPart ^
               (nonceSalt.fetch_add(1) * 0x9e3779b97f4a7c15ULL) ^
               static_cast<uint64_t>(std::time(nullptr));
  if (nodeNonce_ == 0) nodeNonce_ = 1;
}

uint32_t Node::Now() const {
  if (now_ != 0) return now_;
  return static_cast<uint32_t>(std::time(nullptr));
}

std::size_t Node::HandshookPeerCount() const {
  std::size_t n = 0;
  for (const auto& kv : peers_) {
    if (kv.second.handshakeComplete) ++n;
  }
  return n;
}

std::size_t Node::QueryDnsSeeds(std::string& error) {
  std::size_t added = 0;
  std::string failures;
  for (const std::string& seed : dnsSeeds_) {
    std::vector<AddrEntry> addrs;
    std::string seedError;
    if (!ResolveSeed(seed, kMainnetPort, addrs, seedError)) {
      if (!failures.empty()) failures += "; ";
      failures += seedError;
      continue;
    }
    for (const AddrEntry& a : addrs) {
      if (addrman_.Add(a)) ++added;
    }
  }
  error = failures;
  return added;
}

bool Node::Listen(uint16_t port) {
  if (!transport_) return false;
  return transport_->Listen(port);
}

bool Node::GenesisHash(Hash256& out) const {
  if (chain_.GetTipHeight() < 0) return false;
  return chain_.GetBlockHashAtHeight(0, out);
}

bool Node::ConnectTo(const std::string& address) {
  if (!transport_) return false;
  // The ban check is IP-only (Milestone 16): BanManager::BanKey
  // strips the ephemeral port inside IsBanned, so an outbound
  // dial to a banned IP is refused whatever port it uses.
  if (bans_.IsBanned(address, Now())) return false;
  if (static_cast<int>(peers_.size()) >= maxPeers_) return false;
  std::string peerId;
  if (!transport_->Connect(address, peerId)) {
    // A failed outbound attempt counts against the address in the
    // address manager (3 failures and the entry is dropped as
    // dead). Loopback names are not recorded (ParseIpPort rejects
    // them — they are test-only addresses with no port).
    std::string ip;
    uint16_t port = 0;
    if (ParseIpPort(address, ip, port)) addrman_.RecordFailure(ip, port);
    return false;
  }
  Peer peer;
  peer.id = peerId;
  peer.address = address;
  peer.outbound = true;
  peer.nonce = nodeNonce_;
  peer.connectedAt = Now();
  peers_.emplace(peerId, std::move(peer));
  SendVersion(peerId);
  return true;
}

void Node::Poll(int timeoutMs) {
  if (!transport_) return;
  TransportMessage msg;
  // Drain everything already queued, then one blocking wait.
  while (transport_->Poll(msg, 0)) OnBytes(msg.peerId, msg.bytes, msg.remoteAddress);
  if (timeoutMs > 0 && transport_->Poll(msg, timeoutMs)) {
    OnBytes(msg.peerId, msg.bytes, msg.remoteAddress);
    while (transport_->Poll(msg, 0)) OnBytes(msg.peerId, msg.bytes, msg.remoteAddress);
  }
  // Ping peers we have not pinged recently.
  const uint32_t now = Now();
  for (auto& kv : peers_) {
    Peer& peer = kv.second;
    if (!peer.handshakeComplete) continue;
    if (now - peer.lastPingTime >= kPingIntervalSeconds) {
      peer.lastPingTime = now;
      peer.lastPingNonce = now;
      peer.lastPingSentMs = SteadyMs();
      SendMessage(peer.id, {"ping", SerializePing(peer.lastPingNonce)});
    }
  }
  // Block-stalling disconnect, checked on EVERY Poll call (not
  // just the 60-second sweep below): a peer asked for a block
  // (getdata) or block transactions (getblocktxn) that has not
  // answered within kBlockStallTimeoutSeconds is disconnected.
  // No misbehavior points: stalling is not proof of malice — the
  // peer may be slow, overloaded, or on a bad link, and punishing
  // it would ban honest slow peers. There is deliberately NO
  // Core-style 64-second backoff after the disconnect: the
  // backoff only matters if the node kept requesting from the same
  // peer, which it does not — the peer is gone.
  // Frozen test time (SetTime) cannot trigger this: blockRequestAt
  // is stamped with Now(), so the difference stays 0 until the
  // clock actually advances.
  for (auto it = peers_.begin(); it != peers_.end();) {
    Peer& peer = it->second;
    if (peer.awaitingBlock != Hash256{} &&
        now - peer.blockRequestAt >= kBlockStallTimeoutSeconds) {
      const std::string peerId = it->first;
      ++it;  // advance before DisconnectPeer erases the entry
      DisconnectPeer(peerId);
    } else {
      ++it;
    }
  }
  // Mempool expiry: drop entries older than 14 days, at most once
  // a minute (RemoveExpired is a full scan).
  if (now - lastExpirySweep_ >= 60) {
    lastExpirySweep_ = now;
    mempool_.RemoveExpired(now);
    // Orphan expiry (20 minutes) and address-manager expiry (30
    // days) ride the same sweep.
    orphans_.RemoveExpired(now);
    addrman_.RemoveExpired(now);
    // Pending compact blocks that never completed expire, so a
    // peer that sends a header and never answers blocktxn cannot
    // pin memory here.
    for (auto it = pendingCompact_.begin(); it != pendingCompact_.end();) {
      if (now - it->second.receivedAt >= kPendingCompactTimeoutSeconds) {
        it = pendingCompact_.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void Node::SendMessage(const std::string& peerId, const Message& msg) {
  if (!transport_) return;
  transport_->Send(peerId, SerializeMessage(msg));
}

void Node::SendToAll(const Message& msg, const std::string& exceptId) {
  for (const auto& kv : peers_) {
    if (kv.first == exceptId) continue;
    if (!kv.second.handshakeComplete) continue;
    SendMessage(kv.first, msg);
  }
}

void Node::SendVersion(const std::string& peerId) {
  auto it = peers_.find(peerId);
  if (it == peers_.end()) return;
  VersionPayload v;
  v.timestamp = Now();
  v.nonce = nodeNonce_;
  v.userAgent = kUserAgent;
  v.startHeight = chain_.GetTipHeight();
  v.relay = true;
  SendMessage(peerId, {"version", SerializeVersion(v)});
}

void Node::Misbehave(const std::string& peerId, int score) {
  auto it = peers_.find(peerId);
  if (it == peers_.end()) return;
  if (it->second.AddMisbehavior(score)) {
    // Ban key is the IP alone (Milestone 16): BanManager::BanKey
    // strips the ephemeral port inside Ban, so the ban survives
    // a reconnect from a new port. peer.address itself keeps the
    // full "ip:port" for display/diagnostics.
    bans_.Ban(it->second.address, Now());
    DisconnectPeer(peerId);
  }
}

void Node::DisconnectPeer(const std::string& peerId) {
  if (transport_) transport_->Disconnect(peerId);
  // The peer's orphans go with it (Milestone 15): an orphan from a
  // disconnected peer is never promoted later. This also covers
  // ban-disconnects via Misbehave, which funnel through here.
  orphans_.PurgePeer(peerId);
  peers_.erase(peerId);
}

void Node::OnBytes(const std::string& peerId,
                   const std::vector<unsigned char>& bytes,
                   const std::string& remoteAddress) {
  auto it = peers_.find(peerId);
  if (it == peers_.end()) {
    // Inbound connection: register the peer and wait for its version.
    // The peer's address is the transport's remote address (the
    // real "ip:port" for TCP); when the transport does not know it,
    // fall back to the transport peer id as before.
    const std::string address =
        remoteAddress.empty() ? peerId : remoteAddress;
    // A ban survives a reconnect: check the ban list BEFORE
    // registering the peer. Before Milestone 15 inbound peers were
    // never ban-checked — bans were keyed by the ephemeral
    // transport id, so a banned inbound peer could reconnect
    // immediately under a fresh id. Since Milestone 16 the check
    // is IP-only (BanManager::BanKey strips the ephemeral port
    // inside IsBanned), so a TCP peer reconnecting from a new
    // ephemeral port is still refused.
    if (bans_.IsBanned(address, Now())) {
      if (transport_) transport_->Disconnect(peerId);
      return;
    }
    // Past the peer cap, try to evict a worse inbound peer
    // (Milestone 15); only refuse outright when nobody may be
    // evicted. Outbound connections keep refusing at the cap —
    // eviction is inbound-only.
    if (static_cast<int>(peers_.size()) >= maxPeers_) {
      if (!EvictInboundPeerFor(address)) {
        if (transport_) transport_->Disconnect(peerId);
        return;
      }
    }
    Peer peer;
    peer.id = peerId;
    peer.address = address;
    peer.outbound = false;
    peer.connectedAt = Now();
    it = peers_.emplace(peerId, std::move(peer)).first;
  }
  Peer& peer = it->second;
  peer.recvBuffer.insert(peer.recvBuffer.end(), bytes.begin(), bytes.end());
  std::size_t offset = 0;
  while (offset < peer.recvBuffer.size()) {
    // Peek for corruption before waiting for more bytes.
    if (peer.recvBuffer.size() - offset < kMessageHeaderSize) break;
    if (std::memcmp(peer.recvBuffer.data() + offset, kNetworkMagic.data(),
                    4) != 0) {
      Misbehave(peerId, 50);  // bad magic: not our network
      peer.recvBuffer.clear();
      return;
    }
    const uint32_t length =
        ReadLE32(peer.recvBuffer.data() + offset + 16);
    if (length > kMaxMessageLength) {
      Misbehave(peerId, 50);
      peer.recvBuffer.clear();
      return;
    }
    if (peer.recvBuffer.size() - offset - kMessageHeaderSize < length) {
      break;  // wait for the rest
    }
    Message msg;
    std::size_t consumed = 0;
    if (!ParseMessage(peer.recvBuffer, offset, msg, consumed)) {
      Misbehave(peerId, 50);  // bad checksum
      peer.recvBuffer.clear();
      return;
    }
    offset += consumed;
    OnMessage(peerId, msg);
    if (peers_.find(peerId) == peers_.end()) return;  // disconnected
  }
  if (offset > 0) {
    peer.recvBuffer.erase(peer.recvBuffer.begin(),
                          peer.recvBuffer.begin() + offset);
  }
}

// Peer eviction when the node is full (Milestone 15; Bitcoin
// Core's AttemptToEvictConnection, adapted to Oeeoo's peer data).
// Ranking, applied in order:
//   Among INBOUND peers only, exclude from candidacy
//   (a) the peer with the lowest known pingRttMs (unknown RTT —
//       UINT32_MAX — does not count as "lowest known"),
//   (b) peers that sent a tx or block within the last 60 s,
//   (c) the longest-connected peer (smallest connectedAt).
//   Among the rest, evict the one in the largest netgroup (most
//   peers sharing its /16 via AddressManager::NetgroupOf —
//   loopback addresses group as "" and tie); tie-break by
//   most-recently-connected (largest connectedAt); final tie-break
//   by highest peerId string (deterministic).
// The new peer's address is not part of the ranking: the victim is
// chosen among existing inbound peers only. If no candidate
// exists, returns false and the new connection is refused.
bool Node::EvictInboundPeerFor(const std::string& newAddress) {
  (void)newAddress;
  std::vector<std::string> inbound;
  for (const auto& kv : peers_) {
    if (!kv.second.outbound) inbound.push_back(kv.first);
  }
  if (inbound.empty()) return false;
  const uint32_t now = Now();
  // (a) The peer with the lowest known ping RTT is protected.
  std::string lowestPingPeer;
  uint32_t lowestPing = UINT32_MAX;
  for (const std::string& id : inbound) {
    const Peer& peer = peers_.at(id);
    if (peer.pingRttMs < lowestPing) {
      lowestPing = peer.pingRttMs;
      lowestPingPeer = id;
    }
  }
  if (lowestPing == UINT32_MAX) lowestPingPeer.clear();  // none known
  // (c) The longest-connected peer is protected.
  std::string longestPeer;
  uint32_t smallestConnectedAt = UINT32_MAX;
  for (const std::string& id : inbound) {
    const Peer& peer = peers_.at(id);
    if (peer.connectedAt < smallestConnectedAt) {
      smallestConnectedAt = peer.connectedAt;
      longestPeer = id;
    }
  }
  // Candidates: inbound peers excluded by none of (a), (b), (c).
  std::vector<std::string> candidates;
  for (const std::string& id : inbound) {
    const Peer& peer = peers_.at(id);
    if (id == lowestPingPeer) continue;
    if (id == longestPeer) continue;
    // (b) A peer that sent a tx or block within the last 60 s is
    // protected. A zero timestamp means "never": it is not recent.
    const uint32_t lastActivity =
        std::max(peer.lastTxTime, peer.lastBlockTime);
    if (lastActivity != 0 && now - lastActivity < 60) continue;
    candidates.push_back(id);
  }
  if (candidates.empty()) return false;
  // Netgroup of a peer: the /16 of the IP part of its address.
  // Loopback addresses have no IP part, so NetgroupOf returns ""
  // and they all tie in this ranking.
  auto netgroupOfPeer = [&](const std::string& id) {
    const std::string& address = peers_.at(id).address;
    const std::size_t colon = address.rfind(':');
    const std::string ip = (colon == std::string::npos)
                               ? address
                               : address.substr(0, colon);
    return AddressManager::NetgroupOf(ip);
  };
  // Netgroup size counts ALL inbound peers sharing the /16, not
  // just candidates — the group that crowds the node is the one
  // that loses a member.
  auto netgroupSize = [&](const std::string& group) {
    std::size_t n = 0;
    for (const std::string& id : inbound) {
      if (netgroupOfPeer(id) == group) ++n;
    }
    return n;
  };
  // Rank: largest netgroup first, then most-recently-connected,
  // then highest peerId string.
  std::sort(candidates.begin(), candidates.end(),
            [&](const std::string& a, const std::string& b) {
              const std::size_t groupA = netgroupSize(netgroupOfPeer(a));
              const std::size_t groupB = netgroupSize(netgroupOfPeer(b));
              if (groupA != groupB) return groupA > groupB;
              const uint32_t connectedA = peers_.at(a).connectedAt;
              const uint32_t connectedB = peers_.at(b).connectedAt;
              if (connectedA != connectedB) return connectedA > connectedB;
              return a > b;
            });
  const std::string victim = candidates.front();
  DisconnectPeer(victim);
  return true;
}

// Re-validate orphans waiting on `parentTxid` now that the parent
// has arrived. On success the orphan enters the mempool, is
// relayed, and its own children are processed recursively. On a
// missing-input failure it stays in the pool (it still waits on
// another parent); on any other failure it is dropped.
// Bound: at most 100 orphans are processed per call, so one parent
// arrival cannot make the node do unbounded work. (The recursion
// is bounded by the pool itself: at most 100 orphans can exist,
// so a chain of orphans recurses at most 100 deep.)
void Node::ProcessOrphansFor(const Hash256& parentTxid) {
  constexpr std::size_t kMaxOrphansPerCall = 100;
  std::size_t processed = 0;
  const std::vector<Hash256> waiting =
      orphans_.GetOrphansDependingOn(parentTxid);
  for (const Hash256& txid : waiting) {
    if (processed >= kMaxOrphansPerCall) break;
    ++processed;
    Transaction tx;
    if (!orphans_.Get(txid, tx)) continue;  // already promoted
    TxValidationResult r = ValidateTransaction(
        tx, utxo_, chain_.GetTipHeight() + 1, chain_.GetMedianTimePast());
    if (!r.ok) {
      if (r.error == "missing input" ||
          r.error == "sequence lock: missing input") {
        continue;  // still waits on another parent: leave it
      }
      orphans_.Remove(txid);  // now invalid for another reason: drop
      continue;
    }
    std::string error;
    if (!mempool_.AddTransaction(tx, utxo_, chain_.GetTipHeight() + 1,
                                 chain_.GetMedianTimePast(), Now(), error,
                                 chain_.GetVestingParams())) {
      // Validated but the mempool refused it (fee, dust, ...):
      // drop it rather than retrying it forever.
      orphans_.Remove(txid);
      continue;
    }
    orphans_.Remove(txid);
    RelayTxInv(txid, "");
    ProcessOrphansFor(txid);
  }
}

void Node::OnMessage(const std::string& peerId, const Message& msg) {
  auto it = peers_.find(peerId);
  if (it == peers_.end()) return;
  Peer& peer = it->second;

  if (msg.command == "version") {
    VersionPayload v;
    if (!ParseVersion(msg.payload, v)) {
      Misbehave(peerId, 20);
      return;
    }
    if (peer.versionReceived) return;  // duplicate: ignore
    // Peers speaking an older protocol cannot be trusted to
    // follow this node's rules: refuse them.
    if (v.version < kProtocolVersion) {
      Misbehave(peerId, 20);
      DisconnectPeer(peerId);
      return;
    }
    // A peer echoing our own handshake nonce is us: a
    // self-connection. Drop it without punishment.
    if (v.nonce == nodeNonce_) {
      DisconnectPeer(peerId);
      return;
    }
    peer.versionReceived = true;
    peer.version = v.version;
    peer.startHeight = v.startHeight;
    SendVersion(peerId);
    SendMessage(peerId, {"verack", {}});
    return;
  }
  if (msg.command == "verack") {
    if (!peer.versionReceived) {
      Misbehave(peerId, 10);
      return;
    }
    peer.verackReceived = true;
    if (peer.versionReceived && peer.verackReceived) {
      peer.handshakeComplete = true;
      // The handshake completed: the address works. Reset its
      // failure count in the address manager (a peer that failed
      // twice and then connects is not dead). Loopback names are
      // not recorded (ParseIpPort rejects them — test-only
      // addresses with no port).
      std::string ip;
      uint16_t port = 0;
      if (ParseIpPort(peer.address, ip, port)) {
        addrman_.RecordSuccess(ip, port);
      }
      // Advertise compact-block support and our addresses.
      SendMessage(peerId,
                  {"sendcmpct", SerializeSendCmpct(true, 1)});
      std::vector<AddrEntry> addrs = addrman_.GetAddr(10);
      if (!addrs.empty()) {
        SendMessage(peerId, {"addr", SerializeAddr(addrs)});
      }
      if (peer.startHeight > chain_.GetTipHeight()) {
        RequestSync(peerId);
      }
    }
    return;
  }
  if (!peer.handshakeComplete) return;  // ignore everything pre-handshake

  if (msg.command == "ping") {
    uint64_t nonce = 0;
    if (ParsePing(msg.payload, nonce)) {
      SendMessage(peerId, {"pong", SerializePing(nonce)});
    }
    return;
  }
  if (msg.command == "pong") {
    // A pong answering our last ping measures the round-trip.
    // Anything else (an unsolicited pong, or one for an old ping)
    // is ignored: it carries no timing information.
    uint64_t nonce = 0;
    if (ParsePing(msg.payload, nonce) && nonce == peer.lastPingNonce) {
      peer.pingRttMs =
          static_cast<uint32_t>(SteadyMs() - peer.lastPingSentMs);
      peer.lastPongTime = Now();
    }
    return;
  }
  if (msg.command == "sendcmpct") {
    bool announce = false;
    uint64_t version = 0;
    if (ParseSendCmpct(msg.payload, announce, version)) {
      peer.compactAnnounce = announce;
      peer.compactVersion = version;
    }
    return;
  }
  if (msg.command == "addr") {
    std::vector<AddrEntry> addrs;
    if (ParseAddr(msg.payload, addrs)) {
      for (const AddrEntry& a : addrs) addrman_.Add(a);
    }
    return;
  }
  if (msg.command == "inv") {
    std::vector<InventoryItem> items;
    if (!ParseInventory(msg.payload, items)) return;
    std::vector<InventoryItem> want;
    for (const InventoryItem& item : items) {
      if (item.type == kInvTx) {
        if (!mempool_.Contains(item.hash) &&
            peer.knownInventory.count(item.hash) == 0) {
          want.push_back(item);
        }
      } else if (item.type == kInvBlock) {
        if (!chain_.Contains(item.hash) &&
            requestedBlocks_.count(item.hash) == 0) {
          // Headers-first: ask for headers up to this block.
          SendGetHeaders(peerId, item.hash);
          requestedBlocks_.insert(item.hash);
        }
      }
    }
    if (!want.empty()) {
      SendMessage(peerId, {"getdata", SerializeInventory(want)});
    }
    return;
  }
  if (msg.command == "getdata") {
    std::vector<InventoryItem> items;
    if (!ParseInventory(msg.payload, items)) return;
    std::vector<InventoryItem> missing;
    for (const InventoryItem& item : items) {
      if (item.type == kInvTx) {
        // Serve from the mempool.
        bool served = false;
        for (const Mempool::Entry& e : mempool_.GetByFeeRate()) {
          if (e.tx.GetTxId() == item.hash) {
            SendMessage(peerId, {"tx", e.tx.Serialize()});
            served = true;
            break;
          }
        }
        if (!served) missing.push_back(item);
      } else if (item.type == kInvBlock) {
        auto bit = knownBlocks_.find(item.hash);
        if (bit != knownBlocks_.end()) {
          SendMessage(peerId, {"block", bit->second.Serialize()});
        } else {
          missing.push_back(item);
        }
      }
    }
    if (!missing.empty()) {
      SendMessage(peerId, {"notfound", SerializeInventory(missing)});
    }
    return;
  }
  if (msg.command == "getheaders") {
    int32_t version = 0;
    std::vector<Hash256> locator;
    Hash256 hashStop;
    if (!ParseGetHeaders(msg.payload, version, locator, hashStop)) return;
    // Walk back from our tip until a locator hash is found. An empty
    // locator means the peer has no chain yet: send from genesis.
    // (The hashStop limit is applied AFTER the walk: it must not
    // cut the walk short before the locator is reached.)
    std::vector<BlockHeader> headers;
    Hash256 cursor = chain_.GetTipHash();
    bool found = locator.empty();
    while (headers.size() < kMaxHeadersPerMessage) {
      if (cursor == Hash256{}) break;
      BlockHeader h;
      if (!chain_.GetBlockHeader(cursor, h)) break;
      if (!locator.empty() &&
          std::find(locator.begin(), locator.end(), cursor) !=
              locator.end()) {
        found = true;
        break;
      }
      headers.push_back(h);
      Hash256 prev;
      if (!chain_.GetPrevHash(cursor, prev)) break;
      cursor = prev;
    }
    if (!found) return;  // locator not on our chain: send nothing
    std::reverse(headers.begin(), headers.end());
    if (hashStop != Hash256{}) {
      const int stopHeight = chain_.GetHeight(hashStop);
      if (stopHeight >= 0) {
        // Keep only headers at or before the stop block.
        std::vector<BlockHeader> kept;
        for (const BlockHeader& h : headers) {
          if (chain_.GetHeight(h.GetHash()) <= stopHeight) {
            kept.push_back(h);
          }
        }
        headers = std::move(kept);
      }
    }
    if (!headers.empty()) {
      SendMessage(peerId, {"headers", SerializeHeaders(headers)});
    }
    return;
  }
  if (msg.command == "headers") {
    std::vector<BlockHeader> headers;
    if (!ParseHeaders(msg.payload, headers)) return;
    std::vector<InventoryItem> want;
    for (const BlockHeader& h : headers) {
      const Hash256 hash = h.GetHash();
      BlockHeader known;
      if (!chain_.GetBlockHeader(hash, known)) {
        knownHeaders_[hash] = h;
        // Request the block data now (the inv path only asked for
        // headers; this is where the getdata is sent).
        want.push_back({kInvBlock, hash});
        requestedBlocks_.insert(hash);
      }
    }
    if (!want.empty()) {
      SendMessage(peerId, {"getdata", SerializeInventory(want)});
      // The peer should answer the first requested block first:
      // track it for the block-stalling disconnect. (A later
      // block in the batch is not tracked — the field holds one
      // hash — but the peer answers in order, so the first hash
      // is the one whose delay matters.)
      peer.awaitingBlock = want.front().hash;
      peer.blockRequestAt = Now();
    }
    return;
  }
  if (msg.command == "tx") {
    Transaction tx;
    if (!Transaction::Deserialize(msg.payload, tx)) {
      Misbehave(peerId, 10);
      return;
    }
    const Hash256 txid = tx.GetTxId();
    if (mempool_.Contains(txid)) return;
    TxValidationResult r = ValidateTransaction(
        tx, utxo_, chain_.GetTipHeight() + 1, chain_.GetMedianTimePast());
    if (!r.ok) {
      // A missing parent is normal, not malice: the transaction
      // goes to the orphan pool until its parent arrives, with no
      // misbehavior and no relay (orphans are never relayed).
      // An oversized or duplicate orphan is just dropped by the
      // pool — also with no punishment.
      if (r.error == "missing input" ||
          r.error == "sequence lock: missing input") {
        std::string orphanError;
        orphans_.Add(tx, peerId, Now(), orphanError);
        return;
      }
      Misbehave(peerId, 10);
      return;
    }
    std::string error;
    if (!mempool_.AddTransaction(tx, utxo_, chain_.GetTipHeight() + 1,
                                 chain_.GetMedianTimePast(), Now(), error,
                                 chain_.GetVestingParams())) {
      return;
    }
    peer.lastTxTime = Now();
    peer.knownInventory.insert(txid);
    RelayTxInv(txid, peerId);
    // The parent may have arrived: re-validate orphans waiting on
    // this transaction.
    ProcessOrphansFor(txid);
    return;
  }
  if (msg.command == "block") {
    Block block;
    if (!Block::Deserialize(msg.payload, block)) {
      Misbehave(peerId, 20);
      return;
    }
    // The request was answered — whatever the outcome — so the
    // stalling clock for this block stops here.
    if (peer.awaitingBlock == block.GetHash()) {
      peer.awaitingBlock = Hash256{};
    }
    std::string error;
    bool punish = false;
    if (!AcceptBlock(block, error, punish)) {
      if (punish) Misbehave(peerId, 100);
      return;
    }
    peer.lastBlockTime = Now();
    peer.knownInventory.insert(block.GetHash());
    requestedBlocks_.erase(block.GetHash());
    RelayBlock(block, peerId);
    return;
  }
  if (msg.command == "cmpctblock") {
    CompactBlock cb;
    if (!ParseCompactBlock(msg.payload, cb)) {
      Misbehave(peerId, 20);
      return;
    }
    // The request was answered — whatever the outcome — so the
    // stalling clock for this block stops here.
    if (peer.awaitingBlock == cb.header.GetHash()) {
      peer.awaitingBlock = Hash256{};
    }
    // Do not fetch transactions for a header that fails proof of
    // work: a peer could otherwise make this node do useless work
    // for a fake header.
    if (!CheckProofOfWork(cb.header.GetHash(), cb.header.nBits)) {
      Misbehave(peerId, 20);
      return;
    }
    // Per-peer in-flight limit: a peer may have at most
    // kMaxCompactInFlightPerPeer compact blocks awaiting blocktxn
    // at once. Past that, the message is ignored and the peer is
    // misbehaving (+20) — it is flooding the node with headers it
    // never completes.
    std::size_t inFlight = 0;
    for (const auto& kv : pendingCompact_) {
      if (kv.second.peerId == peerId) ++inFlight;
    }
    if (inFlight >= kMaxCompactInFlightPerPeer) {
      Misbehave(peerId, 20);
      return;
    }
    Block block;
    std::vector<uint32_t> missing;
    if (!ReconstructBlock(cb, mempool_, block, missing)) {
      // Too many missing transactions for a getblocktxn: ask for
      // the full block instead, and do NOT create a pending entry
      // (there is nothing to complete later).
      if (missing.size() > kMaxBlockTxnIndexes) {
        SendBlockRequest(peerId, cb.header.GetHash());
        return;
      }
      pendingCompact_[cb.header.GetHash()] = {cb, peerId, Now(), 0};
      SendBlockTxnRequest(peerId, cb.header.GetHash(), missing);
      return;
    }
    std::string error;
    bool punish = false;
    if (!AcceptBlock(block, error, punish)) {
      if (punish) Misbehave(peerId, 100);
      return;
    }
    peer.lastBlockTime = Now();
    RelayBlock(block, peerId);
    return;
  }
  if (msg.command == "getblocktxn") {
    Hash256 blockHash;
    std::vector<uint32_t> indexes;
    if (!ParseGetBlockTxn(msg.payload, blockHash, indexes)) return;
    auto it = knownBlocks_.find(blockHash);
    if (it == knownBlocks_.end()) return;
    std::vector<std::pair<uint32_t, Transaction>> txs;
    for (uint32_t index : indexes) {
      if (index < it->second.vtx.size()) {
        txs.push_back({index, it->second.vtx[index]});
      }
    }
    SendMessage(peerId,
                {"blocktxn", SerializeBlockTxn(blockHash, txs)});
    return;
  }
  if (msg.command == "blocktxn") {
    Hash256 blockHash;
    std::vector<std::pair<uint32_t, Transaction>> txs;
    if (!ParseBlockTxn(msg.payload, blockHash, txs)) return;
    auto pit = pendingCompact_.find(blockHash);
    if (pit == pendingCompact_.end()) return;
    // The getblocktxn request was answered — whatever the outcome
    // — so the stalling clock for this block stops here. (A retry
    // or the full-block fallback below starts a fresh clock.)
    if (peer.awaitingBlock == blockHash) {
      peer.awaitingBlock = Hash256{};
    }
    CompactBlock cb = pit->second.cb;
    const std::string compactPeer = pit->second.peerId;
    // Fill the prefilled list with the received transactions.
    for (const auto& entry : txs) {
      cb.prefilled.push_back(entry);
    }
    Block block;
    std::vector<uint32_t> missing;
    if (!ReconstructBlock(cb, mempool_, block, missing)) {
      // One retry, then the full-block fallback: the peer may
      // have sent a stale or partial answer, so ask once more for
      // exactly what is still missing — keeping the pending entry
      // (and the transactions already received) — before giving
      // up on the compact relay and requesting the full block.
      if (pit->second.retries < 1) {
        ++pit->second.retries;
        pit->second.cb = cb;  // keep the received transactions
        SendBlockTxnRequest(compactPeer, blockHash, missing);
        return;
      }
      pendingCompact_.erase(pit);
      SendBlockRequest(compactPeer, blockHash);
      return;
    }
    pendingCompact_.erase(pit);
    std::string error;
    bool punish = false;
    if (!AcceptBlock(block, error, punish)) {
      if (punish) Misbehave(peerId, 100);
      return;
    }
    peer.lastBlockTime = Now();
    RelayBlock(block, peerId);
    return;
  }
  if (msg.command == "reject") {
    // Informational only; no action.
    return;
  }
  // Unknown command: ignore (no punishment).
}

void Node::SendGetHeaders(const std::string& peerId,
                          const Hash256& hashStop) {
  std::vector<Hash256> locator;
  if (chain_.GetTipHeight() >= 0) locator.push_back(chain_.GetTipHash());
  SendMessage(peerId,
              {"getheaders",
               SerializeGetHeaders(kProtocolVersion, locator, hashStop)});
}

// Request a full block from a peer (getdata) and record the
// request on the peer for the block-stalling disconnect.
void Node::SendBlockRequest(const std::string& peerId,
                            const Hash256& hash) {
  auto it = peers_.find(peerId);
  if (it == peers_.end()) return;
  requestedBlocks_.insert(hash);
  SendMessage(peerId,
              {"getdata", SerializeInventory({{kInvBlock, hash}})});
  it->second.awaitingBlock = hash;
  it->second.blockRequestAt = Now();
}

// Request missing block transactions from a peer (getblocktxn)
// and record the request on the peer for the block-stalling
// disconnect.
void Node::SendBlockTxnRequest(const std::string& peerId,
                               const Hash256& hash,
                               const std::vector<uint32_t>& indexes) {
  auto it = peers_.find(peerId);
  if (it == peers_.end()) return;
  SendMessage(peerId,
              {"getblocktxn", SerializeGetBlockTxn(hash, indexes)});
  it->second.awaitingBlock = hash;
  it->second.blockRequestAt = Now();
}

void Node::RequestSync(const std::string& peerId) {
  SendGetHeaders(peerId, Hash256{});
}

void Node::RelayTxInv(const Hash256& txid, const std::string& exceptId) {
  Message inv{"inv", SerializeInventory({{kInvTx, txid}})};
  SendToAll(inv, exceptId);
}

void Node::RelayBlock(const Block& block, const std::string& exceptId) {
  const Hash256 hash = block.GetHash();
  for (const auto& kv : peers_) {
    if (kv.first == exceptId) continue;
    if (!kv.second.handshakeComplete) continue;
    if (kv.second.compactAnnounce && kv.second.compactVersion >= 1) {
      CompactBlock cb = MakeCompactBlock(block, ++compactNonce_);
      SendMessage(kv.first,
                  {"cmpctblock", SerializeCompactBlock(cb)});
    } else {
      SendMessage(kv.first,
                  {"inv", SerializeInventory({{kInvBlock, hash}})});
    }
  }
}

bool Node::AcceptBlock(const Block& block, std::string& error, bool& punish) {
  punish = false;
  // A block whose parent is unknown is not punished — we are behind
  // and will ask for headers instead.
  if (!block.CheckBasic(error)) {
    punish = true;
    return false;
  }
  // Already accepted: a no-op, never punished.
  if (chain_.Contains(block.GetHash())) return true;
  int height = 0;
  if (block.header.hashPrevBlock == Hash256{}) {
    if (chain_.GetTipHeight() >= 0) {
      error = "unexpected second genesis block";
      punish = true;
      return false;
    }
    height = 0;
  } else {
    const int parentHeight =
        chain_.GetHeight(block.header.hashPrevBlock);
    if (parentHeight < 0) {
      error = "parent block not found";
      punish = false;
      return false;
    }
    height = parentHeight + 1;
  }
  const bool directExtension =
      chain_.GetTipHeight() < 0 ||
      block.header.hashPrevBlock == chain_.GetTipHash();
  if (directExtension) {
    return AcceptBlockDirect(block, height, error, punish);
  }
  return AcceptBlockReorg(block, height, error, punish);
}

bool Node::AcceptBlockDirect(const Block& block, int height,
                             std::string& error, bool& punish) {
  // Validate everything against working copies BEFORE mutating the
  // chain or the UTXO set, so a rejection changes nothing.
  Amount fees = 0;
  if (!ComputeBlockFees(block, utxo_, height, chain_.GetMedianTimePast(),
                        fees, error)) {
    punish = true;
    return false;
  }
  if (!CheckBlockReward(block, height, fees, error,
                        chain_.GetVestingParams())) {
    punish = true;
    return false;
  }
  BlockUndo undo;
  if (!BuildBlockUndo(block, utxo_, height, block.header.nTime, undo,
                      error)) {
    punish = true;
    return false;
  }
  CoinsView working = utxo_;
  std::string connError;
  if (!ConnectBlockTransactions(block, working, height,
                                chain_.GetMedianTimePast(), block.header.nTime,
                                fees, connError,
                                chain_.GetVestingParams())) {
    error = connError;
    punish = true;
    return false;
  }
  if (!chain_.AddBlock(block, Now(), error)) {
    punish = (error != "parent block not found");
    return false;
  }
  utxo_ = std::move(working);
  undo_[block.GetHash()] = undo;
  knownBlocks_[block.GetHash()] = block;
  knownHeaders_[block.GetHash()] = block.header;
  // Confirmed transactions leave the mempool; their fee rates feed
  // the fee estimator (last 24 blocks).
  std::vector<FeeEstimator::ConfirmedTx> confirmed;
  for (std::size_t i = 1; i < block.vtx.size(); ++i) {
    Mempool::Entry entry;
    if (mempool_.GetEntry(block.vtx[i].GetTxId(), entry)) {
      confirmed.push_back({entry.fee, entry.size});
    }
    mempool_.Remove(block.vtx[i].GetTxId());
  }
  feeEstimator_.RecordBlock(height, confirmed);
  // Persist atomically when a store is attached.
  if (store_ != nullptr) {
    Target work;
    if (!chain_.GetChainWork(block.GetHash(), work)) {
      lastStoreError_ = "cannot read chain work for persistence";
    } else {
      std::string storeError;
      if (!store_->ApplyBlock(block, work, undo, storeError)) {
        lastStoreError_ = storeError;
      }
    }
  }
  // The block's transactions are now confirmed parents: orphans
  // waiting on any of them may validate now.
  for (std::size_t i = 1; i < block.vtx.size(); ++i) {
    ProcessOrphansFor(block.vtx[i].GetTxId());
  }
  return true;
}

bool Node::GetUndoForBlock(const Hash256& hash, BlockUndo& undo,
                           std::string& error) {
  auto it = undo_.find(hash);
  if (it != undo_.end()) {
    undo = it->second;
    return true;
  }
  if (store_ != nullptr && store_->GetUndo(hash, undo, error)) {
    return true;
  }
  if (error.empty()) error = "no undo record for block";
  return false;
}

bool Node::AcceptBlockReorg(const Block& block, int height,
                            std::string& error, bool& punish) {
  // 1. Find the fork point: walk back from the new block until we
  //    reach a block on the main chain, collecting the side branch.
  std::vector<Block> sideBlocks;  // oldest first (after the reverse)
  int forkHeight = -1;
  {
    std::vector<Block> newestFirst;
    Block cursorBlock = block;
    while (true) {
      const Hash256 parentHash = cursorBlock.header.hashPrevBlock;
      if (parentHash == Hash256{}) {
        error = "reorg fork point not found (genesis reached)";
        punish = false;
        return false;
      }
      const int parentHeight = chain_.GetHeight(parentHash);
      if (parentHeight < 0) {
        error = "reorg parent not in index";
        punish = false;
        return false;
      }
      Hash256 mainHash;
      if (chain_.GetBlockHashAtHeight(parentHeight, mainHash) &&
          mainHash == parentHash) {
        forkHeight = parentHeight;  // parent is on the main chain
        break;
      }
      Block parentBlock;
      if (!GetBlock(parentHash, parentBlock)) {
        error = "reorg history not available (side-branch block missing)";
        punish = false;
        return false;
      }
      newestFirst.push_back(parentBlock);
      cursorBlock = parentBlock;
    }
    sideBlocks = newestFirst;
    std::reverse(sideBlocks.begin(), sideBlocks.end());
    sideBlocks.push_back(block);
  }
  (void)height;
  // 2. Main-chain blocks to disconnect (newest first).
  std::vector<Block> mainBlocks;
  for (int h = chain_.GetTipHeight(); h > forkHeight; --h) {
    Hash256 hash;
    Block mainBlock;
    if (!chain_.GetBlockHashAtHeight(h, hash) || !GetBlock(hash, mainBlock)) {
      error = "reorg history not available (main-chain block missing)";
      punish = false;
      return false;
    }
    mainBlocks.push_back(mainBlock);
  }
  // 3. Disconnect the main chain back to the fork point on a working
  //    copy, then validate + connect the side branch in order.
  //    Median-Time-Past for each side block comes from a rolling
  //    list of the last 11 ancestor times (fork point's ancestors,
  //    then the earlier side blocks).
  CoinsView working = utxo_;
  for (const Block& mainBlock : mainBlocks) {
    BlockUndo undo;
    if (!GetUndoForBlock(mainBlock.GetHash(), undo, error)) {
      punish = false;
      return false;
    }
    if (!DisconnectBlockTransactions(mainBlock, working, undo, error)) {
      punish = false;
      return false;
    }
  }
  std::vector<uint32_t> recentTimes;  // most recent first, max 11
  for (int h = forkHeight; h >= 0 && (int)recentTimes.size() < 11; --h) {
    Hash256 hash;
    BlockHeader header;
    if (!chain_.GetBlockHashAtHeight(h, hash) ||
        !chain_.GetBlockHeader(hash, header)) {
      break;
    }
    recentTimes.push_back(header.nTime);
  }
  const auto medianTimePast = [&]() {
    std::vector<uint32_t> sorted = recentTimes;
    std::sort(sorted.begin(), sorted.end());
    return sorted[sorted.size() / 2];
  };
  std::vector<BlockUndo> sideUndos;
  for (std::size_t i = 0; i < sideBlocks.size(); ++i) {
    const Block& sideBlock = sideBlocks[i];
    const int sideHeight = forkHeight + 1 + static_cast<int>(i);
    const uint32_t mtp = medianTimePast();
    Amount fees = 0;
    if (!ComputeBlockFees(sideBlock, working, sideHeight, mtp, fees,
                          error)) {
      punish = true;
      return false;
    }
    if (!CheckBlockReward(sideBlock, sideHeight, fees, error,
                          chain_.GetVestingParams())) {
      punish = true;
      return false;
    }
    BlockUndo undo;
    if (!BuildBlockUndo(sideBlock, working, sideHeight,
                        sideBlock.header.nTime, undo, error)) {
      punish = true;
      return false;
    }
    std::string connError;
    if (!ConnectBlockTransactions(sideBlock, working, sideHeight, mtp,
                                  sideBlock.header.nTime, fees, connError,
                                  chain_.GetVestingParams())) {
      error = connError;
      punish = true;
      return false;
    }
    sideUndos.push_back(undo);
    recentTimes.insert(recentTimes.begin(), sideBlock.header.nTime);
    if (recentTimes.size() > 11) recentTimes.pop_back();
  }
  // 4. The header chain decides: only adopt the working UTXO set if
  //    the new block becomes the tip (more cumulative work).
  if (!chain_.AddBlock(block, Now(), error)) {
    punish = (error != "parent block not found");
    return false;
  }
  knownBlocks_[block.GetHash()] = block;
  knownHeaders_[block.GetHash()] = block.header;
  if (chain_.GetTipHash() != block.GetHash()) {
    // Inactive side branch: stored in the index, but the active
    // chain (and its UTXO set) is unchanged.
    if (store_ != nullptr) {
      Target work;
      if (chain_.GetChainWork(block.GetHash(), work)) {
        std::string storeError;
        if (!store_->PutBlockAtHeight(block, work, height, storeError)) {
          lastStoreError_ = storeError;
        }
      }
    }
    return true;
  }
  // The reorg happened: adopt the working UTXO set.
  utxo_ = std::move(working);
  for (const Block& mainBlock : mainBlocks) {
    undo_.erase(mainBlock.GetHash());
  }
  for (std::size_t i = 0; i < sideBlocks.size(); ++i) {
    undo_[sideBlocks[i].GetHash()] = sideUndos[i];
  }
  // Confirmed transactions leave the mempool; disconnected ones go
  // back in (if they are still valid on the new chain). The side
  // branch's confirmed fee rates feed the fee estimator.
  const int firstSideHeight =
      chain_.GetTipHeight() - static_cast<int>(sideBlocks.size()) + 1;
  for (std::size_t s = 0; s < sideBlocks.size(); ++s) {
    const Block& sideBlock = sideBlocks[s];
    std::vector<FeeEstimator::ConfirmedTx> confirmed;
    for (std::size_t i = 1; i < sideBlock.vtx.size(); ++i) {
      Mempool::Entry entry;
      if (mempool_.GetEntry(sideBlock.vtx[i].GetTxId(), entry)) {
        confirmed.push_back({entry.fee, entry.size});
      }
      mempool_.Remove(sideBlock.vtx[i].GetTxId());
    }
    feeEstimator_.RecordBlock(firstSideHeight + static_cast<int>(s),
                              confirmed);
  }
  for (const Block& mainBlock : mainBlocks) {
    for (std::size_t i = 1; i < mainBlock.vtx.size(); ++i) {
      std::string mempoolError;
      mempool_.AddTransaction(mainBlock.vtx[i], utxo_,
                              chain_.GetTipHeight() + 1,
                              chain_.GetMedianTimePast(), Now(), mempoolError,
                              chain_.GetVestingParams());
    }
  }
  // Persist the reorg: disconnect the old tip blocks (tip-first),
  // then apply the side branch (oldest first).
  if (store_ != nullptr) {
    bool storeOk = true;
    for (const Block& mainBlock : mainBlocks) {
      std::string storeError;
      if (!store_->DisconnectBlock(mainBlock.GetHash(), storeError)) {
        lastStoreError_ = storeError;
        storeOk = false;
        break;
      }
    }
    if (storeOk) {
      for (std::size_t i = 0; i < sideBlocks.size(); ++i) {
        Target work;
        if (!chain_.GetChainWork(sideBlocks[i].GetHash(), work)) {
          lastStoreError_ = "cannot read chain work for persistence";
          break;
        }
        std::string storeError;
        if (!store_->ApplyBlock(sideBlocks[i], work, sideUndos[i],
                                storeError)) {
          lastStoreError_ = storeError;
          break;
        }
      }
    }
  }
  // Adopted path only: the side branch's transactions are now
  // confirmed parents, so orphans waiting on any of them may
  // validate now. (The inactive-side-branch path returns earlier
  // and never reaches this.)
  for (const Block& sideBlock : sideBlocks) {
    for (std::size_t i = 1; i < sideBlock.vtx.size(); ++i) {
      ProcessOrphansFor(sideBlock.vtx[i].GetTxId());
    }
  }
  return true;
}

bool Node::GetBlock(const Hash256& hash, Block& block) const {
  auto it = knownBlocks_.find(hash);
  if (it == knownBlocks_.end()) return false;
  block = it->second;
  return true;
}

std::vector<Peer> Node::GetPeers() const {
  std::vector<Peer> out;
  for (const auto& kv : peers_) out.push_back(kv.second);
  return out;
}

bool Node::LoadFromStore(ChainStore& store, std::string& error,
                         bool deepVerify) {
  Hash256 tip;
  if (!store.LoadTip(tip, error)) {
    if (error == "no tip stored yet") {
      error.clear();
      return true;  // fresh store: nothing to load
    }
    return false;
  }
  std::vector<std::tuple<int, BlockHeader, Target>> headers;
  if (!store.GetAllHeaders(headers, error)) return false;
  for (const auto& entry : headers) {
    const BlockHeader& header = std::get<1>(entry);
    const Target& work = std::get<2>(entry);
    std::string indexError;
    if (!chain_.AddIndexEntry(header, work, indexError)) {
      error = "stored header failed index validation: " + indexError;
      return false;
    }
    knownHeaders_[header.GetHash()] = header;
  }
  if (chain_.GetTipHash() != tip) {
    error = "stored tip does not match the rebuilt chain tip";
    return false;
  }
  // Vesting launch-time adoption (mirrors AddBlock): when vesting
  // is enabled but no launch time was configured, the stored
  // genesis block's timestamp is the launch time.
  if (chain_.GetVestingParams().enabled &&
      chain_.GetVestingParams().launchTime == 0 && !headers.empty()) {
    VestingParams params = chain_.GetVestingParams();
    params.launchTime = std::get<1>(headers[0]).nTime;
    chain_.SetVestingParams(params);
  }
  if (deepVerify) {
    Hash256 verifiedTip;
    if (!VerifyChainFromStore(store, chain_.GetVestingParams(),
                              chain_.PowLimitBits(), verifiedTip, error)) {
      error = "deep verification failed: " + error;
      return false;
    }
    if (verifiedTip != tip) {
      error = "deep verification tip does not match the stored tip";
      return false;
    }
  }
  std::vector<std::pair<OutPoint, Coin>> coins;
  if (!store.GetAllCoins(coins, error)) return false;
  for (const auto& entry : coins) {
    if (!utxo_.AddCoin(entry.first, entry.second)) {
      error = "duplicate coin in stored UTXO set";
      return false;
    }
  }
  // Load unpruned bodies for getdata serving.
  for (const auto& entry : headers) {
    const Hash256 hash = std::get<1>(entry).GetHash();
    if (!store.HasBlockBody(hash)) continue;
    Block block;
    std::string blockError;
    if (!store.GetBlock(hash, block, blockError)) {
      error = "stored block failed validation: " + blockError;
      return false;
    }
    knownBlocks_[hash] = block;
  }
  return true;
}

bool Node::SendRawToAddress(const std::string& address, const Message& msg) {
  for (const auto& kv : peers_) {
    if (kv.second.address == address) {
      SendMessage(kv.first, msg);
      return true;
    }
  }
  return false;
}

bool Node::BroadcastTx(const Transaction& tx, std::string& error) {
  const int height = chain_.GetTipHeight() + 1;
  TxValidationResult r = ValidateTransaction(tx, utxo_, height,
                                             chain_.GetMedianTimePast());
  if (!r.ok) {
    error = r.error;
    return false;
  }
  if (!mempool_.AddTransaction(tx, utxo_, height,
                               chain_.GetMedianTimePast(), Now(), error,
                               chain_.GetVestingParams())) {
    return false;
  }
  RelayTxInv(tx.GetTxId(), "");
  // A locally broadcast transaction may be the parent an orphan
  // was waiting on.
  ProcessOrphansFor(tx.GetTxId());
  return true;
}

bool Node::BroadcastBlock(const Block& block, std::string& error) {
  bool punish = false;
  if (!AcceptBlock(block, error, punish)) return false;
  RelayBlock(block, "");
  return true;
}

}  // namespace oeeoo
