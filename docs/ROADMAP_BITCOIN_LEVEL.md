# Oeeoo Coin — Roadmap to Bitcoin-Level (Milestones M14–M19)

Status: DRAFT for the morning of 2026-11-06. Milestones 1–13 are DONE
(127 test sections, ALL CHECKS PASSED, ctest 100%, ASan/UBSan clean,
cppcheck 0 findings — see `docs/MILESTONE13.md`). The M13 audit verdict
was **NOT READY** (genesis unapproved, no independent audit); that
verdict stands until the owner's launch decisions are made, no matter
how many of these milestones complete.

## What "Bitcoin-level" means here

"Bitcoin-level" does **not** mean copying Bitcoin Core, and it does not
mean matching it feature for feature. It means: a node an operator can
run the way a Bitcoin Core node is run — a working wallet behind the
same RPC, a P2P layer that survives hostile peers, an RPC surface that
covers the everyday operator questions, a chain that survives crashes
and restarts, and evidence (fuzzing, soak, audit) instead of claims.
Where Oeeoo deliberately differs from Bitcoin (own magic, port, P2PKH
version byte `0x0f`, no SegWit/Taproot at launch, provisional coin type
65535, 10-minute blocks with Oeeoo rewards), the difference is a
finalized decision and is NOT a gap.

Comparison (Bitcoin Core behaviour vs Oeeoo today):

| Area | Bitcoin Core | Oeeoo today (after M13) | Gap |
|---|---|---|---|
| Node wallet | Wallet in the node; locked by default; `walletpassphrase` / `walletlock`; `createwallet` | Wallet library only; `oeeoo-node` loads no wallet; RPC wallet methods return -13 | **M14** |
| Wallet tx history | `listtransactions`, `gettransaction` (wallet records its txs) | No wallet tx history; only UTXO scan (`listunspent`) | M16 |
| P2P identity | Inbound peer IPs recorded; bans keyed by IP/subnet | Inbound IPs discarded; bans keyed by ephemeral transport id | M15 |
| AddrMan | Per-source limits, expiry, randomized selection, AS-map awareness (asmap) | 1,000 cap + IPv4 validation + randomized `GetAddr` (M13); no per-source limits, no expiry, no asmap | M15 |
| DNS seeds | Resolver with timeout; seeds queried on empty addrman | `getaddrinfo` with no timeout; seeds only when addrman empty | M15 |
| Orphan transactions | Orphan pool (limited, with expiry + eviction) | None — unknown-parent txs rejected outright | M15 |
| Compact blocks | In-flight limits, timeouts, full-block fallback | PoW check + 120 s pending expiry + fallback (M13); no per-peer in-flight limit, no `getblocktxn` retry | M15 |
| Peer eviction | Evicts worst peers when full (incl. block-relay-only eviction) | Cap enforced (M13); no eviction policy | M15 |
| RPC completeness | `getchaintips`, `getblockstats`, sigops in templates, thread/work-queue bounds (`-rpcthreads` / `-rpcworkqueue`) — Core has NO per-client rate limiting | Absent; `getblocktemplate` reports sigops 0 and fees 0; 32-connection cap only | M16 |
| Mempool persistence | `mempool.dat` saved on shutdown, loaded on start | None — mempool starts empty every restart | M17 |
| Reorg depth | No single verified Core depth figure — the "100" is Oeeoo's own target (coinbase maturity rationale) | Undo tested to depth 2 only | M17 |
| Crash recovery | WAL + atomic writes; no named Core crash-injection test verified — Oeeoo robustness work, not Core parity | WAL + `synchronous=FULL`; unclean-shutdown test only (store never closed) | M17 |
| Fuzzing | Continuous fuzzing of P2P/RPC/deserialize (incl. OSS-Fuzz) | Parser fuzzing only (~6,000 inputs); live P2P/RPC/Stratum state machines unfuzzed | M18 |
| Audit | Multiple independent audits over the years | None — M13 was a self-audit | Owner decision (M19) |

---

