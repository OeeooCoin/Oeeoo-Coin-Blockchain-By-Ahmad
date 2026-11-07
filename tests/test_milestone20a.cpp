// Tests: Milestone 20 Stream A — OOC-001 multiple coinbase /
// unauthorized inflation (audit report OOC-SA-001, CRITICAL).
//
// The finding: Block::CheckBasic had no "exactly one coinbase" and
// no "coinbase must be first" rule, CheckBlockReward validated only
// vtx[0], and ConnectBlockTransactions minted EVERY IsCoinbase tx's
// outputs — so a miner could add a second coinbase at index 1 and
// mint coins beyond subsidy + fees, breaking the 21M cap. The fix
// is defense in depth: CheckBasic, CheckBlockReward, and
// ConnectBlockTransactions each enforce exactly one coinbase at
// vtx[0]; ComputeBlockFees still skips coinbase txs (it can neither
// mint nor hide value — see the comment there and the (a) checks).
//
// Ports are pid-derived with retry and data dirs pid-tagged and
// removed afterwards — same discipline as test_milestone18a.cpp.
// Node::AcceptBlock / AcceptBlockDirect are private, so the node
// level is exercised through the public BroadcastBlock (which
// calls AcceptBlock -> AcceptBlockDirect / AcceptBlockReorg).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "blockchain/chain.h"
#include "blockchain/verify.h"
#include "consensus/amount.h"
#include "consensus/consensus.h"
#include "consensus/subsidy.h"
#include "crypto/ecdsa.h"
#include "crypto/sha256.h"
#include "db/chainstore.h"
#include "db/database.h"
#include "mempool/mempool.h"
#include "mining/miner.h"
#include "network/node.h"
#include "network/transport.h"
#include "script/script.h"
#include "test_harness.h"
#include "transactions/transaction.h"
#include "transactions/validation.h"
#include "utxo/coins.h"

using namespace oeeoo;

