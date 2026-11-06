# Oeeoo Coin — Milestone 18 Stream C: M13 audit re-run (DRAFT for coordinator integration)

Status: DRAFT — Stream C (differential tests + ASan/UBSan soak re-run +
M13 audit re-run). Complete: all three parts finished 2026-11-06
~23:15 UTC. For coordinator integration into docs/MILESTONE18.md,
docs/SECURITY_REVIEW.md, docs/AUDIT_PACKAGE.md.

Method: same 18-section brief as docs/MILESTONE13.md, re-run against
HEAD b9977af (M17) plus the M14–M17 changes. Every item is reported as
MATCH / MISMATCH / FIXED / NOT IMPLEMENTED / NOT VERIFIED /
REQUIRES DECISION. Evidence is file:line in the pristine HEAD tree
(verified in ~/workspace/oeeoo-pristine, a clean worktree of b9977af —
NOT the main working tree, which carries other streams' uncommitted
M18 edits; see finding A2).

## Verdict (re-run): NOT READY — unchanged from M13/M14/M15/M16/M17

Genesis unapproved, no independent security audit, no mainnet. The
re-run closes nothing that changes launch readiness; it finds one
previously unflagged decision-vs-code gap (F17 "10 outbound", finding
A1) and confirms the M17 no-dial finding is still true in HEAD
(finding A5). No consensus-touching issue was found: no consensus
code was changed, and the differential tests agree with the
independent Python reference on every vector.

## A. New findings from this re-run (Stream C)

### A1. DECISION-VS-CODE GAP (new, not flagged by M13): F17 "10 outbound" NOT IMPLEMENTED in HEAD
- DECISION_LOG.md F17 says "Peer limits 125 total / 10 outbound".
- Code (HEAD b9977af): `Node::ConnectTo` enforces only the total cap
  (`src/network/node.cpp:141`: `peers_.size() >= maxPeers_`, maxPeers_
  default 125); grep for `kMaxOutbound|maxOutbound` across `src/` in
  HEAD returns nothing — no outbound cap exists anywhere.
- Status: NOT IMPLEMENTED (the 125 half is MATCH; the 10 half is not
  in the code). M13 reported only "Peer cap 125 FIXED" and did not
  check the outbound half.
- Note: M18 Stream A is adding `maxOutboundPeers_ = 10` (uncommitted
  in the main working tree at the time of this audit); the fix is in
  flight, not in HEAD. The coordinator should confirm it lands with
  a test before M18 closes.

### A2. PROCESS FINDING (not a code bug): concurrent-edit "franken-build" aborted under UBSan
- Stream C's first ASan build (`build-m18c`, from the main working
  tree) produced a node binary that aborted at startup:
  `src/network/node.cpp:1401:8: runtime error: load of value 58
  (then 29 on re-run), which is not a valid value for type 'bool'`,
  exit 1, in `Node::LoadMempool` (per the UBSan stack trace).
- Cause: Stream A saved `src/network/node.cpp` (+148 lines, dialing
  implementation) at 20:42:21 UTC while Stream C's build was
  compiling/linking it (binary linked 20:42:34). The compiler read a
  file mid-edit; header and translation unit disagreed.
- Proof it is NOT a code bug: the same startup on a pristine HEAD
  worktree binary (`~/workspace/oeeoo-pristine`, b9977af, same flags)
  starts cleanly, accepts the soak genesis, serves RPC, and shuts
  down gracefully with zero sanitizer output (reproduced 2026-11-06
  ~21:03 UTC). The M17 ASan suite (182 sections) also passed on HEAD.
- Consequence: the soak in this stream runs on the pristine-HEAD
  binary, NOT on the contaminated `build-m18c` binary; the
  `build-m18c` binary must not be used for any evidence. Lesson for
  the coordinator: parallel streams sharing one working tree must
  build from worktrees or pinned commits, not from a tree another
  stream is editing — the M16/M17 reports note "concurrent-edit
  build breaks" as link errors; this is the first time one produced
  a silently wrong binary that only UBSan caught.

