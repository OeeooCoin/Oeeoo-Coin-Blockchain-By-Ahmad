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
const char* kUserAgent = "/OeeooCoin:0.1.0/";
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
  if (bans_.IsBanned(address, Now())) return false;
  if (static_cast<int>(peers_.size()) >= maxPeers_) return false;
  std::string peerId;
  if (!transport_->Connect(address, peerId)) return false;
  Peer peer;
  peer.id = peerId;
  peer.address = address;
  peer.outbound = true;
  peer.nonce = nodeNonce_;
  peers_.emplace(peerId, std::move(peer));
  SendVersion(peerId);
  return true;
}

void Node::Poll(int timeoutMs) {
  if (!transport_) return;
  TransportMessage msg;
  // Drain everything already queued, then one blocking wait.
  while (transport_->Poll(msg, 0)) OnBytes(msg.peerId, msg.bytes);
  if (timeoutMs > 0 && transport_->Poll(msg, timeoutMs)) {
    OnBytes(msg.peerId, msg.bytes);
    while (transport_->Poll(msg, 0)) OnBytes(msg.peerId, msg.bytes);
  }
  // Ping peers we have not pinged recently.
  const uint32_t now = Now();
  for (auto& kv : peers_) {
    Peer& peer = kv.second;
    if (!peer.handshakeComplete) continue;
    if (now - peer.lastPingTime >= kPingIntervalSeconds) {
      peer.lastPingTime = now;
      peer.lastPingNonce = now;
      SendMessage(peer.id, {"ping", SerializePing(peer.lastPingNonce)});
    }
  }
  // Mempool expiry: drop entries older than 14 days, at most once
  // a minute (RemoveExpired is a full scan).
  if (now - lastExpirySweep_ >= 60) {
    lastExpirySweep_ = now;
    mempool_.RemoveExpired(now);
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
    bans_.Ban(it->second.address, Now());
    DisconnectPeer(peerId);
  }
}

void Node::DisconnectPeer(const std::string& peerId) {
  if (transport_) transport_->Disconnect(peerId);
  peers_.erase(peerId);
}

void Node::OnBytes(const std::string& peerId,
                   const std::vector<unsigned char>& bytes) {
  auto it = peers_.find(peerId);
  if (it == peers_.end()) {
    // Inbound connection: register the peer and wait for its version.
    // Past the peer cap, the connection is refused outright.
    if (static_cast<int>(peers_.size()) >= maxPeers_) {
      if (transport_) transport_->Disconnect(peerId);
      return;
    }
    Peer peer;
    peer.id = peerId;
    peer.address = peerId;  // ban key is the transport peer id
    peer.outbound = false;
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
  if (msg.command == "pong") return;
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
      Misbehave(peerId, 10);
      return;
    }
    std::string error;
    if (!mempool_.AddTransaction(tx, utxo_, chain_.GetTipHeight() + 1,
                                 chain_.GetMedianTimePast(), Now(), error,
                                 chain_.GetVestingParams())) {
      return;
    }
    peer.knownInventory.insert(txid);
    RelayTxInv(txid, peerId);
    return;
  }
  if (msg.command == "block") {
    Block block;
    if (!Block::Deserialize(msg.payload, block)) {
      Misbehave(peerId, 20);
      return;
    }
    std::string error;
    bool punish = false;
    if (!AcceptBlock(block, error, punish)) {
      if (punish) Misbehave(peerId, 100);
      return;
    }
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
    // Do not fetch transactions for a header that fails proof of
    // work: a peer could otherwise make this node do useless work
    // for a fake header.
    if (!CheckProofOfWork(cb.header.GetHash(), cb.header.nBits)) {
      Misbehave(peerId, 20);
      return;
    }
    Block block;
    std::vector<uint32_t> missing;
    if (!ReconstructBlock(cb, mempool_, block, missing)) {
      pendingCompact_[cb.header.GetHash()] = {cb, peerId, Now()};
      SendMessage(peerId,
                  {"getblocktxn",
                   SerializeGetBlockTxn(cb.header.GetHash(), missing)});
      return;
    }
    std::string error;
    bool punish = false;
    if (!AcceptBlock(block, error, punish)) {
      if (punish) Misbehave(peerId, 100);
      return;
    }
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
    CompactBlock cb = pit->second.cb;
    // Save the peer id before erasing: the iterator is invalid
    // after the erase, and the fallback below needs the peer.
    const std::string compactPeer = pit->second.peerId;
    pendingCompact_.erase(pit);
    // Fill the prefilled list with the received transactions.
    for (const auto& entry : txs) {
      cb.prefilled.push_back(entry);
    }
    Block block;
    std::vector<uint32_t> missing;
    if (!ReconstructBlock(cb, mempool_, block, missing)) {
      // Full-block fallback: the compact relay could not be
      // completed even after blocktxn (the peer did not supply the
      // missing transactions), so request the full block.
      requestedBlocks_.insert(blockHash);
      SendMessage(compactPeer,
                  {"getdata",
                   SerializeInventory({{kInvBlock, blockHash}})});
      return;
    }
    std::string error;
    bool punish = false;
    if (!AcceptBlock(block, error, punish)) {
      if (punish) Misbehave(peerId, 100);
      return;
    }
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
  return true;
}

bool Node::BroadcastBlock(const Block& block, std::string& error) {
  bool punish = false;
  if (!AcceptBlock(block, error, punish)) return false;
  RelayBlock(block, "");
  return true;
}

}  // namespace oeeoo
