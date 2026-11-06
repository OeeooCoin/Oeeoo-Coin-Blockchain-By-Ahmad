// Tests: Milestone 18 (Stream C) — differential tests.
//
// Differential testing, defined (there is no second Oeeoo
// implementation to diff against — roadmap M18):
//   (a) node-vs-Python-reference cross-checks: this file runs
//       tests/diff_reference.py (pure stdlib, written from the spec
//       and Bitcoin conventions, NOT by translating the C++ code)
//       and compares every vector against the C++ implementation:
//       subsidy, difficulty retarget, vesting calendar, Merkle root,
//       Base58Check. A disagreement is a failure to investigate.
//   (b) two-node agreement: two in-process Nodes fed identical
//       blocks/transactions must agree exactly (tip, height, chain
//       work, UTXO set, mempool size).
//   (c) block-acceptance equivalence: the full Node path
//       (BroadcastBlock) and the standalone harness path
//       (ChainState::AddBlock + ConnectBlockTransactions on a bare
//       CoinsView, the pattern tests/test_reorg.cpp uses) must make
//       the SAME accept/reject decision on every block, valid and
//       invalid, and end with identical tips and UTXO sets.
//
// Temp paths: none on disk except the reference script's stdout
// (read through a pipe). No fixed /tmp paths, no fixed ports —
// this file is safe to run while other streams run their suites.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "blockchain/block.h"
#include "blockchain/chain.h"
#include "blockchain/merkle.h"
#include "consensus/consensus.h"
#include "consensus/difficulty.h"
#include "consensus/subsidy.h"
#include "consensus/vesting.h"
#include "crypto/address.h"
#include "crypto/base58.h"
#include "crypto/ecdsa.h"
#include "crypto/sha256.h"
#include "mining/miner.h"
#include "network/node.h"
#include "script/script.h"
#include "test_harness.h"
#include "transactions/sighash.h"
#include "transactions/transaction.h"
#include "transactions/validation.h"
#include "utxo/coins.h"

using namespace oeeoo;

namespace {
constexpr uint32_t kTestBits = 0x207fffff;
constexpr uint32_t kTime = 1700000000;
constexpr uint32_t kNow = 1800000000;

std::string HexEncode(const std::vector<unsigned char>& bytes) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char b : bytes) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0f]);
  }
  return out;
}

std::string HashHex(const Hash256& h) {
  return HexEncode(std::vector<unsigned char>(h.begin(), h.end()));
}

bool HexToHash(const std::string& hex, Hash256& out) {
  if (hex.size() != 64) return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < 32; ++i) {
    const int hi = nibble(hex[2 * i]);
    const int lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<unsigned char>((hi << 4) | lo);
  }
  return true;
}

bool HexToBytes(const std::string& hex, std::vector<unsigned char>& out) {
  if (hex.size() % 2 != 0) return false;
  out.clear();
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int hi = nibble(hex[i]);
    const int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.push_back(static_cast<unsigned char>((hi << 4) | lo));
  }
  return true;
}

bool ParseU64(const std::string& s, uint64_t& out) {
  if (s.empty()) return false;
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<uint64_t>(c - '0');
  }
  out = v;
  return true;
}

// --- Python reference vectors (see tests/diff_reference.py) ---
struct RefVectors {
  bool loaded = false;
  std::map<int, int64_t> subsidy;              // height -> sats
  int64_t subsidyTotal = -1;
  struct Retarget {
    uint32_t lastBits = 0;
    int64_t timespan = 0;
    uint32_t nextBits = 0;
  };
  std::vector<Retarget> retarget;
  uint32_t launch = 0;
  std::map<int, uint32_t> anniversary;         // years -> unix
  std::map<uint32_t, int64_t> vested;          // unix -> sats
  std::map<int, Hash256> merkle;               // n leaves -> root
  std::vector<std::pair<std::vector<unsigned char>, std::string>> b58;
};

std::vector<std::string> SplitLine(const std::string& line) {
  std::vector<std::string> parts;
  std::string cur;
  for (char c : line) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (!cur.empty()) {
        parts.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) parts.push_back(cur);
  return parts;
}

