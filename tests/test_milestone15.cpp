// Tests: Milestone 15 — orphan transaction pool (Bitcoin Core's
// TxOrphanage model). Orphan-pool sections ONLY: bounds, expiry,
// parent-arrival lookup, purge on disconnect, byte-cap eviction,
// duplicate / oversized rejection. Node-level wiring (when a
// "missing input" failure becomes an orphan, re-validation when a
// parent arrives) belongs to the node sections the integration
// agent appends later.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "consensus/amount.h"
#include "consensus/consensus.h"
#include "crypto/ecdsa.h"
#include "mempool/orphan.h"
#include "script/script.h"
#include "test_harness.h"
#include "transactions/sighash.h"

using namespace oeeoo;

namespace {
constexpr uint32_t kNow = 1800000000;

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
}
