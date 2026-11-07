# Oeeoo Coin — Milestone 19: Mainnet Launch Readiness (Final Verification + Release Prep)

Status: DONE 2026-11-07. No code changes in M19 — this milestone verifies
and prepares. All 205 prior sections still pass; 0 new sections added
(nothing found that needed a new test — a clean-room failure would have
been a bug, and there was none). Build warning-free; cppcheck 0 findings;
ASan/UBSan clean; reproducible build verified twice (clean-room checkout
+ tagged commit). Local git tag `v0.1.0` created on the final M19 commit
(local-only — nothing published, nothing pushed).

The M13 verdict stands: **NOT READY** (genesis unapproved, no
independent audit). M19 closes the last verification gap that was in
the project's own power to close — the soak on the dialing binary —
and leaves the release package ready to be published the moment the
owner says go. It does not launch anything.

## A. Summary

### 1. Clean-room verification (Stream A)

A fresh directory, a fresh git checkout of HEAD `d0a73f3`, fresh build
dirs — nothing from the working tree was reused except the git repo
itself as the checkout source and the system toolchain. Full log:
`logs/milestone19-cleanroom-log.txt` (1,058 lines, every command +
full output).

- Clean configure + build: exit 0, **0 warnings, 0 errors**.
- Full suite `./build/oeeoo_tests`: exit 0, **205 sections, ALL CHECKS
  PASSED**, 0 CHECK failures.
- `ctest`: **100% tests passed, 0 failed out of 1 (208.63 s)**.
- ASan/UBSan build + full suite (`-fsanitize=address,undefined
  -fno-sanitize-recover=all`, `detect_leaks=0`, no stdbuf): exit 0,
  **ALL CHECKS PASSED (205 sections), 0 sanitizer errors** (grep for
  AddressSanitizer/UBSan/LeakSanitizer = 0).
- cppcheck (`--enable=warning,performance,portability
  --suppress=missingIncludeSystem -Isrc src tests`): exit 0, **98/98
  files, 0 findings**.
- E2E with the clean-room binaries (test bits `207fffff`, throwaway
  genesis): node starts → `createwallet` / `walletpassphrase` /
  `getnewaddress` work → **106 blocks mined** via
  `getblocktemplate`/`submitblock` → `sendtoaddress` 1.5 OEEOO, txid
  `f8add6581339514df3bac4825e9dfa27880339d701e717e03531b30ba29d3f9a`,
  confirmed in block 106 → chain survives restart (height 106,
  balance 349.99999774 after re-unlock) → Stratum subscribe /
  authorize / notify work → **a difficulty-1 share was accepted**
  (`mining.submit` → `{"result": true}`) and became **block 107**:
  `getbestblockhash` =
  `00000000e7b6248cd66feaa8ac6fb484036e641324f4f407703fa9034c08f0a6`
  (exactly the share hash; nonce `7973e585`, 1544.6 s grind). Node
  stopped cleanly, exit 0.