RefVectors LoadReference() {
  RefVectors ref;
  // Resolve the script relative to likely working directories
  // (ctest runs from the build dir; a direct run from the repo root).
  static const char* kCandidates[] = {
      "tests/diff_reference.py",
      "../tests/diff_reference.py",
      "../../tests/diff_reference.py",
  };
  std::string script;
  for (const char* cand : kCandidates) {
    FILE* probe = std::fopen(cand, "r");
    if (probe != nullptr) {
      std::fclose(probe);
      script = cand;
      break;
    }
  }
  if (script.empty()) return ref;
  const std::string cmd = "python3 " + script;
  FILE* pipe = ::popen(cmd.c_str(), "r");
  if (pipe == nullptr) return ref;
  char buf[4096];
  while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
    const std::vector<std::string> p = SplitLine(buf);
    if (p.empty()) continue;
    uint64_t a = 0, b = 0;
    if (p[0] == "subsidy" && p.size() == 3 && ParseU64(p[1], a) &&
        ParseU64(p[2], b)) {
      ref.subsidy[static_cast<int>(a)] = static_cast<int64_t>(b);
    } else if (p[0] == "subsidy_total" && p.size() == 2 &&
               ParseU64(p[1], a)) {
      ref.subsidyTotal = static_cast<int64_t>(a);
    } else if (p[0] == "retarget" && p.size() == 4) {
      RefVectors::Retarget r;
      r.lastBits = static_cast<uint32_t>(std::strtoul(p[1].c_str(), nullptr, 16));
      if (ParseU64(p[2], a)) r.timespan = static_cast<int64_t>(a);
      r.nextBits = static_cast<uint32_t>(std::strtoul(p[3].c_str(), nullptr, 16));
      ref.retarget.push_back(r);
    } else if (p[0] == "launch" && p.size() == 2 && ParseU64(p[1], a)) {
      ref.launch = static_cast<uint32_t>(a);
    } else if (p[0] == "anniversary" && p.size() == 3 &&
               ParseU64(p[1], a) && ParseU64(p[2], b)) {
      ref.anniversary[static_cast<int>(a)] = static_cast<uint32_t>(b);
    } else if (p[0] == "vested" && p.size() == 3 && ParseU64(p[1], a) &&
               ParseU64(p[2], b)) {
      ref.vested[static_cast<uint32_t>(a)] = static_cast<int64_t>(b);
    } else if (p[0] == "merkle" && p.size() == 3 && ParseU64(p[1], a)) {
      Hash256 h;
      if (HexToHash(p[2], h)) ref.merkle[static_cast<int>(a)] = h;
    } else if (p[0] == "b58check" && p.size() == 3) {
      std::vector<unsigned char> payload;
      if (HexToBytes(p[1], payload)) ref.b58.push_back({payload, p[2]});
    }
  }
  const int rc = ::pclose(pipe);
  // Loaded only when every section produced vectors.
  ref.loaded = rc == 0 && !ref.subsidy.empty() &&
               ref.subsidyTotal >= 0 && !ref.retarget.empty() &&
               ref.launch != 0 && !ref.anniversary.empty() &&
               !ref.vested.empty() && !ref.merkle.empty() &&
               !ref.b58.empty();
  return ref;
}

// --- Shared block-building helpers (same shapes as test_reorg.cpp) ---
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