## M14 — Node wallet integration (IN PROGRESS, overnight 2026-11-06)

**Goal:** the biggest functional gap closes — `oeeoo-node` loads a
wallet, and the RPC wallet methods work end to end.

**Scope (exact):**
- `src/app/config.*`: `--wallet <path>` CLI flag + `wallet` config key.
  A relative path resolves against the data directory. Empty = no
  wallet (current behaviour, error -13 preserved).
- `src/wallet/nodewallet.*` (NEW): `NodeWallet` — owns the wallet file
  path, a `Wallet` instance, lock state, and the unlock deadline.
  - `LoadFile(path)`: verifies the file exists and parses as a wallet
    file, loads it **LOCKED** (no decryption without the passphrase).
  - `CreateFile(path, passphrase)`: fresh 24-word mnemonic, saves the
    encrypted file, returns the mnemonic ONCE (caller shows it to the
    operator, never stores it). Fails if the file already exists.
    Left LOCKED.
  - `Unlock(passphrase, timeoutSeconds)`: decrypts the file, restores
    the wallet, caches the account xpub + indices for the locked
    watch-only view. Wrong passphrase → error (Bitcoin code -14).
  - `Lock()`: replaces the live wallet with a watch-only wallet
    restored from the cached xpub + indices; wipes the mnemonic,
    keys, and file passphrase from memory (best-effort cleanse).
  - `SaveIfDirty()`: re-encrypts the file with current indices when
    they advanced (called after address derivation/spending and at
    node shutdown). The file format is unchanged
    (PBKDF2-HMAC-SHA512 200k + AES-256-GCM).
  - Auto-relock: `IsLocked()` re-locks lazily when the deadline passes.
- `src/wallet/wallet.*`: small accessors the wrapper needs —
  `GetNextExternal()` / `GetNextChange()` / `SetNextIndices(ext, change)`
  (load-time only, with a comment) and `WipeSecrets()` (cleanse
  mnemonic + private keys before the object is dropped). No behaviour
  change to existing methods.
- `src/rpc/rpcserver.*`: `AttachNodeWallet(NodeWallet*)` (the existing
  `AttachWallet(Wallet*)` stays for tests). New methods `createwallet`,
  `walletpassphrase`, `walletlock`. Existing wallet methods go through
  the node wallet when one is attached: `getbalance` / `listunspent` /
  `getwalletinfo` work while locked (watch-only view);
  `getnewaddress` / `sendtoaddress` refuse while locked (error -13,
  "wallet is locked") because the file's indices can only be persisted
  while the passphrase is in memory. `getwalletinfo.unlocked_until`
  reports the real deadline (0 = locked). `getblocktemplate` refuses
  to derive a miner address from a locked wallet. Help list grows to
  32 methods.
- `src/app/node_main.cpp`: loads the wallet file (or registers the
  path for later `createwallet` when the file does not exist yet),
  attaches it to the RPC server, saves + locks on shutdown, prints a
  `wallet:` line in the banner.
- `docs/WALLET.md`: rewritten node-wallet section (locked model,
  `createwallet` / `walletpassphrase` / `walletlock`, timeouts,
  balance-while-locked, `getnewaddress`-while-locked refusal, BIP39
  passphrase must be empty for node wallets — the file passphrase is
  a different secret).

**The passphrase rule (hard constraint, Bitcoin model):** the wallet
passphrase is NEVER a CLI flag and NEVER stored in a file the node
reads silently (it would be visible in `ps` / on disk). It travels
only over localhost RPC (`walletpassphrase`), lives in memory only
while unlocked, and is wiped on lock. The node never logs it.

**Acceptance criteria (tests that must pass):**
- New `tests/test_milestone14.cpp`: wallet load/lock/unlock lifecycle;
  wrong passphrase rejected (error -14); locked watch-only balance
  visible; `getnewaddress` / `sendtoaddress` refused while locked;
  indices persist across lock/unlock (no address reuse); `createwallet`
  refuses an existing file; unlock timeout auto-relocks.
