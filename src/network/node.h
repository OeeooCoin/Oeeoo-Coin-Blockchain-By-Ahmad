// SPDX-License-Identifier: MIT
// Oeeoo Coin — P2P node (Milestone 6).
// Owns the chain, UTXO set, mempool, address manager and ban list.
// Speaks the wire protocol over a transport (loopback for tests, TCP
// for real peers). Behaviour:
//  - version/verack handshake; peers behind our tip trigger a
//    headers-first sync (getheaders -> headers -> getdata).
//  - new transactions are validated, added to the mempool and
//    announced (inv) to other peers.
//  - new blocks are fully validated (structure, PoW, difficulty,
//    transactions, coinbase reward) and announced; compact-block
//    peers receive cmpctblock, others receive inv.
//  - invalid blocks earn misbehavior points; score >= 100 bans the
//    peer. A block whose parent is unknown is NOT punished — the
//    node asks for its headers instead.
//  - outbound dialing (Milestone 18): Poll() runs a dial loop
//    over the address manager (filled by DNS seeds, `addr`
//    messages and --addnode / the `addnode` RPC), up to 10
//    outbound peers, never dialing a banned address, itself,
//    or an already-connected peer.
// The node never reads the wall clock directly in consensus paths;
// SetTime() fixes the clock for deterministic tests (0 = real time).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "blockchain/chain.h"
#include "db/chainstore.h"
#include "mempool/feeestimator.h"
#include "mempool/mempool.h"
#include "mempool/mempooldump.h"
#include "mempool/orphan.h"
#include "mining/miner.h"
#include "network/addrman.h"
#include "network/compact.h"
#include "network/peer.h"
#include "network/protocol.h"
#include "network/transport.h"
#include "utxo/coins.h"

namespace oeeoo {

class Node {
 public:
  explicit Node(uint32_t powLimitBits = kGenesisBits);

  void SetTransport(std::shared_ptr<ITransport> transport) {
    transport_ = std::move(transport);
  }
  // Attach a persistent store (not owned). Accepted blocks are then
  // written to it atomically. A store failure does NOT un-accept a
  // block — see LastStoreError(); on restart the node reloads from
  // the store's last good state and re-syncs the rest from peers.
  void AttachStore(ChainStore* store) { store_ = store; }
  // Founder vesting parameters (Milestone 9). Must be set before
  // the genesis block is accepted; disabled by default.
  void SetVestingParams(const VestingParams& params) {
    chain_.SetVestingParams(params);
  }
  const VestingParams& GetVestingParams() const {
    return chain_.GetVestingParams();
  }
  // Rebuild chain index + UTXO + known blocks from a store. Returns
  // true on a fresh (empty) store as well. With `deepVerify`, the
  // stored chain is fully re-validated from the genesis block
  // (Milestone 11) and the load fails on the first bad block.
  bool LoadFromStore(ChainStore& store, std::string& error,
                     bool deepVerify = false);
  std::string LastStoreError() const { return lastStoreError_; }
  // Fixed clock for tests (seconds since epoch). 0 = real time.
  void SetTime(uint32_t now) { now_ = now; }
  uint32_t Now() const;

