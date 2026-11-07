# Oeeoo Coin — Milestone 18: Fuzzing & Security Hardening

Status: DONE 2026-11-06. All 182 prior sections still pass;
23 new sections added (205 total), ALL CHECKS PASSED, ctest 100%.
Build warning-free; cppcheck 0 findings; ASan/UBSan clean (see §F).
Built in three parallel streams (outbound dialing; coverage-guided
+ live fuzzing; differential tests + soak + audit re-run) plus
coordinator integration (fuzz LSan fix, docs, verification).

The M13 verdict stands: **NOT READY** (genesis unapproved, no
independent audit). M18 closes the M17 #1 launch-blocking gap
(the node can now dial out and join the network), puts hostile
input through coverage-guided and live fuzzing (5 real bugs
found and fixed, all non-consensus), cross-checks the node
against an independent Python reference, re-runs the soak under
sanitizers, and re-runs the full M13 audit. It does not change
launch readiness.

## A. Summary

### 1. Outbound P2P dialing — the M17 #1 finding, FIXED in M18 (Stream A)

M17 proved live that two stock nodes never peer
(`getpeerinfo == []` on both): no dial loop, no `--addnode`, no
`addnode` RPC, `Node::ConnectTo` called only from tests. That is
launch-blocking — a node that cannot dial out can never join
the network; DNS seeds fill AddrMan but nothing dials them.
The parent agent decided it into M18 (not deferred), and it
lands here, first and complete.

- **Dial loop** (`src/network/node.h/.cpp`): `Node::TryDialPeers()`,
  called from `Node::Poll` (one pass per dial interval, throttled
  by `lastDialAttempt_`). Each pass dials AddrMan addresses until
  the outbound cap or the total peer cap is reached. Rules:
  never dial a banned address; never dial your own listen
  address; never dial a peer already connected; a failed dial
  counts against the address (AddrMan failure counting — 3
  failures and the address is dropped as dead). The loop is
  cheap when there is nothing to do (one pass per interval,
  no spinning on dead addresses).