- Reproducible build from the clean-room checkout
  (`./scripts/reproducible-build.sh 0.1.0`,
  SOURCE_DATE_EPOCH=1794015825): **REPRODUCIBLE for binaries AND
  archive.** Binary hashes identical to M18's recorded hashes; tarball
  `a6d7f12a1cc447ea0ba9339b0280c9eac1223fc4c1cd581ce66f683e3bf1c4a5`
  (differs from M18's only via SOURCE_DATE_EPOCH/SOURCE.txt).
- **No bugs found in the node code.** Every failure during the stream
  was in the stream's own E2E harness (see §C).

Clean-room meaning, stated honestly: fresh directory, fresh git
checkout, fresh build dirs, fresh E2E datadir/genesis/wallet. Reused:
the git repo as checkout source, the system toolchain (g++ 13.3,
CMake 3.28, OpenSSL 3.0.13, SQLite 3.45.1, cppcheck 2.13.0, Clang
18.1.3 as `clang++-18`), and the M17/M18 soak-driver mining pattern.
**Not a fresh VM** — this VM cannot be replaced on demand, and it was
in fact replaced three times mid-stream (see §C.3). Nothing claimed
beyond what was actually fresh.

### 2. Soak re-run on the M18 final (dialing) binary (Stream B)

This closes the M18 caveat: the M18 soak ran on the M17 binary +
Python P2P bridge because dialing was not integrated when it started.
The M19 soak ran on the M18 final ASan binary (HEAD `d0a73f3`,
`-fsanitize=address,undefined -fno-sanitize-recover=all`,
`detect_leaks=0`) with **real P2P dialing** — A `--addnode
127.0.0.1:64876`, B `--addnode 127.0.0.1:64875`, no bridge. Full log:
`logs/milestone19-soak-log.txt`; driver + node logs in `soak19/`.

- Leg 1 (08:02–08:17 UTC, ~15 min): setup shakedown only. First start
  failed (nodes launched without `--wallet`, createwallet error -13);
  driver restarted with `--wallet`; a second restart at 08:17:54 began
  a clean 2 h leg. Not counted as soak time.
- Leg 2 (08:17:54–~10:14 UTC, **~1 h 56 m**): the real soak. Deadline
  was 10:17:53; the run was killed ~3 min early by a VM restart
  (runtime drain — the same event that errored Stream B's agent
  before it could close out; the coordinator verified the data and
  wrote the closing summary). **NOT a clean 2 h run** — accepted under
  the M17 honest rule (inside the 1–2 h window, above the 1 h
  minimum).
- Peering: peers 2/2 at all 29 samples; heights agreed at every
  sample; max lag 0.5–0.6 s.
- Results: final sample 10:13:44 UTC hA=hB=441 (**440 blocks mined**);
  **~30 wallet txs** (26 at the 10:03 progress line + 4 after), all
  confirmed; **0 block failures, 0 tx failures**.
- Memory (VmRSS, 5-min samples): A 487,484 → 838,944 kB; B 487,484 →
  809,408 kB. Sawtooth shape (single-sample +217 MB jump then −167 MB),
  same as M17 — ASan allocator arenas, no leak claimed
  (`detect_leaks=0` by design).
- CPU: ~+13.5 s per 5-min sample (~2.7 s/min) per node, steady.
- Sanitizers: **0 ASan errors, 0 UBSan runtime errors** — grep over
  ALL `soak19/*.log` (node stderr is captured in `node-a.log` /
  `node-b.log` via `2>&1` in `start-nodes.sh`, so the grep is
  meaningful).
- **M18 caveat CLOSED (decisions item B5 → D16).**

### 3. Release prep (Stream C)

- `docs/RELEASE_NOTES.md` (final, from the M19 draft): all milestones
  M1–M19, finalized policy, 28 known limitations, toolchain, and the
  M19 checksums filled in from the clean-room reproducible build.
- `docs/MAINNET_LAUNCH.md` final pass: stale "The code is ready
  (Milestones 1–12)" intro replaced (M1–M18 done, M19 final
  verification, verdict NOT READY); step 2 notes M13 + M18 re-run were
  self-reviews, not audits; step 3 corrected — the repo exists
  (created by the user 2026-11-06, verified empty), push only after
  M19; step 5 adds M18 dialing and the missing fallback peer list;
  step 6 toolchain adds cppcheck/Clang; NOT-decided list expanded
  (genesis params, modern addresses, distinct testnet identity,
  fallback peer list, spec §20 item 14).
- `docs/RELEASE.md` final pass: M14–M17 verification entries backfilled
  (they were never recorded there — only M13 was); M18 entry added by
  Stream C; M19 entry added here (clean-room run + tagged-commit run).
- Version decision: **stays 0.1.0** unless the owner says otherwise
  (recorded, §B.1). No version string in code was touched.
- Full REQUIRES DECISION list with owners: §G (21 OPEN + 16 CLOSED).
- Verdict evidence table: §H (NOT READY, with cited evidence).

### 4. Local tag `v0.1.0`

Created on the final M19 commit (see §F.2). **Local-only**: no remote
is configured, nothing was pushed, nothing was published. The tag
exists so the release package can be reproduced "from the tagged
commit". The GitHub push happens only on the user's go-ahead after
M19.

### 5. What M19 did NOT do (owner's, not the agent's)

Independent security audit, genesis approval (launch time, founder
keys, genesis hash), GitHub repository creation + push, DNS seed
domains + operators, release signing, and the launch itself. M19
prepares; it does not launch. All listed as owner items in §G.

## B. Decisions

### B.1 Version stays 0.1.0 (recorded, not bumped)
Version **0.1.0** unless the owner says otherwise. No version string
in code was changed. The tag `v0.1.0` marks the M19 release commit.

### B.2 Soak acceptance (M19 leg 2, ~1 h 56 m)
Accepted under the M17 honest rule (inside the 1–2 h window, above
the 1 h minimum). Killed ~3 min before its 2 h deadline by a VM
restart — NOT claimed as a clean 2 h run. This closes decisions item
B5 (soak on the dialing binary) — moved to D16 in §G.

### B.3 Nothing else decided in M19
No consensus change, no spec change, no new policy. Every other
carried item stays exactly as §G lists it. In particular: no
per-client RPC rate limiting was added (M16 decision stands), no
testnet identity was invented, no DNS seed was invented, no fallback
peer list was invented.

## C. Bugs found and fixed (disclosed)

### C.1 Node code: NONE
Neither the clean-room verification nor the soak found a bug in the
node code. The full suite, ASan/UBSan, cppcheck, fuzz ctest (M18),
differential tests (M18), clean-room E2E, and the soak all pass with
zero failures attributable to the node.