  bool Listen(uint16_t port);
  // This node's P2P listen port (0 = not listening). The dial
  // loop never dials 127.0.0.1:<this port> — that is this node
  // (the version-handshake nonce check is the backstop for the
  // node's other addresses, which it cannot know).
  uint16_t ListenPort() const { return listenPort_; }
  // Outbound connection; sends our version on success. Refused
  // when the peer cap is reached (default 125, spec section 11),
  // when the address is banned, or when it is this node's own
  // listen address. Always attempts the dial otherwise (even
  // when the address looks connected — a stale peer entry must
  // not block a re-dial; the failure is recorded in the address
  // manager). The already-connected guard lives in the dial loop
  // and the `addnode onetry` RPC, not here.
  bool ConnectTo(const std::string& address);
  // Maximum outbound peers the automatic dial loop opens (default
  // 10 — Oeeoo's own choice, NOT a Bitcoin Core constant).
  // Explicit connections (ConnectTo, `addnode onetry`) are NOT
  // bounded by this; they are bounded only by the total peer cap
  // (maxPeers_, default 125), like Core's manual connections.
  void SetMaxOutboundPeers(int maxOutbound) {
    maxOutboundPeers_ = maxOutbound < 0 ? 0 : maxOutbound;
  }
  int MaxOutboundPeers() const { return maxOutboundPeers_; }
  // Connected peers this node dialed out to.
  std::size_t OutboundPeerCount() const;
  // True when a peer is connected at `address` ("ip:port").
  bool IsConnectedTo(const std::string& address) const;
  // True when "ip:port" is already in the address manager.
  bool IsKnownPeer(const std::string& address) const;
  // Add a manually configured peer address ("ip:port", from
  // --addnode or the `addnode` RPC) to the address manager, so
  // the dial loop dials it. False + `error` when the address is
  // malformed, already known, or the address book is full.
  bool AddManualPeer(const std::string& address, std::string& error);
  // Remove a manually added address from the address manager.
  // Also disconnects a live peer at that address, so removing a
  // node takes effect immediately instead of lingering until the
  // peer drops. False when the address is malformed or unknown.
  bool RemoveManualPeer(const std::string& address);
  // Disconnect the peer connected at `address` ("ip:port"), if
  // any. False when no peer is connected there.
  bool DisconnectAddress(const std::string& address);
  // One pass of the outbound dial loop: dial AddrMan candidates
  // until the outbound cap (MaxOutboundPeers) or the total peer
  // cap is reached. Skips banned addresses (never dialed), this
  // node's own listen address, and already-connected peers.
  // Throttled: at most one pass per dial interval (default 5 s),
  // so a dead address is not spun on every Poll — a failed dial
  // also counts against the address in the AddrManager (3
  // failures and the entry is dropped as dead). When the address
  // book is empty and DNS seeds are configured, the seeds are
  // re-queried first, so a node whose seed addresses all died
  // can still learn peers. Called from Poll(); public so tests
  // can drive a dial pass deterministically.
  void TryDialPeers();
  // Seconds between dial passes (default 5). Tests set 0 to
  // disable the throttle.
  void SetDialIntervalSeconds(uint32_t seconds) {
    dialIntervalSeconds_ = seconds;
  }
  uint32_t DialIntervalSeconds() const { return dialIntervalSeconds_; }
  // Maximum connected peers (default 125, spec section 11). The
  // cap is enforced for outbound connections (ConnectTo) and for
  // inbound connections (OnBytes refuses to register a new peer
  // past the cap).
  void SetMaxPeers(int maxPeers) { maxPeers_ = maxPeers; }
  int MaxPeers() const { return maxPeers_; }
  // Mempool persistence (Milestone 17, Bitcoin's `mempool.dat`
  // model). The file path is set by node_main to
  // `<dataDir>/mempool.dat`; a node without a path has persistence
  // off. Persisting defaults ON (Core-compatible semantics):
  // `SetPersistMempool(false)` disables the automatic load on
  // start and save on shutdown — the `loaded` flag then stays
  // false and no file is written. The explicit `savemempool` RPC
  // (DumpMempool) still writes on demand: an operator who asks
  // for a dump gets one.
  void SetMempoolPath(const std::string& path) { mempoolPath_ = path; }
  const std::string& MempoolPath() const { return mempoolPath_; }
  void SetPersistMempool(bool persist) { persistMempool_ = persist; }
  bool PersistMempool() const { return persistMempool_; }
  // Load the mempool file at the configured path (best-effort:
  // a missing file loads as empty, a corrupt file warns and the
  // node starts with an empty mempool — see mempooldump.h).
  // Sets MempoolLoaded() true once the attempt has finished,
  // including when no file exists. Returns false + `error` when
  // persistence is disabled, no path is configured, or the file
  // is unreadable / has a malformed header. The outcome detail
  // is in LastMempoolLoadStats().
  bool LoadMempool(std::string& error);
  // Save the mempool to the configured path (the automatic
  // shutdown save). A no-op returning false + `error` when
  // persistence is disabled or no path is configured.
  bool SaveMempool(std::string& error);
  // Explicit on-demand dump (the `savemempool` RPC): writes the
  // file even when persistence is disabled. False + `error` when
  // no path is configured or the write fails.
  bool DumpMempool(std::string& error);
  // Import a mempool file at an explicit path (the
  // `importmempool` RPC): same load rules as LoadMempool, merged
  // into the current mempool. Does not touch MempoolLoaded().
  bool ImportMempoolFile(const std::string& path,
                         MempoolLoadStats& stats, std::string& error);
  // True once a startup load attempt has finished (including
  // when no file existed). False when persistence is disabled
  // (the load was skipped) or no load has been attempted yet.
  bool MempoolLoaded() const { return mempoolLoaded_; }
  MempoolLoadStats LastMempoolLoadStats() const {
    return lastMempoolLoadStats_;
  }
  // Mempool size cap in bytes (default 300 MB, spec section 10.4).
  void SetMaxMempoolBytes(std::size_t maxBytes) {
    mempool_.SetMaxBytes(maxBytes);
  }
  // Hash of the genesis block (height 0) of the loaded chain, or
  // false when the chain is empty. Used by the node binary to
  // verify a --genesis-file against an existing data directory.
  bool GenesisHash(Hash256& out) const;
  // Add a peer address to the address manager (the dial loop's
  // source, alongside DNS seeds and `addr` messages).
  void AddPeerAddress(const AddrEntry& entry) { addrman_.Add(entry); }
  // Register a DNS seed hostname (resolved by QueryDnsSeeds).
  // The seed list starts empty; seeds come only from operator
  // configuration — no hostnames are invented here.
  void AddDnsSeed(const std::string& hostname) {
    dnsSeeds_.push_back(hostname);
  }
  // Resolve every registered seed and add the returned addresses
  // to the address manager. Returns the number of addresses
  // added; `error` names the seeds that failed to resolve (empty
  // when all succeeded). A failing seed never stops the others.
  std::size_t QueryDnsSeeds(std::string& error);
  // Drive the node: drain transport messages, send pings.
  void Poll(int timeoutMs = 0);

