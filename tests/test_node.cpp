// Tests: P2P node (Milestone 6) — handshake, tx relay, block relay,
// headers-first sync, compact-block missing-tx flow, misbehavior
// ban, and a real TCP handshake between two nodes.
#include <memory>
#include <string>
#include <vector>

#include "consensus/consensus.h"
#include "crypto/ecdsa.h"
#include "mining/miner.h"
#include "network/node.h"
#include "network/transport.h"
#include "script/script.h"
#include "test_harness.h"
#include "transactions/sighash.h"

using namespace oeeoo;

namespace {
constexpr uint32_t kTestBits = 0x207fffff;
constexpr uint32_t kTime = 1700000000;
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

Hash256 TxIdWith(unsigned char fill) {
  Hash256 h;
  h.fill(fill);
  return h;
}

// The shared genesis: coinbase value 0 (GetBlockSubsidy(0) == 0) and
// the genesis message as extra data so the scriptSig is valid.
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

Transaction MakeSpend(Node& /*node*/, const Hash256& prev, Amount sendValue) {
  Transaction tx;
  tx.version = 1;
  TxIn in;
  in.prevTxHash = prev;
  in.prevOutIndex = 0;
  in.sequence = 0xffffffff;
  tx.vin.push_back(in);
  TxOut out;
  out.value = sendValue;
  out.scriptPubKey = Spk();
  tx.vout.push_back(out);
  tx.lockTime = 0;
  Hash256 h;
  CHECK(LegacySighash(tx, 0, Spk(), kSigHashAll, h));
  std::vector<unsigned char> sig = SignHash(h, g_key.privkey);
  sig.push_back(static_cast<unsigned char>(kSigHashAll));
  tx.vin[0].scriptSig = CreateP2PKHScriptSig(sig, g_key.pubkey);
  return tx;
}

// Pump messages between nodes until `done()` or `rounds` run out.
template <typename F>
void Pump(std::vector<Node*> nodes, F done, int rounds = 30) {
  for (int i = 0; i < rounds; ++i) {
    for (Node* n : nodes) n->Poll();
    if (done()) return;
  }
}

std::unique_ptr<Node> MakeNode(const std::string& loopbackName) {
  auto node = std::make_unique<Node>(kTestBits);
  node->SetTime(kNow);
  node->SetTransport(std::make_shared<LoopbackTransport>(loopbackName));
  return node;
}
}  // namespace