### A3. Spec §20 item 14 (0.0001 OEEOO transfer-fee figure): NOT VERIFIED (minor, unchanged)
- The dynamic fee policy is implemented (min relay fee 1 sat/vB,
  fee estimator over 24 blocks, M13). The historical 0.0001 OEEOO
  figure appears nowhere in `src/` as a fee and no doc records an
  explicit reconciliation of the two. The figure is effectively
  superseded, but no reconciliation note exists. No code change
  proposed; a one-line note in DECISION_LOG item 9 would close it.

### A4. Spec §20 item 16 (hardcoded fallback peer list): NOT IMPLEMENTED (unchanged, documented)
- `src/network/addrman.h:9` notes fallback peers "likewise
  unresolved (item 16)". No fallback list exists. With no DNS seeds
  either, a fresh node with an empty AddrMan learns no peers unless
  dialed (compounded by finding A5).

### A5. MAJOR, carried from M17 (unchanged in HEAD): node cannot open an outbound P2P connection
- No `--addnode`, no `addnode` RPC, no dial loop in HEAD b9977af;
  `Node::ConnectTo` is called only from tests. Two stock nodes never
  peer (`getpeerinfo == []` on both — re-verified live by the M18
  soak setup before the bridge started; the soak uses the same Python
  P2P bridge as M17). DNS seeds only fill AddrMan; nothing dials
  them. Stream A is implementing the fix in M18 (uncommitted at
  audit time); this stream's soak therefore exercises the M17 bridge
  path, NOT real dialing, and says so plainly.

### A6. M13 "NOT VERIFIED" item still open: RBF 100-replacement cap untested
- Implemented (`src/mempool/mempool.cpp:15` kMaxReplacementCount =
  100; enforced at `:181`) but no test in `tests/` references it;
  the M13 RBF section (`tests/test_milestone13.cpp:189`) covers
  signal/fee/fee-rate/non-signaling only. Descendant handling is
  likewise still defense-in-depth (unreachable: validation rejects
  unconfirmed inputs). Status: NOT VERIFIED, unchanged since M13.

### A7. Soak observation (not a code bug — load-correlated, disclosed): B catch-up misses + bridge reconnect storm
- During the soak's leg 2 (21:57–22:03 UTC), the P2P bridge's pumps
  died and reconnected repeatedly (B read failed: connection reset /
  closed by peer; A forward broken pipe), and B missed the 30 s
  catch-up window for blocks 154, 155, 156, 164. Two more misses
  followed at blocks 182, 185, 186 (22:11–22:14) while the bridge
  was stable but the VM ran a second ctest run plus two clang++
  builds (load avg 8–16). B recovered every time: heights agreed at
  every 5-min sample, no blocks or txs were lost, and the driver's
  block/tx failure counters stayed at the two RPC timeouts below.
- Two RPC timeouts under the same load: 21:59:25 "send failed: timed
  out", 22:09:51 "mine failed: timed out" (urllib 30 s timeouts, not
  node rejections). M17's soak had 0 misses in 401 blocks on a
  quieter VM; this run's misses correlate with extreme CPU
  contention, but the correlation is not proof — a quiet-window
  re-run would be needed to clear the node fully. Reported as an
  observation, not a finding against the node.
- Related minor observation: node A's peer count grew 1 → 3 → 5
  across the run while B stayed at 1: each bridge reconnect adds a
  peer entry on A that lingers after the old connection dies
  (stale peer entries are not reaped promptly). No impact on the
  soak; a peer-timeout would be a hardening follow-up, not M18
  scope for this stream.

### A8. Soak interruption 21:31 UTC (killer unidentified — disclosed, not a node bug)
- At ~21:30:20–21:31:18 UTC all four soak processes (2 nodes,
  bridge, driver) died within ~1 minute; BOTH nodes logged graceful
  "Shutting down..." in the same second (21:31:18.9), i.e. a
  SIGTERM-like shutdown, not a crash; the driver wrote no error and
  no final summary; no VM restart (uptime continuous at 1:20);
  no OOM evidence in readable dmesg (OOM sends SIGKILL, which
  would not print "Shutting down..."). The killer was not
  identified. The chain survived intact (restart at height 93,
  same tip). The soak was restarted (leg 2) with setsid-detached
  processes and the same deadline; the pattern did not recur.
  The runtime's handling of backgrounded exec sessions is the
  prime suspect (the processes were children of exec shells), but
  that is unproven — stated as unproven.

