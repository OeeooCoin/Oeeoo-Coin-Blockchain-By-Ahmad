// SPDX-License-Identifier: MIT
// Oeeoo Coin — minimal benchmark harness (Milestone 17).
// Core has src/bench/; this is Oeeoo's minimal equivalent: it builds a
// synthetic chain in-process and times the three operations the M17
// profiling scope names — block validation, mempool insertion, and
// (via the companion Python probe against a live node) RPC latency.
//
// What it measures, exactly:
//   1. Block validation: time of Node::BroadcastBlock per block
//      (AcceptBlock: header/PoW/Merkle/coinbase/tx/script validation
//      + connect + UTXO update) on a growing chain. The chain is an
//      in-memory Node with NO ChainStore attached, so the numbers
//      exclude SQLite I/O by design; the soak test covers the
//      store-backed node instead.
//   2. Mempool insertion: time of Mempool::AddTransaction per tx
//      (validation + admission) for signed P2PKH spends of matured
//      coinbases (coinbase maturity is 100 blocks). Each block
//      matures one new coinbase; the harness spends up to
//      --tx-per-block of them (oldest mature-unspent first, each
//      coinbase spent at most once), so a 1-tx/block run carries one
//      spend per block from height 101 on.
//   3. RPC latency is NOT measured here: it needs a live server.
//      Measure it with the probe in soak17/rpc-latency-probe.py
//      against a node on the same chain and record it alongside.
//
// Method notes (read before quoting the numbers):
//   - Test difficulty only (bits 0x207fffff): MineBlock is instant,
//     so PoW search time is NOT in the numbers.
//   - Single-threaded, one run: no warmup separation, no
//     multi-sample statistics beyond the per-block/per-tx
//     distribution (avg/p50/p95/p99/max) printed below.
//   - Build with optimisation (Release) and WITHOUT sanitizers:
//     an ASan build measures the sanitizer, not the code. The soak
//     runs under ASan/UBSan; this bench does not.
//   - NO optimisation work follows from these numbers in M17:
//     measure first (roadmap M17 scope).
//
// Usage: oeeoo_bench [--blocks N] [--tx-per-block N] [--seed N]
//   Defaults: 1000 blocks, 1 spend tx per block (after maturity).
//   Exit 0 on a completed run; non-zero with a diagnostic if any
//   block or transaction is rejected (a rejection is a BUG, not a
//   slow measurement — the harness must never silently skip one).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

#include "consensus/consensus.h"
#include "consensus/subsidy.h"
#include "crypto/ecdsa.h"
#include "mining/miner.h"
#include "network/node.h"
#include "script/script.h"
#include "transactions/sighash.h"

using namespace oeeoo;