  // Broadcast a locally created transaction: validate, mempool, relay.
  bool BroadcastTx(const Transaction& tx, std::string& error);
  // Validate + accept a locally mined block, then relay it.
  bool BroadcastBlock(const Block& block, std::string& error);

  // Send a raw protocol message to the peer connected at `address`
  // (the address passed to ConnectTo). Test/operator hook.
  bool SendRawToAddress(const std::string& address, const Message& msg);

  // Test / operator accessors.
  int TipHeight() const { return chain_.GetTipHeight(); }
  Hash256 TipHash() const { return chain_.GetTipHash(); }
  // This node's handshake nonce. Exposed so tests can craft a
  // self-connection attempt (a peer echoing our own nonce).
  uint64_t NodeNonce() const { return nodeNonce_; }
  std::size_t PeerCount() const { return peers_.size(); }
  // Full block by hash (from local storage / received blocks).
  bool GetBlock(const Hash256& hash, Block& block) const;
  // Connected peers (snapshot).
  std::vector<Peer> GetPeers() const;
  // Peers whose version/verack handshake has completed.
  std::size_t HandshookPeerCount() const;
  std::size_t MempoolSize() const { return mempool_.Size(); }
  bool HasTx(const Hash256& txid) const { return mempool_.Contains(txid); }
  bool HasBlock(const Hash256& hash) const { return chain_.Contains(hash); }
  const BanManager& Bans() const { return bans_; }
  const AddressManager& AddrMan() const { return addrman_; }
  // Orphan transactions held while their parent is unknown
  // (Milestone 15). Orphans are never relayed.
  std::size_t OrphanCount() const { return orphans_.Size(); }
  OrphanPool& Orphans() { return orphans_; }
  // Pending compact blocks awaiting blocktxn (Milestone 15).
  std::size_t PendingCompactCount() const { return pendingCompact_.size(); }
  CoinsView& Utxo() { return utxo_; }
  Mempool& MempoolRef() { return mempool_; }
  ChainState& Chain() { return chain_; }
  FeeEstimator& FeeEstimatorRef() { return feeEstimator_; }
  // Median fee rate (sat/vB) over the last 24 blocks of confirmed
  // transactions; 1 sat/vB when there is no data yet.
  Amount EstimateFeeRate() const { return feeEstimator_.EstimateFeeRate(); }