## B. Decision cross-check (M13 table re-verified + M14–M17 additions)

### B.1 Finalized decisions (spec + owner) vs HEAD b9977af code

| Decision | Status | Evidence |
|---|---|---|
| Max supply 21,000,000 = 20M mining + 1M founder | MATCH | `src/consensus/consensus.h:13-17`; differential test m18c re-verifies the per-block sum 1..940,000 == 20,000,000 against an independent Python reference |
| Rewards 50/25/12.5/6.25/3.125, halving every 210,000; 0 after 940,000 | MATCH | `src/consensus/subsidy.cpp`; boundary tests + m18c Python cross-check (incl. the short 5th era 840001..940000) |
| Block time 600 s, retarget every 2,016 blocks, 4x clamp | MATCH | `src/consensus/difficulty.cpp`; retarget clamp [302400, 4838400]; m18c Python cross-check on 14 retarget vectors |
| Max block size 1,000,000 bytes | MATCH | `src/consensus/consensus.h:28` MAX_BLOCK_SIZE 1000000 |
| 10-min target / mainnet PoW limit 0x1d00ffff | MATCH | `src/consensus/difficulty.h` kGenesisBits 0x1d00ffff |
| Port 64775, magic e2 d8 04 d6, P2PKH 0x0f ("7" prefix) | MATCH | `src/app/config.h:16`; `src/network/protocol.h:26-27`; `src/crypto/address.h`; m18c Base58Check cross-check |
| Founder vesting 10 × 100,000, launch + 9 anniversaries, Feb 29 → Feb 28 | MATCH | `src/consensus/vesting.h:41-42`; m18c Python cross-check incl. a Feb 29 2028 launch |
| Legacy P2PKH only at launch | MATCH | No SegWit/Taproot/bech32 in `src/` (only comments noting their absence) |
| Min relay fee 1 sat/vB | MATCH | Mempool exact-integer compare; `getmempoolinfo` mempoolminfee 1e-08 |
| Mempool 300 MB default, 14-day expiry | MATCH | `src/mempool/mempool.h:33,42`; `config.h:62`; expiry sweep in Node::Poll |
| Opt-in RBF (BIP125-style) | MATCH (cap untested, see A6) | `src/mempool/mempool.cpp:15-16` |
| Dynamic script-aware dust | MATCH | `src/transactions/dust.*` (P2PKH 182 / 2-of-3 multisig 304 at 1 sat/vB); policy, not consensus |
| Fee estimation over last 24 blocks | MATCH | `src/mempool/feeestimator.*` + `estimatesmartfee` |
| Wallet BIP39 24 words, BIP32, BIP44 m/44'/65535'/… | MATCH | M8 vectors; coin type 65535 provisional |
| Wallet encryption PBKDF2-HMAC-SHA512 200k + AES-256-GCM | MATCH | 16 B salt, 12 B nonce, 16 B tag; M13 tamper/truncation tests pass |
| Keys never logged / never exposed via RPC | MATCH | No dumpprivkey/dumpwallet/xprv/mnemonic/seed method; mnemonic appears only in `createwallet`'s once-return over localhost (by design, M14), never in any log call (grep-verified) |
| SQLite WAL + synchronous=FULL, schema v2, atomicity | MATCH | Schema v2 + undo table; M17 SIGKILL tests recover; M18 soak leg-3 restart after unclean VM restart recovered at height 187, identical tips |
| Pruning safety | MATCH | Bodies NULLed below height; deep verify fails loudly on pruned history |
| RPC binds 127.0.0.1, cookie auth mandatory | MATCH | 32 random bytes, mode 0600, constant-time compare; worker pool 16 + queue 64 (M16), queue-full → HTTP 500 |
| RPC surface 38 callable methods + help | MATCH | Counted from the help list in `src/rpc/rpcserver.cpp:581+`: 38 callable names + `help` |
| Stratum share validation | MATCH | extranonce2 exactly 8 hex (`src/rpc/stratum.cpp:536`); ntime window; low-difficulty shares rejected |
| Genesis deterministic, verified fields | MATCH | ReadGenesisFile verifies hash/time/bits/nonce/Merkle/message; CheckGenesisIdentity refuses mismatches |
| Peer cap 125 | MATCH | `Node::SetMaxPeers`, enforced outbound + inbound (M13) — but see A1 for the outbound-10 half of F17 |
| Peer limits 125 total / 10 outbound (F17) | MISMATCH (partial) | 125 MATCH; 10 outbound NOT IMPLEMENTED in HEAD (finding A1) |
| Compact relay + full-block fallback | MATCH | PoW before fetch; 120 s pending expiry; getblocktxn retry + full-block getdata; per-peer in-flight limit 5 (M15) |
| Node wallet (M14) | MATCH | Loads LOCKED; walletpassphrase/walletlock/createwallet; passphrase never a CLI flag; locked watch-only balance |
| Wallet-tx journal sidecar (M16) | MATCH | `<wallet>.txjournal`, JSON-lines, tmp+fsync+rename; wallet file format unchanged |
| Mempool persistence (M17) | MATCH | `<datadir>/mempool.dat`, JSON-lines, caps 100k entries / 64 MiB, atomic save, re-validated load, `loaded` flag; `savemempool`/`importmempool` RPC |
| Reorg depth 100 | MATCH | Tested to depth 100 (M17); undo correctness + mempool re-adding verified |
| Crash recovery / disk-full | MATCH | SIGKILL tests pass; disk-full rolls back, never corrupts; node_main stderr warning on store error (M17) |
| DNS seed timeout | MATCH | 10 s deadline on worker thread (`src/network/dnsseed.h:33`) |
| AddrMan anti-fill | MATCH | 1,000 cap, /16 cap 64, 30-day expiry, 3 failures → drop (M15) |
| Orphan pool bounded | MATCH | Max 100 / 5 MB / 20-min expiry, per-peer purge, never relayed (M15); promotion still block-arrival-only (M16 decision) |
| Peer eviction at cap | MATCH | Inbound eviction with documented ranking (M15) |
| getchaintips/getblockstats/getchaintxstats | MATCH | M16; real sigops + fees in getblocktemplate; sigoplimit 20000 template POLICY only, not consensus |
| Modern address formats (SegWit/Taproot) | REQUIRES DECISION | Spec: P2PKH only at launch; nothing implemented |
| Genesis timestamp / hash / founder keys | REQUIRES DECISION | Unapproved; no mainnet genesis; no hardcoded checkpoint |
| DNS seed domains / operators | REQUIRES DECISION | Mechanism built, list empty; no hostnames invented |
| SLIP-0044 coin type | REQUIRES DECISION | Provisional 65535, unregistered |
| Release-signing custody | REQUIRES DECISION | Reproducible package exists; signing undecided |
| Stratum V2 | REQUIRES DECISION (deferred) | Spec §13.1 requires it eventually; V1 only; deferred past launch by user delegation (M16) |
| Multi-wallet | REQUIRES DECISION (deferred) | Single wallet; future, post-M19/post-launch (M16) |
| AS-map | NOT IMPLEMENTED (decided) | /16 bucketing accepted permanently for v0.1.0 (M16 decision) |
| Distinct testnet identity | REQUIRES DECISION | Test/mainnet share magic, port, version, userAgent; testing-mode banner only |
| Outbound dialing (M17 #1) | NOT IMPLEMENTED in HEAD; in progress (Stream A, M18) | See A5 |
| Fallback peer list (spec §20.16) | NOT IMPLEMENTED | See A4 |
| Ban persistence across restarts | NOT IMPLEMENTED | ChainStore has PutBan/GetBans; no load path wired into the node (unchanged since M13) |

### B.2 M13 "NOT VERIFIED" items — status now
| Item | Then | Now |
|---|---|---|
| RBF descendant handling | NOT VERIFIED (unreachable) | NOT VERIFIED (unchanged; defense-in-depth only) |
| 100-replacement cap test | NOT VERIFIED | NOT VERIFIED (unchanged; see A6) |
| Disk-full behaviour | NOT VERIFIED | FIXED (M17: RLIMIT_FSIZE tests, rollback-not-corruption, stderr warning) |
| Crash recovery (kill mid-write) | NOT VERIFIED (abandon-store only) | FIXED (M17: SIGKILL mid-burst + mid-shutdown tests) |
| ECDSA low-S / SIGHASH_ALL | verified in M13 | MATCH (unchanged) |

## C. Warning report (re-run)
- Compiler `-Wall -Wextra`: 0 warnings, 0 errors — pristine worktree
  build (`cmake --build build-pristine`, ASan/UBSan flags), full
  `oeeoo_tests` build output shows no warnings.
- cppcheck `--enable=warning,performance,portability
  --suppress=missingIncludeSystem -Isrc src tests` on the pristine
  tree + Stream C's files: **0 findings across 88 files** (run on
  the post-restart VM, 2026-11-06 ~22:33 UTC, log
  `logs/m18c-streamc-cppcheck-full.log`; the single-file check on
  `tests/test_milestone18c.cpp` also returns 0 findings).