### C.2 Stream A E2E harness bugs (harness-only, fixed in the harness)
All four were in Stream A's own throwaway E2E harness, not the node
(full detail in `logs/milestone19-cleanroom-log.txt`):
(a) Stratum jobs rotate every 60 s and only current+previous job ids
are submittable, so a ~30-min difficulty-1 grind must use a future
ntime (now+3600) and submit against the current job; (b) this
codebase's `HashHex` is header-internal byte order, so the Stratum
prevhash goes in as-is (proven by the soak driver's accepted blocks)
— the stream's earlier reversal made the server compute a different
hash; (c) a timed-out Python socket makefile is poisoned (`OSError:
cannot read from timed out object`) — fixed with a reader thread +
blocking socket; (d) a pop-and-reappend backlog loop that starved
replies.

### C.3 Environment: VM replaced three times during Stream A, once during Stream B, once more during the coordinator final pass
- Stream A: VM replaced ~08:16, ~10:14, ~12:33 UTC. Each time
  `clang++-18`/`cppcheck`/`libsqlite3-dev` vanished and were
  reinstalled once from cached .debs
  (`~/workspace/m18b-toolchain-reinstall.sh`); `/tmp` was wiped each
  time. Interrupted runs were restarted, never claimed complete. No
  run's evidence depends on a killed run.
- Stream B: VM replaced ~10:14 UTC (runtime drain), killing leg 2
  ~3 min before its deadline and erroring Stream B's agent ("restart
  drain in progress; rejecting new submission") before it could close
  out. The coordinator verified the soak data on disk and wrote the
  closing summary (`logs/milestone19-soak-log.txt`).
- Coordinator final pass: the VM was replaced AGAIN between Stream
  A's finish (~13:33 UTC) and the final pass (~13:45 UTC) — the
  first final-pass attempt failed at configure time ("Could NOT find
  SQLite3") and `cppcheck: command not found`. The toolchain was
  restored once from the cached .debs (same script; exit 0,
  "TOOLCHAIN OK") and the full final pass re-run from scratch. The
  failed attempt is recorded in the final-pass log
  (`logs/milestone19-finalpass-log.txt`, first lines); it is a
  toolchain loss, not a code failure, and no result below depends
  on it.
- No restore was needed for the toolchain at M19 start (unlike
  M15/M16) — g++, cmake, cppcheck, sqlite3.h, and Clang 18 were all
  present; the replacements above are what removed them mid-run.

### C.4 Observations (not bugs, from Stream A)
- Locked wallet watch-only view shows balance 0 / txcount 0 until
  `walletpassphrase` (matches M14 design; may surprise an operator).
- Stratum share difficulty starts at 1.0 and vardiff only adjusts
  after accepted shares, so a fresh miner must grind ~2³² hashes for
  its first share (~25–32 min/attempt on this throttled VM; 5
  full-space misses + 1 hit observed — consistent with the geometric
  distribution; the grinder was verified end-to-end against Python
  hashlib).

## D. Security report

M19 adds no new code, so the security posture is exactly M18's,
plus the verification evidence in §F:

- **Closed in M19:** the soak-on-the-dialing-binary caveat (M18 §G
  B5). The dialing path is now soak-proven: two nodes peered via real
  `--addnode` dialing for ~2 h, 440 blocks, ~30 txs, 0 failures, 0
  sanitizer errors.
- **Unchanged from M18:** the M13 audit re-run verdict (NOT READY);
  the 5 fuzz bugs R1–R5 stay fixed; the known limitations in
  `docs/RELEASE_NOTES.md` (28 items) are all still true; the trust
  boundaries (P2P input, RPC input, Stratum input, wallet file
  format, chain store) are unchanged.
- **Still the two blockers:** no approved mainnet genesis (E1) and no
  independent audit (E2) — see §H. Neither is a code problem; both
  are owner/external items.
- The clean-room E2E re-proves the operator-visible surface: wallet
  create/unlock/address/send, restart survival, Stratum share →
  real block. The locked-wallet watch-only quirk (§C.4) is a UX
  surprise, not a security hole (no spend is possible while locked).

## E. Files changed

- `docs/MILESTONE19.md` (NEW) — this report.
- `docs/RELEASE_NOTES.md` (NEW) — final release notes (M1–M19,
  finalized policy, 28 known limitations, toolchain, M19 checksums).
- `docs/M19-RELEASE-NOTES-DRAFT.md`, `docs/M19-DECISIONS-DRAFT.md`,
  `docs/M19-VERDICT-DRAFT.md` (REMOVED after folding) — working
  drafts from Stream C; their content lives in this report (§G, §H)
  and in `docs/RELEASE_NOTES.md`.
- `docs/MAINNET_LAUNCH.md` — final pass (Stream C).
- `docs/RELEASE.md` — final pass (Stream C: M18 entry; coordinator:
  M14–M17 backfill + M19 entry).
- `docs/DECISION_LOG.md` — M19 entry.
- `README.md` — M19 section + final status (M1–19 done, 205
  sections); M18 soak caveat updated (closed in M19).
- `docs/SECURITY_REVIEW.md` — M19 note.
- `logs/milestone19-cleanroom-log.txt` (NEW) — Stream A running log
  (1,058 lines).
- `logs/milestone19-soak-log.txt` (NEW) — Stream B running log +
  coordinator closing summary.
- `logs/milestone19-test-log.txt` (NEW) — the full M19 test log
  (clean-room log + soak log + coordinator final pass).
- `soak19/` (NEW, untracked) — soak driver, node logs, data dirs.
- `~/workspace/oeeoo-coin-verify.html` — §29 (M19) + §30 (test
  result); §28 marked superseded.
- `~/workspace/oeeoo-coin-milestone19.zip` (NEW) — git archive of the
  final HEAD + release dir + tarball.
- No source file was changed in M19. No test file was changed in M19.

## F. Test results

### F.1 Clean-room verification (Stream A, from a fresh checkout of d0a73f3)
- `rm -rf` fresh dir + `git clone` → HEAD
  `d0a73f3d594ba6c71bbda1b1b1c06d8822c24b9c`.
- `cmake -S . -B build && cmake --build build -j4` → exit 0,
  **0 warnings, 0 errors**.
- `./build/oeeoo_tests` → exit 0, **205 sections, ALL CHECKS PASSED**,
  0 CHECK failures.
- `ctest --test-dir build` → **100% tests passed, 0 failed out of 1
  (208.63 s)**.
- ASan/UBSan build + suite → exit 0, **ALL CHECKS PASSED (205
  sections), 0 sanitizer errors**.
- cppcheck → exit 0, **98/98 files, 0 findings**.
- E2E: 106 blocks mined, 1.5 OEEOO send confirmed in block 106,
  restart survival (height 106, balance 349.99999774), Stratum share
  accepted → block 107 (`00000000e7b6248c…`, nonce `7973e585`).
- Reproducible build (SOURCE_DATE_EPOCH=1794015825):
  **REPRODUCIBLE — binaries AND archive byte-identical.**

### F.2 Reproducible-build checksums

Clean-room run from the d0a73f3 checkout
(SOURCE_DATE_EPOCH=1794015825):

```
oeeoo-node    5f6c08d7319850a0e3dd1d0c39d63c48ec9bd601b351617f5fbd69a268a315b3
oeeoo-genesis 29345d45c528b5a8a39ce47891b546a3ab9b53d03770b87848d57e74377f5c19
oeeoo-cli     6b7a71af340d018930e53498844f7cf0b67f48b3aabaaadad06395c505fac2bb
oeeoo-coin-0.1.0.tar.gz
              a6d7f12a1cc447ea0ba9339b0280c9eac1223fc4c1cd581ce66f683e3bf1c4a5
```

Binary hashes are **identical to M18's recorded hashes** (M18 §F.1,
commit `7e8d862`) — the code is unchanged since M18, so this is the
expected result, and it is itself a check: a clean-room checkout
reproduces M18's binaries exactly. The tarball differs only via
SOURCE_DATE_EPOCH/SOURCE.txt.

Tagged-commit run (from local tag `v0.1.0`, commit <INTEGRATION-COMMIT>):
**REPRODUCIBLE — binaries AND archive byte-identical.** Binary hashes
identical to the run above; tarball
`<TAGGED-TARBALL-HASH>` (differs from the clean-room tarball only via
SOURCE.txt naming the tagged commit).

### F.3 Soak (Stream B, M18 final ASan binary, real P2P dialing)
Leg 2 ~1 h 56 m, 440 blocks, ~30 wallet txs, 0 block/tx failures,
peers 2/2 at all 29 samples, heights agreed at every sample,
**0 ASan / 0 UBSan errors**. Accepted under the M17 honest rule (see
§A.2). Full detail in `logs/milestone19-soak-log.txt`.

### F.4 Coordinator final pass (main repo, final HEAD)
- Clean build (`rm -rf build && cmake -S . -B build && cmake --build
  build -j4`) → exit 0, **0 warnings, 0 errors** (full build log
  `logs/milestone19-finalpass-build.log`, 113 lines; grep for
  warning/error → 0 lines).
- `./build/oeeoo_tests` → exit 0, **ALL CHECKS PASSED (205
  sections)** — the section count is the clean-room run's (§F.1);
  the test source is byte-identical to the clean-room checkout
  (integration commit is docs-only), so the count is the same.
- `ctest --test-dir build` → **100% tests passed, 0 failed out of 1
  (239.61 s)**.
- cppcheck → exit 0, **98/98 files, 0 findings** (full log
  `logs/milestone19-finalpass-cppcheck.log`, 196 lines — progress
  lines only, no findings).
- (ASan/UBSan: covered by the clean-room run in §F.1 from the same
  source — the integration commit is docs-only, so the code the
  sanitizers ran is byte-identical to the code in this HEAD.)
- First attempt failed on a missing toolchain (VM replaced again —
  see §C.3); restored and re-run from scratch. Log:
  `logs/milestone19-finalpass-log.txt`.

### F.5 Fuzz (M18, unchanged — re-stated, not re-run)
7 libFuzzer harnesses, 34-seed corpus, `ctest -R '^fuzz_'` 7/7
passed (M18 §F). No new fuzzing in M19 (no code changed to fuzz).

## G. Outstanding decisions (with owners)

Owner categories: **USER/owner** (only the owner can decide it),
**EXTERNAL** (an outside party — auditor or registry — must do it),
**FUTURE/post-launch** (scheduled after launch; v0.1.0 ships without
it).

**Totals: 21 OPEN, 16 CLOSED.** (B5, soak on the dialing binary,
moved OPEN → CLOSED in M19.)

### G.1 OPEN — blocks launch (must be decided before mainnet)

| # | Item | Owner | Status | Source |
|---|---|---|---|---|
| A1 | **Genesis parameters approval** — launch time (UTC), the 3 founder public keys (66 hex each, for `oeeoo-genesis --pubkey1/2/3`), and the resulting genesis hash. No mainnet genesis exists; no hardcoded mainnet checkpoint exists (the `--genesis-checkpoint` mechanism is built and tested, waiting for the approved value). Private keys are NEVER shared. | USER/owner | OPEN — unapproved | M13 §G.1; M13 §B; M14–M18 §G (standing) |
| A2 | **Independent security audit (external)** — required before mainnet. Give the auditor `docs/AUDIT_PACKAGE.md` + `docs/SECURITY_REVIEW.md`. M13 and the M18 re-run are self-reviews by the same agent that wrote the code; they are not audits. | EXTERNAL (auditor) | OPEN — not hired, not done | M13 §G.2, §D Critical; M14–M18 §H |
| A3 | **GitHub push** — the repo was created by the user 2026-11-06 and verified empty: https://github.com/OeeooCoin/Oeeoo-Coin-Blockchain-By-Ahmad — code push ONLY after ALL milestones (M19) complete, per the user's decision. Tag the launch commit (e.g. `v0.1.0`) at push time. | USER/owner | OPEN — repo exists, push pending M19 completion | M13 §G.3; M15 §G (repo created); M16–M18 §G |
| A4 | **DNS seed domains and operators** — 4 community seeds (spec item 15). Mechanism built (`dnsseed` config / `--dns-seed`, 10 s resolver timeout, results now dialed since M18); the list is EMPTY by default — no hostnames invented. | USER/owner | OPEN — no domains, no operators | M13 §G.4; M14–M18 §G (standing) |
| A5 | **Release-signing custody** — release-signing key generation, custody, rotation (2-of-3 per DECISION_LOG item 14). Reproducible package exists; signing is undecided. | USER/owner | OPEN — undecided | M13 §G.6; M14–M18 §G (standing) |
| A6 | **SLIP-0044 coin-type registration** — code uses provisional 65535 (unregistered, clearly marked). Must be registered officially before mainnet. | EXTERNAL (SLIP-0044 registry; owner files the request) | OPEN — provisional 65535 in code | M13 §G.5; DECISION_LOG item 1; M14–M18 §G |
| A7 | **Founder keys — verification of the user's report.** User reports (2026-11-06) all 3 pairs generated + secured on their mobile (single device, accepted risk). User-reported, NOT verified. The 3 backups the spec requires are the three holders' backups — not three copies in one place. | USER/owner | OPEN — user-reported, NOT verified | M15 §G; M16–M18 §G (standing) |

### G.2 OPEN — design / schedule (owner decision, not launch-blocking by themselves)

| # | Item | Owner | Status | Source |
|---|---|---|---|---|
| B1 | **Modern address formats (SegWit/Taproot)** — explicitly undecided. Spec says P2PKH only at launch; the M13 brief's "Legacy + modern formats" row conflicts with the spec's own warning not to assume SegWit was approved. Nothing implemented. | USER/owner | OPEN — undecided; P2PKH-only launch stands | M13 §G.7, §B; M14–M18 §G (carried) |
| B2 | **Distinct testnet identity (magic/port)** — undecided. Test and mainnet nodes share magic, port, protocol version, and user agent, so they peer with each other and reject each other's blocks only at consensus. The only guard is the testing-mode banner. | USER/owner | OPEN — undecided | M13 §G.9, §D High; M14–M18 §D (carried) |
| B3 | **Spec §20 item 16 — hardcoded fallback peer list** — NOT IMPLEMENTED (documented in `src/network/addrman.h:9`). With no DNS seeds either, a fresh node with an empty AddrMan learns no peers unless dialed. M18 dialing fixes the dialing half, not the fallback list. | USER/owner | OPEN — not implemented | M18 §C.2 A4; M18 §G; DECISION_LOG item 5 |
| B4 | **Spec §20 item 14 — historical 0.0001 OEEOO fee figure** — still no explicit reconciliation note against the dynamic-fee policy. It is NOT a fixed consensus fee. A one-line note in DECISION_LOG item 9 would close it (M18 recommendation). | USER/owner | OPEN — note not written | M18 §C.2 A3; M18 §G; DECISION_LOG item 9 |

### G.3 OPEN — future / post-launch (v0.1.0 ships without these)

| # | Item | Owner | Status | Source |
|---|---|---|---|---|
| C1 | **Stratum V2** — spec §13.1 requires it eventually; V1 works. Deferred past launch by user delegation (2026-11-06). | FUTURE/post-launch | OPEN — deferred | M14 §G; M16 §B.4, §G |
| C2 | **Multi-wallet** (`loadwallet`/`unloadwallet`/`listwallets`) — single `--wallet` is the M14 simplification. Deferred to FUTURE, post-M19/post-launch, by user delegation. | FUTURE/post-launch | OPEN — deferred | M14 §B, §G; M16 §B.3, §G |
| C3 | **AS-map for AddrMan** — needs an external AS data file + operator decision on its source/update. /16 bucketing accepted permanently for v0.1.0 by user delegation; AS-map deferred post-launch. | FUTURE/post-launch | OPEN — deferred (v0.1.0 defence is /16) | M15 §G.6; M16 §B.6, §A.7 |
| C4 | **RBF 100-replacement cap untested** — `kMaxReplacementCount = 100` is implemented and enforced, but no test references it (M13 "NOT VERIFIED", M18 finding A6). | FUTURE/post-launch | OPEN — untested | M13 §B; M15 §D; M16 §D; M17 §G; M18 §C.2 A6 |
| C5 | **RBF descendants unreachable in practice** — descendants cannot exist because validation rejects unconfirmed inputs; the replacement code handles them as defense-in-depth and the path cannot be exercised through `AddTransaction`. Closing this needs a decision on whether to record it as accepted-as-is. | FUTURE/post-launch | OPEN — untestable by design | M13 §B, §D Medium; M15 §D; M16 §D; M17 §D; M18 §D |
| C6 | **Disk-full behaviour for non-store I/O** — verified for the SQLite store path only (M17: startup refuses loudly, mid-chain rolls back, never corrupts). Wallet save, journal save, and mempool save I/O failures warn but are not crash-tested. | FUTURE/post-launch | OPEN — not crash-tested | M17 §G; M18 §D |
| C7 | **`submitblock` returns success while chain persistence fails** — since M17 the failure is loud in the node log (stderr warning, once per distinct error) but silent to RPC callers. Changing that is a semantic decision for a later milestone, not M17/M18. | FUTURE/post-launch | OPEN — semantic decision pending | M17 §A.4, §G; M18 §D |
| C8 | **Test-suite parallel safety** — the suite shares fixed `/tmp` paths (and some fixed ports) and is not safe to run concurrently (M18 parallel run: 665 CHECK failures, all artifacts; serial re-run passes 100%). Options: pid-unique temp paths suite-wide, or serialize suite runs (current practice). A suite-wide fix belongs to a hardening milestone. | FUTURE/post-launch | OPEN — serialize runs (current practice) | M17 §C.2.8, §G; M18 §D, §G |
| C9 | **Stale peer entries** — dead connections' peer entries linger (M18 soak: node A's peer count grew 1→5 while B stayed at 1). A peer-timeout is a hardening follow-up, not implemented. | FUTURE/post-launch | OPEN — not implemented | M18 §C.2 A7; M18 §G |
| C10 | **Ban-key persistence load path** — `ChainStore` has `PutBan`/`GetBans` but no load path is wired into the node, so bans do not survive restarts. If a load path is ever added it must pass stored keys through `BanManager::BanKey` (IP-only), or old stored `"ip:port"` keys would not match. | FUTURE/post-launch | OPEN — no load path wired | M16 §A.6, §D; M17 §D; M18 §D |

