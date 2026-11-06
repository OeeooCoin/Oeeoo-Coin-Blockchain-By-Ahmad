// Tests: Milestone 15 — orphan transaction pool (Bitcoin Core's
// TxOrphanage model). Orphan-pool sections ONLY: bounds, expiry,
// parent-arrival lookup, purge on disconnect, byte-cap eviction,
// duplicate / oversized rejection. Node-level wiring (when a
// "missing input" failure becomes an orphan, re-validation when a
// parent arrives) belongs to the node sections the integration
// agent appends later.
#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "consensus/amount.h"
#include "consensus/consensus.h"
#include "crypto/ecdsa.h"
#include "mempool/orphan.h"
#include "mining/miner.h"
#include "network/compact.h"
#include "network/node.h"
#include "network/protocol.h"
#include "network/transport.h"
#include "script/script.h"
#include "test_harness.h"
#include "transactions/sighash.h"

using namespace oeeoo;

namespace {
constexpr uint32_t kNow = 1800000000;
constexpr uint32_t kTime = 1700000000;
constexpr uint32_t kTestBits = 0x207fffff;  // easy test-only difficulty

KeyPair g_key;
bool g_keysReady = false;

void EnsureKeys() {
  if (!g_keysReady) {
    CHECK(GenerateKeyPair(g_key));
    g_keysReady = true;
  }
}

std::vector<unsigned char> Spk() {
  return CreateP2PKHScriptPubKey(PubKeyHash160(g_key.pubkey));
}

// A distinct fake parent hash per index (bytes 0-1 encode `i`).
Hash256 PrevHash(int i) {
  Hash256 h{};
  h[0] = static_cast<unsigned char>(i & 0xff);
  h[1] = static_cast<unsigned char>((i >> 8) & 0xff);
  h[2] = 0xaa;
  return h;
}

std::vector<unsigned char> SignInput(const Transaction& tx, std::size_t nIn) {
  Hash256 h;
  CHECK(LegacySighash(tx, nIn, Spk(), kSigHashAll, h));
  std::vector<unsigned char> sig = SignHash(h, g_key.privkey);
  CHECK(!sig.empty());
  sig.push_back(static_cast<unsigned char>(kSigHashAll));
  return sig;
}

// A signed one-input spend of output `index` of `prev`, paying
// `sendValue` to our own P2PKH. The parent is deliberately unknown
// (no UTXO entry): that is what makes the spend an orphan.
Transaction SpendIdx(const Hash256& prev, uint32_t index,
                     Amount sendValue) {
  Transaction tx;
  tx.version = 1;
  TxIn in;
  in.prevTxHash = prev;
  in.prevOutIndex = index;
  in.sequence = 0xffffffff;
  tx.vin.push_back(in);
  TxOut out;
  out.value = sendValue;
  out.scriptPubKey = Spk();
  tx.vout.push_back(out);
  tx.lockTime = 0;
  tx.vin[0].scriptSig =
      CreateP2PKHScriptSig(SignInput(tx, 0), g_key.pubkey);
  return tx;
}

Transaction Spend(const Hash256& prev, Amount sendValue) {
  return SpendIdx(prev, 0, sendValue);
}

// A distinct orphan spending output 0 of PrevHash(i).
Transaction OrphanTx(int i, Amount value = 49 * COIN) {
  return Spend(PrevHash(i), value);
}

bool AddOk(OrphanPool& pool, const Transaction& tx,
           const std::string& peer, uint32_t now = kNow) {
  std::string error;
  bool ok = pool.Add(tx, peer, now, error);
  if (!ok) std::printf("  unexpected orphan rejection: %s\n", error.c_str());
  return ok;
}

// ---- Node-level helpers (appended for the M15 integration tests) ----

Hash256 TxIdWith(unsigned char fill) {
  Hash256 h;
  h.fill(fill);
  return h;
}

Block MineGenesis() {
  Block genesis;
  genesis.header.nVersion = 1;
  genesis.header.nTime = kTime;
  genesis.header.nBits = kTestBits;
  genesis.vtx = {CreateCoinbase(0, Spk(), 0, "THINK ABOUT GOD?")};
  genesis.header.hashMerkleRoot = genesis.ComputeMerkleRoot();
  CHECK(MineBlock(genesis, 0, "THINK ABOUT GOD?"));
  return genesis;
}

void Fund(Node& node, const Hash256& txid, Amount value) {
  Coin coin;
  coin.out.value = value;
  coin.out.scriptPubKey = Spk();
  coin.height = 0;
  coin.blockTime = kTime;
  coin.isCoinbase = false;
  CHECK(node.Utxo().AddCoin({txid, 0}, coin));
}

std::unique_ptr<Node> MakeNode(const std::string& loopbackName) {
  auto node = std::make_unique<Node>(kTestBits);
  node->SetTime(kNow);
  node->SetTransport(std::make_shared<LoopbackTransport>(loopbackName));
  return node;
}

// Pump messages between nodes until `done()` or `rounds` run out.
template <typename F>
void Pump(const std::vector<Node*>& nodes, F done, int rounds = 30) {
  for (int i = 0; i < rounds; ++i) {
    for (Node* n : nodes) n->Poll();
    if (done()) return;
  }
}

// Handshake a bare loopback transport against a victim node. On
// success the attacker is one more handshook peer of the victim.
bool HandshakeAttacker(LoopbackTransport& attacker, Node& victim,
                       const std::string& victimAddr, uint64_t nonce,
                       std::string& attackerPeerId) {
  const std::size_t before = victim.HandshookPeerCount();
  if (!attacker.Connect(victimAddr, attackerPeerId)) return false;
  VersionPayload v;
  v.version = kProtocolVersion;
  v.nonce = nonce;
  if (!attacker.Send(attackerPeerId,
                     SerializeMessage({"version", SerializeVersion(v)}))) {
    return false;
  }
  victim.Poll(0);
  TransportMessage m;
  if (!attacker.Poll(m, 0)) return false;  // victim's version
  if (!attacker.Poll(m, 0)) return false;  // victim's verack
  if (!attacker.Send(attackerPeerId,
                     SerializeMessage({"verack", {}}))) {
    return false;
  }
  victim.Poll(0);
  return victim.HandshookPeerCount() == before + 1;
}

// A block that fails proof of work (impossible target): +100
// misbehavior, i.e. an instant ban.
Block BadBlock(const Hash256& parent) {
  Block bad;
  bad.header.nVersion = 1;
  bad.header.hashPrevBlock = parent;
  bad.header.nTime = kTime + 600;
  bad.header.nBits = 0x02003456;  // target 52: no hash can pass
  bad.vtx = {CreateCoinbase(1, Spk(), 50 * COIN)};
  bad.header.hashMerkleRoot = bad.ComputeMerkleRoot();
  return bad;
}

// Drain a bare transport's inbound queue into parsed messages
// (pings and other traffic included — callers filter by command).
std::vector<Message> DrainMessages(LoopbackTransport& transport) {
  std::vector<Message> out;
  TransportMessage m;
  while (transport.Poll(m, 0)) {
    Message msg;
    std::size_t consumed = 0;
    if (ParseMessage(m.bytes, 0, msg, consumed)) out.push_back(msg);
  }
  return out;
}

int CountCommand(const std::vector<Message>& msgs,
                 const std::string& command) {
  int n = 0;
  for (const Message& m : msgs) {
    if (m.command == command) ++n;
  }
  return n;
}

// True when any drained "inv" announces `txid` as a transaction.
bool HasInvFor(const std::vector<Message>& msgs, const Hash256& txid) {
  for (const Message& m : msgs) {
    if (m.command != "inv") continue;
    std::vector<InventoryItem> items;
    if (!ParseInventory(m.payload, items)) continue;
    for (const InventoryItem& item : items) {
      if (item.type == kInvTx && item.hash == txid) return true;
    }
  }
  return false;
}

// A mined block with a coinbase + `extraCount` spends of distinct
// unknown parents (so the victim's mempool holds none of them).
// The block is a child of `parent` at `time`.
Block MineBlockWithUnknownTxs(const Hash256& parent, uint32_t time,
                              int extraCount, int firstParentIdx) {
  Block block;
  block.header.nVersion = 1;
  block.header.hashPrevBlock = parent;
  block.header.nTime = time;
  block.header.nBits = kTestBits;
  block.vtx = {CreateCoinbase(1, Spk(), 50 * COIN)};
  for (int i = 0; i < extraCount; ++i) {
    block.vtx.push_back(Spend(PrevHash(firstParentIdx + i), 49 * COIN));
  }
  block.header.hashMerkleRoot = block.ComputeMerkleRoot();
  CHECK(MineBlock(block, 1, ""));
  return block;
}

// True when a node currently has a peer with this address
// (inbound peers are keyed by their remote address).
bool HasPeerAddress(const Node& node, const std::string& address) {
  for (const Peer& p : node.GetPeers()) {
    if (p.address == address) return true;
  }
  return false;
}
}  // namespace