- ASan leak-detection note (observed, not a code finding): a ctest
  run WITHOUT ASAN_OPTIONS failed at exit with "164952 byte(s)
  leaked in 311 allocation(s)" — ALL of them indirect leaks whose
  roots are libcrypto's one-time legacy-provider globals
  (EnsureLegacyProvider / EVP_Digest) and SQLite one-time
  allocations (Database::Open → sqlite3_open); ZERO direct leaks
  from project code. The volume grew since M13 (6,513 bytes / 117
  allocations) because the suite now opens many more stores and
  does much more crypto — the benign-leak total scales with suite
  size. The repo's documented practice (M13/M16/M17) is
  ASAN_OPTIONS=detect_leaks=0; with that set the suite passes.
  The same run also showed ASan's leak-exit discards the tail of
  block-buffered stdout (sections after m17b never flushed) — a
  harness artifact, not a test failure: the direct binary run with
  detect_leaks=0 printed all sections + ALL CHECKS PASSED.
  No new warnings introduced by Stream C's files.

## D. Security report (updated gaps)
Gaps closed since M13 (for SECURITY_REVIEW.md): node wallet (M14),
inbound-IP bans (M15/M16), AddrMan anti-fill + DNS timeout + orphan
pool + compact bounds + eviction (M15), RPC load control + real
template fees/sigops + wallet history + passphrase rotation (M16),
mempool persistence + 100-deep reorg + crash recovery + disk-full
loudness + measured limits (M17), differential-test evidence (M18
Stream C: subsidy/difficulty/vesting/Merkle/Base58Check cross-checked
against an independent Python reference; two-node agreement;
node-vs-standalone acceptance equivalence).
Gaps that remain (unchanged unless noted):
- No independent audit (external, still required before mainnet).
- No approved mainnet genesis.
- No distinct testnet identity.
- Stratum mining.authorize accepts any credentials (localhost-only
  mitigation, documented).