namespace {
constexpr uint32_t kTestBits = 0x207fffff;  // easy test-only difficulty
constexpr uint32_t kTime = 1700000000;
constexpr uint32_t kNow = 1800000000;

std::string PidTag() { return std::to_string(static_cast<long>(::getpid())); }

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

// A genesis block mined at test difficulty (the M15 pattern).
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

// A valid empty block (coinbase only) on `prev` at `height`/`time`.
Block MakeEmptyBlock(const Hash256& prev, int height, uint32_t time) {
  Block block;
  block.header.nVersion = 1;
  block.header.hashPrevBlock = prev;
  block.header.nTime = time;
  block.header.nBits = kTestBits;
  block.vtx.push_back(
      CreateCoinbase(height, Spk(), GetBlockSubsidy(height), ""));
  block.header.hashMerkleRoot = block.ComputeMerkleRoot();
  CHECK(MineBlock(block, height, ""));
  return block;
}

// A block with a SECOND coinbase at index 1 paying `secondValue`.
// The second coinbase differs from vtx[0] in value, extraData and
// extraNonce, so it has a distinct txid: the ONLY defect of this
// block is the second coinbase (valid PoW, valid Merkle, no
// duplicate txids, every output in MoneyRange).
Block MakeMultiCoinbaseBlock(const Hash256& prev, int height, uint32_t time,
                             Amount secondValue, uint32_t secondExtraNonce) {
  Block block;
  block.header.nVersion = 1;
  block.header.hashPrevBlock = prev;
  block.header.nTime = time;
  block.header.nBits = kTestBits;
  block.vtx.push_back(
      CreateCoinbase(height, Spk(), GetBlockSubsidy(height), ""));
  block.vtx.push_back(CreateCoinbase(height, Spk(), secondValue,
                                     "THINK ABOUT GOD?", secondExtraNonce));
  block.header.hashMerkleRoot = block.ComputeMerkleRoot();
  CHECK(MineBlock(block, height, ""));
  return block;
}

// A structurally valid (unsigned) spend: 1 input, 1 output, values
// in range. Scripts are never verified on the paths under test
// here (the coinbase rule fires first), so no signature is needed.
Transaction MakeFakeSpend() {
  Transaction tx;
  tx.version = 1;
  TxIn in;
  in.prevTxHash.fill(0x22);
  in.prevOutIndex = 0;
  in.sequence = 0xffffffff;
  in.scriptSig = {0x51};
  tx.vin.push_back(in);
  TxOut out;
  out.value = 49 * COIN;
  out.scriptPubKey = Spk();
  tx.vout.push_back(out);
  tx.lockTime = 0;
  return tx;
}

// Connect `genesis` into a fresh CoinsView; returns false on error.
bool ConnectGenesis(const Block& genesis, CoinsView& utxo,
                    std::string& error) {
  Amount fees = 0;
  return ConnectBlockTransactions(genesis, utxo, 0, kTime,
                                  genesis.header.nTime, fees, error);
}

// Pid-derived TCP ports for in-process listeners (M18a pattern).
uint16_t NextTestPort() {
  static int next = 0;
  const int base = 48000 + (static_cast<int>(::getpid()) % 300) * 7;
  return static_cast<uint16_t>(base + (next++) * 13);
}

std::unique_ptr<Node> TcpNode() {
  auto node = std::make_unique<Node>(kTestBits);
  node->SetTransport(std::make_shared<TcpTransport>());
  return node;
}

bool ListenOn(Node& node, uint16_t& portOut) {
  for (int attempt = 0; attempt < 10; ++attempt) {
    const uint16_t port = NextTestPort();
    if (node.Listen(port)) {
      portOut = port;
      return true;
    }
  }
  return false;
}

template <typename F>
bool PumpTcp(const std::vector<Node*>& nodes, F done, int rounds = 400) {
  for (int i = 0; i < rounds; ++i) {
    for (Node* n : nodes) n->Poll(5);
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return done();
}
}  // namespace

void TestMilestone20A() {
  EnsureKeys();
  const Block genesis = MineGenesis();

  test::BeginSection(
      "m20a: second coinbase at index 1 — rejected at every layer");
  {
    // A mined, Merkle-valid block whose only defect is the second
    // coinbase minting 25 OEEOO extra.
    const Block bad =
        MakeMultiCoinbaseBlock(genesis.GetHash(), 1, kTime + 600, 25 * COIN, 1);
    CHECK(bad.vtx.size() == 2);
    CHECK(IsCoinbase(bad.vtx[0]));
    CHECK(IsCoinbase(bad.vtx[1]));

    // Layer 1: structure.
    std::string error;
    CHECK(!bad.CheckBasic(error));
    CHECK(error.find("coinbase") != std::string::npos);
    std::printf("  [m20a] CheckBasic rejects %s: %s\n",
                HexStr(bad.GetHash()).c_str(), error.c_str());

    // Layer 2: reward (belt-and-braces — validates vtx[0] only
    // before M20, so this is the layer the auditor flagged).
    CHECK(!CheckBlockReward(bad, 1, 0, error));
    CHECK(error.find("coinbase") != std::string::npos);

    // Layer 3: connection — the mint itself. The second
    // coinbase's outputs must never reach the UTXO set, and the
    // UTXO set must be untouched by the attempt.
    CoinsView utxo;
    CHECK(ConnectGenesis(genesis, utxo, error));
    const Hash256 secondTxid = bad.vtx[1].GetTxId();
    Amount fees = 0;
    CHECK(!ConnectBlockTransactions(bad, utxo, 1, kTime, bad.header.nTime,
                                    fees, error));
    CHECK(error.find("coinbase") != std::string::npos);
    Coin coin;
    CHECK(!utxo.GetCoin({secondTxid, 0}, coin));  // never minted
    CHECK(utxo.GetCoin({genesis.vtx[0].GetTxId(), 0}, coin));  // intact
    CHECK(coin.out.value == 0);

    // The fee path: ComputeBlockFees skips coinbase txs, so it
    // neither rejects nor mints here — fees 0, UTXO untouched.
    // The rejection above is what makes that skip safe.
    CoinsView feeView;
    CHECK(ConnectGenesis(genesis, feeView, error));
    Amount feeTotal = 999;
    CHECK(ComputeBlockFees(bad, feeView, 1, kTime, feeTotal, error));
    CHECK(feeTotal == 0);
    CHECK(!feeView.GetCoin({secondTxid, 0}, coin));

    // Node level (public BroadcastBlock -> AcceptBlock ->
    // AcceptBlockDirect): rejected, tip and UTXO unchanged.
    Node node(kTestBits);
    node.SetTime(kNow);
    CHECK(node.BroadcastBlock(genesis, error));
    CHECK(node.TipHeight() == 0);
    CHECK(!node.BroadcastBlock(bad, error));
    CHECK(error.find("coinbase") != std::string::npos);
    CHECK(node.TipHeight() == 0);
    CHECK(node.TipHash() == genesis.GetHash());
    CHECK(!node.Utxo().GetCoin({secondTxid, 0}, coin));
    CHECK(node.HasBlock(genesis.GetHash()));
    CHECK(!node.HasBlock(bad.GetHash()));

    // The rule holds for the genesis block too: a genesis with a
    // second coinbase is rejected, while the real genesis passes.
    Block badGenesis = genesis;
    badGenesis.vtx.push_back(
        CreateCoinbase(0, Spk(), 1 * COIN, "THINK ABOUT GOD?", 7));
    badGenesis.header.hashMerkleRoot = badGenesis.ComputeMerkleRoot();
    CHECK(!badGenesis.CheckBasic(error));
    CHECK(error.find("coinbase") != std::string::npos);
    std::string genesisError;
    CHECK(genesis.CheckBasic(genesisError));
  }

  test::BeginSection(
      "m20a: coinbase at index 1, normal tx at index 0 — rejected");
  {
    Block bad;
    bad.header.nVersion = 1;
    bad.header.hashPrevBlock = genesis.GetHash();
    bad.header.nTime = kTime + 600;
    bad.header.nBits = kTestBits;
    bad.vtx.push_back(MakeFakeSpend());  // index 0: not a coinbase
    bad.vtx.push_back(
        CreateCoinbase(1, Spk(), 25 * COIN, "THINK ABOUT GOD?", 1));
    bad.header.hashMerkleRoot = bad.ComputeMerkleRoot();
    CHECK(MineBlock(bad, 1, ""));
    CHECK(!IsCoinbase(bad.vtx[0]));
    CHECK(IsCoinbase(bad.vtx[1]));

    std::string error;
    CHECK(!bad.CheckBasic(error));
    CHECK(error.find("coinbase") != std::string::npos);
    CHECK(!CheckBlockReward(bad, 1, 0, error));
    CHECK(error.find("coinbase") != std::string::npos);
    CoinsView utxo;
    CHECK(ConnectGenesis(genesis, utxo, error));
    Amount fees = 0;
    CHECK(!ConnectBlockTransactions(bad, utxo, 1, kTime, bad.header.nTime,
                                    fees, error));
    CHECK(error.find("coinbase") != std::string::npos);
    Coin coin;
    CHECK(!utxo.GetCoin({bad.vtx[1].GetTxId(), 0}, coin));

    Node node(kTestBits);
    node.SetTime(kNow);
    CHECK(node.BroadcastBlock(genesis, error));
    CHECK(!node.BroadcastBlock(bad, error));
    CHECK(node.TipHeight() == 0);
    CHECK(node.TipHash() == genesis.GetHash());
  }

  test::BeginSection(
      "m20a: second coinbase minting near MAX_MONEY — rejected");
  {
    // One output of exactly MAX_MONEY: CheckBasic's per-tx money
    // range still passes (<= MAX_MONEY), so the rejection must
    // come from the coinbase rule alone, not from MoneyRange.
    const Block bad = MakeMultiCoinbaseBlock(genesis.GetHash(), 1,
                                             kTime + 600, MAX_MONEY, 1);
    std::string error;
    CHECK(!bad.CheckBasic(error));
    CHECK(error.find("coinbase") != std::string::npos);
    CHECK(!CheckBlockReward(bad, 1, 0, error));
    CoinsView utxo;
    CHECK(ConnectGenesis(genesis, utxo, error));
    Amount fees = 0;
    CHECK(!ConnectBlockTransactions(bad, utxo, 1, kTime, bad.header.nTime,
                                    fees, error));
    Coin coin;
    CHECK(!utxo.GetCoin({bad.vtx[1].GetTxId(), 0}, coin));
    Node node(kTestBits);
    node.SetTime(kNow);
    CHECK(node.BroadcastBlock(genesis, error));
    CHECK(!node.BroadcastBlock(bad, error));
    CHECK(node.TipHeight() == 0);
    std::printf("  [m20a] huge second coinbase %s rejected: %s\n",
                HexStr(bad.GetHash()).c_str(), error.c_str());
  }

  test::BeginSection("m20a: valid single-coinbase block still accepted");
  {
    // No regression: exactly one coinbase at index 0 passes every
    // layer and lands on the chain with its outputs spendable.
    const Block good = MakeEmptyBlock(genesis.GetHash(), 1, kTime + 600);
    std::string error;
    CHECK(good.CheckBasic(error));
    CHECK(CheckBlockReward(good, 1, 0, error));
    CoinsView utxo;
    CHECK(ConnectGenesis(genesis, utxo, error));
    Amount fees = 0;
    CHECK(ConnectBlockTransactions(good, utxo, 1, kTime, good.header.nTime,
                                   fees, error));
    Coin coin;
    CHECK(utxo.GetCoin({good.vtx[0].GetTxId(), 0}, coin));
    CHECK(coin.out.value == GetBlockSubsidy(1));
    CHECK(coin.isCoinbase);

    Node node(kTestBits);
    node.SetTime(kNow);
    CHECK(node.BroadcastBlock(genesis, error));
    CHECK(node.BroadcastBlock(good, error));
    CHECK(node.TipHeight() == 1);
    CHECK(node.TipHash() == good.GetHash());
    CHECK(node.Utxo().GetCoin({good.vtx[0].GetTxId(), 0}, coin));
    CHECK(coin.out.value == GetBlockSubsidy(1));
  }

  test::BeginSection(
      "m20a: reorg onto a fork with a two-coinbase tip — rejected");
  {
    // Main chain: genesis + A1. Fork: B1 (same height, less work
    // parity — an inactive side branch) then B2, a mined
    // two-coinbase block that would out-work the main chain.
    // The reorg must be refused and the main chain untouched.
    Node node(kTestBits);
    node.SetTime(kNow);
    std::string error;
    CHECK(node.BroadcastBlock(genesis, error));
    const Block a1 = MakeEmptyBlock(genesis.GetHash(), 1, kTime + 600);
    CHECK(node.BroadcastBlock(a1, error));
    CHECK(node.TipHeight() == 1);
    CHECK(node.TipHash() == a1.GetHash());
    const Block b1 = MakeEmptyBlock(genesis.GetHash(), 1, kTime + 1200);
    CHECK(node.BroadcastBlock(b1, error));  // inactive side branch
    CHECK(node.TipHash() == a1.GetHash());
    CHECK(node.TipHeight() == 1);
    const Block b2bad = MakeMultiCoinbaseBlock(b1.GetHash(), 2, kTime + 1800,
                                               25 * COIN, 1);
    CHECK(!node.BroadcastBlock(b2bad, error));
    CHECK(error.find("coinbase") != std::string::npos);
    CHECK(node.TipHeight() == 1);
    CHECK(node.TipHash() == a1.GetHash());
    Coin coin;
    CHECK(!node.Utxo().GetCoin({b2bad.vtx[1].GetTxId(), 0}, coin));
    CHECK(!node.HasBlock(b2bad.GetHash()));
    std::printf("  [m20a] fork tip %s rejected: %s\n",
                HexStr(b2bad.GetHash()).c_str(), error.c_str());

    // The reorg's inner layers reject it too (defense in depth
    // below the AcceptBlock CheckBasic gate): reward + connect
    // against the fork's own UTXO state (genesis + B1).
    CHECK(!CheckBlockReward(b2bad, 2, 0, error));
    CoinsView forkUtxo;
    CHECK(ConnectGenesis(genesis, forkUtxo, error));
    Amount fees = 0;
    CHECK(ConnectBlockTransactions(b1, forkUtxo, 1, kTime, b1.header.nTime,
                                   fees, error));
    CHECK(!ConnectBlockTransactions(b2bad, forkUtxo, 2, kTime,
                                    b2bad.header.nTime, fees, error));
    CHECK(!forkUtxo.GetCoin({b2bad.vtx[1].GetTxId(), 0}, coin));
  }

  test::BeginSection("m20a: stored chain with a multi-coinbase block "
                     "fails full verification");
  {
    // A poisoned store, as one written by a pre-fix binary (or by
    // hand) would look: genesis + a height-1 block carrying a
    // second coinbase. The store API itself refuses to persist
    // such a block post-fix, so the poison is written with raw
    // SQL through the project's own Database wrapper — exactly
    // the rows a pre-fix ApplyBlock would have written.
    const std::string path = "/tmp/oeeoo-m20a-" + PidTag() + "-verify.db";
    ::remove(path.c_str());
    const Block block1 = MakeEmptyBlock(genesis.GetHash(), 1, kTime + 600);
    const Block bad1 =
        MakeMultiCoinbaseBlock(genesis.GetHash(), 1, kTime + 600, 25 * COIN, 1);
    {
      ChainStore store;
      std::string error;
      CHECK(store.Open(path, error));
      ChainState chain(kTestBits);
      CoinsView utxo;
      CHECK(chain.AddBlock(genesis, kNow, error));
      BlockUndo undo;
      CHECK(BuildBlockUndo(genesis, utxo, 0, genesis.header.nTime, undo,
                           error));
      Amount fees = 0;
      CHECK(ConnectBlockTransactions(genesis, utxo, 0, kTime,
                                     genesis.header.nTime, fees, error));
      Target work;
      CHECK(chain.GetChainWork(genesis.GetHash(), work));
      CHECK(store.ApplyBlock(genesis, work, undo, error));
      CHECK(chain.AddBlock(block1, kNow, error));
      CHECK(BuildBlockUndo(block1, utxo, 1, block1.header.nTime, undo,
                           error));
      CHECK(ConnectBlockTransactions(block1, utxo, 1, kTime,
                                     block1.header.nTime, fees, error));
      CHECK(chain.GetChainWork(block1.GetHash(), work));
      CHECK(store.ApplyBlock(block1, work, undo, error));
      // The store itself refuses the multi-coinbase block.
      BlockUndo badUndo;
      CHECK(BuildBlockUndo(bad1, utxo, 1, bad1.header.nTime, badUndo, error));
      CHECK(!store.ApplyBlock(bad1, work, badUndo, error));
      CHECK(error.find("coinbase") != std::string::npos);
      store.Close();

      // Poison the stored height-1 row: same height/work, the bad
      // block's hash/header/body, and the tip pointer.
      // NOTE (M20 integration fix): the hashes MUST be bound from
      // named locals — GetHash() returns by value, so
      // bad1.GetHash().begin() and bad1.GetHash().end() iterate two
      // different temporaries (undefined behavior; the stored tip
      // blob came out the wrong size and verification failed with
      // "stored tip has wrong size" instead of the coinbase error).
      const Hash256 badHash = bad1.GetHash();
      const Hash256 block1Hash = block1.GetHash();
      Database raw;
      CHECK(raw.Open(path, error));
      Database::Statement stmt;
      CHECK(stmt.Prepare(raw,
                         "UPDATE blocks SET hash=?1, header=?2, work=?3, "
                         "body=?4 WHERE hash=?5;",
                         error));
      stmt.BindBlob(1, std::vector<unsigned char>(badHash.begin(),
                                                  badHash.end()));
      stmt.BindBlob(2, bad1.header.Serialize());
      stmt.BindBlob(3, std::vector<unsigned char>(work.begin(), work.end()));
      stmt.BindBlob(4, bad1.Serialize());
      stmt.BindBlob(5, std::vector<unsigned char>(block1Hash.begin(),
                                                  block1Hash.end()));
      (void)stmt.Step();
      CHECK(!stmt.HasError());
      Database::Statement tipStmt;
      CHECK(tipStmt.Prepare(raw,
                            "UPDATE meta SET value=?1 WHERE key='tip';",
                            error));
      tipStmt.BindBlob(1, std::vector<unsigned char>(badHash.begin(),
                                                     badHash.end()));
      (void)tipStmt.Step();
      CHECK(!tipStmt.HasError());
      raw.Close();
    }
    // Full-chain verification from the store rejects it.
    {
      ChainStore store2;
      std::string error;
      CHECK(store2.Open(path, error));
      Hash256 verifiedTip;
      CHECK(!VerifyChainFromStore(store2, VestingParams{}, kTestBits,
                                  verifiedTip, error));
      CHECK(error.find("coinbase") != std::string::npos);
      std::printf("  [m20a] VerifyChainFromStore rejects poisoned "
                  "chain: %s\n",
                  error.c_str());
      // Deep load rejects it too — and so does the light load,
      // whose body-load loop re-validates each stored block
      // through GetBlock -> CheckBasic.
      Node node(kTestBits);
      node.SetTime(kNow);
      CHECK(!node.LoadFromStore(store2, error, true));
      Node nodeLight(kTestBits);
      nodeLight.SetTime(kNow);
      CHECK(!nodeLight.LoadFromStore(store2, error, false));
      store2.Close();
    }
    ::remove(path.c_str());
  }

  test::BeginSection("m20a: live — honest node rejects a malicious "
                     "peer's multi-coinbase block and bans");
  {
    auto honest = TcpNode();
    auto badPeer = TcpNode();
    honest->SetTime(kNow);
    badPeer->SetTime(kNow);
    std::string error;
    CHECK(honest->BroadcastBlock(genesis, error));
    CHECK(badPeer->BroadcastBlock(genesis, error));
    uint16_t portH = 0;
    CHECK(ListenOn(*honest, portH));
    const std::string addrH = "127.0.0.1:" + std::to_string(portH);
    CHECK(badPeer->ConnectTo(addrH));
    const bool peered = PumpTcp({honest.get(), badPeer.get()}, [&] {
      return honest->HandshookPeerCount() == 1 &&
             badPeer->HandshookPeerCount() == 1;
    });
    CHECK(peered);

    // The malicious block: valid PoW, valid Merkle — the ONLY
    // defect is the second coinbase minting 25 OEEOO.
    const Block bad =
        MakeMultiCoinbaseBlock(genesis.GetHash(), 1, kTime + 600, 25 * COIN, 1);
    std::string directError;
    CHECK(!bad.CheckBasic(directError));
    std::string badAddr;
    for (const Peer& p : badPeer->GetPeers()) badAddr = p.address;
    CHECK(!badAddr.empty());
    if (badAddr.empty()) {
      std::printf("  [m20a] LIVE: no peer address to send from; "
                  "aborting section\n");
      return;
    }
    CHECK(badPeer->SendRawToAddress(
        badAddr, {"block", bad.Serialize()}));
    const bool banned = PumpTcp({honest.get(), badPeer.get()}, [&] {
      return honest->Bans().Size() == 1;
    });
    CHECK(banned);
    CHECK(honest->TipHeight() == 0);
    CHECK(honest->TipHash() == genesis.GetHash());
    CHECK(honest->PeerCount() == 0);  // ban-disconnect drops the peer
    Coin coin;
    CHECK(!honest->Utxo().GetCoin({bad.vtx[1].GetTxId(), 0}, coin));
    CHECK(!honest->HasBlock(bad.GetHash()));
    std::printf("  [m20a] LIVE: malicious block %s rejected: %s; "
                "honest bans=%zu peers=%zu tip=%d\n",
                HexStr(bad.GetHash()).c_str(), directError.c_str(),
                honest->Bans().Size(), honest->PeerCount(),
                honest->TipHeight());
  }

  test::BeginSection(
      "m20a: miner builds exactly one coinbase; mempool refuses coinbase");
  {
    // AssembleBlock: one coinbase at vtx[0], nothing else.
    CoinsView utxo;
    std::string error;
    CHECK(ConnectGenesis(genesis, utxo, error));
    Mempool pool;
    BlockTemplate templ = AssembleBlock(genesis.GetHash(), 1, kTime + 600,
                                        kTestBits, pool, utxo, kTime, Spk());
    CHECK(templ.block.vtx.size() == 1);
    CHECK(IsCoinbase(templ.block.vtx[0]));
    CHECK(templ.block.CheckBasic(error));
    CHECK(CheckBlockReward(templ.block, 1, templ.totalFees, error));
    CHECK(templ.block.vtx[0].vout[0].value == GetBlockSubsidy(1));

    // Mempool: a coinbase tx never enters — ValidateTransaction
    // fails it on the missing (zero-hash) input.
    const Transaction cb = CreateCoinbase(1, Spk(), 50 * COIN, "");
    CHECK(!pool.AddTransaction(cb, utxo, 1, kTime, kNow, error));
    CHECK(pool.Size() == 0);
    std::printf("  [m20a] mempool refuses coinbase: %s\n", error.c_str());
  }
}