namespace {

constexpr uint32_t kTestBits = 0x207fffff;
constexpr uint32_t kTime = 1700000000;   // genesis time (as in tests)
constexpr uint32_t kNow = 1800000000;    // fixed node clock (as in tests)
constexpr Amount kSpendFee = 10000;      // sats per bench spend tx

KeyPair g_key;

std::vector<unsigned char> Spk() {
  return CreateP2PKHScriptPubKey(PubKeyHash160(g_key.pubkey));
}

double Ms(std::chrono::steady_clock::time_point a,
          std::chrono::steady_clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

struct Stats {
  double avg = 0, p50 = 0, p95 = 0, p99 = 0, max = 0, total = 0;
  std::size_t n = 0;
};

Stats Summarize(std::vector<double> v) {
  Stats s;
  s.n = v.size();
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  s.total = std::accumulate(v.begin(), v.end(), 0.0);
  s.avg = s.total / static_cast<double>(v.size());
  auto pct = [&](double p) {
    const std::size_t i =
        std::min(v.size() - 1, static_cast<std::size_t>(std::floor(p * v.size())));
    return v[i];
  };
  s.p50 = pct(0.50);
  s.p95 = pct(0.95);
  s.p99 = pct(0.99);
  s.max = v.back();
  return s;
}

void PrintStats(const char* name, const char* unit, const Stats& s) {
  std::printf("%-28s n=%-6zu avg=%9.4f  p50=%9.4f  p95=%9.4f  p99=%9.4f  "
              "max=%9.4f  total=%10.3f %s\n",
              name, s.n, s.avg, s.p50, s.p95, s.p99, s.max, s.total, unit);
}

Transaction MakeSpend(const Hash256& prevTx, Amount value) {
  Transaction tx;
  tx.version = 1;
  TxIn in;
  in.prevTxHash = prevTx;
  in.prevOutIndex = 0;
  in.sequence = 0xffffffff;
  tx.vin.push_back(in);
  TxOut out;
  out.value = value - kSpendFee;
  out.scriptPubKey = Spk();
  tx.vout.push_back(out);
  tx.lockTime = 0;
  Hash256 h;
  if (!LegacySighash(tx, 0, Spk(), kSigHashAll, h)) {
    std::fprintf(stderr, "bench: LegacySighash failed\n");
    std::exit(2);
  }
  std::vector<unsigned char> sig = SignHash(h, g_key.privkey);
  sig.push_back(static_cast<unsigned char>(kSigHashAll));
  tx.vin[0].scriptSig = CreateP2PKHScriptSig(sig, g_key.pubkey);
  return tx;
}

}  // namespace

int main(int argc, char** argv) {
  int nBlocks = 1000;
  int txPerBlock = 1;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--blocks") == 0 && i + 1 < argc) {
      nBlocks = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--tx-per-block") == 0 && i + 1 < argc) {
      txPerBlock = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--help") == 0 ||
               std::strcmp(argv[i], "-h") == 0) {
      std::printf("Usage: oeeoo_bench [--blocks N] [--tx-per-block N]\n");
      return 0;
    } else {
      std::fprintf(stderr, "bench: unknown argument %s\n", argv[i]);
      return 2;
    }
  }
  if (nBlocks < 1 || txPerBlock < 0 || txPerBlock > 10) {
    std::fprintf(stderr, "bench: bad --blocks/--tx-per-block\n");
    return 2;
  }
  if (!GenerateKeyPair(g_key)) {
    std::fprintf(stderr, "bench: GenerateKeyPair failed\n");
    return 2;
  }

  std::printf("oeeoo_bench: %d blocks, %d spend tx/block (after maturity 100)\n",
              nBlocks, txPerBlock);
  std::printf("  method: in-memory Node, no ChainStore (excludes SQLite I/O); "
              "test bits 0x207fffff; single-threaded; Release build\n");

  Node node(kTestBits);
  node.SetTime(kNow);

  // Genesis: coinbase value 0 (as in tests), mined at test bits.
  Block genesis;
  genesis.header.nVersion = 1;
  genesis.header.nTime = kTime;
  genesis.header.nBits = kTestBits;
  genesis.vtx = {CreateCoinbase(0, Spk(), 0, "THINK ABOUT GOD?")};
  genesis.header.hashMerkleRoot = genesis.ComputeMerkleRoot();
  if (!MineBlock(genesis, 0, "THINK ABOUT GOD?")) {
    std::fprintf(stderr, "bench: genesis mining failed\n");
    return 2;
  }
  std::string error;
  auto t0 = std::chrono::steady_clock::now();
  if (!node.BroadcastBlock(genesis, error)) {
    std::fprintf(stderr, "bench: genesis rejected: %s\n", error.c_str());
    return 2;
  }
  const double genesisMs = Ms(t0, std::chrono::steady_clock::now());

  std::vector<double> blockMs;
  std::vector<double> insertMs;
  std::vector<double> assembleMs;
  std::vector<Hash256> coinbaseTxid;  // by height (index 0 = genesis)
  coinbaseTxid.push_back(genesis.vtx[0].GetTxId());
  std::vector<int> matureUnspent;  // heights whose coinbase is mature + unspent
  std::size_t spendsInserted = 0;

  for (int h = 1; h <= nBlocks; ++h) {
    const uint32_t blockTime = kTime + static_cast<uint32_t>(h) * 600;
    const uint32_t mtp = kTime + static_cast<uint32_t>(h - 1) * 600;
    // Each block matures exactly one new coinbase (height h-100).
    // Spend up to txPerBlock entries from the mature-unspent queue
    // (oldest first); each coinbase is spent at most once, so the
    // mempool can never see a double-spend from this harness.
    if (h - COINBASE_MATURITY >= 1) {
      matureUnspent.push_back(h - COINBASE_MATURITY);
    }
    int spendsThisBlock = 0;
    while (spendsThisBlock < txPerBlock && !matureUnspent.empty()) {
      const int spendHeight = matureUnspent.front();
      matureUnspent.erase(matureUnspent.begin());
      const Hash256 prev = coinbaseTxid[static_cast<std::size_t>(spendHeight)];
      Transaction tx = MakeSpend(prev, GetBlockSubsidy(spendHeight));
      t0 = std::chrono::steady_clock::now();
      if (!node.MempoolRef().AddTransaction(tx, node.Utxo(), h, mtp, kNow,
                                            error)) {
        std::fprintf(stderr, "bench: height %d: mempool insert rejected: %s\n",
                     h, error.c_str());
        return 2;
      }
      insertMs.push_back(Ms(t0, std::chrono::steady_clock::now()));
      ++spendsInserted;
      ++spendsThisBlock;
    }
    t0 = std::chrono::steady_clock::now();
    BlockTemplate templ = AssembleBlock(node.TipHash(), h, blockTime, kTestBits,
                                        node.MempoolRef(), node.Utxo(), mtp,
                                        Spk());
    assembleMs.push_back(Ms(t0, std::chrono::steady_clock::now()));
    Block block = templ.block;
    if (!MineBlock(block, h, "")) {
      std::fprintf(stderr, "bench: height %d: mining failed\n", h);
      return 2;
    }
    t0 = std::chrono::steady_clock::now();
    if (!node.BroadcastBlock(block, error)) {
      std::fprintf(stderr, "bench: height %d: block rejected: %s\n", h,
                   error.c_str());
      return 2;
    }
    blockMs.push_back(Ms(t0, std::chrono::steady_clock::now()));
    coinbaseTxid.push_back(block.vtx[0].GetTxId());
    if (node.TipHeight() != h) {
      std::fprintf(stderr, "bench: height %d: tip is %d after broadcast\n", h,
                   node.TipHeight());
      return 2;
    }
    if (h % 250 == 0) {
      std::printf("  ... height %d, mempool %zu, utxo coins (tip %d)\n", h,
                  node.MempoolRef().Size(), node.TipHeight());
    }
  }

  std::printf("chain: tip height %d\n", node.TipHeight());
  std::printf("genesis BroadcastBlock: %.4f ms\n", genesisMs);
  std::printf("spend txs inserted into mempool: %zu\n", spendsInserted);
  PrintStats("block validation (BroadcastBlock)", "ms", Summarize(blockMs));
  PrintStats("mempool insertion (AddTransaction)", "ms", Summarize(insertMs));
  PrintStats("template assembly (AssembleBlock)", "ms", Summarize(assembleMs));
  std::printf("throughput: %.1f blocks/s validated (%.3f ms/block avg)\n",
              1000.0 / Summarize(blockMs).avg, Summarize(blockMs).avg);
  return 0;
}