- **Outbound cap — finding A1 CLOSED:** `maxOutboundPeers_ = 10`
  (default), `SetMaxOutboundPeers()` / `MaxOutboundPeers()` /
  `OutboundPeerCount()`, enforced in the dial loop
  (`node.cpp:282-290`). DECISION_LOG F17 ("125 total / 10
  outbound") is now fully implemented: the 125 half was MATCH
  since M13; the 10 half was NOT IMPLEMENTED in HEAD and is
  implemented + tested here. A test asserts the cap is respected.
- **`--addnode` CLI flag + config key** (`src/app/config.*`,
  `src/app/node_main.cpp`): repeatable `--addnode <ip:port>`
  and `addnode` config key. A manually added peer goes into
  the address book, so the dial loop dials it.
- **`addnode` RPC** (`src/rpc/rpcserver.cpp`): Bitcoin-shaped
  `addnode <node> <command>` — add / remove / onetry. `onetry`
  opens an immediate explicit connection (not counted against
  the dial loop's AddrMan pass). RPC surface is now **39
  callable methods + `help`** (40 names in the help list; M17
  was 38 + help — `addnode` is the one new name, verified by
  counting the help list).
- **DNS-seed results are dialed:** `QueryDnsSeeds` fills
  AddrMan (called once at startup); the dial loop dials from
  it. A fresh node with an empty AddrMan that queries seeds
  ends up with peers — covered by a test.
- **Tests (11 new sections, `tests/test_milestone18a.cpp`):**
  dial loop respects the outbound cap; never dials a banned
  address; skips already-connected peers and itself; drops a
  dead address after 3 failures; two nodes peer via the
  outbound dial loop; DNS-seed results are dialed; a dropped
  peer can be re-dialed; config `--addnode` flag and `addnode`
  key; `AddManualPeer` / `RemoveManualPeer` (node API); E2E —
  two stock nodes peer via `--addnode`, `addnode` RPC
  add/remove/onetry; E2E — malformed `--addnode` refuses
  startup.
- **`docs/OPERATOR.md` updated:** the M17 peering-limitation
  note is replaced by a dialing section (dial loop, `--addnode`,
  `addnode` RPC, caps, ban behaviour).

### 2. Coverage-guided fuzzing — libFuzzer (Stream B)

- **Toolchain check (recorded first, as the roadmap demands):**
  at M18 start the VM had NO Clang (`which clang clang++
  afl-fuzz` empty), no cppcheck, no libsqlite3-dev. `apt-get
  update` was unusable (another process held the apt lock for
  100+ minutes), so Clang 18.1.3 + compiler-rt (libFuzzer) +
  cppcheck 2.13.0 + libsqlite3-dev 3.45.1 were installed from
  the Ubuntu 24.04 archive by direct `.deb` download
  (`azure.archive.ubuntu.com`) + `dpkg -i`. A libFuzzer smoke
  test passed. **Choice: libFuzzer (Clang 18).** AFL++ was not
  needed; no fallback was required. Mid-M18 the VM was
  REPLACED: all system packages were gone again, `/tmp` was
  wiped, and every background process was killed (a
  `fuzz_script` seed-11 run at ~17k/20k and a build). The
  boot-time os-intent replay restored clang-18 / cppcheck /
  libclang-rt-18-dev / libsqlite3-dev from recorded intent, so
  the toolchain is now persistent across restarts. The manual
  `.deb` cache + reinstall script live at
  `~/workspace/m18b-debs/` and
  `~/workspace/m18b-toolchain-reinstall.sh` (outside the repo)
  as a fallback. `llvm-symbolizer` is NOT installed and is not
  in the apt index (`llvm-symbolizer-18` unlocatable), so ASan
  stacks show offsets only (resolved with `addr2line`) and
  llvm-cov percentages were not measurable — coverage is
  reported as libFuzzer feature counts below.
- **What was built:** `src/fuzz/` — 7 libFuzzer harnesses
  (Clang, ASan/UBSan): `fuzz_tx` (deserialize + serialize
  round-trip trap + ValidateTransaction), `fuzz_block` (block
  + bare 80-byte header, CheckBasic, Merkle root),
  `fuzz_script` (scriptSig vs fixed P2PKH pubkey, pubkey vs
  fixed scriptSig, both sides), `fuzz_json` (Parse + Dump
  round-trip trap + IntFromString), `fuzz_p2p` (whole framed
  message via ParseMessage + per-command payload parsers;
  stops at parsing — deeper net-processing is covered by the
  live harness, as Core does), `fuzz_compact`
  (ParseCompactBlock + ReconstructBlock vs empty mempool,
  ParseGetBlockTxn, ParseBlockTxn), `fuzz_wallet`
  (LoadWalletFile on a pid-unique scratch file; traps on
  empty/>1024-byte decrypted mnemonic). Shared helpers in
  `src/fuzz/fuzz_common.h` (fixed test keypair, ValidTx /
  ValidBlock — valid except PoW, documented caveat).
  `tests/fuzz/corpus/` — 34 seed files, generated by
  `tests/fuzz/gen_corpus.cpp` (`oeeoo_gen_corpus`, run with
  `ASAN_OPTIONS=detect_leaks=0` — LeakSanitizer flags OpenSSL
  one-time allocations, noise). `tests/fuzz/README.md` —
  both fuzzing layers, build/run commands, findings R1–R5.
  `CMakeLists.txt` — core sources factored into
  `OEEOO_CORE_SOURCES` (shared by `oeeoo_core` and
  `oeeoo_core_fuzz`); `option(OEEOO_BUILD_FUZZERS ... OFF)`
  builds the 7 fuzzers + `oeeoo_gen_corpus` + 7 `ctest`
  entries (`-timeout=10 -rss_limit_mb=1024`, fixed seeds).
  **Corpus-copy rule (learned the hard way):** libFuzzer
  writes new inputs back into the corpus directory it runs
  against — during development this polluted the checked-in
  seeds with 600+ stray files. CMake now copies the seeds
  into `${CMAKE_CURRENT_BINARY_DIR}/corpus` at configure time
  and the tests run against the copy; never run a fuzzer
  directly against `tests/fuzz/corpus/`.
- **Fuzz results (all ASan/UBSan, fixed seeds):** `ctest -R
  '^fuzz_'`: **7/7 passed** (tx 1.3 s, block 2.7 s, script
  10.6 s, json 6.9 s, p2p 1.8 s, compact 2.4 s, wallet 15.3 s;
  ~41 s total on the coordinator's final run). CI counts:
  tx/block/json/p2p/compact 20,000 runs; script 2,000 runs
  (~73 exec/s, each input pays up to 3 ECDSA verifications);
  wallet 20 runs (~35 s — PBKDF2 200k iters is 1.6–2.1 s per
  call raw, 4.4–6.4 s per LoadWalletFile under ASan on this
  VM; VM CPU throttling, not a code bug). Longer runs, all
  clean: tx/block/json/p2p/compact 20,000 each; **script
  20,000 clean on seed 3 (273 s) and on seed 23 (185 s)**;
  wallet 100 runs clean. No crashes in any completed run.
  LibFuzzer feature counts (2000 runs, seed 1; wallet 20
  runs): tx cov 270 / ft 586; block 377 / 885; script 399 /
  867 (seed-23 long run reached ft 1440); json 400 / 1269;
  p2p 490 / 904; compact 321 / 897; wallet 50 / 51. (cov =
  covered PCs, ft = edge features — not percentages; see
  toolchain note above.)
- **One unreproduced event (Stream B) — now REPRODUCED and
  EXPLAINED by the coordinator (see §C.1 item 6):** the first
  script run (seed 3) wrote a 0-byte crash artifact whose
  SHA1 is that of the EMPTY input. It is not a code defect:
  the 2,000 runs complete cleanly and LeakSanitizer fires at
  process exit on the known benign libcrypto one-time leak;
  libFuzzer then writes a "crash" artifact for the last unit.
  The coordinator's fix (§C.1 item 6) makes the fuzz ctest
  entries run with `ASAN_OPTIONS=detect_leaks=0`, matching
  the main suite's documented practice; the final 7/7 run
  confirms it.

### 3. Live fuzzing harnesses (Stream B, `tests/test_milestone18b.cpp`, 6 sections)

- **P2P mutated messages:** 7 commands × 200 mutations into
  a real node (base block Merkle-zeroed so it can never
  become valid); ≥1000/1400 delivered, tip stays at genesis,
  mempool/orphan bounds hold, fresh valid tx accepted
  afterwards.
- **P2P raw byte stream:** 200 mutated versions (fresh peer
  each), 1-byte-chunk tx, two messages in one Send, bad magic
  then valid tx, oversize length refused, 300 random garbage
  sends.
- **RPC mutated JSON-RPC:** 300 byte-mutated requests (≥250
  answered, 200/400 only), depth 100/1000 rejected, depth-60
  params parse, huge int/double → 200, wrong types → error,
  unknown method −32601, batch of 500 → 500 responses, empty
  body + 1 MB `[` body → 200, GET → 405, invalid UTF-8 → 200.
  Includes R1 regressions (`Content-Length: abc` → 400;
  out-of-range → no crash; `-5` → 400).
- **Stratum mutated mining.submit:** 300 mutated lines over
  one connection against a real job id + ntime from
  `mining.notify`, 10 KB extranonce2, non-hex ntime/nonce,
  wrong types, unknown method + depth-100 nesting, truncated
  JSON; 70 KB line skipped, server keeps serving. Includes
  R3 regressions (5 MB no-newline flood → connection dropped,
  new clients fine).
- **R5 regression + R2/R4/JSON-depth regressions** (R2:
  crafted `mnemonicLen = 0xFFFFFFF0` wallet → clean `wallet
  payload is corrupt`; R4: `IntFromString("abc")`/`("")`
  null, `("123")` == 123, `("1.5")` == 1 — `stoll` takes the
  leading-int prefix; depth 63 parses, 100 fails).

### 4. Differential tests (Stream C)

Defined, since there is no second Oeeoo implementation to
diff against — all three applicable forms, none left vague:

- **(a) Node vs Python reference:** `tests/diff_reference.py`
  (independent pure-stdlib Python reference written from the
  spec / Bitcoin conventions, NOT translated from C++) +
  `tests/test_milestone18c.cpp`. Subsidy (14 heights + total
  20M), difficulty retarget (14 vectors), vesting (11
  anniversaries + 9 vested probes, Feb 29 2028 launch),
  Merkle (0/1/2/3/5/7 leaves), Base58Check (3 payloads +
  decode + tamper rejection; 2000 random 20-byte payloads
  all encode to `7…` under version `0x0f`).
- **(b) Two-node agreement on identical inputs:** 7 blocks
  incl. a spend — tip/height/UTXO/mempool identical.
- **(c) Block-acceptance equivalence, node
  (`BroadcastBlock`) vs standalone harness
  (`ChainState::AddBlock` + `ConnectBlockTransactions`):** 4
  valid blocks + 4 invalid variants (unmined, bad Merkle,
  1-sat overclaim, unknown parent) — rejected identically,
  state untouched.
- **Result:** pristine serial run, ASan/UBSan,
  `ASAN_OPTIONS=detect_leaks=0`: 188 sections (182 M17 + 6
  new m18c), ALL CHECKS PASSED. The 18c:570 overclaim case
  was flagged mid-M18 as possibly consensus-adjacent — it is
  NOT: both paths reject the overclaim identically; the
  earlier failures were test-helper bugs (the standalone
  harness called `AddBlock` before
  `ConnectBlockTransactions`, so a tx-invalid block still
  advanced the standalone tip — now mirrors
  `AcceptBlockDirect` step for step). No consensus code was
  changed by any stream; no consensus-touching issue was
  found anywhere in M18.

### 5. ASan/UBSan soak re-run (Stream C, `soak18/`)

- **Binary:** pristine HEAD `b9977af` `oeeoo-node`
  (ASan/UBSan, `ASAN_OPTIONS=detect_leaks=0`), worktree
  `~/workspace/oeeoo-pristine`. NOT `build-m18c` (finding
  A2: that binary was built from a tree Stream A was editing
  mid-compile and aborted under UBSan at startup — a
  franken-build, not a code bug). **Dialing was NOT
  integrated when the soak started** (Stream A mid-landing),
  so the soak ran on the M17 binary + the M17 Python P2P
  bridge, as the task directs for that case. The dialing
  itself is covered by Stream A's E2E tests (§A.1); a soak
  on the dialing binary is a possible M19 follow-up, not
  claimed here.
- **Setup:** nodes A (P2P 64875/RPC 64877) + B (64876/64878),
  M17 Python P2P bridge (sendcmpct dropped by design), driver
  1 block/15 s, 1 tx/3 min, RSS/CPU every 5 min. Deadline
  23:06:10 UTC 2026-11-06. Genesis `7819075d…af8c01`
  (fresh `soak18/genesis18.json`, throwaway test keys,
  privkeys never saved).
- **Leg 1:** 21:06:16–21:31:18 UTC (~25 min), height 0→93,
  0 txs (coinbases immature until 101), 0 failures. Ended
  by an unexplained SIGTERM-like kill (finding A8) — NOT a
  VM restart.
- **Leg 2:** 21:41:08–22:14:45 UTC (~34 min), height 93→187,
  6 wallet txs (all confirmed), 1 block failure + 1 tx
  failure (both RPC timeouts under extreme load, finding
  A7), B missed the 30 s catch-up window 9 times. Ended by
  a VM restart ~22:15 UTC.
- **Leg 3:** 22:18:24–23:06:10 UTC (47.8 min), height
  187→374, 13 wallet txs (all confirmed), 0 failures, max B
  catch-up lag 0.67 s, 0 missed windows. Driver stopped
  itself at the deadline, clean shutdown.
- **Totals: ~107 min node runtime, height 0→374 (374
  blocks), 19 wallet txs sent, all confirmed, 1 block + 1 tx
  failure (both leg-2 RPC timeouts under extreme CPU load),
  heights agreed at every 5-min sample. Sanitizer grep over
  both node logs (all legs): 0 ASan errors, 0 UBSan runtime
  errors.** Leg-3 restart after the unclean VM restart
  recovered both nodes at height 187 with identical tips
  (`0a60c1ec…`) — WAL recovery held, including the two blocks
  B had "missed". RSS climbs linearly under ASan in every leg
  (leg 3: A 497→583 MB, B 499→646 MB) — same shape/magnitude
  as M17's accepted profile, so no new unbounded-growth
  signal; the M17 caveat still applies (ASan RSS is not a
  leak proof; a non-ASan long run would be needed to
  distinguish a real leak from allocator behaviour).
- Samples: `logs/soak18-samples.log`; per-leg evidence in
  `soak18/` (driver.log, bridge.log, node logs); driver leg-3
  summary also at `logs/soak18-summary.json`.

### 6. M13 audit re-run (Stream C, full draft in `soak18/M18C-AUDIT-DRAFT.md`)

Same 18-section brief as M13, re-run against HEAD `b9977af`
(M17) plus the M14–M17 changes, with file:line evidence
verified in the pristine HEAD worktree. **Verdict: NOT READY
— unchanged.** Genesis unapproved, no independent security
audit, no mainnet. The re-run closes nothing that changes
launch readiness; it finds one previously unflagged
decision-vs-code gap (A1 — now FIXED in M18, §A.1) and
confirms the M17 no-dial finding was still true in HEAD (A5
— now FIXED in M18, §A.1). No consensus-touching issue was
found. New findings A1–A8 are listed in §C.2; the full
decision cross-check table is in the audit draft (§B there)
and summarized in §B below.

## B. Decision cross-check

- No finalized spec decision was changed (supply 21M, rewards,
  P2PKH-only launch, vesting schedule spec §6, port 64775, magic
  `e2 d8 04 d6`, direct mainnet / NO public testnet per spec F24).
- Decisions taken in M18 (all documented in code or here):
  1. Outbound dialing → IN M18 (parent-agent decision on the
     user's standing delegation; launch-blocking). Dial loop +
     `--addnode` + `addnode` RPC (§A.1).
  2. Outbound cap → 10 (F17's second half, now implemented +
     tested; finding A1 CLOSED).
  3. Fuzzer engine → **libFuzzer (Clang 18)**. AFL++ not
     needed; no fallback required (§A.2).
  4. Fuzz ctest entries run with
     `ASAN_OPTIONS=detect_leaks=0` (coordinator fix, §C.1
     item 6) — same documented practice as the main suite;
     the benign libcrypto one-time leak is not a code defect.
  5. Soak acceptance (Stream C + coordinator): the M18 soak
     ran in 3 legs (~107 min total, 374 blocks, 19 txs, 0
     sanitizer errors) on the M17 binary + bridge because
     dialing was not integrated when it started; leg 3 (47.8
     min, 187 blocks, 0 failures, clean shutdown) is the
     reported leg. A soak on the dialing binary is a possible
     M19 follow-up, not claimed here.
  6. RSS creep under ASan — accepted again as ASan-quarantine
     behaviour (same shape/magnitude as M17; no new signal).
  7. `addnode` RPC shape → Bitcoin-shaped add/remove/onetry;
     `onetry` is an immediate explicit connection.
- Audit re-run cross-check (full table in
  `soak18/M18C-AUDIT-DRAFT.md` §B; HEAD b9977af + M18 deltas):
  every finalized decision re-verified MATCH except the items
  already carried as REQUIRES DECISION / NOT IMPLEMENTED
  (modern address formats, genesis parameters, DNS seed
  domains, SLIP-0044, release-signing custody, Stratum V2
  deferred, multi-wallet deferred, distinct testnet identity,
  fallback peer list, ban persistence). Two items changed
  status in M18: F17 "10 outbound" MISMATCH → FIXED (§A.1);
  outbound dialing NOT IMPLEMENTED → FIXED (§A.1).

## C. Warning report

### C.1 Bugs found and fixed (disclosed)

1. **M18B-R1 (HIGH) — unauthenticated remote crash via
   Content-Length (Stream B, `src/rpc/rpcserver.cpp`).**
   Unguarded `std::stoul` on the HTTP `Content-Length`
   header, before the auth check; the exception escaped the
   worker thread → `std::terminate` → the whole node aborts.
   Any unauthenticated remote client could crash the node
   with a single malformed header. Fixed: strict digits-only
   parse, saturating at 4 MB+1, `400 invalid Content-Length`.
   Regression test added (live RPC fuzz section).
2. **M18B-R2 — wallet loader heap-buffer-overflow (Stream B,
   `src/wallet/walletfile.cpp` `ParsePayload()`).** `16 +
   mnemonicLen` in uint32 wrapped (mnemonicLen 0xFFFFFFF0 →
   0), so `std::string::assign` read ~4 GB out of a 16-byte
   buffer (ASan heap-buffer-overflow). Reachable only with a
   valid GCM tag, but the loader must never trust the length
   field. Fixed: size_t comparison. Regression test added.
3. **M18B-R3 — Stratum session buffer unbounded (Stream B,
   `src/rpc/stratum.cpp` `ClientLoop()`).** The session buffer
   grew without bound with no newline (20 MB held for a 20 MB
   stream). Fixed: drop the connection when the unterminated
   tail exceeds **1 MB** — deliberately ABOVE the longest
   skipped line (65536): the first fix capped at 65536 and the
   70 KB-line test caught that it kills legitimate long lines
   mid-stream. Corrected cap + test.
4. **M18B-R4 (latent, no callers) — `src/rpc/json.cpp`
   `Json::IntFromString`.** Called `std::stod` unguarded
   inside its own catch block; garbage input threw out. Fixed
   defensively (nested guard, returns null). Zero behavior
   change for valid inputs.
5. **M18B-R5 — heap-use-after-free in `Node::OnBytes`
   (Stream B, `src/network/node.cpp`).** `Peer& peer` held
   across `Misbehave()` calls that can ban + erase the peer
   (100-point threshold), then `peer.recvBuffer.clear()` on
   the freed Peer (ASan heap-use-after-free). Trigger: two
   bad-magic messages from one handshook peer (50 + 50).
   Found by the garbage-stream live fuzzing. Fixed: the same
   `peers_.find(peerId) == peers_.end()` guard the `OnMessage`
   path already uses, at all three Misbehave sites in
   `OnBytes` (bad magic, oversize length, bad checksum).
   Audited `OnMessage`: it also holds `Peer&` across
   Misbehave calls, but every site returns immediately after
   with no further `peer.` use — no other instance of the
   pattern. Regression test added. Note: the fix touches
   `node.cpp`, which the dialing stream also modified — the
   R5 change is ~15 lines, marked `M18B-R5`.
6. **Coordinator fix — fuzz_script ctest flake (NOT a code
   bug in the node).** The coordinator's re-run of `ctest -R
   '^fuzz_'` showed fuzz_script FAILED (6/7) while a solo
   run passed. Root cause, established from the log: the
   2,000 runs complete cleanly (`#2000 DONE`), then
   LeakSanitizer fires at process exit on the known benign
   libcrypto one-time leak (6,513 bytes in 117 allocations —
   the exact signature documented since M13, all indirect
   inside libcrypto's legacy-provider globals, zero direct
   leaks from project code, independently confirmed in the
   M18 audit re-run), libFuzzer writes a 0-byte "crash"
   artifact (SHA1 of the empty input — this is Stream B's
   "unreproduced event", now reproduced and explained), and
   ctest reports the test failed. The ctest fuzz entries did
   not set `ASAN_OPTIONS`, unlike every other ASan run in
   the repo. Fix: `set_tests_properties(... PROPERTIES
   ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")` for all 7 fuzz
   tests in `CMakeLists.txt`. Real ASan/UBSan errors are
   unaffected by `detect_leaks=0` and still fail the test.
   Re-run after the fix: **7/7 passed**, 0 LeakSanitizer
   mentions. The fix is coordinator-owned, disclosed here,
   and does not touch any fuzzed code.
7. **Test/reference bugs found by the differential tests
   themselves (Stream C, fixed before the final run).** (a)
   The Python reference's first-draft subsidy used a uniform
   `(h-1)//210000` era division, wrongly extending the short
   5th era past 940,000 — the C++ code was already correct.
   (b) The third Base58Check vector was 19 bytes, not 20
   (the assertion, not the code, was wrong; now
   `78QK5DydnDZYfn8SeraAhmVsiid6ifxHHj`, 34 chars; 2000
   random 20-byte payloads all encode to `7…` under version
   `0x0f`). (c) The first-draft standalone harness called
   `AddBlock` before `ConnectBlockTransactions`, so a
   tx-invalid block still advanced the standalone tip — now
   mirrors `AcceptBlockDirect` step for step. The 18c:570
   overclaim case was flagged mid-M18 as possibly
   consensus-adjacent; it is NOT — both paths reject the
   overclaim identically (see §A.4).
8. **Environment / process (not code bugs).**
   (a) **A2 — concurrent-edit "franken-build" aborted under
   UBSan (Stream C).** The first `build-m18c` binary aborted
   at startup (`node.cpp:1401:8: load of value 58/29, not a
   valid bool`, in `LoadMempool`) because Stream A saved
   `node.cpp` (+148 lines) while Stream C's build was
   compiling/linking it. Proof it is not a code bug: the
   same startup on a pristine HEAD worktree binary starts
   cleanly. Lesson: evidence builds must come from a pinned
   commit/worktree, never a tree another stream is editing.
   `build-m18c` was never used for evidence and is left
   untracked. (b) VM replaced mid-M18 (Stream B: toolchain
   + `/tmp` wiped, a fuzz run at ~17k/20k killed; Stream C:
   soak leg 2 ended by a VM restart ~22:15 UTC). Both
   recovered via the os-intent toolchain replay (Stream B)
   and legged soak restarts (Stream C). (c) A background
   `make -j4` in `build-m18b` failed with undefined
   references to `TestMilestone18A/C` — a concurrent-build
   race (two makes in one build dir), resolved by a clean
   serial rebuild; not a code problem. (d) Stream A's early
   `ctest` run in `build-m18a` failed with 7 CHECK(S)
   FAILED in Stream C's then-in-progress differential test
   (18c:577/579/580) — the standalone-harness ordering bug
   in item 7(c) above, fixed by Stream C since; the
   coordinator's final suite run passes all 205 sections.

### C.2 Audit re-run findings (Stream C — new in this re-run)

- **A1 (NEW, not flagged by M13): F17 "10 outbound" NOT
  IMPLEMENTED in HEAD.** DECISION_LOG F17 says "125 total /
  10 outbound"; `Node::ConnectTo` enforced only the total
  cap; no `kMaxOutbound|maxOutbound` anywhere in HEAD `src/`.
  **FIXED in M18** (§A.1) — coordinator-confirmed landed
  with a test (`maxOutboundPeers_ = 10`, enforced in the dial
  loop, "dial loop respects the outbound cap" section).
- **A2 (PROCESS):** concurrent-edit franken-build (item 8(a)
  above). Not a code bug.
- **A3:** Spec §20 item 14 (0.0001 OEEOO fee figure) NOT
  VERIFIED (minor, unchanged) — dynamic fee policy
  implemented, no explicit reconciliation note. No code
  change proposed; a one-line note in DECISION_LOG item 9
  would close it.
- **A4:** Spec §20 item 16 (fallback peer list) NOT
  IMPLEMENTED (documented in `src/network/addrman.h:9`).
  With no DNS seeds either, a fresh node with an empty
  AddrMan learns no peers unless dialed (compounded by A5
  in HEAD; the dialing fix in M18 addresses the dialing
  half, not the fallback list).
- **A5 (carried from M17, unchanged in HEAD):** node cannot
  open an outbound P2P connection. **FIXED in M18** (§A.1).
- **A6:** RBF 100-replacement cap implemented but untested
  (M13 "NOT VERIFIED" still open; `kMaxReplacementCount =
  100`, enforced, no test references it).
- **A7:** Soak load-correlated observations (9 B misses, 2
  RPC timeouts, stale peer entries) — disclosed, not a
  finding against the node (§A.5).
- **A8:** Leg-1 kill (~21:31 UTC) unidentified — all four
  soak processes died within ~1 min, both nodes graceful
  `Shutting down...` in the same second (SIGTERM-like, not a
  crash); not a VM restart, no OOM evidence. Killer not
  identified; reported as such. The runtime's handling of
  backgrounded exec sessions is the prime suspect, unproven.

### C.3 Warnings (compiler / static analysis)

- None. Clean build at `-Wall -Wextra`: 0 warnings, 0 errors.
  cppcheck `--enable=warning,performance,portability`: 0 findings
  across all 98 files (coordinator's final run). One
  pre-existing clang-only warning, not from this milestone:
  `tests/test_milestone14.cpp:400: unused variable 'nodeMutex'`
  (seen in Stream B's clang build only).

## D. Security report

Gaps closed by M18 (see also `docs/SECURITY_REVIEW.md`):

- **Outbound dialing (M17 #1, launch-blocking):** the node
  can now join the network — dial loop, `--addnode`,
  `addnode` RPC, outbound cap 10 enforced + tested.
- **Unauthenticated remote crash (R1):** a single malformed
  HTTP `Content-Length` aborted the whole node; fixed with a
  strict parse + 400 response.
- **Wallet loader heap overflow (R2):** a crafted wallet
  file's length field wrapped and read ~4 GB out of bounds;
  fixed with a size_t comparison.
- **Stratum memory DoS (R3):** unbounded session buffer on
  a newline-less stream; connection now dropped over 1 MB.
- **Heap-use-after-free in P2P handling (R5):** a banned
  peer's freed `Peer` was used after `Misbehave()`; guarded
  at all three sites.
- **Fuzzing evidence (new):** 7 coverage-guided harnesses
  (34-seed corpus, fixed seeds, CI-time) + live P2P/RPC/
  Stratum fuzzing + differential cross-checks against an
  independent Python reference.
- **Audit re-run:** full 18-section brief re-run with a
  verdict; one new decision-vs-code gap found (A1) and fixed
  in the same milestone.

Gaps that remain (unchanged from M17 unless noted):

- No independent audit (external, still required before mainnet).
- No approved mainnet genesis.
- No distinct testnet identity (test/mainnet share magic, port,
  version, userAgent — only the testing-mode banner guards).
- `Stratum mining.authorize` accepts any credentials
  (localhost-only mitigation, documented in MINING.md +
  OPERATOR.md).
- RBF descendants unreachable in practice; 100-replacement cap
  untested (A6).
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
  fails — the failure is now loud in the log (M17 warning) but
  silent to RPC callers.
- The test suite is not safe to run in parallel (shared fixed
  `/tmp` paths); run it serially. A parallel run during M18
  produced 665 CHECK failures in shared-path sections — all
  artifacts, serial re-run passes 100%.
- Spec §20 item 14 (0.0001 fee figure) still has no explicit
  reconciliation note (A3).
- Spec §20 item 16 (fallback peer list) NOT IMPLEMENTED (A4).
- Stale peer entries linger after a connection dies (A7
  observation: A's peer count grew 1→5 across the soak while
  B stayed at 1) — a peer-timeout is a hardening follow-up.
- The M18 soak ran on the M17 binary + bridge, not on the
  dialing binary (§A.5) — a soak on the dialing binary is a
  possible M19 follow-up.
- The fuzz corpus in the build dir accumulates libFuzzer-added
  inputs across runs (213 files in `build-m18b/corpus/script`
  vs the checked-in seeds); a re-configure resets it to the
  34 checked-in seeds.

## E. Files changed

Commit `7e8d862` on top of M17's `b9977af`:

`git diff --stat b9977af..7e8d862`: 86 files, +9535/−36.

| File | Change |
|---|---|
| `src/network/node.h` / `node.cpp` | Dial loop `TryDialPeers()` (+148 lines), `maxOutboundPeers_ = 10` + accessors, `AddManualPeer`/`RemoveManualPeer`, R5 UAF guard at 3 Misbehave sites (marked `M18B-R5`) |
| `src/network/transport.h` / `transport.cpp` | Dial-path support for the loop (Stream A) |
| `src/app/config.h` / `config.cpp` | `--addnode` flag + `addnode` config key (repeatable) + help text |
| `src/app/node_main.cpp` | `--addnode` wired; manual peers into AddrMan at startup |
| `src/rpc/rpcserver.cpp` | `addnode` RPC (add/remove/onetry); R1 Content-Length strict parse + 400 |
| `src/rpc/json.cpp` | R4 `IntFromString` nested guard |
| `src/rpc/stratum.cpp` | R3 1 MB unterminated-tail drop |
| `src/wallet/walletfile.cpp` | R2 size_t length comparison |
| `src/fuzz/*` (8 files) | NEW — 7 libFuzzer harnesses + `fuzz_common.h` |
| `tests/fuzz/` | NEW — `README.md`, `gen_corpus.cpp`, `corpus/` (34 seed files) |
| `tests/test_milestone18a.cpp` | NEW — 11 sections (outbound dialing) |
| `tests/test_milestone18b.cpp` | NEW — 6 sections (live fuzzing + regressions) |
| `tests/test_milestone18c.cpp` | NEW — 6 sections (differential tests) |
| `tests/diff_reference.py` | NEW — independent Python reference (subsidy/difficulty/vesting/Merkle/Base58Check) |
| `tests/test_main.cpp` | `TestMilestone18A/B/C()` wiring |
| `CMakeLists.txt` | `OEEOO_CORE_SOURCES` factoring; `OEEOO_BUILD_FUZZERS` option; 7 fuzzers + 7 ctest entries; coordinator fix: `detect_leaks=0` for fuzz tests |
| `docs/OPERATOR.md` | Dialing section replaces the M17 peering-limitation note |
| `soak18/` | Soak driver/bridge/genesis/logs + `M18C-AUDIT-DRAFT.md` |
| `logs/` | `milestone18b-test-log.txt`, `m18c-streamc-*.log` (pristine/ctest/cppcheck/parallel), `soak18-samples.log`, `soak18-summary.json/.txt` |
| `.gitignore` | `soak18/data-*/`, `soak18/*.pid`, `__pycache__/` |

## F. Test results

Toolchain: g++ 13.3, CMake 3.28, OpenSSL 3.0.13, SQLite 3.45.1,
cppcheck 2.13.0, Clang 18.1.3 (fuzzers only). Full log:
`logs/milestone18-test-log.txt`.

- Clean build (coordinator, from the final tree):
  `rm -rf build && cmake -S . -B build` exit 0;
  `cmake --build build -j4` exit 0, **0 warnings, 0 errors**.
- Full suite: `./build/oeeoo_tests` exit 0 — **205 sections, ALL
  CHECKS PASSED** (182 M17 + 11 m18a + 6 m18b + 6 m18c).
  `ctest --test-dir build` — **100% tests passed, 0 failed out
  of 1** (210.51 s, final pass).
- ASan/UBSan build (`-fsanitize=address,undefined
  -fno-sanitize-recover=all`, `detect_leaks=0`, no `stdbuf`):
  full suite exit 0, ALL CHECKS PASSED, **0 sanitizer errors**
  (205 sections).
- cppcheck `--enable=warning,performance,portability
  --suppress=missingIncludeSystem -Isrc src tests`: exit 0,
  **0 findings** (98/98 files checked).
- Fuzz ctest (fresh Clang 18 build `build-fuzz-m18`,
  ASan/UBSan, `detect_leaks=0`): `ctest -R '^fuzz_'` —
  **7/7 passed, 58.00 s total** (tx 1.30 s, block 3.41 s,
  script 14.59 s, json 7.38 s, p2p 1.60 s, compact 2.01 s,
  wallet 27.68 s), 0 LeakSanitizer mentions.
- Section arithmetic: 182 (M17) + 11 (dialing) + 6 (live
  fuzzing) + 6 (differential) = **205**.
- New sections (23):
  - `m18a: dial loop respects the outbound cap`
  - `m18a: dial loop never dials a banned address`
  - `m18a: dial loop skips already-connected peers and itself`
  - `m18a: dial loop drops a dead address after 3 failures`
  - `m18a: two nodes peer via the outbound dial loop`
  - `m18a: DNS-seed results are dialed`
  - `m18a: a dropped peer can be re-dialed`
  - `m18a: config — --addnode flag and addnode key`
  - `m18a: AddManualPeer / RemoveManualPeer (node API)`
  - `m18a: E2E — two stock nodes peer via --addnode, addnode RPC add/remove/onetry`
  - `m18a: E2E — malformed --addnode refuses startup`
  - `m18b: live p2p fuzz — mutated messages into a real node`
  - `m18b: live p2p fuzz — raw byte stream (framing attacks)`
  - `m18b: live rpc fuzz — mutated JSON-RPC over HTTP`
  - `m18b: live stratum fuzz — mutated mining.submit lines`
  - `m18b: regression — M18B-R5 OnBytes use-after-free`
  - `m18b: regressions — fuzzing finds R2/R4 + JSON depth`
  - `m18c: differential subsidy (node vs Python reference)`
  - `m18c: differential difficulty retarget (node vs Python reference)`
  - `m18c: differential vesting (node vs Python reference)`
  - `m18c: differential Merkle + Base58Check (node vs Python reference)`
  - `m18c: two-node agreement on identical inputs`
  - `m18c: block acceptance equivalence node vs standalone ChainState`
- Soak: 3 legs, ~107 min, 374 blocks, 19 txs, 0 sanitizer
  errors (§A.5).
- Reproducible build: `./scripts/reproducible-build.sh` from
  the final commit — **REPRODUCIBLE: all three binaries are
  byte-identical across two builds** (checksums in §F.1 below;
  recorded at delivery time in the test log).

### F.1 Reproducible-build checksums

Run 1 from the code commit `7e8d862`
(SOURCE_DATE_EPOCH=1794009307, the commit's timestamp;
log `logs/m18-repro-build-run1.log`):

- **REPRODUCIBLE: all three binaries are byte-identical
  across two builds.**
- **REPRODUCIBLE: archive is byte-identical across two
  builds.**
- oeeoo-node
  `5f6c08d7319850a0e3dd1d0c39d63c48ec9bd601b351617f5fbd69a268a315b3`
- oeeoo-genesis
  `29345d45c528b5a8a39ce47891b546a3ab9b53d03770b87848d57e74377f5c19`
- oeeoo-cli
  `6b7a71af340d018930e53498844f7cf0b67f48b3aabaaadad06395c505fac2bb`
- tarball `oeeoo-coin-0.1.0.tar.gz`
  `c86a127d6961ba7bbd442edf1a59d778cf0e657c453eac7299a744f94939ea74`

A confirmation run from the docs commit follows the same
pattern as M17 (see the test log); its tarball checksum is
recorded there.

## G. Outstanding decisions (carried + new)

New from M18 (coordinator's recommendation in brackets):

- **Soak on the dialing binary.** The M18 soak ran on the M17
  binary + bridge (§A.5). A soak on the integrated dialing
  binary would exercise real peer dialing end-to-end.
  [Possible M19 follow-up; the dialing itself is covered by
  Stream A's E2E tests.]
- **Stale peer entries.** A's peer count grew 1→5 across the
  soak while B stayed at 1 — dead connections' peer entries
  linger. [A peer-timeout is a hardening follow-up, not M18
  scope.]
- **Spec §20 item 14 (0.0001 fee figure).** Still no explicit
  reconciliation note (A3). [A one-line note in DECISION_LOG
  item 9 would close it.]

Carried (still open, unchanged):

- Test suite parallel safety: the suite shares fixed `/tmp`
  paths (and some fixed ports) and is not safe to run
  concurrently; a parallel run during M18 produced 665 CHECK
  failures in shared-path sections — all artifacts, serial
  re-run passes 100%. Options: pid-unique temp paths
  suite-wide, or serialize suite runs (the current practice).
- `submitblock` returns success while chain persistence fails
  (§A.4 of M17): the failure is now loud in the log, but RPC
  callers still see success. Changing that is a semantic
  decision for a later milestone.
- RBF 100-replacement cap untested (A6, carried from M13).
- Disk-full behaviour is verified for the SQLite store path
  only; other I/O failures (wallet save, journal save,
  mempool save) warn but are not crash-tested.
- No new consensus-touching decision arose in M18 — nothing
  here needs a user decision beyond what the coordinator
  already owns.

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
needed for M18 — needed only at genesis time (M19/launch): the
3 PUBLIC keys (66 hex each) for `oeeoo-genesis --pubkey1/2/3`;
private keys NEVER to be shared.

## H. Verdict

**NOT READY** — unchanged from M13/M14/M15/M16/M17. Genesis
unapproved, no independent security audit, no mainnet. M18
closes the fuzzing & security-hardening scope of the roadmap
and the M17 #1 launch-blocking gap (outbound dialing); the
next milestone (M19, launch readiness) is the last step of
the M14–M19 roadmap.