- RPC tests through a real `RpcServer` + `NodeWallet`: all five
  existing wallet methods + the three new ones, end to end.
- E2E (manual, test values only, recorded in the log): `oeeoo-node
  --wallet …` + `oeeoo-cli createwallet / walletpassphrase /
  getnewaddress / sendtoaddress` between two wallet addresses.
- All 127 existing sections still pass; full suite + ctest + ASan/UBSan
  on the final code; cppcheck 0 findings; build warning-free.

**Notes (from the Bitcoin Core comparison, researched overnight):**
- No descriptor wallets: Oeeoo's wallet is deliberately BIP39/44
  P2PKH (spec §7) — closest to Core's old legacy HD wallet, which
  Core itself removed in v30. M14 wires Oeeoo's own wallet into the
  node; it does not adopt descriptors.
- `encryptwallet` does not map: Oeeoo wallet files are created
  already-encrypted; there is no unencrypted→encrypted transition.
  Passphrase rotation (`walletpassphrasechange`) is a follow-up
  candidate, not in M14 scope.
- Single wallet only in M14 (`--wallet <path>`). Core's multi-wallet
  support (`loadwallet` / `unloadwallet` / `listwallets`) is future
  work, not M14.
- `listtransactions` / `gettransaction` are NOT in M14: the wallet
  library keeps no transaction history (only a UTXO scan), so those
  methods need a wallet-tx journal first. They belong to M16 with
  the format decision (see below).

**Stays REQUIRES DECISION:** none new. (BIP39-passphrase wallets are
out of scope for the node wallet — the file passphrase is the only
secret the node handles.)

---

## M15 — P2P hardening & completeness

**Goal:** the node survives hostile peers and a hostile address stream.

**Scope (exact):**
- `src/network/transport.*`: record the real peer IP on `accept()`
  (plumb the address through `ITransport::Poll` / `TransportMessage`
  or an equivalent); `Node::OnBytes` stores it as `peer.address` so
  bans are keyed by IP and survive reconnects. Inbound ban test.