 private:
  void SendMessage(const std::string& peerId, const Message& msg);
  void SendToAll(const Message& msg, const std::string& exceptId = "");
  // `remoteAddress` is the peer's "ip:port" from the transport
  // (empty when unknown); a new inbound peer is registered under
  // it. The BAN key for that address is the IP alone
  // (BanManager::BanKey, Milestone 16) — the full "ip:port" is
  // kept only as peer.address for display/diagnostics.
  void OnBytes(const std::string& peerId,
               const std::vector<unsigned char>& bytes,
               const std::string& remoteAddress = "");
  void OnMessage(const std::string& peerId, const Message& msg);
  void Misbehave(const std::string& peerId, int score);
  void DisconnectPeer(const std::string& peerId);
  // Evict one inbound peer to make room for a new connection.
  // Returns false when no peer may be evicted (see node.cpp for
  // the ranking). Inbound-only: outbound connections still
  // refuse at the cap instead of evicting.
  bool EvictInboundPeerFor(const std::string& newAddress);
  // Re-validate orphans waiting on `parentTxid` now that the
  // parent has arrived (Milestone 15).
  void ProcessOrphansFor(const Hash256& parentTxid);
  void SendVersion(const std::string& peerId);
  void RequestSync(const std::string& peerId);
  void RelayTxInv(const Hash256& txid, const std::string& exceptId);
  void RelayBlock(const Block& block, const std::string& exceptId);
  // Full block acceptance shared by block/cmpctblock/blocktxn paths.
  // Returns false + `error` on rejection; `punish` says whether the
  // sender earned misbehavior points.
  bool AcceptBlock(const Block& block, std::string& error, bool& punish);
  // A block extending the tip: validate + connect + persist.
  bool AcceptBlockDirect(const Block& block, int height, std::string& error,
                         bool& punish);
  // A block on a side branch: disconnect the tip back to the fork
  // point, validate + connect the side branch, and adopt it only if
  // it has more work (a reorg). Otherwise the block is stored in
  // the index as an inactive side branch.
  bool AcceptBlockReorg(const Block& block, int height, std::string& error,
                        bool& punish);
  // Undo record for a block: the in-memory map first, then the
  // store's undo table (for blocks accepted before a restart).
  bool GetUndoForBlock(const Hash256& hash, BlockUndo& undo,
                       std::string& error);
  void SendGetHeaders(const std::string& peerId, const Hash256& hashStop);
  // Request a full block (getdata) / missing block transactions
  // (getblocktxn) from a peer, and record the request on the peer
  // for the block-stalling disconnect (Milestone 15).
  void SendBlockRequest(const std::string& peerId, const Hash256& hash);
  void SendBlockTxnRequest(const std::string& peerId, const Hash256& hash,
                           const std::vector<uint32_t>& indexes);

  ChainState chain_;
  CoinsView utxo_;
  Mempool mempool_;
  // Orphan transactions: validated when their parent arrives,
  // never relayed (Milestone 15).
  OrphanPool orphans_;
  FeeEstimator feeEstimator_;
  uint32_t lastExpirySweep_ = 0;  // last mempool expiry sweep (seconds)
  int maxPeers_ = 125;            // spec section 11
  // Outbound dialing (Milestone 18): at most this many outbound
  // peers (default 10). listenPort_ is set by Listen() so the
  // dial loop can skip this node's own address.
  // lastDialAttempt_ throttles TryDialPeers to one pass per
  // dialIntervalSeconds_ (default 5 s).
  int maxOutboundPeers_ = 10;
  uint16_t listenPort_ = 0;
  uint32_t lastDialAttempt_ = 0;
  uint32_t dialIntervalSeconds_ = 5;
  // Last DNS-seed query (Now() seconds, stamped by
  // QueryDnsSeeds). The dial loop re-queries seeds only when the
  // address book is empty and this long has passed.
  uint32_t lastSeedQuery_ = 0;
  AddressManager addrman_;
  std::vector<std::string> dnsSeeds_;
  BanManager bans_;
  std::unordered_map<std::string, Peer> peers_;
  // Full blocks this node holds (for getdata / cmpctblock serving).
  std::unordered_map<Hash256, Block, Hash256Hash> knownBlocks_;
  // Undo records for blocks accepted since startup (the store's
  // undo table covers older blocks). Needed to roll the UTXO set
  // back during a reorg.
  std::unordered_map<Hash256, BlockUndo, Hash256Hash> undo_;
  std::unordered_map<Hash256, BlockHeader, Hash256Hash> knownHeaders_;
  std::unordered_set<Hash256, Hash256Hash> requestedBlocks_;
  struct PendingCompact {
    CompactBlock cb;
    std::string peerId;
    uint32_t receivedAt = 0;  // Node::Now() seconds; expires in Poll()
    // getblocktxn retries used for this compact block (Milestone
    // 15): one retry, then the full-block getdata fallback.
    int retries = 0;
  };
  std::unordered_map<Hash256, PendingCompact, Hash256Hash> pendingCompact_;
  std::shared_ptr<ITransport> transport_;
  ChainStore* store_ = nullptr;
  std::string lastStoreError_;
  // Mempool persistence (Milestone 17). mempoolPath_ is
  // `<dataDir>/mempool.dat` in the node binary; empty in tests
  // that never configure persistence. mempoolLoaded_ is set by
  // LoadMempool once the startup load attempt has finished.
  std::string mempoolPath_;
  bool persistMempool_ = true;
  bool mempoolLoaded_ = false;
  MempoolLoadStats lastMempoolLoadStats_;
  uint32_t now_ = 0;
  uint64_t nodeNonce_ = 0;
  uint64_t compactNonce_ = 0;
};

}  // namespace oeeoo
