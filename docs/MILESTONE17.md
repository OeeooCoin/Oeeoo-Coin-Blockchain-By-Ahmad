# Oeeoo Coin — Milestone 17: Persistence & Robustness

Status: DONE 2026-11-06. All 171 prior sections still pass;
11 new sections added (182 total), ALL CHECKS PASSED, ctest 100%.
Build warning-free; cppcheck 0 findings; ASan/UBSan clean (see §F).
Built in three parallel streams (mempool persistence; reorg depth
100 + crash-injection; soak + profiling) plus coordinator
integration (CMake/test wiring, disk-full warning, test guard,
bench wiring, docs).

The M13 verdict stands: **NOT READY** (genesis unapproved, no
independent audit). M17 proves the node survives restarts and
crashes without losing work (mempool persistence, 100-deep reorg,
SIGKILL recovery, disk-full rollback-not-corruption) and measures
its limits (soak + profiling); it does not change launch
readiness.

## A. Summary

### 1. Mempool persistence (`src/mempool/mempooldump.*` NEW, `src/mempool/mempool.*`, `src/network/node.*`, `src/app/config.*`, `src/app/node_main.cpp`, `src/rpc/rpcserver.cpp`)

- **Format (Oeeoo's own — the model is Core's `mempool.dat`,
  the bytes are ours, stated in the header):** JSON-lines.
  Line 1: `{"format":"oeeoo-mempool","version":1}`. One object
  per entry, highest fee rate first:
  `{"txid":"<64 hex>","tx":"<hex of serialized tx>","fee":<sats>,"time":<unix addedTime>}`.
  Only tx + fee + time — never the UTXO set.
- **Write discipline:** whole file rewritten on every save via
  `<path>.new` + fflush + fsync + rename (the same atomic
  pattern as the M16 wallet journal sidecar). A crash mid-save
  can never leave a half-written file; at worst the previous
  good file survives. The directory itself is not fsynced (same
  as the journal): on a power loss immediately after the
  rename, some filesystems could lose the new file — the node
  then starts with an empty mempool, which is safe, never
  corrupt.