Transaction MakeSpend(const Hash256& prev, Amount value) {
  Transaction tx;
  tx.version = 1;
  TxIn in;
  in.prevTxHash = prev;
  in.prevOutIndex = 0;
  in.sequence = 0xffffffff;
  tx.vin.push_back(in);
  TxOut out;
  out.value = value;
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

// Hand-built block on `prev` at `height`/`time` carrying `txs`.
// Every test spend consumes a 50-coin fund, as in test_reorg.cpp.
Block MakeBlock(const Hash256& prev, int height, uint32_t time,
                const std::vector<Transaction>& txs) {
  Amount fees = 0;
  for (const Transaction& tx : txs) {
    Amount in = 50 * COIN;
    Amount out = 0;
    for (const TxOut& o : tx.vout) out += o.value;
    fees += in - out;
  }
  Block block;
  block.header.nVersion = 1;
  block.header.hashPrevBlock = prev;
  block.header.nTime = time;
  block.header.nBits = kTestBits;
  block.vtx.push_back(
      CreateCoinbase(height, Spk(), GetBlockSubsidy(height) + fees, ""));
  for (const Transaction& tx : txs) block.vtx.push_back(tx);
  block.header.hashMerkleRoot = block.ComputeMerkleRoot();
  CHECK(MineBlock(block, height, ""));
  return block;
}

void Fund(CoinsView& utxo, const Hash256& txid, Amount value) {
  Coin coin;
  coin.out.value = value;
  coin.out.scriptPubKey = Spk();
  coin.height = 0;
  coin.blockTime = kTime;
  coin.isCoinbase = false;
  CHECK(utxo.AddCoin({txid, 0}, coin));
}

Coin MakeFundCoin(Amount value) {
  Coin coin;
  coin.out.value = value;
  coin.out.scriptPubKey = Spk();
  coin.height = 0;
  coin.blockTime = kTime;
  coin.isCoinbase = false;
  return coin;
}

// Canonical UTXO fingerprint: sorted "txid:vout=value:height" lines.
std::string UtxoFingerprint(const CoinsView& utxo) {
  std::vector<std::pair<OutPoint, Coin>> coins;
  utxo.GetAllCoins(coins);
  std::vector<std::string> lines;
  for (const auto& entry : coins) {
    lines.push_back(HashHex(entry.first.txid) + ":" +
                    std::to_string(entry.first.index) + "=" +
                    std::to_string(entry.second.out.value) + ":" +
                    std::to_string(entry.second.height));
  }
  std::sort(lines.begin(), lines.end());
  std::string out;
  for (const std::string& line : lines) out += line + "\n";
  return out;
}

// Standalone-harness acceptance, mirroring Node::AcceptBlockDirect
// (src/network/node.cpp) step for step: CheckBasic (structure +
// Merkle) first, then fee/reward checks, then undo + transaction
// connection on a COPY of `view`, and only then ChainState::AddBlock
// (header/PoW/bits/MTP/parent). Nothing is mutated on any failure —
// the first draft called AddBlock before Connect, so a block whose
// transactions were invalid still advanced the standalone tip;
// the node validates everything before AddBlock, so the harness
// must too, or the comparison is meaningless.
bool StandaloneAccepts(ChainState& chain, const Block& block, int height,
                       CoinsView& view, std::string& error) {
  if (!block.CheckBasic(error)) return false;
  Amount fees = 0;
  if (!ComputeBlockFees(block, view, height, chain.GetMedianTimePast(),
                        fees, error)) {
    return false;
  }
  if (!CheckBlockReward(block, height, fees, error)) return false;
  CoinsView trial = view;
  BlockUndo undo;
  if (!BuildBlockUndo(block, trial, height, block.header.nTime, undo,
                      error)) {
    return false;
  }
  if (!ConnectBlockTransactions(block, trial, height,
                                chain.GetMedianTimePast(),
                                block.header.nTime, fees, error)) {
    return false;
  }
  if (!chain.AddBlock(block, kNow, error)) return false;
  view = trial;
  return true;
}
}  // namespace

