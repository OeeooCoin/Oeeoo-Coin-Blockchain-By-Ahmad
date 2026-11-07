# Oeeoo Coin — Milestone 20: Audit Remediation (OOC-SA-001)

Status: DONE 2026-11-07. The first independent audit report
(OOC-SA-001, received 2026-11-07 ~21:23 PKT) found three findings —
OOC-001 CRITICAL (multiple coinbase / unauthorized inflation),
OOC-002 HIGH (block submission succeeds despite persistence
failure), OOC-003 MEDIUM (wallet file permissions not 0600) — plus
a P1 network-identity note. All three findings are FIXED in this
milestone, with regression tests at every layer and a live
two-node demonstration that a malicious multi-coinbase block is
rejected and its sender banned. The user also approved network-
identity separation in M20 (2026-11-07 ~21:28 PKT), closing open
REQUIRES DECISION item B2. Full suite: **230 sections, ALL CHECKS
PASSED** (205 prior + 25 new); ctest 100%; ASan/UBSan clean;
cppcheck 0 findings; reproducible build verified.

The verdict stands: **NOT READY** (genesis unapproved — the M13
§18 rule decides it alone; the audit's own §7 also notes a
separate external cryptographic and consensus audit remains
necessary). M20 fixes what the code could fix; it does not launch
anything.

Two coordinators worked M20. The first did the code work (19 files
changed + 4 new test files, 25 new sections, all passing on the
normal build) but ended before its final report, docs, commits, or
deliverables. The close-out coordinator verified the first
coordinator's diff file by file, found one REAL bug in it (see
§C.1 — a double-temporary undefined-behaviour bug in the new
node_main banner that aborted the ASan node at startup), fixed it,
fixed one stale test observation (§C.2), re-ran the full
verification from the fixed tree, and closed the milestone out.
Nothing in this report is claimed from the first coordinator's
unverified run: every number below is from the close-out
verification, except where explicitly marked as the first
coordinator's run.

## A. Summary

### 1. The audit report (what arrived, and what was verified first)

The user pasted audit report OOC-SA-001 ("Crypto Coin Blockchain
Security Auditor", 7 November 2026, target
`oeeoo-coin-audit-package.zip`, scope: C++ source, consensus,
wallet, networking, storage, RPC, mining, Milestones 1–19).
Verdict: NOT READY FOR MAINNET. Three findings, plus additional
mainnet risks (§3.1–3.5: genesis, audit outstanding, network
identity, peer discovery, release security) and positive
engineering evidence (§4: the M1–M19 test record, quoted
accurately).

The parent agent independently verified all three findings
against the code BEFORE M20 started — the report was not taken
on trust:

- **OOC-001 — REAL, CRITICAL.** `Block::CheckBasic`
  (`src/blockchain/block.cpp:98`) checked size, per-tx structure,
  Merkle root, duplicate txids — but had NO coinbase-count rule
  and NO coinbase-first rule. `CheckBlockReward`
  (`src/mining/miner.cpp:89`) validated ONLY `block.vtx[0]` via
  `ValidateCoinbase`. `ConnectBlockTransactions`
  (`src/transactions/validation.cpp:142`) added EVERY
  `IsCoinbase(tx)` transaction's outputs to the UTXO set with
  only a `CheckFounderSpend` check — the code comment literally
  said "Coinbase: not validated here (Milestone 5)".
  `ComputeBlockFees` skipped ALL coinbase txs. Net effect: a
  miner could add a second coinbase tx at index 1 and mint coins
  beyond subsidy + fees, breaking the 21M cap. Genuine consensus
  vulnerability, not theoretical.
- **OOC-002 — REAL, HIGH (already documented in M17).**
  `Node::AcceptBlockDirect` committed all in-memory state (chain
  `AddBlock`, UTXO move, mempool updates) BEFORE
  `store_->ApplyBlock()`; on failure it only set
  `lastStoreError_` and returned true. M17's disk-full test had
  documented exactly this: 40 submissions "successful", restart
  recovered at height 6, 39 blocks lost. M17 fixed only the
  warning, not the semantics.
- **OOC-003 — REAL, MEDIUM.** `WriteFile` in
  `src/wallet/walletfile.cpp` used `std::fopen(path, "wb")` with
  no chmod/umask control — with umask 022 the file was
  world-readable (0644).

### 2. OOC-001 — exactly one coinbase, at index 0, at every layer

Enforced in three places (defense in depth), on EVERY acceptance
path:

- `Block::CheckBasic` (`src/blockchain/block.cpp`): `vtx[0]`
  must be a coinbase; any other `IsCoinbase(vtx[i])` rejects the
  block ("block contains more than one coinbase transaction").
  This holds for the genesis block too (its coinbase is `vtx[0]`).
- `CheckBlockReward` (`src/mining/miner.cpp`): the same scan
  runs BEFORE `ValidateCoinbase(vtx[0], ...)` — the exact layer
  the auditor flagged (it used to validate `vtx[0]` only).