- RBF descendants unreachable; 100-replacement cap untested (A6).
- Fuzzing covers parsers, not the live peer/RPC/Stratum state
  machines (other M18 streams).
- Reorg across pre-v2 blocks fails closed.
- AS-map NOT implemented — /16 bucketing accepted for v0.1.0.
- Orphan promotion via the "tx"-handler call site cannot fire in
  practice — block arrival is the real trigger.
- getblockstats cannot price a coin whose origin is not in a stored
  block; pre-journal sends are not recoverable.
- Ban keys are not persisted across restarts.
- submitblock still returns success while chain persistence fails —
  loud in the log (M17 warning) but silent to RPC callers.
- The test suite is not safe to run in parallel (shared fixed /tmp
  paths); run it serially. Stream C's new test uses no fixed paths
  or ports. A parallel run during this stream produced 665 CHECK
  failures in shared-path sections — all artifacts, serial re-run
  passes 100% (see §E).
- NEW (A1): F17 "10 outbound" peer limit not implemented in HEAD.
- NEW (A5): no outbound dialing in HEAD (fix in flight in M18).
- NEW (A2): concurrent-edit builds from a shared working tree can
  produce a silently wrong binary; only UBSan caught it. Builds for
  evidence must come from a pinned commit/worktree.
- NEW (A7): soak B catch-up misses under extreme load (7 misses by
  block 186) + stale peer entries accumulating on A (1→5).