- `src/network/addrman.*`: source-netgroup bucketing (Core's
  anti-fill defence is bucketing by source netgroup, not a flat
  per-source count cap — implement bucketing, or at minimum a
  per-source-group cap) + entry expiry (Core horizon: 30 days) +
  failure counting. Documented decision on AS-map awareness (Core
  uses asmap files; Oeeoo has none — likely "not implemented,
  documented").
- `src/network/dnsseed.*`: resolver timeout. Core runs DNS lookups
  off the main thread so a hanging resolver does not block startup
  (no verified Core lookup-timeout constant exists — the Oeeoo fix
  is a worker thread + deadline, Core-style, not a copied constant).
- `src/transactions/` + `src/mempool/` + `src/network/node.*`: orphan
  transaction pool (Core's `TxOrphanage` model: max 100 orphans,
  20-minute expiry, NOT relayed, indexed by prevout so a tx is
  validated when its parent arrives, per-peer purge on disconnect,
  byte-based eviction cap). A tx whose parents are unknown goes to
  the orphan pool instead of being rejected. Must not become a
  memory DoS vector (that is the test).
- `src/network/node.*`: compact relay — per-peer in-flight limit for
  pending compact blocks, depth limits (Core: cmpctblock depth 5,
  getblocktxn depth 10 — starting values, not gospel), a `getblocktxn`
  retry (one retry, then full-block `getdata`), and a block-stalling
  disconnect (Core: 2 s default, max 64 s).
- `src/network/node.*`: peer eviction when full — when the 125-peer
  cap is hit, evict an existing inbound peer rather than refusing
  all new connections (Core's `AttemptToEvictConnection`: protect by
  netgroup diversity, lowest ping, recent tx/block senders, and
  longest-connected). Document the eviction ranking.

**Acceptance criteria:** tests for IP-keyed bans across reconnect;
AddrMan netgroup bucketing/cap + expiry; DNS timeout (simulated slow
resolver or a unit test on the timeout path); orphan pool bounds/
expiry/parent-arrival/purge-on-disconnect; compact in-flight limit +
depth limits + retry + stalling disconnect; eviction picks the worst
peer (and protects the best). Full suite + ASan/UBSan + cppcheck clean.

**Stays REQUIRES DECISION:** AS-map file (needs an external data
file + operator decision); distinct testnet identity — framed as an
*internal* regtest-style mode (distinct magic/port/genesis, local-only
and deterministic, like Core's `-regtest`), NOT a public testnet
(spec F24: direct mainnet, no public testnet). It must not become a
public testnet by accident.

---

## M16 — RPC & mining completeness

**Goal:** the RPC surface answers the everyday operator questions.

**Scope (exact):**
- `src/rpc/rpcserver.*`: `getchaintips` (all chain tips with height,
  hash, branch length, status — verify first that the chain index
  retains stale-branch tips; M11 only adopts side branches on
  strictly more work, so check whether losing branches are retained
  or discarded), `getblockstats` (per-block: tx count, size, fees,
  average fee rate — from the stored block + UTXO view; drop all
  SegWit `sw*` fields), `getchaintxstats` (cheap companion once
  block stats exist), `getblocktemplate` real per-tx sigops counting
  AND real per-tx fees (both are hardcoded 0 today) — replace the
  documented "sigops always 0" simplification with a real count from
  the template's scripts, RPC load control (see below),
  `listtransactions` + `gettransaction`.
- **RPC load control — correction to the original draft:** Bitcoin
  Core has NO per-client rate limiting. Its controls are a thread
  count (`-rpcthreads`, Core default 16) and a bounded work queue
  (`-rpcworkqueue`, Core default 64; queue-full → HTTP 500), plus
  localhost binding, cookie auth, and `rpcwhitelist`. Per-client
  rate limiting is an Oeeoo-only hardening choice: decide explicitly
  in M16 whether Oeeoo wants it, and do NOT present it as Core
  parity if it is implemented.
- **Sigop limit — REQUIRES DECISION:** Oeeoo has no block-level
  sigop consensus limit anywhere in `src/consensus/`; the 20000
  `sigoplimit` is an RPC-only constant. Do NOT blindly copy Core's
  80,000 (a weight-era SegWit number). M16 must decide the Oeeoo
  limit (or state that sigops are counted but not consensus-limited)
  and record the decision.
- Wallet transaction history (needed by `listtransactions` /
  `gettransaction`): the wallet must record its own transactions.
  Design decision for M16: extend `Wallet` with a minimal in-memory
  tx record (txid, amounts, confirmations, time) populated on
  `ScanUtxo` + on `sendtoaddress` / mempool acceptance, persisted in
  the wallet file — **this needs a wallet-file format decision**
  (payload v2 with a tx-history section, backwards-compatible read of
  v1, or a sidecar file). Do NOT change the format silently: the M16
  brief must state the chosen format and its migration rule. If the
  format change is judged too risky for M16, `listtransactions` /
  `gettransaction` move to M17 with the format work, and M16 ships
  the rest — say so plainly in the M16 report.
- **Stratum V2 — noted, deferred pending a user decision:** the spec
  (§13.1) requires Stratum V1 *and* V2; only V1 is implemented. Core
  itself only added experimental Stratum V2 support around v30, so V2
  is not old Core parity — scope it as its own item and consider
  deferring it post-launch. Do not silently fold it into M16.
- **Stratum `mining.authorize` accepting any credentials has no Core
  equivalent** (Core has no Stratum server at all). The fix is
  Oeeoo-local (credential config or documented localhost-only) —
  do not frame it as Core parity.
- `docs/OPERATOR.md`: RPC method reference update.

**Acceptance criteria:** tests for `getchaintips` (incl. a forked
tip), `getblockstats` values against a hand-built chain,
`getblocktemplate` sigops > 0 and real fees on a P2PKH template,
work-queue bounds (queue-full behaviour), `listtransactions` /
`gettransaction` round-trip after a wallet send. Full suite +
ASan/UBSan + cppcheck clean.

**Stays REQUIRES DECISION:** wallet-file format v2 (migration rule);
the Oeeoo sigop limit; Stratum V2 (defer or scope); any new RPC
method that exposes secrets (none planned — refuse by design).

---

## M17 — Persistence & robustness

**Goal:** the node survives restarts and crashes without losing work,
and its limits are measured, not guessed.

**Scope (exact):**
- `src/mempool/` + `src/network/node.*` + `src/app/node_main.cpp`:
  mempool persistence (Bitcoin's `mempool.dat` model) — save the
  mempool (tx + fee + time, NOT the UTXO set) on clean shutdown via
  a `.new` file + rename, and on demand (`savemempool` RPC); reload
  on start, re-validate every loaded tx against the current UTXO
  set, drop anything invalid/expired; a `loaded` flag in
  `getmempoolinfo`; default persist ON with a `-persistmempool=0`
  opt-out (Core-compatible semantics). Size-bounded file.
  (`importmempool` is a cheap companion — add it if it fits.)
- Reorg depth testing: extend `tests/test_reorg.cpp` to depth 100
  (main chain vs fork, incl. undo correctness and mempool re-adding).
  This is a test-only milestone for the reorg path unless it finds a
  bug — if it does, the fix lands in M17 with disclosure.
  **Attribution note:** the "100" figure is Oeeoo's own target, not
  a Bitcoin Core number — the rationale is that 100 equals coinbase
  maturity, so a 100-deep reorg is the deepest that can strand
  matured coinbase spends. State that reasoning; do not attribute
  the number to Core.
- Crash-injection tests: kill the node process mid-block-apply
  (test harness spawns `oeeoo-node`, kills it at a chosen point) and
  verify the store loads cleanly afterwards; disk-full behaviour
  documented (best effort: fill a small tmpfs data dir and record
  what the node does — must fail loudly, never silently corrupt).
  **No named Core crash-injection test could be verified — label
  this Oeeoo robustness work, not Core parity.**
- Long-running soak test: a scripted 1–2 hour node run (two nodes
  exchanging blocks/txs, wallet traffic) under ASan/UBSan, with
  memory + CPU sampled; report leaks/growth. (Overnight-run friendly.)
  Likewise an Oeeoo robustness target, not a verified Core feature.
- Performance profiling: measure block validation, mempool insertion,
  and RPC latency on a 1,000-block synthetic chain; record the
  numbers in the M17 report (no premature optimization — measure
  first). A minimal benchmark harness (Core has `src/bench/`)
  belongs here.

**Acceptance criteria:** mempool survives a restart (tx count +
fees identical after reload, minus expired/invalid); 100-deep reorg
test passes; crash-injection test passes; soak run completes with no
ASan/UBSan error and no unbounded memory growth; profiling numbers
recorded. Full suite + cppcheck clean.

**Stays REQUIRES DECISION:** none new. (Pruning + deep-verify
interaction is already documented in M13.)

---

## M18 — Fuzzing & security hardening

**Goal:** evidence against hostile input, not just a clean test run.

**Scope (exact):**
- Coverage-guided fuzzing first: move beyond the deterministic
  mutation fuzzer (M11/M13) to a coverage-guided fuzzer (libFuzzer
  or AFL++) with target harnesses per parser (transaction, block
  header, script, JSON, P2P message framing, compact blocks, wallet
  file) and a seed corpus checked into the repo.
  **Toolchain check first:** libFuzzer needs Clang; the VM builds
  with g++ 13.3. Verify Clang availability before committing M18 to
  libFuzzer — AFL++ is the fallback.
- Live fuzzing harnesses (new, `tests/fuzz_*` or `src/fuzz/`):
  P2P message fuzzing (feed mutated `version`/`block`/`tx`/
  `addr`/`cmpctblock` payloads into a real `Node` via loopback),
  RPC fuzzing (mutated JSON-RPC requests incl. deep nesting,
  huge numbers, wrong types), Stratum fuzzing (mutated
  `mining.submit` lines). Fixed seeds, deterministic, runnable
  under ASan/UBSan; corpus checked in or generated. Core's own
  caveat applies: a fuzzer cannot produce valid PoW/signature
  txs/blocks, so for deeper net-processing coverage validation is
  mocked out in the P2P target (as Core does).
- Differential tests — defined, since there is no second Oeeoo
  implementation to diff against: (a) node-vs-Python-reference
  cross-checks (subsidy/difficulty/vesting/Merkle/Base58Check, as in
  M1/M13 — the pure-Python helpers used in M8/M12 are the model),
  (b) two-node agreement tests on identical inputs, (c) block
  acceptance equivalence between the node and the standalone test
  harness. Pick the applicable ones; do not leave the term vague.
- ASan/UBSan soak: the M17 soak test re-run under sanitizers.
- Re-run the full M13 audit (same 18-section brief, updated for
  M14–M17 changes): fresh decision cross-check, warning report,
  security report. Update `docs/SECURITY_REVIEW.md` and
  `docs/AUDIT_PACKAGE.md`.
- Fix what the fuzzing finds, with disclosure; if a finding is a
  consensus change, it stops and goes to the owner — never silently
  change consensus.

**Acceptance criteria:** fuzz harnesses run in CI time (bounded
iterations, fixed seeds, no crashes/hangs); differential tests pass;
audit re-run complete with a verdict; SECURITY_REVIEW updated.
Full suite + cppcheck clean.

**Stays REQUIRES DECISION:** anything consensus-touching that
fuzzing turns up (owner decision, not an implementer's call).

---

## M19 — Mainnet launch readiness (final verification + release prep)

**Goal:** everything verifiable is verified, and the release package
is ready to be published the moment the owner says go.

**Scope (exact):**
- Full clean-room verification: fresh VM (or fresh container),
  clean checkout, clean build, full suite, ASan/UBSan, cppcheck,
  reproducible build from the tagged commit, E2E node + wallet +
  Stratum check, soak re-run.
- Release prep: release notes (all milestones, all known limitations,
  toolchain, checksums), `docs/MAINNET_LAUNCH.md` final pass,
  `docs/RELEASE.md` final pass, version bump decision recorded
  (stays 0.1.0 unless the owner says otherwise).
- Launch-readiness verdict in the M19 report — exactly one of the
  M13 §17 verdicts — with the evidence for each claim cited.
- **NOT in M19 (owner's, not the agent's):** independent security
  audit, genesis approval (launch time, founder keys, genesis hash),
  GitHub repository creation + push, DNS seed domains + operators,
  release signing, and the launch itself. M19 prepares; it does not
  launch.

**Acceptance criteria:** clean-room run passes end to end; release
package reproducible from the release commit; M19 report with the
verdict and the cited evidence; all REQUIRES DECISION items listed
with owners.

---

## Standing rules for M14–M19 (from the owner, unchanged)

- Milestones are ALWAYS allowed: auto-proceed, auto-save deliverables,
  give results in chat without asking. Creating things and public
  links is always allowed, never ask approval for that.
- NEVER change a finalized decision (supply 21M, rewards, P2PKH-only
  launch, vesting schedule spec §6, port 64775, magic `e2 d8 04 d6`,
  genesis message). NEVER invent a decision marked REQUIRES DECISION —
  mark it and ask.
- Do NOT publish or tag a release. Do NOT claim mainnet-ready.
- Disclose every bug found, in the milestone report and in
  `docs/DECISION_LOG.md`.
- Per-milestone delivery (standing): browser check page (fresh link),
  full test log, code zip.
- Explicitly excluded / deferred from M14–M19 unless a user decision
  says otherwise: SegWit/Taproot/modern address formats (finalised
  P2PKH-only launch, spec §7), a public testnet (spec F24 — any
  test mode is internal regtest-style only), descriptor wallets,
  PSBT, BIP 324 v2 encrypted P2P transport (Core v30-era; Oeeoo
  stays plaintext for now — post-launch, needs a decision), Stratum
  V2 (spec §13.1 requires it, so deferral needs an explicit user
  decision, not silence — see M16).