void TestMilestone15() {
  EnsureKeys();

  test::BeginSection("m15: orphan pool bounds: 100 max, 101st evicts oldest");
  {
    OrphanPool pool;
    CHECK(pool.MaxOrphans() == OrphanPool::kMaxOrphans);
    CHECK(pool.MaxOrphans() == 100);
    CHECK(pool.MaxBytes() == OrphanPool::kMaxOrphanBytes);
    std::vector<Hash256> ids;
    for (int i = 0; i < 101; ++i) {
      Transaction tx = OrphanTx(i);
      ids.push_back(tx.GetTxId());
      CHECK(AddOk(pool, tx, "peer1", kNow + static_cast<uint32_t>(i)));
    }
    CHECK(pool.Size() == 100);
    CHECK(!pool.Contains(ids[0]));  // oldest (added at kNow) evicted
    CHECK(pool.Contains(ids[1]));   // second-oldest kept
    CHECK(pool.Contains(ids[100]));  // newest present
    CHECK(pool.Bytes() > 0);
    CHECK(pool.Bytes() <= pool.MaxBytes());
  }

  test::BeginSection("m15: orphan pool expiry at 20 minutes");
  {
    OrphanPool pool;
    CHECK(OrphanPool::kOrphanExpirySeconds == 20 * 60);
    Transaction a = OrphanTx(400);
    Transaction b = OrphanTx(401);
    CHECK(AddOk(pool, a, "peer1", kNow));
    CHECK(AddOk(pool, b, "peer1", kNow + 600));  // 10 minutes later
    // 19 minutes 59 seconds after `a` was added: both alive.
    pool.RemoveExpired(kNow + 20 * 60 - 1);
    CHECK(pool.Contains(a.GetTxId()));
    CHECK(pool.Contains(b.GetTxId()));
    // Exactly 20 minutes after `a`: `a` expires, `b` (added later)
    // survives.
    pool.RemoveExpired(kNow + 20 * 60);
    CHECK(!pool.Contains(a.GetTxId()));
    CHECK(pool.Contains(b.GetTxId()));
    // 30 minutes after `a` (20 after `b`): `b` expires too.
    pool.RemoveExpired(kNow + 20 * 60 + 600);
    CHECK(!pool.Contains(b.GetTxId()));
    CHECK(pool.Size() == 0);
    CHECK(pool.Bytes() == 0);
  }

  test::BeginSection("m15: parent-arrival lookup via the prevout index");
  {
    OrphanPool pool;
    // Two orphans spending different outputs of the same parent.
    Transaction x = SpendIdx(PrevHash(500), 0, 49 * COIN);
    Transaction y = SpendIdx(PrevHash(500), 1, 48 * COIN);
    CHECK(AddOk(pool, x, "peer1"));
    CHECK(AddOk(pool, y, "peer2"));
    std::vector<Hash256> waiting =
        pool.GetOrphansDependingOn(PrevHash(500));
    CHECK(waiting.size() == 2);
    CHECK(std::find(waiting.begin(), waiting.end(), x.GetTxId()) !=
          waiting.end());
    CHECK(std::find(waiting.begin(), waiting.end(), y.GetTxId()) !=
          waiting.end());
    // No orphan waits on an unrelated parent.
    CHECK(pool.GetOrphansDependingOn(PrevHash(501)).empty());
    // Removing one leaves the other indexed.
    CHECK(pool.Remove(x.GetTxId()));
    waiting = pool.GetOrphansDependingOn(PrevHash(500));
    CHECK(waiting.size() == 1);
    CHECK(waiting[0] == y.GetTxId());
    // Removing the last one clears the index row entirely.
    CHECK(pool.Remove(y.GetTxId()));
    CHECK(pool.GetOrphansDependingOn(PrevHash(500)).empty());
  }

  test::BeginSection(
      "m15: purge on disconnect removes only that peer's orphans");
  {
    OrphanPool pool;
    Transaction a1 = OrphanTx(600);
    Transaction a2 = OrphanTx(601);
    Transaction b1 = OrphanTx(602);
    CHECK(AddOk(pool, a1, "peerA"));
    CHECK(AddOk(pool, a2, "peerA"));
    CHECK(AddOk(pool, b1, "peerB"));
    CHECK(pool.PurgePeer("peerC") == 0);  // unknown peer: nothing
    CHECK(pool.PurgePeer("peerA") == 2);
    CHECK(pool.Size() == 1);
    CHECK(pool.Contains(b1.GetTxId()));  // peerB's orphan kept
    CHECK(!pool.Contains(a1.GetTxId()));
    CHECK(!pool.Contains(a2.GetTxId()));
    // The purged orphans are gone from the prevout index too.
    CHECK(pool.GetOrphansDependingOn(PrevHash(600)).empty());
    CHECK(pool.GetOrphansDependingOn(PrevHash(602)).size() == 1);
    // A second purge of the same peer removes nothing.
    CHECK(pool.PurgePeer("peerA") == 0);
  }

  test::BeginSection("m15: byte-cap eviction and oversized rejection");
  {
    // Byte-cap eviction on a small pool: a signed spend is ~193
    // bytes, so a 1000-byte cap holds about five and must evict
    // the oldest as more arrive.
    OrphanPool pool(1000, 1000);
    Transaction first = OrphanTx(200);
    CHECK(AddOk(pool, first, "peer1", kNow));
    Transaction last = first;
    for (int i = 201; i < 211; ++i) {
      last = OrphanTx(i);
      // Distinct added times so "oldest" is the earliest added.
      CHECK(AddOk(pool, last, "peer1",
                  kNow + static_cast<uint32_t>(i - 200)));
    }
    CHECK(pool.Bytes() <= 1000);
    CHECK(!pool.Contains(first.GetTxId()));  // evicted by bytes
    CHECK(pool.Contains(last.GetTxId()));    // newest never evicted
    // A transaction larger than the byte cap alone is rejected.
    OrphanPool tiny(100, 100);
    std::string error;
    CHECK(!tiny.Add(OrphanTx(300), "peer1", kNow, error));
    CHECK(!error.empty());
    CHECK(tiny.Size() == 0);
    // A transaction over MAX_BLOCK_SIZE (the per-transaction limit
    // the codebase already uses) is rejected, with the pool
    // untouched.
    OrphanPool big;
    Transaction oversized = OrphanTx(800);
    oversized.vin[0].scriptSig.assign(
        static_cast<std::size_t>(MAX_BLOCK_SIZE), 0x00);
    CHECK(oversized.Serialize().size() >
          static_cast<std::size_t>(MAX_BLOCK_SIZE));
    CHECK(!big.Add(oversized, "peer1", kNow, error));
    CHECK(error.find("size") != std::string::npos);
    CHECK(big.Size() == 0);
    // Exactly MAX_BLOCK_SIZE is the limit's edge, not over it: the
    // pool accepts it. (The scriptSig length VarInt grows from 1
    // to 5 bytes for a ~1 MB sig, hence the -4.)
    Transaction edge = OrphanTx(801);
    const std::size_t baseSize = edge.Serialize().size();
    const std::size_t oldSig = edge.vin[0].scriptSig.size();
    edge.vin[0].scriptSig.assign(
        static_cast<std::size_t>(MAX_BLOCK_SIZE) - baseSize + oldSig - 4,
        0x00);
    CHECK(edge.Serialize().size() ==
          static_cast<std::size_t>(MAX_BLOCK_SIZE));
    CHECK(AddOk(big, edge, "peer1"));
    CHECK(big.Size() == 1);
  }

  test::BeginSection("m15: duplicate add and get/remove round-trip");
  {
    OrphanPool pool;
    Transaction tx = OrphanTx(900);
    CHECK(AddOk(pool, tx, "peer1"));
    std::string error;
    CHECK(!pool.Add(tx, "peer2", kNow, error));  // duplicate
    CHECK(error.find("already") != std::string::npos);
    CHECK(pool.Size() == 1);
    Transaction got;
    CHECK(pool.Get(tx.GetTxId(), got));
    CHECK(got.GetTxId() == tx.GetTxId());
    Transaction missing;
    CHECK(!pool.Get(PrevHash(999), missing));
    CHECK(!pool.Contains(PrevHash(999)));
    CHECK(!pool.Remove(PrevHash(999)));  // removing an absent tx
    CHECK(pool.Remove(tx.GetTxId()));
    CHECK(!pool.Contains(tx.GetTxId()));
    CHECK(pool.Size() == 0);
    CHECK(pool.Bytes() == 0);
  }

  test::BeginSection("m15: inbound ban survives reconnect (loopback)");
  {
    auto victim = MakeNode("loopback:ban-victim");
    const Block genesis = MineGenesis();
    std::string error;
    CHECK(victim->BroadcastBlock(genesis, error));
    LoopbackTransport attacker("loopback:ban-attacker");
    std::string attackerPeerId;
    CHECK(HandshakeAttacker(attacker, *victim, "loopback:ban-victim",
                            111111ULL, attackerPeerId));
    CHECK(victim->PeerCount() == 1);
    // The attacker sends an invalid block: +100, an instant ban.
    Block bad = BadBlock(genesis.GetHash());
    CHECK(attacker.Send(attackerPeerId,
                        SerializeMessage({"block", bad.Serialize()})));
    victim->Poll(0);
    CHECK(victim->Bans().Size() == 1);
    CHECK(victim->PeerCount() == 0);  // banned peer disconnected
    // Reconnect from the same address: the ban is keyed by the
    // address itself for loopback names (BanManager::BanKey leaves
    // "loopback:<name>" as-is), so the new connection is refused
    // before any handshake happens.
    std::string attackerPeerId2;
    CHECK(attacker.Connect("loopback:ban-victim", attackerPeerId2));
    VersionPayload v;
    v.version = kProtocolVersion;
    v.nonce = 222222ULL;
    CHECK(attacker.Send(attackerPeerId2,
                        SerializeMessage({"version", SerializeVersion(v)})));
    victim->Poll(0);
    CHECK(victim->PeerCount() == 0);  // still refused
    CHECK(victim->Bans().Size() == 1);
  }

  test::BeginSection("m15: inbound ban survives reconnect (real TCP)");
  {
    // The accept() path must record the real IP, not the
    // ephemeral transport id.
    auto h = std::make_unique<Node>(kTestBits);
    h->SetTransport(std::make_shared<TcpTransport>());
    CHECK(h->Listen(46479));
    const Block genesis = MineGenesis();
    std::string error;
    CHECK(h->BroadcastBlock(genesis, error));
    auto i = std::make_unique<Node>(kTestBits);
    i->SetTransport(std::make_shared<TcpTransport>());
    CHECK(i->ConnectTo("127.0.0.1:46479"));
    bool connected = false;
    for (int round = 0; round < 40 && !connected; ++round) {
      h->Poll(50);
      i->Poll(50);
      connected = h->PeerCount() == 1 && i->PeerCount() == 1;
    }
    CHECK(connected);
    // The listening node's inbound peer carries the real IP
    // recorded at accept() time — "127.0.0.1:<ephemeral port>",
    // never a "tcp:N" transport id.
    bool sawRealIp = false;
    for (const Peer& p : h->GetPeers()) {
      if (!p.outbound && p.address.rfind("127.0.0.1:", 0) == 0) {
        sawRealIp = true;
      }
    }
    CHECK(sawRealIp);
    // An invalid block from that peer earns a ban keyed by the
    // IP alone (Milestone 16): the port is ephemeral, so the ban
    // must survive a reconnect from a NEW ephemeral port.
    Block bad = BadBlock(genesis.GetHash());
    CHECK(i->SendRawToAddress("127.0.0.1:46479",
                              {"block", bad.Serialize()}));
    bool banned = false;
    for (int round = 0; round < 40 && !banned; ++round) {
      h->Poll(50);
      i->Poll(50);
      banned = h->Bans().Size() == 1;
    }
    CHECK(banned);
    CHECK(h->PeerCount() == 0);
    // Reconnect from a new ephemeral port: a fresh node dialing
    // the same listener gets a different source port, so under
    // the old "ip:port" keying this would be a fresh key and the
    // ban would NOT hold. With the IP-only key it is refused
    // before registration, exactly like the first ban.
    auto i2 = std::make_unique<Node>(kTestBits);
    i2->SetTransport(std::make_shared<TcpTransport>());
    CHECK(i2->ConnectTo("127.0.0.1:46479"));
    for (int round = 0; round < 40; ++round) {
      h->Poll(50);
      i2->Poll(50);
    }
    CHECK(h->PeerCount() == 0);  // still refused, new port or not
    CHECK(h->Bans().Size() == 1);
  }

  test::BeginSection(
      "m15: eviction picks the worst peer and protects the best");
  {
    auto victim = MakeNode("loopback:ev-victim");
    victim->SetMaxPeers(3);
    const Block genesis = MineGenesis();
    std::string error;
    CHECK(victim->BroadcastBlock(genesis, error));
    const Hash256 kA = TxIdWith(0xE1);
    Fund(*victim, kA, 50 * COIN);
    // Three handshook inbound peers, connected 10 s apart.
    LoopbackTransport a("loopback:ev-a");
    LoopbackTransport b("loopback:ev-b");
    LoopbackTransport c("loopback:ev-c");
    std::string idA, idB, idC;
    victim->SetTime(kNow);
    CHECK(HandshakeAttacker(a, *victim, "loopback:ev-victim", 101ULL, idA));
    victim->SetTime(kNow + 10);
    CHECK(HandshakeAttacker(b, *victim, "loopback:ev-victim", 102ULL, idB));
    victim->SetTime(kNow + 20);
    CHECK(HandshakeAttacker(c, *victim, "loopback:ev-victim", 103ULL, idC));
    CHECK(victim->PeerCount() == 3);
    // B sends a valid transaction at kNow+20: a recent tx sender
    // is protected. A is the longest-connected peer: protected.
    // C is the most recently connected peer with no recent
    // activity: the eviction candidate. (All three share the
    // loopback netgroup "", so the netgroup ranking ties and the
    // most-recently-connected tie-break decides.)
    Transaction tx = Spend(kA, 49 * COIN);
    CHECK(b.Send(idB, SerializeMessage({"tx", tx.Serialize()})));
    victim->Poll(0);
    CHECK(victim->HasTx(tx.GetTxId()));
    // A fourth inbound peer arrives at the cap: C is evicted
    // while D registers. (The handshake is done by hand here:
    // the shared helper's count check would not survive the
    // eviction happening mid-handshake.)
    victim->SetTime(kNow + 30);
    LoopbackTransport d("loopback:ev-d");
    std::string idD;
    CHECK(d.Connect("loopback:ev-victim", idD));
    VersionPayload v;
    v.version = kProtocolVersion;
    v.nonce = 104ULL;
    CHECK(d.Send(idD, SerializeMessage({"version", SerializeVersion(v)})));
    victim->Poll(0);
    CHECK(victim->PeerCount() == 3);  // cap held
    CHECK(!HasPeerAddress(*victim, "loopback:loopback:ev-c"));  // evicted
    CHECK(HasPeerAddress(*victim, "loopback:loopback:ev-d"));   // newcomer
    TransportMessage m;
    CHECK(d.Poll(m, 0));  // victim's version
    CHECK(d.Poll(m, 0));  // victim's verack
    CHECK(d.Send(idD, SerializeMessage({"verack", {}})));
    victim->Poll(0);
    CHECK(victim->HandshookPeerCount() == 3);
    CHECK(HasPeerAddress(*victim, "loopback:loopback:ev-a"));  // protected
    CHECK(HasPeerAddress(*victim, "loopback:loopback:ev-b"));  // protected
  }

  test::BeginSection("m15: compact in-flight limit");
  {
    auto victim = MakeNode("loopback:cf-victim");
    const Block genesis = MineGenesis();
    std::string error;
    CHECK(victim->BroadcastBlock(genesis, error));
    LoopbackTransport attacker("loopback:cf-attacker");
    std::string attackerPeerId;
    CHECK(HandshakeAttacker(attacker, *victim, "loopback:cf-victim",
                            333333ULL, attackerPeerId));
    (void)DrainMessages(attacker);  // clear handshake traffic
    // Six distinct mined compact blocks, each missing one tx the
    // victim has never seen. Only five pending entries may exist.
    for (int i = 0; i < 6; ++i) {
      Block block = MineBlockWithUnknownTxs(genesis.GetHash(), kTime + 600 + i,
                                            1, 3000 + i);
      CompactBlock cb = MakeCompactBlock(block, static_cast<uint64_t>(i));
      CHECK(attacker.Send(attackerPeerId,
                          SerializeMessage(
                              {"cmpctblock", SerializeCompactBlock(cb)})));
    }
    victim->Poll(0);
    CHECK(victim->PendingCompactCount() == 5);
    // Exactly five getblocktxn requests went out: the sixth
    // cmpctblock was ignored (and the peer earned +20).
    std::vector<Message> replies = DrainMessages(attacker);
    CHECK(CountCommand(replies, "getblocktxn") == 5);
  }

  test::BeginSection("m15: getblocktxn depth limit");
  {
    auto victim = MakeNode("loopback:depth-victim");
    const Block genesis = MineGenesis();
    std::string error;
    CHECK(victim->BroadcastBlock(genesis, error));
    LoopbackTransport attacker("loopback:depth-attacker");
    std::string attackerPeerId;
    CHECK(HandshakeAttacker(attacker, *victim, "loopback:depth-victim",
                            444444ULL, attackerPeerId));
    (void)DrainMessages(attacker);
    // A compact block missing 11 transactions: past the
    // getblocktxn depth limit of 10, so the victim asks for the
    // full block instead — and creates no pending entry.
    Block block = MineBlockWithUnknownTxs(genesis.GetHash(), kTime + 600,
                                          11, 4000);
    CompactBlock cb = MakeCompactBlock(block, 0);
    CHECK(attacker.Send(attackerPeerId,
                        SerializeMessage(
                            {"cmpctblock", SerializeCompactBlock(cb)})));
    victim->Poll(0);
    CHECK(victim->PendingCompactCount() == 0);
    std::vector<Message> replies = DrainMessages(attacker);
    CHECK(CountCommand(replies, "getblocktxn") == 0);
    CHECK(CountCommand(replies, "getdata") == 1);
  }

  test::BeginSection("m15: getblocktxn retry then full block");
  {
    auto victim = MakeNode("loopback:retry-victim");
    const Block genesis = MineGenesis();
    std::string error;
    CHECK(victim->BroadcastBlock(genesis, error));
    LoopbackTransport attacker("loopback:retry-attacker");
    std::string attackerPeerId;
    CHECK(HandshakeAttacker(attacker, *victim, "loopback:retry-victim",
                            555555ULL, attackerPeerId));
    (void)DrainMessages(attacker);
    // One missing transaction: the victim asks for it.
    Block block = MineBlockWithUnknownTxs(genesis.GetHash(), kTime + 600,
                                          1, 5000);
    const Hash256 blockHash = block.GetHash();
    CompactBlock cb = MakeCompactBlock(block, 0);
    CHECK(attacker.Send(attackerPeerId,
                        SerializeMessage(
                            {"cmpctblock", SerializeCompactBlock(cb)})));
    victim->Poll(0);
    CHECK(victim->PendingCompactCount() == 1);
    std::vector<Message> replies = DrainMessages(attacker);
    CHECK(CountCommand(replies, "getblocktxn") == 1);
    CHECK(CountCommand(replies, "getdata") == 0);
    // First blocktxn answers with nothing: reconstruction still
    // fails, so the victim retries getblocktxn once — keeping the
    // pending entry.
    CHECK(attacker.Send(attackerPeerId,
                        SerializeMessage(
                            {"blocktxn",
                             SerializeBlockTxn(blockHash, {})})));
    victim->Poll(0);
    CHECK(victim->PendingCompactCount() == 1);
    replies = DrainMessages(attacker);
    CHECK(CountCommand(replies, "getblocktxn") == 1);  // the retry
    CHECK(CountCommand(replies, "getdata") == 0);
    // Second empty blocktxn: the retry is spent, so the victim
    // drops the pending entry and requests the full block.
    CHECK(attacker.Send(attackerPeerId,
                        SerializeMessage(
                            {"blocktxn",
                             SerializeBlockTxn(blockHash, {})})));
    victim->Poll(0);
    CHECK(victim->PendingCompactCount() == 0);
    replies = DrainMessages(attacker);
    CHECK(CountCommand(replies, "getblocktxn") == 0);
    CHECK(CountCommand(replies, "getdata") == 1);
  }

  test::BeginSection("m15: block stalling disconnect");
  {
    // A peer that never answers a block request is disconnected
    // after 2 s — without misbehavior points.
    auto victim = MakeNode("loopback:stall-victim");
    const Block genesis = MineGenesis();
    std::string error;
    CHECK(victim->BroadcastBlock(genesis, error));
    LoopbackTransport attacker("loopback:stall-attacker");
    std::string attackerPeerId;
    CHECK(HandshakeAttacker(attacker, *victim, "loopback:stall-victim",
                            666666ULL, attackerPeerId));
    (void)DrainMessages(attacker);
    Block block = MineBlockWithUnknownTxs(genesis.GetHash(), kTime + 600,
                                          1, 6000);
    CompactBlock cb = MakeCompactBlock(block, 0);
    CHECK(attacker.Send(attackerPeerId,
                        SerializeMessage(
                            {"cmpctblock", SerializeCompactBlock(cb)})));
    victim->Poll(0);
    CHECK(victim->PendingCompactCount() == 1);
    CHECK(victim->PeerCount() == 1);
    // Frozen time alone does not stall: the request went out at
    // kNow and the clock has not moved.
    victim->Poll(0);
    CHECK(victim->PeerCount() == 1);
    // 2 s later with no answer: disconnected.
    victim->SetTime(kNow + 2);
    victim->Poll(0);
    CHECK(victim->PeerCount() == 0);
    // A peer that answers in time is NOT disconnected.
    auto victim2 = MakeNode("loopback:stall-victim2");
    CHECK(victim2->BroadcastBlock(genesis, error));
    const Hash256 kA = TxIdWith(0x57);
    Fund(*victim2, kA, 50 * COIN);
    LoopbackTransport attacker2("loopback:stall-attacker2");
    std::string attacker2PeerId;
    CHECK(HandshakeAttacker(attacker2, *victim2, "loopback:stall-victim2",
                            777777ULL, attacker2PeerId));
    (void)DrainMessages(attacker2);
    // A block with one real transaction the victim has never seen.
    Transaction spend = Spend(kA, 49 * COIN);
    Block block2;
    block2.header.nVersion = 1;
    block2.header.hashPrevBlock = genesis.GetHash();
    block2.header.nTime = kTime + 600;
    block2.header.nBits = kTestBits;
    block2.vtx = {CreateCoinbase(1, Spk(), 50 * COIN), spend};
    block2.header.hashMerkleRoot = block2.ComputeMerkleRoot();
    CHECK(MineBlock(block2, 1, ""));
    CompactBlock cb2 = MakeCompactBlock(block2, 0);
    CHECK(attacker2.Send(attacker2PeerId,
                         SerializeMessage(
                             {"cmpctblock", SerializeCompactBlock(cb2)})));
    victim2->Poll(0);
    CHECK(victim2->PendingCompactCount() == 1);
    // Answer in time with the missing transaction: the block is
    // accepted and the stalling clock is cleared.
    CHECK(attacker2.Send(attacker2PeerId,
                         SerializeMessage(
                             {"blocktxn",
                              SerializeBlockTxn(block2.GetHash(),
                                                {{1, spend}})})));
    victim2->Poll(0);
    CHECK(victim2->TipHeight() == 1);
    CHECK(victim2->PendingCompactCount() == 0);
    victim2->SetTime(kNow + 10);
    victim2->Poll(0);
    CHECK(victim2->PeerCount() == 1);  // still connected
  }

  test::BeginSection("m15: orphan end-to-end via two nodes");
  {
    auto a = MakeNode("loopback:orph-a");
    auto b = MakeNode("loopback:orph-b");
    const Block genesis = MineGenesis();
    std::string error;
    CHECK(a->BroadcastBlock(genesis, error));
    CHECK(b->BroadcastBlock(genesis, error));
    const Hash256 kA = TxIdWith(0x6F);
    Fund(*a, kA, 50 * COIN);
    Fund(*b, kA, 50 * COIN);
    CHECK(a->ConnectTo("loopback:orph-b"));
    Pump({a.get(), b.get()},
         [&] {
           return a->HandshookPeerCount() == 1 && b->HandshookPeerCount() == 1;
         });
    CHECK(a->HandshookPeerCount() == 1);
    CHECK(b->HandshookPeerCount() == 1);
    // A watcher sees what B relays (and what it does not).
    LoopbackTransport watcher("loopback:orph-watcher");
    std::string watcherPeerId;
    CHECK(HandshakeAttacker(watcher, *b, "loopback:orph-b", 888888ULL,
                            watcherPeerId));
    (void)DrainMessages(watcher);
    // The child arrives before its parent: orphaned, not
    // mempooled, not relayed.
    Transaction parent = Spend(kA, 49 * COIN);
    Transaction child = Spend(parent.GetTxId(), 48 * COIN);
    CHECK(a->SendRawToAddress("loopback:orph-b",
                              {"tx", child.Serialize()}));
    Pump({a.get(), b.get()}, [&] { return b->OrphanCount() == 1; });
    CHECK(b->OrphanCount() == 1);
    CHECK(!b->HasTx(child.GetTxId()));
    CHECK(!HasInvFor(DrainMessages(watcher), child.GetTxId()));
    // The parent arrives in a block: the child validates, enters
    // the mempool, and is relayed. (The parent must arrive
    // confirmed — validation rejects unconfirmed inputs, so a
    // parent that only entered the mempool could never promote
    // its orphan.)
    Block block1;
    block1.header.nVersion = 1;
    block1.header.hashPrevBlock = genesis.GetHash();
    block1.header.nTime = kTime + 600;
    block1.header.nBits = kTestBits;
    block1.vtx = {CreateCoinbase(1, Spk(), 50 * COIN), parent};
    block1.header.hashMerkleRoot = block1.ComputeMerkleRoot();
    CHECK(MineBlock(block1, 1, ""));
    CHECK(a->BroadcastBlock(block1, error));
    Pump({a.get(), b.get()},
         [&] { return b->TipHeight() == 1 && b->HasTx(child.GetTxId()); });
    CHECK(b->TipHeight() == 1);
    CHECK(b->OrphanCount() == 0);
    CHECK(b->HasTx(child.GetTxId()));
    CHECK(!b->HasTx(parent.GetTxId()));  // confirmed, not mempooled
    // The relayed child reaches A (which now holds the parent's
    // output in its UTXO set) and the watcher saw its inv.
    Pump({a.get(), b.get()}, [&] { return a->HasTx(child.GetTxId()); });
    CHECK(a->HasTx(child.GetTxId()));
    CHECK(HasInvFor(DrainMessages(watcher), child.GetTxId()));
    // A remaining orphan from A is purged when A is disconnected
    // (here: banned for an invalid block).
    Transaction child2 = Spend(PrevHash(7777), 48 * COIN);
    CHECK(a->SendRawToAddress("loopback:orph-b",
                              {"tx", child2.Serialize()}));
    Pump({a.get(), b.get()}, [&] { return b->OrphanCount() == 1; });
    CHECK(b->OrphanCount() == 1);
    Block bad = BadBlock(block1.GetHash());
    CHECK(a->SendRawToAddress("loopback:orph-b",
                              {"block", bad.Serialize()}));
    Pump({a.get(), b.get()}, [&] { return b->Bans().Size() == 1; });
    CHECK(b->Bans().Size() == 1);
    CHECK(!HasPeerAddress(*b, "loopback:loopback:orph-a"));  // A is gone
    CHECK(b->PeerCount() == 1);  // only the watcher remains
    CHECK(b->OrphanCount() == 0);  // purged with the peer
  }

  test::BeginSection(
      "m15: addrman expiry and failure recording via the node");
  {
    // Old entries expire on the Poll sweep.
    auto node = MakeNode("loopback:am-node");
    node->AddPeerAddress({kNow - AddressManager::kAddrExpirySeconds - 1, 1,
                          "10.20.30.40", 64775});
    node->AddPeerAddress({kNow, 1, "10.20.30.41", 64775});
    CHECK(node->AddrMan().Size() == 2);
    node->Poll(0);  // the 60-second sweep runs (last sweep was 0)
    CHECK(node->AddrMan().Size() == 1);
    CHECK(node->AddrMan().Contains("10.20.30.41", 64775));
    CHECK(!node->AddrMan().Contains("10.20.30.40", 64775));
    // A failed ConnectTo records a failure; 3 failures drop the
    // entry.
    auto dialer = std::make_unique<Node>(kTestBits);
    dialer->SetTime(kNow);
    dialer->SetTransport(std::make_shared<TcpTransport>());
    dialer->AddPeerAddress({kNow, 1, "127.0.0.1", 1});
    CHECK(dialer->AddrMan().Contains("127.0.0.1", 1));
    CHECK(!dialer->ConnectTo("127.0.0.1:1"));  // port 1: nothing listens
    CHECK(dialer->AddrMan().Contains("127.0.0.1", 1));  // 1 failure
    CHECK(!dialer->ConnectTo("127.0.0.1:1"));
    CHECK(dialer->AddrMan().Contains("127.0.0.1", 1));  // 2 failures
    CHECK(!dialer->ConnectTo("127.0.0.1:1"));
    CHECK(!dialer->AddrMan().Contains("127.0.0.1", 1));  // 3: gone
    // A completed handshake records success: the failure count
    // resets, so one more failure after a success does not drop
    // the entry.
    auto h = std::make_unique<Node>(kTestBits);
    h->SetTime(kNow);
    auto hTransport = std::make_shared<TcpTransport>();
    h->SetTransport(hTransport);
    auto i = std::make_unique<Node>(kTestBits);
    i->SetTime(kNow);
    i->SetTransport(std::make_shared<TcpTransport>());
    i->AddPeerAddress({kNow, 1, "127.0.0.1", 46481});
    CHECK(!i->ConnectTo("127.0.0.1:46481"));  // failure 1
    CHECK(!i->ConnectTo("127.0.0.1:46481"));  // failure 2
    CHECK(i->AddrMan().Contains("127.0.0.1", 46481));
    // Now the listener starts: the next connection succeeds and
    // the handshake completes, resetting the failures.
    CHECK(h->Listen(46481));
    CHECK(i->ConnectTo("127.0.0.1:46481"));
    bool connected = false;
    for (int round = 0; round < 40 && !connected; ++round) {
      h->Poll(50);
      i->Poll(50);
      connected = h->PeerCount() == 1 && i->PeerCount() == 1;
    }
    CHECK(connected);
    CHECK(i->HandshookPeerCount() == 1);
    // The listener stops: one more failure must NOT drop the
    // entry (the handshake reset the count to 0).
    hTransport->Close();
    CHECK(!i->ConnectTo("127.0.0.1:46481"));  // failure 1 (after reset)
    CHECK(i->AddrMan().Contains("127.0.0.1", 46481));
    CHECK(!i->ConnectTo("127.0.0.1:46481"));  // failure 2
    CHECK(i->AddrMan().Contains("127.0.0.1", 46481));
    CHECK(!i->ConnectTo("127.0.0.1:46481"));  // failure 3: gone
    CHECK(!i->AddrMan().Contains("127.0.0.1", 46481));
  }
}