## E. Differential-test evidence (Stream C, for the M18 report)
- `tests/diff_reference.py` (pure stdlib) + `tests/test_milestone18c.cpp`
  (6 new sections): subsidy (14 heights + total), difficulty retarget
  (14 vectors), vesting (11 anniversaries + 9 vested probes, Feb 29
  2028 launch), Merkle (0/1/2/3/5/7 leaves), Base58Check (3 payloads
  + decode + tamper rejection), two-node agreement (7 blocks incl. a
  spend; tip/height/UTXO/mempool identical), block-acceptance
  equivalence (4 valid blocks + 4 invalid variants — unmined, bad
  Merkle, 1-sat overclaim, unknown parent — rejected identically by
  both paths with state untouched).
- Results: pristine serial run, ASan/UBSan, ASAN_OPTIONS=detect_leaks=0:
  `./build-pristine/oeeoo_tests` exit 0 — **188 sections, ALL CHECKS
  PASSED, 0 failures** (182 M17 sections + 6 new m18c sections).
  All 6 m18c sections pass. Full log:
  `logs/m18c-streamc-pristine-suite.log` (also
  `logs/m18c-streamc-parallel-contaminated-suite.log` for the
  parallel attempt, 665 artifact failures).
- ctest (final, post-restart VM, ASAN_OPTIONS=detect_leaks=0):
  `ctest --test-dir build-pristine --output-on-failure` —
  **100% tests passed, 0 tests failed out of 1, 451.02 s**
  (log `logs/m18c-streamc-ctest-final.log`). The earlier ctest
  re-run (without ASAN_OPTIONS) failed only on the benign-leak
  exit check (see §C), and the second attempt was killed by the
  VM restart at ~22:15 UTC before finishing — both are superseded
  by this passing run. Section-level evidence remains the direct
  binary run above (188 sections, ALL CHECKS PASSED).
- Three test bugs found by the tests themselves and fixed before the
  final run (disclosed): (1) the Python reference's first-draft
  subsidy used a uniform (h-1)//210000 era division, wrongly
  extending the short 5th era past 940,000 — the C++ code was already
  correct; fixed in the reference. (2) The third Base58Check vector
  was 19 bytes, not 20, producing a '2…'-prefix address that both
  implementations agreed on — the '7'-prefix assertion (not the
  code) was wrong for a 19-byte payload; fixed to a 20-byte payload
  (now `78QK…`, 34 chars). (3) The first-draft standalone harness
  called ChainState::AddBlock BEFORE ConnectBlockTransactions, so a
  tx-invalid block still advanced the standalone tip — the node
  validates everything before AddBlock; the harness now mirrors
  AcceptBlockDirect step for step (CheckBasic → ComputeBlockFees →
  CheckBlockReward → BuildBlockUndo → Connect on a copy → AddBlock).

## F. Soak re-run evidence (Stream C, for the M18 report)
- Binary: pristine HEAD b9977af `oeeoo-node` (ASan/UBSan,
  ASAN_OPTIONS=detect_leaks=0), worktree ~/workspace/oeeoo-pristine.
  NOT the contaminated build-m18c binary (see A2).
- Genesis: fresh soak18/genesis18.json, hash
  7819075d2e1f77757476a7bee410c3d1e92b38b0d292d8f70a6bb443b0af8c01
  (throwaway test keys; privkeys never saved — vault unspendable).