void TestMilestone18C() {
  const RefVectors ref = LoadReference();
  CHECK(ref.loaded);
  if (!ref.loaded) return;  // without the reference there is nothing to diff

  test::BeginSection("m18c: differential subsidy (node vs Python reference)");
  {
    for (const auto& entry : ref.subsidy) {
      CHECK(GetBlockSubsidy(entry.first) == entry.second);
    }
    CHECK(GetTotalMiningIssuance() == ref.subsidyTotal);
    CHECK(GetTotalMiningIssuance() == MINING_ALLOCATION);
    // Independent per-block sum in C++, same shape as test_subsidy.cpp.
    __int128 sum = 0;
    for (int h = 1; h <= LAST_SUBSIDY_HEIGHT; ++h) sum += GetBlockSubsidy(h);
    CHECK(sum == (__int128)ref.subsidyTotal);
  }

  test::BeginSection("m18c: differential difficulty retarget (node vs Python reference)");
  {
    for (const RefVectors::Retarget& r : ref.retarget) {
      Target last;
      CHECK(BitsToTarget(r.lastBits, last));
      const uint32_t next =
          CalculateNextWorkRequired(last, r.timespan, kTestBits);
      CHECK(next == r.nextBits);
      // Compact encoding round-trips on both endpoints.
      Target back;
      CHECK(BitsToTarget(r.nextBits, back));
      CHECK(TargetToBits(back) == r.nextBits);
    }
    // The Python reference's clamp edges: a 100,000 s span clamps to
    // the 302,400 s floor and a 10,000,000 s span to the 4,838,400 s
    // ceiling, so both agree with the exact floor/ceiling spans.
    Target limit;
    CHECK(BitsToTarget(kTestBits, limit));
    CHECK(CalculateNextWorkRequired(limit, 100000, kTestBits) ==
          CalculateNextWorkRequired(limit, 302400, kTestBits));
    CHECK(CalculateNextWorkRequired(limit, 10000000, kTestBits) ==
          CalculateNextWorkRequired(limit, 4838400, kTestBits));
  }

  test::BeginSection("m18c: differential vesting (node vs Python reference)");
  {
    VestingParams params;
    params.enabled = true;
    params.launchTime = ref.launch;
    for (const auto& entry : ref.anniversary) {
      CHECK(AnniversaryTime(ref.launch, entry.first) == entry.second);
    }
    for (const auto& entry : ref.vested) {
      CHECK(VestedAmount(params, entry.first) == entry.second);
    }
    // Leap-day launch: the 1st anniversary is Feb 28, 2029 (not
    // Feb 29 — 2029 is not a leap year), the 4th is Feb 29, 2032.
    CHECK(AnniversaryTime(ref.launch, 1) == ref.anniversary.at(1));
    CHECK(AnniversaryTime(ref.launch, 4) == ref.anniversary.at(4));
    // Disabled vesting imposes no restriction.
    CHECK(VestedAmount(VestingParams{}, ref.launch) == FOUNDER_ALLOCATION);
  }

  test::BeginSection("m18c: differential Merkle + Base58Check (node vs Python reference)");
  {
    for (const auto& entry : ref.merkle) {
      std::vector<Hash256> txids;
      for (int i = 0; i < entry.first; ++i) {
        txids.push_back(
            Sha256d("diff-tx-" + std::to_string(i)));
      }
      CHECK(ComputeMerkleRoot(txids) == entry.second);
    }
    for (const auto& entry : ref.b58) {
      const std::string encoded = Base58CheckEncode(0x0f, entry.first);
      CHECK(encoded == entry.second);
      CHECK(encoded.size() == 34);  // 25 payload bytes -> 34 chars
      CHECK(encoded[0] == '7');  // version 0x0f -> "7" prefix
      unsigned char version = 0;
      std::vector<unsigned char> payload;
      CHECK(Base58CheckDecode(entry.second, version, payload));
      CHECK(version == 0x0f);
      CHECK(payload == entry.first);
      // A single flipped character breaks the checksum.
      std::string tampered = entry.second;
      tampered[tampered.size() - 1] =
          tampered[tampered.size() - 1] == '1' ? '2' : '1';
      CHECK(!Base58CheckDecode(tampered, version, payload));
    }
  }

  test::BeginSection("m18c: two-node agreement on identical inputs");
  {
    EnsureKeys();
    const Hash256 fund = TxIdWith(0xC8);
    const Block genesis = MineGenesis();
    const Transaction spend = MakeSpend(fund, 49 * COIN);
    std::vector<Block> chain;
    chain.push_back(genesis);
    Hash256 prev = genesis.GetHash();
    for (int h = 1; h <= 5; ++h) {
      chain.push_back(MakeBlock(prev, h, kTime + 600 * h, {}));
      prev = chain.back().GetHash();
    }
    chain.push_back(MakeBlock(prev, 6, kTime + 3600, {spend}));

    Node nodeA(kTestBits);
    Node nodeB(kTestBits);
    nodeA.SetTime(kNow);
    nodeB.SetTime(kNow);
    std::string error;
    for (Node* node : {&nodeA, &nodeB}) {
      CHECK(node->BroadcastBlock(genesis, error));
      CHECK(node->Utxo().AddCoin({fund, 0}, MakeFundCoin(50 * COIN)));
      CHECK(node->BroadcastTx(spend, error));
      CHECK(node->MempoolSize() == 1);
    }
    // Same blocks, same order, both nodes.
    for (std::size_t i = 1; i < chain.size(); ++i) {
      CHECK(nodeA.BroadcastBlock(chain[i], error));
      CHECK(nodeB.BroadcastBlock(chain[i], error));
    }
    CHECK(nodeA.TipHeight() == 6);
    CHECK(nodeB.TipHeight() == 6);
    CHECK(nodeA.TipHash() == nodeB.TipHash());
    CHECK(nodeA.TipHash() == chain.back().GetHash());
    CHECK(nodeA.MempoolSize() == 0);
    CHECK(nodeB.MempoolSize() == 0);
    CHECK(UtxoFingerprint(nodeA.Utxo()) == UtxoFingerprint(nodeB.Utxo()));
    CHECK(nodeA.Utxo().Size() == nodeB.Utxo().Size());
    // The funding coin is spent on both; the spend output exists on both.
    Coin coin;
    CHECK(!nodeA.Utxo().GetCoin({fund, 0}, coin));
    CHECK(!nodeB.Utxo().GetCoin({fund, 0}, coin));
    CHECK(nodeA.Utxo().GetCoin({spend.GetTxId(), 0}, coin));
    CHECK(coin.out.value == 49 * COIN);
  }

  test::BeginSection("m18c: block acceptance equivalence node vs standalone ChainState");
  {
    EnsureKeys();
    const Hash256 fund = TxIdWith(0xC9);
    const Block genesis = MineGenesis();
    const Transaction spend = MakeSpend(fund, 49 * COIN);
    std::vector<Block> chain;
    chain.push_back(genesis);
    Hash256 prev = genesis.GetHash();
    for (int h = 1; h <= 3; ++h) {
      chain.push_back(MakeBlock(prev, h, kTime + 600 * h, {}));
      prev = chain.back().GetHash();
    }
    chain.push_back(MakeBlock(prev, 4, kTime + 2400, {spend}));

    Node node(kTestBits);
    node.SetTime(kNow);
    ChainState standalone(kTestBits);
    CoinsView view;
    std::string error;
    CHECK(node.BroadcastBlock(genesis, error));
    CHECK(StandaloneAccepts(standalone, genesis, 0, view, error));
    CHECK(node.Utxo().AddCoin({fund, 0}, MakeFundCoin(50 * COIN)));
    Fund(view, fund, 50 * COIN);
    CHECK(node.TipHash() == standalone.GetTipHash());
    for (std::size_t i = 1; i < chain.size(); ++i) {
      const Block& block = chain[i];
      const bool nodeOk = node.BroadcastBlock(block, error);
      const bool soloOk = StandaloneAccepts(standalone, block,
                                            static_cast<int>(i), view, error);
      CHECK(nodeOk);
      CHECK(soloOk);
      CHECK(node.TipHash() == standalone.GetTipHash());
      CHECK(node.TipHeight() == standalone.GetTipHeight());
      CHECK(UtxoFingerprint(node.Utxo()) == UtxoFingerprint(view));
    }

    // Invalid variants: both paths must reject, and neither tip nor
    // UTXO set may move.
    const Hash256 tipBefore = node.TipHash();
    const std::string utxoBefore = UtxoFingerprint(node.Utxo());
    const std::string viewBefore = UtxoFingerprint(view);
    // (a) Unmined block: valid structure, no proof of work. At test
    // bits a random nonce passes PoW about half the time, so scan
    // nonces until one deterministically FAILS CheckProofOfWork
    // instead of assuming nonce 0 fails.
    Block unmined = MakeBlock(chain.back().GetHash(), 5, kTime + 3000, {});
    for (uint32_t n = 0;; ++n) {
      unmined.header.nNonce = n;
      if (!CheckProofOfWork(unmined.GetHash(), kTestBits)) break;
    }
    CHECK(!node.BroadcastBlock(unmined, error));
    CHECK(!StandaloneAccepts(standalone, unmined, 5, view, error));
    // (b) Tampered Merkle root, re-mined so PoW still passes.
    Block badMerkle = MakeBlock(chain.back().GetHash(), 5, kTime + 3000, {});
    badMerkle.header.hashMerkleRoot = Hash256{};
    CHECK(MineBlock(badMerkle, 5, ""));
    CHECK(!node.BroadcastBlock(badMerkle, error));
    CHECK(!StandaloneAccepts(standalone, badMerkle, 5, view, error));
    // (c) Coinbase over-claims by 1 sat (subsidy + fees + 1).
    Block overclaim;
    overclaim.header.nVersion = 1;
    overclaim.header.hashPrevBlock = chain.back().GetHash();
    overclaim.header.nTime = kTime + 3000;
    overclaim.header.nBits = kTestBits;
    overclaim.vtx.push_back(CreateCoinbase(
        5, Spk(), GetBlockSubsidy(5) + 1, ""));
    overclaim.header.hashMerkleRoot = overclaim.ComputeMerkleRoot();
    CHECK(MineBlock(overclaim, 5, ""));
    CHECK(!node.BroadcastBlock(overclaim, error));
    CHECK(!StandaloneAccepts(standalone, overclaim, 5, view, error));
    // (d) Unknown parent: an orphan neither path accepts.
    Block orphan = MakeBlock(TxIdWith(0xEE), 5, kTime + 3000, {});
    CHECK(!node.BroadcastBlock(orphan, error));
    CHECK(!StandaloneAccepts(standalone, orphan, 5, view, error));
    // State is untouched by all four rejections.
    CHECK(node.TipHash() == tipBefore);
    CHECK(standalone.GetTipHash() == tipBefore);
    CHECK(UtxoFingerprint(node.Utxo()) == utxoBefore);
    CHECK(UtxoFingerprint(view) == viewBefore);
    CHECK(UtxoFingerprint(node.Utxo()) == UtxoFingerprint(view));
  }
}