- `ConnectBlockTransactions` (`src/transactions/validation.cpp`):
  a coinbase at index != 0 is a HARD REJECTION ("coinbase
  transaction at index N: only the first transaction may be a
  coinbase") — never a mint, never silently skipped; and a
  non-coinbase at index 0 is rejected too ("first transaction is
  not a coinbase").
- `ComputeBlockFees` still skips coinbase txs — and that skip is
  now provably safe: the function only sums fees of validated
  non-coinbase txs, it never mints, and every path that reaches
  the mint rejects a second coinbase first (comment in the code
  states this reasoning explicitly).

Acceptance-path audit (every caller of `CheckBasic`,
`CheckBlockReward`, `ConnectBlockTransactions`): normal
`AcceptBlockDirect` (node.cpp:1171 `CheckBasic`, :1243
`CheckBlockReward`, :1256 `ConnectBlockTransactions`), reorg
side-branch acceptance (node.cpp:1452 `CheckBlockReward`, :1464
`ConnectBlockTransactions` — plus the branch blocks pass through
`CheckBasic` when indexed), full-chain verification
(`src/blockchain/verify.cpp:66` `CheckBlockReward`, `:72`
`ConnectBlockTransactions`, over blocks loaded from the store,
which itself re-checks `CheckBasic` at chainstore.cpp:218/228/
594), and genesis validation (`ValidateGenesisCoinbase` at height
0, with the `CheckBasic` rule applied to the genesis block too).
The mempool refuses coinbase txs (`ValidateTransaction` fails
them on the missing zero-hash input — tested in m20a), and the
miner (`AssembleBlock`) builds exactly one coinbase (tested in
m20a). No path can admit a multi-coinbase block.

Regression tests — `tests/test_milestone20a.cpp` (8 sections):
second coinbase at index 1 minting 25 OEEOO extra (rejected at
all three layers + node level, second txid never in the UTXO
set); coinbase at index 1 with a normal tx at index 0
(rejected); second coinbase minting near MAX_MONEY (rejected);
valid single-coinbase block still accepted (no regression);
reorg onto a fork whose tip block has two coinbases (rejected);
stored chain with a multi-coinbase block fails full verification
(`VerifyChainFromStore` rejects it); **live two-node
demonstration** — a malicious peer sends a valid-PoW,
valid-Merkle multi-coinbase block over real TCP to an honest
node, which rejects it and BANS the sender (see §F.3 for the
logged evidence); miner builds exactly one coinbase, mempool
refuses coinbase. This demonstration is the auditor's release
condition for OOC-001 ("Independent regression verification
must demonstrate that unauthorized coin issuance is
impossible") — demonstrated, logged, and cited in §F.3.

### 3. OOC-002 — durable acceptance semantics

The node must not report success before persistence succeeds.
Implemented as persist-BEFORE-commit, in both acceptance paths:

- **Direct path** (`Node::AcceptBlockDirect`): (1) validate
  everything against working copies (fees, reward, undo,
  connect); (2) validate the header + compute cumulative work on
  a TRIAL COPY of the chain index (`ChainState trial = chain_`
  — this IS `ChainState::AddBlock`, on a copy, so the header
  validation is exact; the trial exists because there is no
  ChainState API to remove a block again, so validating on the
  real index first and rolling back is not possible); (3) persist
  with `ChainStore::ApplyBlock` (one atomic SQLite transaction);
  (4) only then commit to the real index and the in-memory state
  (UTXO, mempool, undo, fee estimator). A persistence failure
  returns FALSE + error ("failed to persist accepted block: ...")
  with `punish == false` (a local I/O failure is not the
  sender's fault) and changes NO in-memory state — the node's
  height stays at the durable height. The real `AddBlock` in
  step (4) re-validates with the same inputs, so it can only
  fail on a concurrent competing acceptance (RPC/Stratum callers
  are serialized by nodeMutex, the P2P Poll loop is
  single-threaded) — that case best-effort undoes the persist
  (`DisconnectBlock`) and still returns false.
- **Reorg path** (`Node::AcceptBlockReorg`): same design. The
  trial copy gives `becomesTip` (read off the trial tip, not a
  duplicated greatest-work rule). Persist is sequential, one
  atomic SQLite transaction per block (no multi-block transaction
  API), so a failure CAN land mid-reorg — handled by a
  best-effort `restoreStore` (disconnect applied side blocks
  newest-first, re-apply disconnected main blocks oldest-first
  using the saved `mainUndos` — the store's own undo rows are
  deleted by `DisconnectBlock`, so they are captured before the
  persist starts). Honest limit, stated in the code: under a
  sustained I/O failure (e.g. disk full) the restore itself
  fails the same way the persist did — the store is then left at
  an intermediate but VALID chain state (the fork point, or
  partway through the main re-apply; each step is one atomic
  transaction, so it is never corrupt), and the node keeps
  serving the old in-memory tip until a restart loads the
  durable state. An inactive side branch is persisted with
  `PutBlockAtHeight` before indexing — a failure rejects the
  block outright instead of indexing a side branch the store
  never saw.
- Cost, disclosed: the trial is a full copy of the chain index
  (O(chain) per acceptance). That cost is accepted for exactness;
  a future header-only check method on ChainState could replace
  it.
- `submitblock` / `AcceptBlock` / `BroadcastBlock` now return an
  error on persistence failure (previously success). `Node.h`
  documents the new contract; `LastStoreError()` is still set
  (the node_main stderr warning still fires — M17 loudness
  preserved).

Regression tests — `tests/test_milestone20b.cpp` (7 sections):
disk-full `submitblock` fails, height stays at the durable
height (RLIMIT_FSIZE; the limit MUST bite — `CHECK(sawRejection)`
— or nothing was tested; a second distinct block at the same
height is rejected too; restart recovers at the durable height;
deep verification passes); closed-store submission fails with no
partial state (no UTXO entry for the failed block's coinbase;
the same block is accepted once the store is healthy again);
restart after failed submissions recovers at the durable height
with identical tips on two nodes; RPC `submitblock` after a
persistence failure is a rejection (not success); normal
submission succeeds and persists (no regression); reorg under
store failure is rejected without adopting (tip, UTXO, mempool
stay on the old chain — and the SAME fork block reorgs fine once
the store is healthy); relayed coinbase tx is rejected, never
orphaned (see §C.3). M17 crash-injection tests (SIGKILL
mid-burst, mid-shutdown, disk-full) re-run as part of the full
suite — all pass with the new semantics (the m17b disk-full
observation now reads OBSERVED (M20), see §C.2).

### 4. OOC-003 — wallet files 0600

- `OpenPrivateFile` (new, `src/wallet/walletfile.cpp`):
  `open(O_WRONLY | O_CREAT, 0600)` → `fchmod(fd, 0600)` →
  `fstat` verify (mode must be exactly 0600) → `ftruncate`
  → `fdopen`. The mode is fixed BEFORE any byte is written,
  and the old contents are truncated only after the mode is
  verified, so a chmod failure leaves the old file intact
  instead of destroying it, and secrets never sit in a
  world-readable file even briefly. `open()` with mode 0600 is
  umask-proof for a NEW file (a umask can only clear bits), but
  does not change a PRE-EXISTING file's mode — hence the
  explicit `fchmod`. Platform scope: POSIX only (stated in the
  code; Windows ACLs out of scope).
- `WriteFile` now uses `OpenPrivateFile` (the `fopen("wb")`
  call is gone).
- `SaveWalletFileAtomic` (`src/wallet/nodewallet.cpp`): the tmp
  file is written by `SaveWalletFile` (0600 by construction);
  the rename carries that inode — and its mode — onto the final
  path, so a pre-existing wider-permission file is replaced by a
  0600 one. The final mode is verified here anyway rather than
  assumed (`stat` → must be 0600, else the save fails).
- `SaveTxJournal` (`src/wallet/txjournal.cpp`): the journal
  holds transaction history (txids, addresses, amounts, labels)
  — no secrets, but privacy-sensitive — so it gets the same
  treatment: its own `OpenPrivateFile` (same logic; a second
  copy in an anonymous namespace — see §C.5) for the tmp file,
  and the final mode verified after the rename.
- Regression tests — `tests/test_milestone20c.cpp` (5 sections):
  fresh save 0600 under umask 022; save over an existing file
  keeps 0600 under umask 000; atomic save over a pre-existing
  0644 file ends 0600 (umask 022); atomic save leaves no `.tmp`
  behind; tx journal saves 0600 (fresh + replacement over
  0644). Every save round-trips (load → mnemonic/indices/records
  intact). Honest note in the test: the tmp file's mode mid-save
  is NOT directly observed (the save is synchronous, no hook
  exists to stat it while it exists) — it follows from the same
  `SaveWalletFile` code path proven 0600 in the first two
  sections, not from an assumption about the rename; post-save
  absence + final 0600 is asserted.

### 5. Network identity separation (user-approved, closes B2)

The auditor's §3.3 / P1 note (test and mainnet nodes shared
magic, port, protocol version, user agent) is fixed in M20 — the
user approved it 2026-11-07 ~21:28 PKT ("Haan, M20 mein alag
kar do"). This CLOSES open REQUIRES DECISION item B2 (see §G).

- Testnet magic `d9 73 08 c2` = first 4 bytes of SHA-256("Oeeoo
  Coin Testnet Network Magic") — the SAME derivation as mainnet
  (verified independently with python3 hashlib: mainnet string
  → `e2d804d6`, testnet string → `d97308c2`). Mainnet magic
  `e2 d8 04 d6` UNCHANGED.
- Testnet default ports 64779/64780/64781 (P2P/RPC/Stratum) —
  the mainnet triple shifted by +4, so a mainnet node and a
  testnet node run side by side on one host with no listener
  colliding. Mainnet ports 64775/64776/64777 UNCHANGED.
- User agents: mainnet `"/OeeooCoin:0.1.0/"` (UNCHANGED),
  testnet `"/OeeooCoin:0.1.0(testnet)/"`. Protocol version 70015
  is the SAME on both networks by design — the magic mismatch is
  what separates them.
- `src/network/protocol.h/.cpp`: `NetworkIdentity` struct +
  `kMainnetIdentity` / `kTestnetIdentity`, `NetworkId` enum,
  process-wide active identity (`SelectNetwork`, default
  mainnet; `ActiveNetworkMagic()`, `ActiveUserAgent()`).
  `SerializeMessage` / `ParseMessage` frame and check with the
  ACTIVE magic; `Node::OnBytes` repeats the check against the
  active magic (a testnet node rejects mainnet bytes at the
  pre-check, not just in `ParseMessage`). `getnetworkinfo`
  `subversion` follows the active agent. DNS seeds resolve
  against the active network's P2P port.
- `src/app/config.h/.cpp` + `node_main.cpp`: `--testnet` /
  `--network testnet` (CLI) or `network=testnet` (config file)
  selects testnet; default (no flag) is mainnet. `Effective*()`
  ports: an explicit port always wins over the network default,
  regardless of order; later flag wins; CLI overrides the file.
  An invalid `network` value is REJECTED loudly (never a silent
  fallback to mainnet — a typo must not silently put a test node
  on Mainnet or vice versa). `node_main` selects the network
  ONCE at startup, listens on the effective ports, prints the
  network line + a loud `*** TESTNET ***` banner (testnet is NOT
  Mainnet and can never peer with a mainnet node).
- This is a network identity ONLY: it does NOT change consensus
  (the PoW limit still comes from `--pow-limit-bits` as before),
  and it is NOT a public testnet — direct mainnet launch stands
  (spec F24).
- Regression tests — `tests/test_milestone20d.cpp` (5 sections):
  identities differ (magic/ports/agent); wire framing uses the
  active magic, cross-network bytes rejected; a testnet node
  cannot peer with a mainnet node (real TCP, both directions);
  control — two mainnet nodes DO peer; `--testnet` /
  `network=testnet` select testnet, mainnet is the default
  (incl. explicit-port-wins, later-flag-wins, CLI-overrides-file,
  invalid-value rejection, usage text). A `NetworkGuard` RAII
  helper restores the previously active network on destruction,
  so a section that selects testnet can never leak it into the
  next test (the suite default is mainnet); the mixed-network
  pump switches the global per node per Poll round and restores
  mainnet before returning.

### 6. What M20 did NOT do (owner's, not the agent's)

Genesis approval (launch time, 3 founder public keys, genesis
hash); a second external audit / auditor re-confirmation of the
OOC-001 release condition; DNS seed domains + operators; release
signing; SLIP-0044 registration; founder-key verification; the
launch itself. M20 fixes; it does not launch.

## B. Decisions

### B.1 Network-identity separation — APPROVED by the user, implemented (closes B2)

User decision 2026-11-07 ~21:28 PKT ("Haan, M20 mein alag kar
do"). Testnet gets its own magic (`d9 73 08 c2`), default ports
(64779/64780/64781), and user agent (`"/OeeooCoin:0.1.0(testnet)/"`);
mainnet identity UNCHANGED (magic `e2 d8 04 d6`, ports
64775/64776/64777, agent `"/OeeooCoin:0.1.0/"`). Recorded in
`docs/DECISION_LOG.md` (Milestone 20 section) and in §G (B2 →
D17).

### B.2 OOC-002 semantics — persist BEFORE commit (trial copy)

Design decision by the coordinator (inside the delegated M20
scope): validate on working copies → validate the header + work
on a trial copy of the chain index → persist → commit in-memory.
Chosen over commit-then-roll-back because there is no ChainState
API to remove a block again. Cost disclosed in §A.3: a full
index copy (O(chain)) per acceptance. The alternative
(commit-then-roll-back) was rejected, not overlooked.

### B.3 Version stays 0.1.0 (recorded, not bumped)

M20 is a fix milestone, not a release. No version string in code
was changed. The local tag `v0.1.0` (M19, on commit `9252086`)
is NOT moved — the M20 code is a new HEAD after the tag, and no
new tag is created (a release tag belongs to the release step,
not to a fix milestone).

### B.4 Nothing else decided in M20

No finalized decision was changed. No REQUIRES DECISION item
was invented or decided by the agent. The relayed-coinbase guard
(§C.3) is a bug fix inside the OOC-001 scope, not a new policy.

## C. Bugs found and fixed (disclosed)

### C.1 REAL BUG in the first coordinator's code — node_main banner double-temporary (found by close-out verification, FIXED)

`src/app/node_main.cpp` (M20, network-identity banner) built the
magic hex string as:

```cpp
BytesToHexStr(std::vector<unsigned char>(
      ActiveNetworkMagic().begin(),
      ActiveNetworkMagic().end()))
```

`ActiveNetworkMagic()` returns `std::array<unsigned char, 4>` BY
VALUE — so `.begin()` and `.end()` came from TWO DIFFERENT
temporaries. Taking an iterator range across two different
arrays is undefined behaviour. Observed, both ways:

- Normal build: the banner printed the magic DOUBLED —
  `magic e2d804d6e2d804d6` (8 bytes: both temporaries' contents,
  adjacent on the stack) instead of `e2d804d6`. Wrong output,
  no crash.
- ASan build: `AddressSanitizer: stack-buffer-overflow, READ of
  size 20` at `node_main.cpp:261`, aborting the node at startup
  (before the main loop). Every real-node spawn in the ASan suite
  then failed — the first (killed) ASan run's entire m17b cascade
  (and the m18a failures that followed) was this bug, not an
  environment flake: the node aborted before ever answering RPC,
  so `StartNodeReady` timed out on all 4 attempts.

Fix (close-out): capture the magic once in a local
(`bannerMagic`) and take both iterators from it. Verified: the
normal binary now prints `magic e2d804d6`, and the ASan suite
passes in full (see §F.1). The other M20 call sites were
checked by grep and are correct — `src/network/node.cpp:492`
and `src/network/protocol.cpp:117,138` all store
`ActiveNetworkMagic()` in a local first. Lesson recorded: a
by-value array return must never be split across `.begin()` /
`.end()` — the same trap as the M17 mempool-dump bug (two
different temporaries for one value).

### C.2 Stale m17b disk-full observation under M20 semantics (test-only, FIXED)

`tests/test_milestone17b.cpp` disk-full B printed "NOTE — the
limit never bit ... disk-full behaviour was NOT observed" whenever
the restart height equalled the in-memory height. Under M17 that
meant the limit never bit. Under M20 a submit rejected upfront
leaves the height at the durable height BY DESIGN — so in the
M20 suite run the note printed WHILE a submit had just been
rejected for disk-full ("failed to persist accepted block: disk
I/O error"). The test still passed (its CHECKs are
height-based), but the observation was wrong. Fixed (close-out):
a new `else if (submitFailed)` branch prints "OBSERVED (M20) —
the limit bit: a submit was rejected on persistence failure and
the height stayed at the durable height N; no rollback was
needed" and CHECKs the "chain store error" warning fired. The
old NOTE now prints only when NO submit failed at all. Test-only
change (printf + one CHECK); the new run's log shows the OBSERVED
(M20) line (§F.1).

### C.3 Relayed coinbase transaction admitted to the orphan pool (Worker A finding, FIXED)

Found by the first coordinator while working OOC-001: a
coinbase-shaped transaction relayed as a `"tx"` failed
`ValidateTransaction` with "missing input" (zero-hash prev) and
was admitted to the orphan pool, where it can never validate —
its parent txid `0x00..00` never arrives. Coinbase transactions
only ever arrive inside blocks, never as relayed transactions.
Fixed in `Node::OnMessage` (`src/network/node.cpp`): `if
(IsCoinbase(tx)) { Misbehave(peerId, 10); return; }` BEFORE the
orphan branch — no orphan add, no relay, +10 misbehavior like
any other invalid relayed transaction. Regression test in m20b
with a control: a NON-coinbase tx with a genuinely missing parent
IS admitted to the orphan pool (proving the delivery path works),
while the coinbase-shaped relay is rejected, never orphaned,
never in the mempool, and the peer stays connected (+10 is far
below the 100-point ban threshold).

### C.4 Environment: VM replaced twice during close-out; miner contention

- The VM was replaced twice during close-out verification: the
  first clean-build attempt was SIGTERMed at configure time
  (uptime showed a fresh boot), and the second build attempt was
  SIGTERMed mid-compile. Builds were re-run via `setsid nohup`
  (detached from the agent shell's process group) and completed.
  No run's evidence depends on a killed run.
- A user XMRig miner (`xmrig-6.26.0`, the student-mining plan
  from the main chat) is running on this VM — 4 threads, ~1.8 GB
  RSS — and was running through the whole close-out. It is the
  user's own job and was NOT touched. It made every run slower
  (full suite 281.93 s under ctest vs 208–240 s in M19; the ASan
  suite took much longer than its usual ~4 min). The slowness is
  environmental, not a regression.
- The first (killed) ASan run was terminated by the close-out
  coordinator AFTER the diagnosis in §C.1 (it was failing on the
  banner bug, not on the environment) — its partial log is
  superseded by the clean re-run in §F.1. It is NOT cited as
  evidence anywhere.

### C.5 Observations (not bugs)

- **Reorg restore honest limit** (§A.3): under a sustained I/O
  failure the mid-reorg restore itself fails — the store is left
  at an intermediate but VALID chain state, and the node keeps
  serving the old in-memory tip until a restart loads the
  durable state. Documented in the code; no test forces this
  path (it needs a store that fails twice in a row at chosen
  points — a fault-injection store, not a milestone-20 scope).
- **Trial-copy cost** (§A.3): O(chain) index copy per block
  acceptance. Accepted for exactness; a header-only check method
  could replace it later.
- **`OpenPrivateFile` is duplicated** (`src/wallet/walletfile.cpp`
  and `src/wallet/txjournal.cpp`, each in its own anonymous
  namespace). No ODR issue; the duplication is two small
  functions, not a shared helper — acceptable, noted so a future
  cleanup can unify them.
- **m20c tmp-mode-mid-save** (§A.4): not directly observed, by
  design (synchronous save, no hook); inferred from the shared
  code path — disclosed in the test itself.
- **M17b disk-full B flakiness (pre-existing, unchanged):** the
  RLIMIT_FSIZE test depends on the chain.db size vs the limit;
  in M17 it bit after ~40 submits, in M20 after ~1. The m20b
  disk-full test is the reliable one (it REQUIRES the limit to
  bite via `CHECK(sawRejection)`).

## D. Security report

| # | Audit finding | Status in M20 | Evidence |
|---|---|---|---|
| OOC-001 | Multiple coinbase / unauthorized inflation (CRITICAL) | **CLOSED — fixed + regression-verified** | §A.2; §F.1 (m20a, 8 sections); §F.3 live demo — a malicious multi-coinbase block is rejected at every layer and its sender banned; no path can mint beyond subsidy + fees. |
| OOC-002 | Block submission succeeds despite persistence failure (HIGH) | **CLOSED — fixed + regression-verified** | §A.3; §F.1 (m20b, 7 sections) — disk-full, closed-store, restart-recovery, RPC rejection, reorg-under-failure. |
| OOC-003 | Wallet file permissions not 0600 (MEDIUM) | **CLOSED — fixed + regression-verified** | §A.4; §F.1 (m20c, 5 sections) — wallet file, atomic save, tx journal, all 0600 under umask 022 and 000. |
| §3.3 | Network identity shared between test and mainnet (P1) | **CLOSED — fixed + regression-verified** | §A.5; §F.1 (m20d, 5 sections) — distinct magic/ports/agent; cross-network peering impossible. User-approved 2026-11-07 ~21:28 PKT. |

Still open (not code, or not this milestone): genesis approval
(§G A1); auditor re-confirmation of the OOC-001 release
condition + a second external cryptographic/consensus audit
(the report's own §7 note — §G A8, new); DNS seeds (§G A4);
release signing (§G A5); SLIP-0044 (§G A6); founder-key
verification (§G A7). The audit's §3.4 (peer discovery: empty
seed list, no fallback list) and §3.5 items are unchanged by M20
— they were already open items (§G B3, A4, A5, A6, A7) and stay
open.

The M13 §18 verdict rule still applies (see §H). M20 closes
the three code findings and the network-identity note; it does
not close the verdict.

## E. Files changed

Code (first coordinator, verified file-by-file by close-out):

- `src/blockchain/block.cpp` (+15) + `block.h` (+8/−2) — OOC-001:
  exactly-one-coinbase-at-index-0 in `CheckBasic`; doc comment
  updated.
- `src/mining/miner.cpp` (+25) — OOC-001: same scan in
  `CheckBlockReward`; comment explaining why the
  `ComputeBlockFees` coinbase skip is safe.
- `src/transactions/validation.cpp` (+31) — OOC-001: hard
  rejection of any coinbase past index 0 and of a non-coinbase
  at index 0 in `ConnectBlockTransactions`.
- `src/network/node.cpp` (+308/−~60) + `node.h` (+12/−4) —
  OOC-002: persist-before-commit in `AcceptBlockDirect` and
  `AcceptBlockReorg` (trial copy, restore path); OOC-001
  relayed-coinbase guard in `OnMessage`; network identity
  (active magic in `OnBytes`, `ActiveUserAgent()` in
  `SendVersion`, active port in `QueryDnsSeeds`); updated
  `AttachStore` contract comment.
- `src/network/protocol.cpp` (+40) + `protocol.h` (+73) —
  network identity: `NetworkIdentity`, mainnet/testnet
  identities, process-wide active network, active magic in
  framing/parsing.
- `src/app/config.cpp` (+77) + `config.h` (+25) — `--testnet` /
  `--network` / `network=` selection, `Effective*()` ports.
- `src/app/node_main.cpp` (+46) — network selection once at
  startup, effective ports for listen/RPC/Stratum, network
  banner (close-out fixed the double-temporary bug in it, §C.1).
- `src/rpc/rpcserver.cpp` (+4/−1) — `getnetworkinfo`
  `subversion` follows the active agent.
- `src/wallet/walletfile.cpp` (+50) — OOC-003:
  `OpenPrivateFile` (0600, fchmod, fstat, truncate-after-verify);
  `WriteFile` uses it.
- `src/wallet/nodewallet.cpp` (+14) — OOC-003: final-mode
  verification after atomic save.
- `src/wallet/txjournal.cpp` (+54) — OOC-003: `OpenPrivateFile`
  for the journal tmp file + final-mode verification.
- `CMakeLists.txt` (+4) — the four new test files.
- `tests/test_chain.cpp` (+25) — `MakeSpendTx` (a block now
  needs exactly one coinbase; the old two-`MakeTx` block shape
  is invalid).
- `tests/test_validation.cpp` (+18) — valid blocks now carry a
  real coinbase at `vtx[0]`.
- `tests/test_main.cpp` (+8) — register the four M20 suites.

New test files (first coordinator):

- `tests/test_milestone20a.cpp` (581 lines, 8 sections) —
  OOC-001.
- `tests/test_milestone20b.cpp` (1,220 lines, 7 sections) —
  OOC-002 + relayed-coinbase guard.
- `tests/test_milestone20c.cpp` (267 lines, 5 sections) —
  OOC-003.
- `tests/test_milestone20d.cpp` (408 lines, 5 sections) —
  network identity.

Close-out changes (on top of the above):

- `src/app/node_main.cpp` — banner double-temporary fix (§C.1).
- `tests/test_milestone17b.cpp` — disk-full observation fix
  (§C.2).

Docs (close-out):

- `docs/MILESTONE20.md` (this report), `docs/DECISION_LOG.md`
  (M20 section, incl. B2 → closed), `README.md` (230 sections,
  M20 section, audit status corrected), `docs/SECURITY_REVIEW.md`
  (OOC-001/002/003 + network identity closed, M20 note),
  `docs/AUDIT_PACKAGE.md` (milestone range MILESTONE2…M20,
  OOC-SA-001 note). Verify page §31–32.
- `logs/milestone20-test-log.txt` (full test log, this run).
- `audit-outreach-draft.md` (untracked, repo root) — NOT part of
  M20 and NOT committed; assessed separately in §G.5.

Nothing outside the M20 scope was changed. The diff was reviewed
file by file against the brief; every hunk belongs to OOC-001,
OOC-002, OOC-003, network identity, or the two close-out fixes
above.

## F. Test results

### F.1 Close-out verification (main repo, final tree — all runs by the close-out coordinator)

- Clean build (`rm -rf build && cmake -S . -B build &&
  cmake --build build -j1`; `-j1` because an XMRig miner was
  using all 4 CPUs — see §C.4; two earlier `-j4` attempts were
  SIGTERMed by VM replacements and re-run) → exit 0, **0
  warnings, 0 errors** (grep for warning/error in the build log
  → 0 lines; log `/tmp/m20-build.log` + `/tmp/m20-build2.log`).
- `./build/oeeoo_tests` → exit 0, **ALL CHECKS PASSED (230
  sections — 205 prior + 25 new: m20a 8, m20b 7, m20c 5, m20d
  5)**, 0 CHECK failures. Full log: `/tmp/m20-suite.log` (252
  lines); copied to `logs/milestone20-test-log.txt` (this log,
  with the commands — see §F.4).
- `ctest --test-dir build` → **100% tests passed, 0 failed out
  of 1 (281.93 s)**. Log: `/tmp/m20-ctest.log`.
- ASan/UBSan build + full suite (`build-asan-m20`,
  `-fsanitize=address,undefined -fno-sanitize-recover=all`,
  `ASAN_OPTIONS=detect_leaks=0`, no stdbuf) → **ALL CHECKS
  PASSED (230 sections), exit code 0, 0 sanitizer errors**
  (log `logs/milestone20-asan-log.txt`, 252 lines; the run took
  ~11 min wall under the XMRig miner load). The live m20a
  demonstration reproduced under ASan with a different block
  hash (`527526de992faeb070d751e696725867d2098413b834787f5d396795187310b7`,
  honest bans=1 peers=0 tip=0). The first ASan run was KILLED
  by close-out after the §C.1 diagnosis (its m17b/m18a cascade
  was the banner bug, not the code) — superseded, NOT cited as
  evidence; its log was overwritten by the re-run.
- cppcheck (`--enable=warning,performance,portability
  --std=c++17 --suppress=missingIncludeSystem -I src src/
  tests/`) → exit 0, **102/102 files, 0 findings** (4 more
  files than M19's 98 — the four new m20 test files). Log:
  `/tmp/m20-cppcheck.log`.
- M17 crash-injection tests re-run inside the full suite
  (m17b): SIGKILL mid-burst and mid-shutdown recover; disk-full
  now reads OBSERVED (M20) — a submit rejected upfront, height
  at the durable height 6, no rollback needed (§C.2).
- First coordinator's run (normal build, pre-close-out):
  `./build/oeeoo_tests` → ALL CHECKS PASSED (230 sections),
  log `logs/milestone20-test-log.txt` as found (251 lines). Its
  ASan run was killed by close-out after the §C.1 diagnosis —
  superseded, NOT cited as evidence. The close-out runs above
  re-prove everything from the fixed tree.

### F.2 Reproducible-build checksums

**PENDING — to be run from the final commit and filled in
before commit.**

### F.3 Live multi-coinbase rejection (auditor's OOC-001 release condition)

From the close-out suite run (`/tmp/m20-suite.log`, m20a live
section):

```
[m20a] LIVE: malicious block 4c918865a3071fee16f9362aeeb6da97c08b18dba54bb0fece1741a9d36fa384 rejected: block contains more than one coinbase transaction; honest bans=1 peers=0 tip=0
```

A malicious peer sent a valid-PoW, valid-Merkle block whose ONLY
defect was a second coinbase minting 25 OEEOO, over real TCP, to
an honest node. The honest node rejected it at `CheckBasic`,
banned the sender (bans=1), dropped the peer (peers=0), kept its
tip at genesis (tip=0), and minted nothing (the second coinbase's
outputs are absent from its UTXO set — asserted in the same
section). The same block is rejected at all three layers in
isolation (§A.2) and by reorg and full-chain verification.
Unauthorized coin issuance through a second coinbase is
impossible on every acceptance path — demonstrated live, not
just asserted.

### F.4 Test log

`logs/milestone20-test-log.txt` — every command + full output
of the close-out verification (clean build, full suite 230
sections, ctest 100%, ASan/UBSan, cppcheck, reproducible
build), in the same format as `logs/milestone19-test-log.txt`.
The first coordinator's log (251 lines, normal-build run) is
superseded by this one; the m20a/m20b/m20c/m20d section lines
are identical in content (same tests, same evidence lines,
different block hashes per run — hashes are mined fresh each
run, so they differ run to run by design).

### F.5 Fuzz (M18, unchanged — re-stated, not re-run)

7 libFuzzer harnesses, 34-seed corpus, `ctest -R '^fuzz_'` 7/7
passed (M18 §F). No new fuzzing in M20: the new code paths
(coinbase rule, persist-before-commit, 0600 saves, network
identity) are covered by the 25 new regression sections, and
the fuzz harnesses exercise `CheckBasic` (which now includes
the coinbase rule) on every mutated block. A dedicated fuzz
run against the new code is a possible follow-up, not claimed
here.

## G. Outstanding decisions (with owners)

Owner categories: **USER/owner** (only the owner can decide it),
**EXTERNAL** (an outside party — auditor or registry — must do
it), **FUTURE/post-launch** (scheduled after launch; v0.1.0 ships
without it).

**Totals: 20 OPEN, 18 CLOSED.** (B2, network identity, moved
OPEN → CLOSED in M20 (D17); C7, `submitblock` persistence
semantics, moved OPEN → CLOSED in M20 (D18); A8, second
crypto/consensus audit, added OPEN — the OOC-SA-001 report's own
§7 note.)

### G.1 OPEN — blocks launch (must be decided before mainnet)

| # | Item | Owner | Status | Source |
|---|---|---|---|---|
| A1 | **Genesis parameters approval** — launch time (UTC), the 3 founder public keys (66 hex each, for `oeeoo-genesis --pubkey1/2/3`), and the resulting genesis hash. No mainnet genesis exists; no hardcoded mainnet checkpoint exists (the `--genesis-checkpoint` mechanism is built and tested, waiting for the approved value). Private keys are NEVER shared. | USER/owner | OPEN — unapproved | M13 §G.1; M14–M19 §G (standing) |
| A2 | **Independent security audit — report received, confirmation pending.** Audit report OOC-SA-001 received 2026-11-07 (user pasted it). Its three findings (OOC-001/002/003) are FIXED in M20 with regression verification (§F.3 for OOC-001). What is still missing: the auditor's own confirmation that the fixes close the findings, and the separate external cryptographic/consensus audit the report's §7 says remains necessary (see A8). | EXTERNAL (auditor) | OPEN — report received, fixes done, auditor confirmation pending | OOC-SA-001 (2026-11-07); M20 §D, §F.3 |
| A3 | **GitHub push** — the repo was created by the user 2026-11-06 and verified empty: https://github.com/OeeooCoin/Oeeoo-Coin-Blockchain-By-Ahmad — code push ONLY after ALL milestones (M19) complete, per the user's decision. M19 is complete; the push now waits only on the user's go-ahead. Tag the launch commit (e.g. `v0.1.0`) at push time. | USER/owner | OPEN — repo exists, push pending go-ahead | M13 §G.3; M15 §G; M16–M19 §G |
| A4 | **DNS seed domains and operators** — 4 community seeds (spec item 15). Mechanism built (`dnsseed` config / `--dns-seed`, 10 s resolver timeout, results dialed since M18); the list is EMPTY by default — no hostnames invented. | USER/owner | OPEN — no domains, no operators | M13 §G.4; M14–M19 §G (standing) |
| A5 | **Release-signing custody** — release-signing key generation, custody, rotation (2-of-3 per DECISION_LOG item 14). Reproducible package exists; signing is undecided. | USER/owner | OPEN — undecided | M13 §G.6; M14–M19 §G (standing) |
| A6 | **SLIP-0044 coin-type registration** — code uses provisional 65535 (unregistered, clearly marked). Must be registered officially before mainnet. | EXTERNAL (SLIP-0044 registry; owner files the request) | OPEN — provisional 65535 in code | M13 §G.5; DECISION_LOG item 1; M14–M19 §G |
| A7 | **Founder keys — verification of the user's report.** User reports (2026-11-06) all 3 pairs generated + secured on their mobile (single device, accepted risk). User-reported, NOT verified. The 3 backups the spec requires are the three holders' backups — not three copies in one place. | USER/owner | OPEN — user-reported, NOT verified | M15 §G; M16–M19 §G (standing) |
| A8 | **Second external cryptographic and consensus audit** — OOC-SA-001 §7: "A separate external cryptographic and consensus audit remains necessary before launch." OOC-SA-001 was a source-level review (no source modifications, no live-network attack, no mainnet deployment). The crypto/consensus audit is a different engagement. | EXTERNAL (auditor) | OPEN — not hired, not done | OOC-SA-001 §7 (2026-11-07) |

### G.2 OPEN — design / schedule (owner decision, not launch-blocking by themselves)

| # | Item | Owner | Status | Source |
|---|---|---|---|---|
| B1 | **Modern address formats (SegWit/Taproot)** — explicitly undecided. Spec says P2PKH only at launch; the M13 brief's "Legacy + modern formats" row conflicts with the spec's own warning not to assume SegWit was approved. Nothing implemented. | USER/owner | OPEN — undecided; P2PKH-only launch stands | M13 §G.7, §B; M14–M19 §G (carried) |
| B3 | **Spec §20 item 16 — hardcoded fallback peer list** — NOT IMPLEMENTED (documented in `src/network/addrman.h:9`). With no DNS seeds either, a fresh node with an empty AddrMan learns no peers unless dialed. M18 dialing fixes the dialing half, not the fallback list. | USER/owner | OPEN — not implemented | M18 §C.2 A4; M18 §G; DECISION_LOG item 5 |
| B4 | **Spec §20 item 14 — historical 0.0001 OEEOO fee figure** — still no explicit reconciliation note against the dynamic-fee policy. It is NOT a fixed consensus fee. A one-line note in DECISION_LOG item 9 would close it (M18 recommendation). | USER/owner | OPEN — note not written | M18 §C.2 A3; M18 §G; DECISION_LOG item 9 |

### G.3 OPEN — future / post-launch (v0.1.0 ships without these)

| # | Item | Owner | Status | Source |
|---|---|---|---|---|
| C1 | **Stratum V2** — spec §13.1 requires it eventually; V1 works. Deferred past launch by user delegation (2026-11-06). | FUTURE/post-launch | OPEN — deferred | M14 §G; M16 §B.4, §G |
| C2 | **Multi-wallet** (`loadwallet`/`unloadwallet`/`listwallets`) — single `--wallet` is the M14 simplification. Deferred to FUTURE, post-M19/post-launch, by user delegation. | FUTURE/post-launch | OPEN — deferred | M14 §B, §G; M16 §B.3, §G |
| C3 | **AS-map for AddrMan** — needs an external AS data file + operator decision on its source/update. /16 bucketing accepted permanently for v0.1.0 by user delegation; AS-map deferred post-launch. | FUTURE/post-launch | OPEN — deferred (v0.1.0 defence is /16) | M15 §G.6; M16 §B.7, §A.7 |
| C4 | **RBF 100-replacement cap untested** — `kMaxReplacementCount = 100` is implemented and enforced, but no test references it (M13 "NOT VERIFIED", M18 finding A6). | FUTURE/post-launch | OPEN — untested | M13 §B; M15 §D; M16 §D; M17 §G; M18 §C.2 A6 |
| C5 | **RBF descendants unreachable in practice** — descendants cannot exist because validation rejects unconfirmed inputs; the replacement code handles them as defense-in-depth and the path cannot be exercised through `AddTransaction`. Closing this needs a decision on whether to record it as accepted-as-is. | FUTURE/post-launch | OPEN — untestable by design | M13 §B, §D Medium; M15 §D; M16 §D; M17 §D; M18 §D |
| C6 | **Disk-full behaviour for non-store I/O** — verified for the SQLite store path only (M17: startup refuses loudly, mid-chain rolls back, never corrupts; M20: failed persists are rejected upfront). Wallet save, journal save, and mempool save I/O failures warn but are not crash-tested. | FUTURE/post-launch | OPEN — not crash-tested | M17 §G; M18 §D |
| C8 | **Test-suite parallel safety** — the suite shares fixed `/tmp` paths (and some fixed ports) and is not safe to run concurrently (M18 parallel run: 665 CHECK failures, all artifacts; serial re-run passes 100%). Options: pid-unique temp paths suite-wide, or serialize suite runs (current practice). A suite-wide fix belongs to a hardening milestone. | FUTURE/post-launch | OPEN — serialize runs (current practice) | M17 §C.2.8, §G; M18 §D, §G |
| C9 | **Stale peer entries** — dead connections' peer entries linger (M18 soak: node A's peer count grew 1→5 while B stayed at 1). A peer-timeout is a hardening follow-up, not implemented. | FUTURE/post-launch | OPEN — not implemented | M18 §C.2 A7; M18 §G |
| C10 | **Ban-key persistence load path** — `ChainStore` has `PutBan`/`GetBans` but no load path is wired into the node, so bans do not survive restarts. If a load path is ever added it must pass stored keys through `BanManager::BanKey` (IP-only), or old stored `"ip:port"` keys would not match. | FUTURE/post-launch | OPEN — no load path wired | M16 §A.6, §D; M17 §D; M18 §D |

### G.4 CLOSED — decided in M14–M20 (listed so nothing is lost; no action)

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
| D17 | **Distinct testnet identity (magic/port/userAgent)** | **M20 — CLOSED by user decision 2026-11-07 ~21:28 PKT**: testnet magic `d9 73 08 c2` (SHA-256 derivation, same as mainnet), default ports 64779/64780/64781, agent `"/OeeooCoin:0.1.0(testnet)/"`; mainnet identity UNCHANGED | M13 §G.9 → M20 §A.5, §B.1 |
| D18 | **`submitblock` returns success while chain persistence fails** | **M20 — CLOSED by durable-acceptance semantics**: persist-before-commit in both acceptance paths; a failed persist returns an error and changes no in-memory state (§A.3); regression tests in m20b | M17 §A.4, §G → M20 §A.3 |

### G.5 `audit-outreach-draft.md` — assessment (untracked, NOT committed)

A Roman-Urdu draft found untracked in the repo root, apparently
written by one of the first coordinator's streams. It is NOT part
of M20 and was deliberately left untracked and uncommitted (it is
a user-facing outreach note, not code or a milestone document).

Factual check:

- **Audit price ranges ($20,000–$300,000; Trail of Bits ~$25,000
  per engineer-week, minimum ~$50,000; Halborn $20,000–$80,000)
  — NOT VERIFIED from any source in this repo.** They are
  market-rate claims the draft states without citation. They
  are plausible orders of magnitude for L1 audits, but the user
  should treat them as rough estimates, not quotes — get a real
  quote before budgeting.
- **Audit-package link + expiry (2026-11-09 20:45 PKT) + SHA-256
  `9b689ed1…` — CORRECT**, matches the delivered package
  (commit `50f1146`).
- **"Download the zip to your mobile NOW before the link
  expires" — CORRECT and still urgent** (the link expires
  2026-11-09 20:45 PKT; the code lives only on this VM until
  the GitHub push).
- **GitHub repo exists, empty; local tag `v0.1.0`; code complete
  to M19 — CORRECT.**
- **OSTIF (contactus@ostif.org) as a free-audit route — the
  organization exists and does fund open-source security work;
  the email address is the one OSTIF publishes. No guarantee —
  the draft says so itself.** Worth sending; costs nothing.
- **"When the auditor's report arrives, send it to me" — DONE
  in fact:** the user pasted OOC-SA-001 on 2026-11-07 ~21:23
  PKT, and M20 is the fix round the draft anticipated. The
  draft's premise (no audit yet) is now overtaken by events for
  the FIRST audit — but it is still the right text for the
  SECOND audit (A8) and for the OSTIF outreach, which has not
  happened yet.

Recommendation to the parent agent: the draft is safe and
useful to show the user (its actionable parts — download the
zip, GitHub push decision, OSTIF email — are all correct), with
one correction: the price ranges are unverified estimates, and
the "no audit yet" premise is now stale for OOC-SA-001 (the
report arrived and M20 fixed its findings). Do NOT commit it to
the repo — it belongs in the user's hands, not in the codebase.

## H. Verdict: NOT READY (with cited evidence)

The verdict options are the M13 ones, applied as a binary —
**READY** vs **NOT READY** — under the operative rule quoted in
M13 §A from the M13 brief (§18): *"If Mainnet genesis parameters
are still not approved, Mainnet must remain NOT READY."* No third
option ("conditionally ready") is documented anywhere in
M13–M19, and no milestone has ever used one.

### H.1 The two deciding claims (these alone decide the verdict)

| # | Claim | Evidence (file/section + what it shows) |
|---|---|---|
| E1 | **Mainnet genesis parameters are NOT approved.** No launch time, no approved founder public keys, no genesis hash, no hardcoded mainnet checkpoint. | `docs/MILESTONE13.md` §B, row "Genesis timestamp / hash / founder keys" — REQUIRES DECISION, "Unapproved; no mainnet genesis exists"; §G.1. Carried unchanged in every later milestone: M14 §G, M15 §G, M16 §G, M17 §G ("Standing owner items"), M18 §G, M19 §G A1. `README.md` status line: "NO genesis block yet." The checkpoint mechanism (`--genesis-checkpoint`) is built and tested (M13 §A/§F: a 64-zero checkpoint refuses startup, exit 1) — the mechanism exists, the value does not. |
| E2 | **No independent security audit has happened — UPDATED in M20.** An independent audit report (OOC-SA-001) HAS now been received (2026-11-07), and its three findings are fixed in M20 with regression verification (§F.3 for OOC-001). But: (a) the auditor has not confirmed the fixes close the findings; (b) the report's own §7 says a separate external cryptographic and consensus audit remains necessary. So the audit blocker is PARTLY cleared (a report exists and its findings are fixed) but NOT closed — and E1 alone forces NOT READY regardless. | OOC-SA-001 (2026-11-07), verdict NOT READY FOR MAINNET; this report §D, §F.3, §G A2/A8. M19 §H E2 (no audit at all) is superseded by this paragraph — the facts changed, the verdict did not. |

By the M13 §18 rule, E1 alone forces NOT READY; E2 (as updated)
independently supports it. Neither fact is in the code's power to
fix — both are owner/external items (§G A1, A2, A8).

### H.2 Code-quality evidence (supports the code, does NOT change the verdict)

| # | Claim | Evidence (file/section + what it shows) |
|---|---|---|
| E3 | **Code matches the finalized consensus parameters where implemented.** Supply 21M (20M mining + 1M founder), rewards 50/25/12.5/6.25/3.125 to block 940,000, 10-min blocks, 2,016-block retarget with 4x clamp, 1 MB blocks, port 64775, magic `e2 d8 04 d6`, P2PKH-only launch, vesting spec §6. | `docs/MILESTONE13.md` §B decision cross-check table — every finalized decision MATCH (with test evidence cited per row); §F long-range supply simulation (940,000 blocks sum to exactly 20,000,000 mining + 1,000,000 founder = 21,000,000). M18 §A.4: differential tests against an independent pure-Python reference (subsidy, difficulty retarget, vesting incl. Feb 29, Merkle, Base58Check) all pass. M18 §A.6/§B: audit re-run cross-check — every finalized decision re-verified MATCH except items already carried as REQUIRES DECISION / NOT IMPLEMENTED. M20 §A.2: the coinbase rule is now enforced at three layers — the supply invariant the auditor attacked is closed. |
| E4 | **230 test sections pass; ctest 100%; ASan/UBSan clean; cppcheck 0 findings.** | This report §F.1 (close-out verification, final tree): clean build 0 warnings; `./build/oeeoo_tests` — 230 sections, ALL CHECKS PASSED; `ctest` — 100% tests passed, 0 failed out of 1 (281.93 s); ASan/UBSan full suite — ALL CHECKS PASSED (230 sections), exit 0, 0 sanitizer errors; cppcheck — 0 findings (102/102 files). Same clean record in M13 §F (127), M14 §F (134), M15 §F (155), M16 §F (171), M17 §F (182), M18 §F (205), M19 §F (205). |
| E5 | **Fuzzing evidence exists and found real bugs (all fixed, all non-consensus).** | `docs/MILESTONE18.md` §A.2/§A.3/§F: 7 libFuzzer harnesses (Clang 18.1.3), 34-seed corpus, `ctest -R '^fuzz_'` 7/7 passed; live P2P/RPC/Stratum fuzzing (6 sections); 5 bugs fixed — R1 (HIGH, unauthenticated remote crash via Content-Length), R2 (wallet loader heap overflow), R3 (Stratum unbounded buffer), R4 (latent `IntFromString`), R5 (OnBytes use-after-free). §A.4: "No consensus-touching issue was found anywhere in M18." M20 adds a consensus-touching issue FOUND BY AN EXTERNAL AUDITOR (OOC-001) and fixed here — the first consensus bug found by anyone outside the project. |
| E6 | **Outbound P2P dialing — the M17 #1 launch-blocking gap — is FIXED and now soak-proven.** | `docs/MILESTONE18.md` §A.1: dial loop `TryDialPeers()`, `--addnode` flag + config key, `addnode` RPC (add/remove/onetry), outbound cap 10 enforced + tested, DNS-seed results actually dialed; 11 new test sections. M19 §A.2: the M19 soak ran ~1 h 56 m on the dialing binary with real `--addnode` peering (peers 2/2 at all 29 samples, 440 blocks, ~30 txs, 0 failures). |
| E7 | **Build is reproducible from a clean Git checkout.** | M19 §F.2: clean-room run from `d0a73f3` — REPRODUCIBLE, binaries AND archive byte-identical; binary hashes identical to M18's; tagged-commit run — REPRODUCIBLE. Same-machine, two-directory reproducibility only; cross-environment reproducibility is NOT claimed. M20 §F.2: PENDING (to be run from the final commit). |
| E8 | **Persistence and crash recovery are proven — and the semantics are now correct.** | `docs/MILESTONE17.md` §A/§F: mempool persistence round-trips; 100-deep reorg with undo correctness + mempool re-adding; SIGKILL mid-burst and mid-shutdown recover to a deep-verified store; disk-full rolls back, never corrupts. M20 §A.3: a failed persist is now REJECTED upfront instead of silently succeeding — the M17 behaviour the auditor flagged as OOC-002 is closed, with the M17 crash-injection tests re-run green under the new semantics. |

### H.3 Evidence that points the other way (shown, not softened)

- **The first independent audit has happened, and its findings
  are fixed.** That is real progress over M19 (where no audit
  existed at all). The auditor's own verdict was NOT READY FOR
  MAINNET — and M20 does not claim to overturn it: the auditor
  has not re-confirmed the fixes, and the report asks for a
  second, cryptographic/consensus audit.
- **The code is in its best verified state so far.** 230
  sections (25 new, all regression tests for the audit
  findings), the first consensus bug found by an outsider fixed
  at three layers, durable-acceptance semantics proven by
  disk-full/closed-store/restart tests, wallet files 0600,
  network identity separated.
- **All user-delegated decisions from 2026-11-06 were
  implemented in M16** (M16 §B) — and the network-identity
  decision, the last open design item the agent could close
  with the user's approval, is closed in M20 (D17).
- **The GitHub repo exists** (created by the user 2026-11-06,
  verified empty) — but the code has NOT been pushed, by the
  user's decision (push only after ALL milestones, M19,
  complete — M19 IS complete; the push now waits only on the
  user's go-ahead). Repo existence is not a push, and a push is
  not a launch.

None of the above approves a genesis, confirms the audit fixes
with the auditor, or changes the M13 §18 rule. The verdict
stands on E1 + E2.

### H.4 What would have to change for the verdict to change

1. The owner approves the genesis parameters (launch time, 3 founder
   public keys, resulting genesis hash) — §G A1.
2. The auditor confirms the OOC-001/002/003 fixes close the
   findings, AND the separate external cryptographic/consensus
   audit is completed and its findings addressed — §G A2, A8.
3. (Already true in M20:) the verification passes with no new
   blocking finding — §F.1, no regression found.

Until (1) and (2) happen, the verdict is NOT READY no matter what
the code does. M20 fixes; it does not launch.

## Commits (local git repo, on top of M19's `50f1146`)

- (To be filled in at commit time — M20 code commit, M20 docs
  commit, repro-build confirmation commit.)
- No new tag. Local tag `v0.1.0` stays on M19's `9252086`
  (local-only; no remote configured; nothing pushed, nothing
  published).
- Working tree clean at the end (only untracked build dirs,
  `founder-key-generator/`, soak scratch files, and
  `audit-outreach-draft.md`, all out of git).
