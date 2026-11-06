# Profiling — draft section for docs/MILESTONE17.md (Stream C)

> Status: DRAFT for the M17 coordinator to paste into docs/MILESTONE17.md.
> Numbers below are the measured results; method and caveats are part of
> the section so the numbers are never quoted without them.

## Performance profiling (measured, not estimated)

Machine: this VM — AMD EPYC 9D25, 4 vCPUs, 8 GB RAM, g++ 13.3.0,
OpenSSL 3.0.13, SQLite 3.45.1. Harness: new `src/bench/bench.cpp`
(`oeeoo_bench`, minimal equivalent of Core's `src/bench/`; registered
with the two CMake lines below — the coordinator adds them, Stream C
does not edit CMakeLists.txt). Bench binary built Release (`-O2`,
no sanitizers) in its own build dir `build-m17c` against the
in-tree `liboeeoo_core.a`; the soak, by contrast, runs the ASan/UBSan
binary. All three measurements are on a 1,000-block synthetic chain
at test difficulty (bits `0x207fffff`, so PoW search time is NOT in
the numbers). No optimisation work follows from these numbers in M17
— measure first, per the M17 scope.

### 1. Block validation — `Node::BroadcastBlock` per block

In-memory `Node`, no `ChainStore` attached, so the numbers exclude
SQLite I/O by design (the store-backed path is covered by the soak).
Each block after height 100 carries one signed P2PKH spend of the
newly matured coinbase (maturity 100, fee 10,000 sats), so validation
includes one ECDSA verification per block plus Merkle, coinbase,
header/PoW and UTXO work. 3 runs of 1,000 blocks (+1 re-run after a
harness fix; all agree):

| run | avg | p50 | p95 | p99 | max | total (1,000 blocks) |
|-----|-----|-----|-----|-----|-----|----------------------|
| 1 | 1.299 ms | 1.380 ms | 1.563 ms | 2.069 ms | 26.31 ms | 1,299 ms |
| 2 | 1.280 ms | 1.373 ms | 1.643 ms | 2.071 ms | 2.15 ms | 1,280 ms |
| 3 | 1.250 ms | 1.345 ms | 1.541 ms | 1.777 ms | 4.01 ms | 1,250 ms |
| re-run | 1.302 ms | 1.362 ms | 1.522 ms | 3.561 ms | 25.78 ms | 1,302 ms |

Throughput ≈ 770–800 blocks/s validated. The max outliers (up to
~26 ms, single samples) are scheduler/allocator spikes, not a trend:
p99 stays ≤ ~3.6 ms. Empty-block validation (5-block smoke run, no
spends) is ~0.03 ms/block — the ~1.3 ms figure is dominated by the
one ECDSA verification per block.

### 2. Mempool insertion — `Mempool::AddTransaction` per tx

Same runs, 900 spend txs inserted per run (blocks 101–1000):

| run | avg | p50 | p95 | p99 | max |
|-----|-----|-----|-----|-----|-----|
| 1 | 0.584 ms | 0.570 ms | 0.643 ms | 0.954 ms | 2.48 ms |
| 2 | 0.589 ms | 0.569 ms | 0.747 ms | 0.923 ms | 1.04 ms |
| 3 | 0.577 ms | 0.569 ms | 0.606 ms | 0.857 ms | 1.40 ms |
| re-run | 0.580 ms | 0.569 ms | 0.612 ms | 0.854 ms | 1.66 ms |

A 5-tx/block sensitivity run inserted the same 900 txs (the chain
only matures one coinbase per block, so the queue empties and the run
is not a 5× load — recorded, not claimed otherwise) with an
unchanged per-tx avg of 0.582 ms: insertion cost is per-tx, not
per-block-batch.

For reference, `AssembleBlock` (template assembly, not in the M17
scope but measured by the same harness) averages 0.62–0.66 ms/block.

### 3. RPC latency — live node, 100 samples per method

Probe: `soak17/rpc-latency-probe.py` (cookie Basic auth, localhost),
run 2026-11-06 11:28:51 UTC against the running soak nodes (both at
height ~20, in sync, best hash
`14bd0f3847deb56c2d2abb3aca528b0c2ef9385667fe1e2ec7f9545d85de0268`):

| node | method | avg | p50 | p95 | p99 | max |
|------|--------|-----|-----|-----|-----|-----|
| A (RPC 64877) | `getblockcount` | 0.990 ms | 0.653 ms | 1.012 ms | 31.44 ms | 31.44 ms |
| A | `getblock` (best hash) | 1.144 ms | 1.142 ms | 1.526 ms | 2.137 ms | 2.137 ms |
| B (RPC 64878) | `getblockcount` | 1.010 ms | 0.671 ms | 1.309 ms | 30.35 ms | 30.35 ms |
| B | `getblock` (best hash) | 1.470 ms | 1.421 ms | 2.061 ms | 9.042 ms | 9.042 ms |

The ~30 ms p99/max on `getblockcount` is a single slow call on each
node (first-call/scheduler jitter); p95 is ~1.0–1.3 ms. Both nodes
agree on the tip hash, i.e. the probe ran against a synchronised pair.

### Harness registration (for the coordinator — Stream C does not edit CMakeLists.txt)

```cmake
# Benchmark harness (Milestone 17).
add_executable(oeeoo_bench src/bench/bench.cpp)
target_link_libraries(oeeoo_bench PRIVATE oeeoo_core)
```

Note: at the time Stream C built, the in-tree `oeeoo_core` did NOT
link on its own — `src/network/node.cpp` (Stream A work in progress)
calls `LoadMempoolFile`/`SaveMempoolFile` from the new, untracked
`src/mempool/mempooldump.cpp`, which is not yet listed in
CMakeLists.txt. Stream C linked it manually on the `g++` command
line (`g++ -O2 -std=c++17 -Isrc src/bench/bench.cpp
src/mempool/mempooldump.cpp build-m17c/liboeeoo_core.a -lcrypto
-lsqlite3`). The coordinator should confirm Stream A's registration
of `src/mempool/mempooldump.cpp` in `oeeoo_core` lands with the
bench lines above, or the `oeeoo_bench` (and `oeeoo-node`) link fails
on a clean build.

### Harness bug found and fixed (disclosed)

The first version of the 5-tx/block sensitivity run double-spent: it
spent coinbases `h-100-k` for `k = 0..4`, overlapping the previous
block's spends, and the node correctly rejected the second spend
with `missing input` at height 102. That was a harness bug, not a
node bug — the mempool behaved correctly. Fixed by spending from a
mature-unspent queue (each coinbase spent at most once) and re-run;
the 1-tx/block numbers were re-measured after the fix and agree
with the pre-fix runs.

### Files

- Harness: `src/bench/bench.cpp`
- Full bench output: `logs/milestone17-profiling.txt`
- RPC probe: `soak17/rpc-latency-probe.py`
- Bench build (own dir, Release, `-j2`): `build-m17c/oeeoo_bench`