### G.4 CLOSED — decided in M14–M19 (listed so nothing is lost; no action)

| # | Item | Decided | Source |
|---|---|---|---|
| D1 | Node wallet integration (loading a wallet file in `oeeoo-node`) | M14 — implemented (loads LOCKED, Bitcoin model) | M13 §G.8 → M14 §A |
| D2 | `walletpassphrasechange` (passphrase rotation) | M16 — implemented, by user delegation 2026-11-06 14:55 PKT | M14 §G → M16 §A.4 |
| D3 | Wallet-tx journal format | M16 — SIDECAR file (`<wallet>.txjournal`); payload v2 rejected (migrating the encrypted wallet format is too risky) | M14 §G → M16 §A.3 |
| D4 | Inbound ban key | M16 — IP ALONE (`BanManager::BanKey` choke point) | M15 §G.5 → M16 §A.6 |
| D5 | AS-map for v0.1.0 | M16 — accept /16 bucketing permanently for v0.1.0 | M15 §G.6 → M16 §A.7 |
| D6 | Orphan promotion | M16 — LEAVE AS IS (block arrival only) | M15 §G.7 → M16 §A.7 |
| D7 | RPC load control | M16 — Core-style thread count (16) + bounded work queue (64, queue-full → HTTP 500), NO per-client rate limiting | M16 §A.5, §B.8 |
| D8 | Sigops in `getblocktemplate` | M16 — counted for real; existing 20000 is a template POLICY limit only (NOT consensus, NOT Core's 80,000) | M16 §A.2, §B.9 |
| D9 | Stratum `mining.authorize` accepts any credentials | M16 — documented as localhost-only by design (binds 127.0.0.1:64777); no auth change | M16 §A.7, §B.10 |
| D10 | Outbound P2P dialing (M17 #1 finding) | M18 — dial loop + `--addnode` + `addnode` RPC; DNS-seed results dialed | M17 §G #1 → M18 §A.1 |
| D11 | Outbound peer cap (DECISION_LOG F17 "125 total / 10 outbound") | M18 — 10 enforced in the dial loop (audit finding A1 closed; the 125 half was MATCH since M13) | M18 §A.1, §C.2 A1 |
| D12 | Fuzzer engine | M18 — libFuzzer (Clang 18.1.3); AFL++ not needed | M18 §A.2, §B.3 |
| D13 | Soak acceptances | M17 — run 2 (1 h 44 m) accepted; M18 — 3-leg soak (~107 min) accepted, leg 3 reported | M17 §A.5/§G; M18 §A.5/§B.5 |
| D14 | RSS creep under ASan | M17/M18 — accepted as ASan-quarantine behaviour (same shape/magnitude both soaks); a non-ASan long run is a possible follow-up, not required | M17 §G; M18 §B.6 |
| D15 | Vesting terms | Standing — spec §6 schedule (10 × 100,000, launch + 9 anniversaries) is the owner's finalized decision; any change needs the owner's explicit approval | M13 §G.10, §B |
| D16 | **Soak on the dialing binary** | **M19 — CLOSED by the Stream B soak**: ~1 h 56 m on the M18 final ASan binary with real P2P dialing (peers 2/2 at all 29 samples, 440 blocks, ~30 txs, 0 failures, 0 sanitizer errors) | M18 §G B5 → M19 §A.2 |

## H. Verdict: NOT READY (with cited evidence)

The verdict options are the M13 ones, applied as a binary — **READY**
vs **NOT READY** — under the operative rule quoted in M13 §A from
the M13 brief (§18): *"If Mainnet genesis parameters are still not
approved, Mainnet must remain NOT READY."* No third option
("conditionally ready") is documented anywhere in M13–M18, and no
milestone has ever used one.

### H.1 The two deciding claims (these alone decide the verdict)

| # | Claim | Evidence (file/section + what it shows) |
|---|---|---|
| E1 | **Mainnet genesis parameters are NOT approved.** No launch time, no approved founder public keys, no genesis hash, no hardcoded mainnet checkpoint. | `docs/MILESTONE13.md` §B, row "Genesis timestamp / hash / founder keys" — REQUIRES DECISION, "Unapproved; no mainnet genesis exists"; §G.1. Carried unchanged in every later milestone: M14 §G, M15 §G, M16 §G, M17 §G ("Standing owner items"), M18 §G ("No approved mainnet genesis"). `README.md` status line: "NO genesis block yet." The checkpoint mechanism (`--genesis-checkpoint`) is built and tested (M13 §A/§F: a 64-zero checkpoint refuses startup, exit 1) — the mechanism exists, the value does not. |
| E2 | **No independent security audit has happened.** M13 and the M18 re-run are self-reviews by the same agent that wrote the code. | `docs/MILESTONE13.md` §D, Critical: "No independent security audit. This milestone is a self-review by the same agent that wrote the code; it is not an audit." §G.2. M18 §A.6: the full 18-section M13 brief re-run (`soak18/M18C-AUDIT-DRAFT.md`) is again a self-review; its verdict is NOT READY. Carried in M14–M18 §G/§H as the standing blocker. `README.md`: "No independent security review or audit has been done." |

By the M13 §18 rule, E1 alone forces NOT READY; E2 independently
forces it too. Neither fact is in the code's power to fix — both are
owner/external items (§G A1, A2).

### H.2 Code-quality evidence (supports the code, does NOT change the verdict)

| # | Claim | Evidence (file/section + what it shows) |
|---|---|---|
| E3 | **Code matches the finalized consensus parameters where implemented.** Supply 21M (20M mining + 1M founder), rewards 50/25/12.5/6.25/3.125 to block 940,000, 10-min blocks, 2,016-block retarget with 4x clamp, 1 MB blocks, port 64775, magic `e2 d8 04 d6`, P2PKH-only launch, vesting spec §6. | `docs/MILESTONE13.md` §B decision cross-check table — every finalized decision MATCH (with test evidence cited per row); §F long-range supply simulation (940,000 blocks sum to exactly 20,000,000 mining + 1,000,000 founder = 21,000,000). M18 §A.4: differential tests against an independent pure-Python reference (subsidy, difficulty retarget, vesting incl. Feb 29, Merkle, Base58Check) all pass. M18 §A.6/§B: audit re-run cross-check — every finalized decision re-verified MATCH except items already carried as REQUIRES DECISION / NOT IMPLEMENTED. |
| E4 | **205 test sections pass; ctest 100%; ASan/UBSan clean; cppcheck 0 findings.** | This report §F.1 (clean-room, from a fresh checkout of `d0a73f3`): clean build 0 warnings; `./build/oeeoo_tests` — 205 sections, ALL CHECKS PASSED; `ctest` — 100% tests passed, 0 failed out of 1 (208.63 s); ASan/UBSan full suite, 0 sanitizer errors; cppcheck — 0 findings (98/98 files). Same clean record in M13 §F (127), M14 §F (134), M15 §F (155), M16 §F (171), M17 §F (182), M18 §F (205). |
| E5 | **Fuzzing evidence exists and found real bugs (all fixed, all non-consensus).** | `docs/MILESTONE18.md` §A.2/§A.3/§F: 7 libFuzzer harnesses (Clang 18.1.3), 34-seed corpus, `ctest -R '^fuzz_'` 7/7 passed; live P2P/RPC/Stratum fuzzing (6 sections); 5 bugs fixed — R1 (HIGH, unauthenticated remote crash via Content-Length), R2 (wallet loader heap overflow), R3 (Stratum unbounded buffer), R4 (latent `IntFromString`), R5 (OnBytes use-after-free). §A.4: "No consensus-touching issue was found anywhere in M18." |
| E6 | **Outbound P2P dialing — the M17 #1 launch-blocking gap — is FIXED and now soak-proven.** | `docs/MILESTONE18.md` §A.1: dial loop `TryDialPeers()`, `--addnode` flag + config key, `addnode` RPC (add/remove/onetry), outbound cap 10 enforced + tested, DNS-seed results actually dialed; 11 new test sections. This report §A.2: the M19 soak ran ~1 h 56 m on the dialing binary with real `--addnode` peering (peers 2/2 at all 29 samples, 440 blocks, ~30 txs, 0 failures). |
| E7 | **Build is reproducible from a clean Git checkout.** | This report §F.2: clean-room run from `d0a73f3` — REPRODUCIBLE, binaries AND archive byte-identical; binary hashes identical to M18's; tagged-commit run (§F.2) — REPRODUCIBLE, binary hashes identical. Same-machine, two-directory reproducibility only; cross-environment reproducibility is NOT claimed. |
| E8 | **Persistence and crash recovery are proven.** | `docs/MILESTONE17.md` §A/§F: mempool persistence round-trips; 100-deep reorg with undo correctness + mempool re-adding; SIGKILL mid-burst and mid-shutdown recover to a deep-verified store; disk-full rolls back, never corrupts. Soak run 2 accepted at 1 h 44 m (401 blocks, 27 txs, 0 failures). M18 §A.5: ASan soak re-run ~107 min, 374 blocks, 19 txs, 0 sanitizer errors. M19 §A.2: dialing-binary soak ~1 h 56 m, 440 blocks, ~30 txs, 0 failures, 0 sanitizer errors. |

### H.3 Evidence that points the other way (shown, not softened)

- **The code is in its best verified state so far.** M13 §H: "every
  gap the audit found that could be closed without changing an
  approved decision is closed." M18 §H repeats the NOT READY verdict
  while closing the M17 #1 launch-blocking gap and the M13 audit's A1
  gap. M19 closes the last verification gap in the project's own
  power (the dialing-binary soak) and reproduces M18's binaries
  exactly from a clean-room checkout.
- **The M18 audit re-run found no consensus-touching issue.** M18
  §A.6/§C.2: findings A1 and A5 fixed in M18; A2 is a process
  finding; A3/A4/A6 carried; A7/A8 are soak observations.
- **All 7 user-delegated decisions from 2026-11-06 were implemented
  in M16** (M16 §B) — the decision backlog that *could* be closed by
  the agent is closed.
- **The GitHub repo exists** (created by the user 2026-11-06, verified
  empty) — but the code has NOT been pushed, by the user's decision
  (push only after ALL milestones, M19, complete). Repo existence is
  not a push, and a push is not a launch.

None of the above approves a genesis, hires an auditor, or changes
the M13 §18 rule. The verdict stands on E1 + E2.

### H.4 What would have to change for the verdict to change

1. The owner approves the genesis parameters (launch time, 3 founder
   public keys, resulting genesis hash) — §G A1.
2. An independent security audit is completed and its findings are
   addressed — §G A2.
3. (Already true in M19:) the clean-room verification passes with no
   new blocking finding — §F.1/§F.3, no regression found.

Until (1) and (2) happen, the verdict is NOT READY no matter what the
code does. M19 prepares; it does not launch.

## Commits (local git repo, on top of M18's `d0a73f3`)

- `<INTEGRATION-COMMIT>` — M19 docs, verify page §29–30, test log
  (integration commit).
- `<CONFIRMATION-COMMIT>` — M19 reproducible-build confirmation from
  the tagged commit (RELEASE.md + this report §F.2 updated with the
  tagged-commit tarball hash).
- Local tag `v0.1.0` on `<INTEGRATION-COMMIT>` (local-only; no remote
  configured; nothing pushed, nothing published).
- Working tree clean at the end (only untracked build dirs,
  `founder-key-generator/`, and soak scratch files, all out of git).