void TestNode() {
  EnsureKeys();
  const Block genesis = MineGenesis();

  test::BeginSection("node: handshake + tx relay across 3 nodes");
  {
    auto a = MakeNode("loopback:a1");
    auto b = MakeNode("loopback:b1");
    auto c = MakeNode("loopback:c1");
    std::string error;
    CHECK(a->BroadcastBlock(genesis, error));
    CHECK(b->BroadcastBlock(genesis, error));
    CHECK(c->BroadcastBlock(genesis, error));
    CHECK(a->ConnectTo("loopback:b1"));
    CHECK(b->ConnectTo("loopback:c1"));
    Pump({a.get(), b.get(), c.get()},
         [&] { return a->HandshookPeerCount() == 1 &&
                      b->HandshookPeerCount() == 2 &&
                      c->HandshookPeerCount() == 1; });
    CHECK(a->HandshookPeerCount() == 1);
    CHECK(b->HandshookPeerCount() == 2);
    CHECK(c->HandshookPeerCount() == 1);

    const Hash256 kA = TxIdWith(0x41);
    Fund(*a, kA, 50 * COIN);
    Fund(*b, kA, 50 * COIN);
    Fund(*c, kA, 50 * COIN);
    Transaction tx = MakeSpend(*a, kA, 49 * COIN);
    CHECK(a->BroadcastTx(tx, error));
    CHECK(a->HasTx(tx.GetTxId()));
    Pump({a.get(), b.get(), c.get()},
         [&] { return b->HasTx(tx.GetTxId()) && c->HasTx(tx.GetTxId()); });
    CHECK(b->HasTx(tx.GetTxId()));
    CHECK(c->HasTx(tx.GetTxId()));  // relayed B -> C
  }

  test::BeginSection("node: block relay (compact) + mempool cleanup");
  {
    auto a = MakeNode("loopback:a2");
    auto b = MakeNode("loopback:b2");
    std::string error;
    CHECK(a->BroadcastBlock(genesis, error));
    CHECK(b->BroadcastBlock(genesis, error));
    CHECK(a->ConnectTo("loopback:b2"));
    Pump({a.get(), b.get()},
         [&] {
           return a->HandshookPeerCount() == 1 && b->HandshookPeerCount() == 1;
         });
    const Hash256 kA = TxIdWith(0x42);
    Fund(*a, kA, 50 * COIN);
    Fund(*b, kA, 50 * COIN);
    Transaction tx = MakeSpend(*a, kA, 49 * COIN);
    CHECK(a->BroadcastTx(tx, error));
    Pump({a.get(), b.get()}, [&] { return b->HasTx(tx.GetTxId()); });
    CHECK(b->HasTx(tx.GetTxId()));
    // A mines the tx into a block and broadcasts it.
    BlockTemplate templ = AssembleBlock(genesis.GetHash(), 1, kTime + 600,
                                        kTestBits, a->MempoolRef(), a->Utxo(),
                                        kTime, Spk());
    Block block = templ.block;
    CHECK(MineBlock(block, 1, ""));
    CHECK(a->BroadcastBlock(block, error));
    CHECK(a->TipHeight() == 1);
    CHECK(!a->HasTx(tx.GetTxId()));  // confirmed: out of A's mempool
    Pump({a.get(), b.get()}, [&] { return b->TipHeight() == 1; });
    CHECK(b->TipHeight() == 1);
    CHECK(b->TipHash() == block.GetHash());
    CHECK(!b->HasTx(tx.GetTxId()));  // confirmed: out of B's mempool
  }

  test::BeginSection("node: headers-first sync of a fresh node");
  {
    auto a = MakeNode("loopback:a3");
    auto d = MakeNode("loopback:d3");
    std::string error;
    CHECK(a->BroadcastBlock(genesis, error));
    // A builds one more block (no txs).
    BlockTemplate templ = AssembleBlock(genesis.GetHash(), 1, kTime + 600,
                                        kTestBits, a->MempoolRef(), a->Utxo(),
                                        kTime, Spk());
    Block block1 = templ.block;
    CHECK(MineBlock(block1, 1, ""));
    CHECK(a->BroadcastBlock(block1, error));
    CHECK(a->TipHeight() == 1);
    // D has an empty chain and connects to A.
    CHECK(d->ConnectTo("loopback:a3"));
    Pump({a.get(), d.get()}, [&] { return d->TipHeight() == 1; });
    CHECK(d->TipHeight() == 1);
    CHECK(d->TipHash() == block1.GetHash());
  }

  test::BeginSection("node: compact block with missing tx (getblocktxn)");
  {
    auto a = MakeNode("loopback:a4");
    auto e = MakeNode("loopback:e4");
    std::string error;
    CHECK(a->BroadcastBlock(genesis, error));
    CHECK(e->BroadcastBlock(genesis, error));
    CHECK(a->ConnectTo("loopback:e4"));
    Pump({a.get(), e.get()},
         [&] {
           return a->HandshookPeerCount() == 1 && e->HandshookPeerCount() == 1;
         });
    // A mines a block with a tx E has never seen (not broadcast).
    const Hash256 kA = TxIdWith(0x44);
    Fund(*a, kA, 50 * COIN);
    Fund(*e, kA, 50 * COIN);
    Transaction tx2 = MakeSpend(*a, kA, 49 * COIN);
    std::string addError;
    CHECK(a->MempoolRef().AddTransaction(tx2, a->Utxo(), 1, kTime, kNow,
                                         addError));
    BlockTemplate templ = AssembleBlock(genesis.GetHash(), 1, kTime + 600,
                                        kTestBits, a->MempoolRef(), a->Utxo(),
                                        kTime, Spk());
    Block block = templ.block;
    CHECK(MineBlock(block, 1, ""));
    CHECK(a->BroadcastBlock(block, error));
    // E must fetch the missing tx via getblocktxn, then accept.
    Pump({a.get(), e.get()}, [&] { return e->TipHeight() == 1; });
    CHECK(e->TipHeight() == 1);
    CHECK(e->TipHash() == block.GetHash());
    CHECK(!e->HasTx(tx2.GetTxId()));  // confirmed tx never entered mempool
  }

  test::BeginSection("node: invalid block earns a ban");
  {
    auto a = MakeNode("loopback:a5");
    auto f = MakeNode("loopback:f5");
    std::string error;
    CHECK(a->BroadcastBlock(genesis, error));
    CHECK(f->BroadcastBlock(genesis, error));
    CHECK(f->ConnectTo("loopback:a5"));
    Pump({a.get(), f.get()},
         [&] {
           return a->HandshookPeerCount() == 1 && f->HandshookPeerCount() == 1;
         });
    // F crafts a block with an impossible target and sends it raw
    // to A over the established connection.
    Block bad;
    bad.header.nVersion = 1;
    bad.header.hashPrevBlock = genesis.GetHash();
    bad.header.nTime = kTime + 600;
    bad.header.nBits = 0x02003456;  // target 52: no hash can pass
    bad.vtx = {CreateCoinbase(1, Spk(), 50 * COIN)};
    bad.header.hashMerkleRoot = bad.ComputeMerkleRoot();
    CHECK(f->SendRawToAddress("loopback:a5", {"block", bad.Serialize()}));
    // A processes the bad block: ban + disconnect.
    Pump({a.get(), f.get()}, [&] { return a->Bans().Size() == 1; });
    CHECK(a->Bans().Size() == 1);
    CHECK(a->PeerCount() == 0);  // banned peer disconnected
    CHECK(a->TipHeight() == 0);  // bad block not accepted
  }

  test::BeginSection("node: real TCP handshake (localhost)");
  {
    auto h = std::make_unique<Node>(kTestBits);
    h->SetTransport(std::make_shared<TcpTransport>());
    CHECK(h->Listen(46477));
    auto i = std::make_unique<Node>(kTestBits);
    i->SetTransport(std::make_shared<TcpTransport>());
    CHECK(i->ConnectTo("127.0.0.1:46477"));
    bool connected = false;
    for (int round = 0; round < 40 && !connected; ++round) {
      h->Poll(50);
      i->Poll(50);
      connected = h->PeerCount() == 1 && i->PeerCount() == 1;
    }
    CHECK(connected);
    CHECK(h->PeerCount() == 1);
    CHECK(i->PeerCount() == 1);
  }
}