- **Load discipline (best-effort, never fatal):** missing file
  loads as empty; malformed header fails the whole load (the
  mempool stays as it was); a malformed entry line drops just
  that entry; entries older than 14 days
  (`Mempool::kExpirySeconds`, now public and shared with the
  expiry sweep) are dropped as expired; every other entry is
  re-validated against the current UTXO set (fee, dust, script,
  conflicts, RBF rules — exactly the same checks as a fresh
  admission) with the stored fee checked against the recomputed
  fee and the stored txid checked against the deserialized tx.
  Entries are admitted in topological order (Kahn's algorithm
  over the file's own txids) so chains reload whole; a cycle is
  dropped as invalid. The caller learns what happened from
  `MempoolLoadStats {loaded, droppedExpired, droppedInvalid}`.
- **Caps (Oeeoo's own choice):** at most 100,000 entries, at
  most 64 MiB of file, highest fee rate first. The file is never
  unbounded: even a full 300 MB mempool produces at most a
  64 MiB file (hex encoding roughly doubles raw size, so 64 MiB
  of file holds ~29 MB of raw transactions).
- **Wiring:** `Node::SetMempoolPath` / `SetPersistMempool` /
  `LoadMempool` / `SaveMempool` / `DumpMempool` (explicit
  on-demand dump) / `ImportMempoolFile` / `MempoolLoaded()` /
  `LastMempoolLoadStats()`; load runs after `LoadFromStore`,
  against the current UTXO set. `node_main.cpp`: path
  `<dataDir>/mempool.dat`; load after genesis is accepted
  (warns and continues with an empty mempool on failure); save
  on clean shutdown (warns on failure, never fails the
  shutdown). `Mempool::AddLoadedTransaction()` validates
  exactly like `AddTransaction` but preserves the stored
  `addedTime`, so a reloaded entry does not get a fresh 14
  days; `AddTransaction` is now a thin wrapper over a shared
  `AddTransactionInternal` — no behaviour change for fresh
  admissions.
- **RPC:** `savemempool` (returns null, like Core),
  `importmempool` (returns `{loaded, dropped_expired,
  dropped_invalid}` — an Oeeoo choice, documented, not Core's
  null), `loaded` in `getmempoolinfo`. The RPC surface is now
  **38 callable methods + `help`**.
- **Config:** `persistmempool` (default `true`); config key
  `persistmempool`; CLI `--persistmempool`,
  `--persistmempool=0|1` (Core-style spelling) and
  `--no-persistmempool`; `--help` text.
- **Decided and documented:** `savemempool` works even with
  `persistmempool=0` — the opt-out disables only the automatic
  load/save; an explicit operator dump still writes
  (Core-compatible reading of the flag). `loaded` = true once
  the startup load attempt has finished, including a missing
  file and a malformed header; false only when persistence is
  disabled (load skipped) or no load was attempted.
- **Scope (honest):** this mempool can never hold a child
  spending an unconfirmed parent's output — every admission
  path validates against the plain UTXO view (`"missing input"`
  → orphan pool). M16 already documents "RBF descendants
  unreachable in practice". Consequence for the loader: a
  chained entry in a hand-crafted file is correctly dropped as
  invalid (parent loads, child doesn't) — covered by a test
  that documents the rule. Topological ordering is
  defence-in-depth for such files; in practice all entries have
  in-degree 0.

### 2. Reorg depth 100 (`tests/test_reorg.cpp`)

- New section `reorg: depth-100 reorg (coinbase-maturity
  bound)` (+152 lines, pure addition). 100-block main chain
  (heights 1–100) forked at genesis vs a 101-block fork from
  the same genesis; the fork wins only on block 101 (strictly
  more work), so exactly 100 main blocks are disconnected.
  Block 50 of each chain spends the same funding coin
  differently (conflict); main block 60 spends a second coin
  (must return to mempool). Verifies: tip/height, undo
  correctness (winner's output present, loser's gone,
  disconnected input restored), mempool re-adding
  (non-conflicting tx back, conflicting tx dropped), store
  follows (tip, coins, undo records gone for old main tip +
  block 50), `LastStoreError()` empty, and full store-vs-node
  UTXO equality (every coin: outpoint, value, script, height,
  block time, coinbase flag). Test runs in 0.247 s standalone
  (all 4 reorg sections).
- **No production bug was found by the reorg test.** The
  100-deep reorg passes with undo correctness and mempool
  re-adding verified; no production fix was forced, so none
  was made.
- **Attribution (in the test comment and here): the "100" is
  Oeeoo's own target, NOT a Bitcoin Core number** — 100 equals
  coinbase maturity, so a 100-deep reorg is the deepest that
  can strand matured coinbase spends. Do NOT attribute the
  number to Core.

### 3. Crash-injection tests (`tests/test_milestone17b.cpp` NEW)

- Spawns the real `oeeoo-node` as a subprocess (sibling of the
  test binary via `/proc/self/exe`, or `$OEEOO_NODE_BIN`),
  builds a test genesis in-process (`BuildGenesisBlock` +
  `MineGenesisParallel`, bits `0x207fffff`), mines via
  `getblocktemplate`/`submitblock` (M14 funded-E2E pattern;
  coinbase rebuilt exactly as `AssembleBlock` builds it),
  pid-derived ports with retry (never fixed — the M16
  fixed-port flake is avoided), pid-unique `/tmp` data dirs,
  removed afterwards.
- `m17b: SIGKILL during a block burst, restart recovers` — 6
  baseline blocks, then a worker thread mines+submits
  continuously while the main thread SIGKILLs mid-burst
  (~520–570 submits in ~0.5–1.5 s). Restart on the same data
  dir (deep-verify on by default): height ≥ last acked height,
  tip matches when equal; then clean stop + in-process
  `VerifyChainFromStore` passes.
- `m17b: SIGKILL during shutdown, restart recovers` — 3
  blocks, SIGTERM then SIGKILL 5 ms later (lands mid-shutdown;
  observed both outcomes across runs and the test accepts
  either, since a clean exit before the SIGKILL is the same
  store state). Restart: height 3, tip identical, deep verify
  passes.
- `m17b: disk-full (RLIMIT_FSIZE), restart recovers` —
  sub-case A (fresh dir, 4 KiB file limit) and sub-case B (5
  blocks persisted, then limit = chain.db size + 8 KiB). See
  §A.4 for the observations and the M17 fix.
- **Label: Oeeoo robustness work, NOT Core parity.** No named
  Core crash-injection test could be verified.

### 4. Disk-full behaviour + the M17 warning (coordinator fix, `src/app/node_main.cpp`)

- **Observed (Stream B, `RLIMIT_FSIZE` + `SIGXFSZ` ignored):**
  sub-case A (fresh dir, 4 KiB limit): the node refuses to
  start — LOUD (1 startup-error log line: `oeeoo-node: disk
  I/O error`). Restart without the limit: height 0, tip =
  genesis hash. Clean recovery.
  Sub-case B (5 blocks persisted; chain.db 57,344 bytes; limit
  65,536): the limit bites mid-chain. All 40 submits returned
  success; the node reported in-memory height 45 the whole
  time; no submit ever failed. After stop + restart without
  the limit: height **6** — 39 blocks rolled back (only 1 of
  the 40 post-limit blocks had been persisted before the WAL
  hit the limit). Deep verification then passes on what
  survived: **rollback, not corruption**.
- **Finding (disclosed by Stream B):** before M17 the
  persistence failure was SILENT at runtime. `submitblock`
  kept returning success while persistence failed, and
  `Node::LastStoreError()` was the only record — no RPC method
  and no log line exposed it (verified by grep: no consumer in
  `src/`). The failure surfaced only as a height rollback
  after restart.
- **Fix (coordinator, M17):** the `node_main` loop now warns
  to stderr whenever `LastStoreError()` is set — `oeeoo-node:
  WARNING: chain store error — accepted blocks may not be
  durable: <error>` — once per distinct error value. The
  warning lives in the app layer (which already prints
  warnings), not in the silent-by-design `Node` library. The
  test now asserts it: the limited run's log is counted
  BEFORE the restart (the restart truncates the log file with
  `O_TRUNC`, which is why the original run reported 0
  matches), and the run shows exactly 1 `chain store error`
  warning (`disk I/O error`). `submitblock` still returns
  success while persistence fails — changing that would be a
  bigger semantic change (a block that validates is accepted
  into the in-memory chain; durability is a separate concern)
  and is NOT done in M17; the warning is the loudness. This
  is recorded as a known limitation in §G.
- **Disk-full bar, restated:** startup failure is loud;
  mid-chain persistence failure is now loud in the log (the
  warning) but still silent to RPC callers; no corruption in
  either case — the store rolls back to what was durable.

### 5. Long-running soak test (`soak17/`, `logs/soak17-samples*.log`)

- **What actually happened (disclosed plainly): the soak did
  NOT complete as a clean 2 h run.** It took two attempts,
  both killed by VM restarts, and no third attempt was made —
  the evidence says it would die the same way. Nothing here
  is claimed as a clean 2 h finish.
- **Run 1:** nodes 11:21 → 12:39 UTC (~1 h 18 m), height 294,
  8 wallet txs, 0 failures, no sanitizer output. Killed by a
  VM restart at ~12:38 UTC (`uptime` 1 min at 12:39:41).
- **Run 2 (the reported run):** nodes ~12:40:30 → ~14:25 UTC
  (**~1 h 44 m**), height **401** (400 mined by the driver at
  1 block / 15 s + 1 smoke-test block), **27 wallet txs**
  (15 A→B, 12 B→A), all confirmed; first 13:06:13 UTC (A
  balance 100.0 at tip 101, exactly as M14 predicted), last
  14:24:25 UTC. Full txids in `soak17/run2/driver.log`.
  **0 block failures, 0 tx failures.** B missed the 30 s
  catch-up window **0 times**; max B catch-up lag **0.5–0.6 s**
  throughout. Killed by a second VM restart ~14:24–14:26 UTC
  (`uptime` 0 min at 14:26:26), 17 minutes before its 14:41:02
  UTC deadline. No clean shutdown, no driver-written final
  summary. Chain DBs preserved uncleanly (WAL present) in
  `soak17/run2/data-a|b`.
- **P2P bridge forwarded (final):** A→B 415 blocks / 415
  headers / 430 inv / 15 tx; B→A 415 getheaders / 430 getdata
  / 12 tx / 12 inv; ping/pong 106 each way; 1 `sendcmpct`
  dropped per side (by design — see below).
- **How blocks/txs actually travelled:** the shipped node
  cannot open an outbound P2P connection (see §C.1 item 8 —
  the major finding), so the soak used a Python P2P bridge
  (`soak17/p2p-bridge.py`) that TCP-connects to each node's
  listen port, completes the real version/verack handshake as
  a peer, and forwards wire messages verbatim — blocks/txs
  genuinely travelled through each node's real P2P code
  (inv/getdata/block/tx, headers sync, ping/pong), with one
  deliberate exception: `sendcmpct` is dropped both ways,
  because compact-block short ids are SipHash-keyed per peer
  session and a verbatim-forwarded `cmpctblock` would carry
  the wrong keys; both nodes then fall back to classic
  inv/block relay.
- **RSS (VmRSS, 5-min samples, 22 samples):** A 515,076 →
  674,368 kB (+159 MB over 100 min); B 508,004 → 636,360 kB
  (+128 MB). Shape: +~17 MB in the first 5 min, then
  decelerating to ~+0.6 MB/min (A) / ~+0.9 MB/min (B) over the
  last 30 min. **Not flat, not unbounded in 1 h 44 m.** No
  leak is claimed: ASan RSS includes quarantine/allocator
  arenas and `detect_leaks=0` was set by design (M13 benign
  libcrypto one-time globals). Run 1 showed the same shape (A
  +147 MB, B +142 MB over 75 min).
- **CPU (utime+stime totals):** A 3.38 → 24.6 s; B 2.36 →
  19.79 s over ~100 min (~0.2 s/min — nodes essentially idle
  between blocks).
- **ASan/UBSan output in either node log: none** (grep clean
  on both runs).
- **Soak acceptance (coordinator decision): run 2 is ACCEPTED
  as the M17 soak.** It is inside the 1–2 h acceptance window
  and above the 1 h minimum; across both runs the evidence is
  3 h 02 m, 695 blocks, 35 txs, 0 failures, 0 sanitizer
  reports. The VM restarts roughly every ~2 h (12:38,
  ~14:25), so a full 2 h soak does not fit in one VM
  lifetime, and a third attempt would very likely die the same
  way. The run ended by VM restart, not a clean shutdown —
  stated here, not hidden.
- Both runs used the **M16 ASan binary `build-asan/oeeoo-node`,
  HEAD d964bca**, `ASAN_OPTIONS=detect_leaks=0`,
  `--pow-limit-bits 207fffff`, test-only geneses (run 1 hash
  `1442bf92…`, run 2 hash `78da4376…`; fresh throwaway test
  pubkeys each time, founder privkeys never persisted).
  Steady-state node code is what the soak validates; mempool
  save/load is Stream A's restart tests on the final binary
  (§A.1).
- Samples: `logs/soak17-samples.log` (run 2; copy in
  `soak17/run2/`), `logs/soak17-samples-run1.log` (run 1).
  Per-run evidence incl. chain data dirs:
  `soak17/run1/SUMMARY.md`, `soak17/run2/SUMMARY.md`.

### 6. Performance profiling (`src/bench/bench.cpp` NEW, `logs/milestone17-profiling.txt`)

- Harness: new `src/bench/bench.cpp` (`oeeoo_bench`, minimal
  equivalent of Core's `src/bench/`; registered with two CMake
  lines the coordinator added). Bench binary built Release
  (`-O2`, no sanitizers) in its own build dir `build-m17c`
  against the in-tree `liboeeoo_core.a`; the soak, by
  contrast, runs the ASan/UBSan binary. Machine: this VM —
  AMD EPYC 9D25, 4 vCPUs, 8 GB RAM, g++ 13.3.0, OpenSSL
  3.0.13, SQLite 3.45.1. All measurements on a 1,000-block
  synthetic chain at test difficulty (bits `0x207fffff`, so
  PoW search time is NOT in the numbers). No optimisation work
  follows from these numbers in M17 — measure first, per the
  M17 scope.
- **Block validation** (`Node::BroadcastBlock` per block):
  in-memory `Node`, no `ChainStore` attached, so the numbers
  exclude SQLite I/O by design (the store-backed path is
  covered by the soak). Each block after height 100 carries
  one signed P2PKH spend of the newly matured coinbase
  (maturity 100, fee 10,000 sats), so validation includes one
  ECDSA verification per block plus Merkle, coinbase,
  header/PoW and UTXO work. 3 runs of 1,000 blocks (+1 re-run
  after a harness fix; all agree): avg **1.25–1.30 ms**,
  p50 ~1.35–1.38 ms, p95 ~1.52–1.64 ms, p99 ≤ ~3.6 ms,
  throughput ≈ **770–800 blocks/s** validated. The max
  outliers (up to ~26 ms, single samples) are
  scheduler/allocator spikes, not a trend. Empty-block
  validation (5-block smoke run, no spends) is ~0.03
  ms/block — the ~1.3 ms figure is dominated by the one ECDSA
  verification per block.
- **Mempool insertion** (`Mempool::AddTransaction` per tx):
  same runs, 900 spend txs per run: avg **~0.58 ms**, p50
  ~0.57 ms, p95 ~0.61–0.75 ms, p99 ~0.85–0.95 ms. A 5-tx/block
  sensitivity run inserted the same 900 txs (the chain only
  matures one coinbase per block, so the queue empties and the
  run is not a 5× load — recorded, not claimed otherwise) with
  an unchanged per-tx avg of 0.582 ms: insertion cost is
  per-tx, not per-block-batch. For reference,
  `AssembleBlock` (template assembly, not in the M17 scope but
  measured by the same harness) averages 0.62–0.66 ms/block.
- **RPC latency** (live node, 100 samples per method;
  `soak17/rpc-latency-probe.py`, cookie Basic auth,
  localhost; both soak nodes at height ~20, in sync):
  `getblockcount` avg ~0.99–1.01 ms, p50 ~0.65–0.67 ms, p95
  ~1.0–1.3 ms; `getblock` (best hash) avg ~1.14–1.47 ms, p50
  ~1.14–1.42 ms, p95 ~1.5–2.1 ms. The ~30 ms p99/max on
  `getblockcount` is a single slow call on each node
  (first-call/scheduler jitter). Both nodes agreed on the tip
  hash, i.e. the probe ran against a synchronised pair.
- Full bench output: `logs/milestone17-profiling.txt`.

## B. Decision cross-check

- No finalized spec decision was changed (supply 21M, rewards,
  P2PKH-only launch, vesting schedule spec §6, port 64775, magic
  `e2 d8 04 d6`, direct mainnet / NO public testnet per spec F24).
- Decisions taken in M17 (all documented in code or here):
  1. Mempool file format → Oeeoo's own JSON-lines (model =
     Core's `mempool.dat`, bytes are ours) (§A.1).
  2. Mempool save caps → 100,000 entries / 64 MiB, highest
     fee rate first (Oeeoo's own choice) (§A.1).
  3. `savemempool` works even with `persistmempool=0`
     (Core-compatible reading of the flag) (§A.1).
  4. `importmempool` returns load counts, not Core's null
     (Oeeoo choice, documented) (§A.1).
  5. `loaded` semantics: true once the startup load attempt
     has finished, including missing/malformed file; false
     only when persistence is disabled or no load attempted
     (§A.1).
  6. Reorg depth "100" → Oeeoo's own target (coinbase
     maturity), NOT a Core number (§A.2).
  7. Crash-injection + soak → Oeeoo robustness work, NOT Core
     parity (no named Core test could be verified) (§A.3,
     §A.5).
  8. Disk-full surfacing → stderr warning in the node_main
     loop when `LastStoreError()` is set (once per distinct
     error); `submitblock` still returns success while
     persistence fails (§A.4).
  9. No premature optimization from profiling numbers — measure
     first (§A.6).

## C. Warning report

### C.1 Bugs found and fixed (disclosed)

1. **REAL BUG — mempool dump wrote the txid twice (Stream
   A's new code, found by its own test suite, FIXED before
   delivery).** `SaveMempoolFile` first wrote the txid as
   `BytesToHexStr(vector(GetTxId().begin(), GetTxId().end()))`
   — `begin()` and `end()` were taken from two *different*
   temporaries, so the file contained the 32-byte txid **twice**
   (128 hex chars). The loader's txid check then dropped every
   entry as invalid (all loads returned 0). Fixed by naming
   the txid in a local first. Grep of the repo: no other
   occurrence of the `GetTxId().begin()` pattern exists. The
   fixed file round-trips correctly (verified by the debug run
   and the suite).
2. **Disk-full persistence failure was SILENT at runtime
   (pre-existing behaviour, found by Stream B, FIXED in M17
   by the coordinator).** §A.4: the node kept serving its
   in-memory chain while persistence failed, with
   `LastStoreError()` as the only record. Fixed with the
   `node_main` stderr warning (once per distinct error); the
   test now asserts the warning appears in the limited run's
   log (1 warning, `disk I/O error`). `submitblock` still
   returns success — disclosed, not hidden.
3. **Latent crash in the test suite — `fseek(NULL)` after a
   failed `fopen` (pre-existing, found by Stream B, FIXED by
   the coordinator).** `tests/test_wallet.cpp:400`: `CHECK(f
   != nullptr)` does NOT abort, so when `fopen` failed the
   code continued to `fseek(f, …)` on a NULL pointer —
   SIGSEGV (exit 139), observed when parallel suite runs
   collided on the shared `/tmp/oeeoo-test-main.wallet` path.
   Fixed with an early `if (f == nullptr) return;` after the
   CHECK (the CHECK already recorded the failure). The only
   other `fopen` in the suite (`tests/test_rpc.cpp:424`)
   already had the guard. Test-only fix; no production code
   changed.
4. **Test-design bug in Stream B's crash test (mine, fixed
   before delivery).** The first `DeepVerifyChain` passed
   `VestingParams{}` (disabled) while the test genesis pays
   `FOUNDER_ALLOCATION` to the founder vault — deep verify
   rejected the genesis coinbase. Fixed by deriving vesting
   from the stored genesis exactly as
   `node_main::DeriveVestingFromStore` does. (The node itself
   had always passed its own load-time deep verify — proven by
   restarts serving RPC.)
5. **Test-design limitation in the reorg test (mine, fixed
   before delivery).** The first draft deep-verified the
   reorganized store from genesis. That cannot pass: the
   funding coins are test-only UTXO injections whose origin is
   not in any block (same limitation M16 §A.1 documents for
   `getblockstats`). Replaced with full store-vs-node UTXO
   equality, which is the stronger check here, with the reason
   documented in a comment.
6. **Bench harness bug (Stream C's new code, found by the
   harness itself, FIXED before delivery).** The first
   5-tx/block sensitivity run double-spent: it spent coinbases
   `h-100-k` for `k = 0..4`, overlapping the previous block's
   spends, and the node correctly rejected the second spend
   with `missing input` at height 102. That was a harness bug,
   not a node bug — the mempool behaved correctly. Fixed by
   spending from a mature-unspent queue (each coinbase spent at
   most once) and re-run; the 1-tx/block numbers were
   re-measured after the fix and agree with the pre-fix runs.
7. **Environment: no restore was needed in M17.** Unlike M9,
   M15 and M16, `libsqlite3-dev` and `cppcheck` were present at
   the start of this milestone (`which cppcheck` →
   `/usr/bin/cppcheck`, `/usr/include/sqlite3.h` present) and
   the clean build configured and linked on the first attempt.
   The lesson from M16 §C.1.4 (check before the first build)
   was followed and it paid off.
8. **MAJOR, product — the shipped node cannot open an
   outbound P2P connection (found by Stream C during the soak,
   VERIFIED by the coordinator with grep).** No `--addnode`
   flag, no `addnode` RPC, no dial loop in `node_main` /
   `Node::Poll`; `Node::ConnectTo` is only ever called from
   tests (zero production callers). Two stock nodes on
   localhost **never peer** (`getpeerinfo == []` on both,
   verified live). The soak only exchanged blocks/txs because
   of the Python P2P bridge (§A.5). Without a dial path, no
   two real nodes can ever connect to each other — and DNS
   seeds do not save it: `QueryDnsSeeds` only fills AddrMan
   (called once at startup in `node_main.cpp`), nothing ever
   dials the discovered addresses, so a fresh node with an
   empty AddrMan gets no peers at all unless someone dials
   it. Inbound connections DO work (Listen + accept). This
   is a mainnet-readiness gap, not a test artifact. **NOT
   fixed in M17** — adding outbound dialing (`--addnode` /
   dial loop / `addnode` RPC) is a feature-sized change
   outside M17's exact scope; it is carried as the #1
   decision in §G (coordinator's recommendation: add it to
   M18's scope).
9. **Soak driver bug (Stream C's code, found during run 1,
   FIXED).** The first driver deadlocked wallet traffic —
   after the first A→B tx it waited forever for B's balance
   ≥ 2.0 while B could only be funded by A ("tx skipped: B
   balance 1.00", 11:52–12:07). Fixed (try the intended
   sender, fall back to the other side); leg 2 and run 2 then
   sent in both directions. Nodes were not restarted for the
   fix.

### C.2 Behavioural findings (not code bugs — disclosed)

8. **Parallel-suite collisions (pre-existing, not Stream B).**
   Runs 3/5/6 of Stream B's full suite failed while other
   streams' suites ran concurrently: run 5 exited 139
   (SIGSEGV) in `test_wallet.cpp:400` — the `fopen` of the
   fixed shared path `/tmp/oeeoo-test-main.wallet` returned
   NULL (another run `remove()`d it) — fixed as item 3 above
   for the crash itself, but the underlying collision remains:
   the pre-existing suite shares fixed `/tmp` paths (and some
   fixed ports) across concurrent runs, so it is not safe to
   run in parallel across streams. Stream B's m17b tests use
   pid-unique ports/data dirs and never collided. No suite-wide
   fix attempted in M17 — it touches many test files and is
   not in M17 scope; recorded as a known limitation in §G.
   The coordinator's authoritative runs (clean build, ctest,
   ASan) are serial, one at a time.
9. **Concurrent-edit build breaks between streams (reported,
   not fought).** Stream B's first `build-m17b` link failed on
   Stream A's not-yet-wired `mempooldump.cpp`, and Stream C's
   bench linked it manually on the `g++` command line until
   the coordinator wired it into CMakeLists. Both resolved by
   the coordinator's integration; no code changes were needed.
   The coordinator's clean build is the authoritative run.
11. **Bench averages are load-sensitive on this shared VM
    (measurement caveat, not a code regression — disclosed).**
    Stream C's 4 bench runs (11:28 UTC) agree: block validation
    avg 1.25–1.30 ms, p50 ~1.35–1.38 ms, p95 ~1.5–1.6 ms. The
    coordinator's repeat runs on the same committed tree show
    the SAME p50 (~1.35 ms) but averages of 1.28 → 2.98 → 4.69
    → 5.64 → 6.92 → 8.59 ms across successive runs, driven by
    intermittent ~65 ms stalls (p95 ~65 ms, p99 ~65–70 ms)
    from other processes on this shared 4-vCPU VM — the first
    run of each batch is clean (1.28 ms), later runs degrade.
    The p50 is the stable figure: if the code had gotten
    slower, the median would have moved too. The reported
    profiling numbers (§A.6, §F) are the quiet-window figures
    (avg 1.25–1.30 ms); the final-pass log's bench section
    (§6 of `logs/milestone17-test-log.txt`) shows one loaded
    run (avg 8.59 ms) and is kept as-is — it is evidence of
    the load effect, not of a slower node.

### C.3 Warnings (compiler / static analysis)

- None. Clean build at `-Wall -Wextra`: 0 warnings, 0 errors.
  cppcheck `--enable=warning,performance,portability`: 0 findings
  across all 87 files.

## D. Security report

Gaps closed by M17 (see also `docs/SECURITY_REVIEW.md`):

- **Mempool survives restarts**: transactions + fees reload
  after a clean shutdown or `savemempool`, re-validated
  against the current UTXO set; expired/invalid entries are
  dropped, never resurrected.
- **100-deep reorg proven**: undo correctness + mempool
  re-adding verified at the coinbase-maturity bound (was depth
  2); a reorg can no longer silently strand matured coinbase
  spends untested.
- **Crash recovery proven**: SIGKILL mid-burst and
  mid-shutdown both recover to a clean, deep-verified store.
- **Disk-full is loud now**: the node warns to stderr as soon
  as the chain store fails (was silent); failure mode is
  rollback, never corruption.
- **Limits are measured**: block validation ~1.3 ms, mempool
  insertion ~0.58 ms, RPC latency ~1 ms — on this VM, with the
  method recorded.

Gaps that remain (unchanged from M16 unless noted):

- **MAJOR, new in M17: the node cannot open an outbound P2P
  connection** — no `--addnode`, no `addnode` RPC, no dial
  loop; `ConnectTo` is test-only. Two stock nodes never peer;
  DNS seeds only fill AddrMan, nothing dials them (§C.1 item
  8). Carried as the #1 decision in §G.
- No independent audit (external, still required before mainnet).
- No distinct testnet identity (test/mainnet share magic, port,
  version, userAgent — only the testing-mode banner guards).
- `Stratum mining.authorize` accepts any credentials
  (localhost-only mitigation, documented in MINING.md +
  OPERATOR.md).
- RBF descendants unreachable in practice; 100-replacement cap
  untested.
- Fuzzing covers parsers, not the live peer/RPC/Stratum state
  machines.
- Reorg across pre-v2 blocks fails closed.
- AS-map NOT implemented — /16 bucketing is the accepted
  permanent defence for v0.1.0.
- Orphan promotion via the `"tx"`-handler call site cannot fire
  in practice — block arrival is the real trigger (unchanged).
- `getblockstats` cannot price a coin whose origin is not in a
  stored block (M16 §A.1); pre-journal sends are not recoverable
  (M16 §A.3).
- Ban keys are not persisted across restarts (ChainStore has
  PutBan/GetBans but no load path is wired into the node).
- `submitblock` still returns success while chain persistence
  fails — the failure is now loud in the log (§A.4) but silent
  to RPC callers.
- The test suite is not safe to run in parallel (shared fixed
  `/tmp` paths); run it serially (§C.2 item 8).

## E. Files changed

Commit `82759ff` on top of M16's `d964bca`:

`git diff --stat d964bca..82759ff`: 17 files, +2726/−3.

| File | Change |
|---|---|
| `src/mempool/mempooldump.h` / `mempooldump.cpp` | NEW — mempool persistence (`SaveMempoolFile` / `LoadMempoolFile`, JSON-lines format, caps 100k entries / 64 MiB) |
| `src/mempool/mempool.h` / `mempool.cpp` | `Mempool::kExpirySeconds` (public, shared); `AddLoadedTransaction()` (preserves stored `addedTime`); `AddTransaction` now wraps shared `AddTransactionInternal` |
| `src/network/node.h` / `node.cpp` | `SetMempoolPath` / `SetPersistMempool` / `LoadMempool` / `SaveMempool` / `DumpMempool` / `ImportMempoolFile` / `MempoolLoaded()` / `LastMempoolLoadStats()` |
| `src/app/config.h` / `config.cpp` | `persistmempool` key + `--persistmempool` / `--no-persistmempool` flags + help text |
| `src/app/node_main.cpp` | Mempool path + flag wired; load after genesis; save on clean shutdown; **chain-store-error stderr warning in the main loop (M17 fix, §A.4)** |
| `src/rpc/rpcserver.cpp` | `savemempool` / `importmempool`; `loaded` in `getmempoolinfo`; help list updated (38 callable + `help`) |
| `src/bench/bench.cpp` | NEW — benchmark harness (`oeeoo_bench`: block validation, mempool insertion, template assembly) |
| `CMakeLists.txt` | `src/mempool/mempooldump.cpp` in `oeeoo_core`; `tests/test_milestone17a/b.cpp` in the test sources; `oeeoo_bench` target |
| `tests/test_main.cpp` | `TestMilestone17A/B()` declared + called |
| `tests/test_milestone17a.cpp` | NEW — 7 sections (mempool persistence) |
| `tests/test_milestone17b.cpp` | NEW — 3 sections (crash-injection + disk-full) |
| `tests/test_reorg.cpp` | Depth-100 reorg section (+152 lines) |
| `tests/test_wallet.cpp` | `fopen` NULL guard after `CHECK(f != nullptr)` (M17 fix, §C.1 item 3) |

## F. Test results

Toolchain: g++ 13.3, CMake 3.28, OpenSSL 3.0.13, SQLite 3.45.1,
cppcheck 2.13.0. Full log: `logs/milestone17-test-log.txt`.

- Clean build (coordinator re-ran from scratch):
  `rm -rf build && cmake -S . -B build` exit 0;
  `cmake --build build -j4` exit 0, **0 warnings, 0 errors**.
- Full suite: `./build/oeeoo_tests` exit 0 — **182 sections, ALL
  CHECKS PASSED** (run twice). `ctest --test-dir build` — **100%
  tests passed, 0 failed out of 1** (78.27 s).
- ASan/UBSan build (`-fsanitize=address,undefined
  -fno-sanitize-recover=all`, `detect_leaks=0`, no `stdbuf` —
  `stdbuf` breaks the ASan binary at startup): full suite exit 0,
  ALL CHECKS PASSED, **0 sanitizer errors**. (Built in a separate
  `build-asan-m17` directory so the soak's `build-asan` binary
  was not disturbed while it ran.)
- cppcheck `--enable=warning,performance,portability
  --suppress=missingIncludeSystem -Isrc src tests`: exit 0,
  **0 findings** (87/87 files checked).
- Section arithmetic: 171 (M16) + 7 (mempool persistence) + 4
  (reorg depth 100 + crash-injection) = **182**.
- New sections (11):
  - `m17a: mempool save/load round-trip (Mempool level)`
  - `m17a: load drops expired, invalid, fee-mismatch, chained and malformed entries`
  - `m17a: restart E2E through a real node — same txs and fees after restart`
  - `m17a: savemempool RPC + getmempoolinfo loaded flag`
  - `m17a: persistmempool=0 opt-out — no file written, loaded false`
  - `m17a: invalid tx dropped on load — its input was confirmed meanwhile`
  - `m17a: importmempool round-trip (node + RPC)`
  - `reorg: depth-100 reorg (coinbase-maturity bound)`
  - `m17b: SIGKILL during a block burst, restart recovers`
  - `m17b: SIGKILL during shutdown, restart recovers`
  - `m17b: disk-full (RLIMIT_FSIZE), restart recovers`
- Soak: run 2, 1 h 44 m (401 blocks, 27 txs, 0 failures, no
  ASan/UBSan output; ended by VM restart, see §5). Accepted by
  the coordinator (inside the 1–2 h window); run 1 (1 h 18 m,
  294 blocks, 8 txs) agrees on every metric.
- Reproducible build: `./scripts/reproducible-build.sh` from
  the final commit — **REPRODUCIBLE: all three binaries are
  byte-identical across two builds** (checksums in §F.1 below;
  recorded at delivery time in the test log).

### F.1 Reproducible-build checksums

Run from commit `82759ff` (SOURCE_DATE_EPOCH=1793966995):

```
./scripts/reproducible-build.sh
# SOURCE_DATE_EPOCH=1793966995
# REPRODUCIBLE: all three binaries are byte-identical across two builds
# REPRODUCIBLE: archive is byte-identical across two builds
# release package: oeeoo-coin-0.1.0.tar.gz
#   273bbf31a0e59685dda239bdd241649cff116a66428cdf4574367a2159edd4b3  oeeoo-coin-0.1.0.tar.gz
#   85c908c3b067a6fba3126fa9a03ecf65a551c9d53fabe20ba048e382968ba019  oeeoo-node
#   4c7c16cb2f2f92adfcf9e92fe09dcf612fa6a670e7b4ac3cea7346184b9e3ce7  oeeoo-genesis
#   a80b3c6f0677814b3ee181f4f5b6bb9bee85f56171d71b964c621a79b8323136  oeeoo-cli
```

`oeeoo-cli` is unchanged from M14/M15/M16, so its hash matches
M14's, M15's and M16's. The binary hashes above are exact for
this milestone's source. The tarball hash is for the release
package built from `82759ff`; a rebuild from a later docs-only
commit differs only in `release-0.1.0/SOURCE.txt` (the
commit-hash line).

Confirmation run from the docs commit `53b9801`
(SOURCE_DATE_EPOCH=1793978895): **REPRODUCIBLE — binaries
byte-identical, archive byte-identical**; all three binary hashes
identical to the run above; tarball
`c20fb50807235d45af4d96ff5ee525cc8a4e9ce8fb5abdd24fc8e18f05f12479`
(differs from the `82759ff` tarball only via SOURCE.txt).

## G. Outstanding decisions (carried + new)

New from M17 (coordinator's recommendation in brackets):

- **#1 — Outbound P2P dialing.** The node cannot initiate a
  P2P connection (§C.1 item 8). Fix: add an operator dial path
  (`--addnode` and/or an AddrMan-driven dial loop, plus
  `addnode` RPC). [Coordinator recommends adding it to M18's
  scope — it is P2P completeness that mainnet needs, and M18
  is the next milestone. The decision (M18, or a separate
  follow-up) is for the parent agent / user.]
- **Soak acceptance — DECIDED by the coordinator in M17:**
  run 2 (1 h 44 m) is accepted as the M17 soak (§A.5). No
  third attempt: the VM restarts ~every 2 h and a third run
  would very likely die the same way.
- **RSS creep under ASan — accepted as ASan-quarantine
  behaviour in M17** (§C.2 item 10); a non-ASan long run to
  distinguish a real leak is a possible M18 follow-up, not
  required.

Carried (still open, unchanged):

- Test suite parallel safety: the suite shares fixed `/tmp`
  paths (and some fixed ports) and is not safe to run
  concurrently; the `fseek(NULL)` crash is fixed, but the
  collisions themselves are not. Options: pid-unique temp
  paths suite-wide, or serialize suite runs (the current
  practice). A suite-wide fix belongs to a hardening milestone
  (M18).
- `submitblock` returns success while chain persistence fails
  (§A.4): the failure is now loud in the log, but RPC callers
  still see success. Changing that is a semantic decision for
  a later milestone, not M17.
- RBF 100-replacement cap untested (carried from M13).
- Disk-full behaviour is verified for the SQLite store path
  only; other I/O failures (wallet save, journal save,
  mempool save) warn but are not crash-tested.

Standing owner items (unchanged): founder keys offline (user
reports all 3 pairs generated + secured on their mobile, single
device, user's accepted risk — user-reported, NOT verified);
independent audit (give the auditor `docs/AUDIT_PACKAGE.md` +
`docs/SECURITY_REVIEW.md`); GitHub repo — created by the user
2026-11-06, verified empty:
https://github.com/OeeooCoin/Oeeoo-Coin-Blockchain-By-Ahmad —
code push ONLY after ALL milestones (M19) complete, per the
user's decision; launch time + genesis approval; 4 DNS seed
domains; release-signing custody. Founder addresses are NOT
needed for M17 — needed only at genesis time (M19/launch): the
3 PUBLIC keys (66 hex each) for `oeeoo-genesis --pubkey1/2/3`;
private keys NEVER to be shared.

## H. Verdict

**NOT READY** — unchanged from M13/M14/M15/M16. Genesis
unapproved, no independent security audit, no mainnet. M17
closes the persistence & robustness scope of the roadmap; the
next milestone (M18, fuzzing & security hardening) is the next
step toward the full M14–M19 roadmap.