- Setup: nodes A (P2P 64875/RPC 64877) + B (64876/64878), M17 Python
  P2P bridge (sendcmpct dropped by design), driver 1 block/15 s,
  1 tx/3 min, RSS/CPU every 5 min. Deadline 23:06:10 UTC 2026-11-06.
  Dialing was NOT integrated when the soak started (Stream A
  mid-landing, header + partial implementation, no --addnode/dial
  loop wired), so the soak runs on the M17 binary + bridge, as the
  task directs for that case.
- Leg 1: 21:06:16–21:31:18 UTC (~25 min), nodes to height 93, 0 txs
  (coinbases immature until 101), 0 failures. Ended by the
  unexplained SIGTERM-like kill (see A8), NOT a VM restart.
  First two RSS samples for A are bogus (708 kB — wrong pid 11841
  in the pid file; real node was 11843) and must be excluded.
- Leg 2: 21:41:08–22:14:45 UTC (~34 min, driver restarted with the
  same deadline; nodes restarted on the same datadirs, height 93),
  height 93 → 187, 89 blocks mined by the driver, 6 wallet txs
  (4 A→B, 2 B→A, all confirmed), 1 block failure + 1 tx failure
  (both RPC timeouts under extreme load, see A7), B missed the 30 s
  catch-up window 7 times (blocks 154, 155, 156, 164, 182, 185,
  186) but heights agreed at every 5-min sample. Ended by a VM
  restart ~22:15 UTC (uptime reset to 0).
- Leg 3: 22:18:24–23:06:10 UTC (2,866.6 s, 47.8 min; driver
  stopped itself at the deadline, clean shutdown of both nodes +
  bridge), height 187 → 374 (187 blocks mined), 13 wallet txs
  sent (7 A→B, 6 B→A per the bridge's leg-3 counters), 0 block
  failures, 0 tx failures, max B catch-up lag 0.67 s, **0 missed
  30 s windows**. Final sample RSS 582,632 / 645,948 kB.
  Sanitizer grep over both node logs (all three legs): **0 ASan
  errors, 0 UBSan runtime errors**. Bridge leg-3 counters at
  close: 186 blocks, 193 inv, 186 headers forwarded A→B.
  NOTE: the driver's SOAK DONE summary contains stale template
  strings copied from the M17 driver ("driver restarted once
  ~12:09 UTC", "nodes ran continuously since 11:21 UTC",
  "leg 1 (160 blocks, 1 tx)") — cosmetic only; the NUMBERS in
  the summary are this leg's own live counts. The driver should
  get a one-line cleanup (not Stream C scope).
- TOTALS (all legs): ~107 min node runtime (leg 1 ~25 min + leg 2
  ~34 min + leg 3 ~48 min, with gaps); chain height 0 → 374
  (374 blocks: 93 + 94 + 187); 19 wallet txs sent (0 + 6 + 13),
  all confirmed; 1 block failure + 1 tx failure (both leg-2 RPC
  timeouts under extreme load, see A7); B missed the 30 s
  catch-up window 9 times, all in leg 2 (blocks 154, 155, 156,
  164, 179, 181, 182, 185, 186) — leg 1 and leg 3 had zero
  misses. Heights agreed at every 5-min sample in every leg.
- RSS shape: steady linear climb under ASan in every leg (leg 3:
  A 497 → 583 MB, B 499 → 646 MB; leg 2: A 492 → 612, B 490 →
  556). This matches M17's accepted profile (A +159 MB / B +128
  MB over 100 min, decelerating) — same binary class, same
  pattern, no new unbounded-growth signal. The same caveat M17
  carried still applies: under ASan the allocator's RSS is not a
  leak proof; a non-ASan long run would be needed to distinguish
  a real leak from ASan allocator behaviour. No ASan/UBSan error
  was raised in any leg.
- Samples: `logs/soak18-samples.log` (all legs appended).
  Per-leg evidence: `soak18/` (driver.log, bridge.log, node logs).
  Driver's own leg-3 summary is in driver.log at 23:06:10 UTC.
