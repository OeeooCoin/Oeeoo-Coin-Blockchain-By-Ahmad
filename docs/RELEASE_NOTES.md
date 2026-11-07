# Oeeoo Coin v0.1.0 — Release Notes

**Status: FINAL — Milestone 19 (release prep). Not published, not
signed. Local tag `v0.1.0` exists (local-only — no remote, no push,
no publication). The launch-readiness verdict is NOT READY
(genesis unapproved + no independent audit); see
`docs/MILESTONE19.md` §H.**

Version: **0.1.0** (stays 0.1.0 unless the owner says otherwise — no
version bump in M19).

## What landed, milestone by milestone

- **M1 — Phase 1 foundation (2026-11-05):** consensus constants, integer
  money type, subsidy schedule (`GetBlockSubsidy`), exact 20,000,000
  OEEOO mining-issuance verification, SHA-256/SHA-256d, 80-byte block
  header with deterministic little-endian serialization.
- **M2 — Transactions & addresses (2026-11-05):** VarInt (CompactSize),
  transaction serialization + txid, Bitcoin-style Merkle tree, Base58Check,
  HASH160, P2PKH addresses (version byte `0x0f`, 34-char, `7` prefix).
- **M3 — Blocks & difficulty (2026-11-05):** 256-bit integer (no Boost),
  nBits/target, PoW check, retarget every 2,016 blocks (4x clamp), full
  block, chain index + cumulative work + reorg, Median-Time-Past and
  future-time rules, genesis params structure (not finalized).
- **M4 — Script & validation (2026-11-05):** ECDSA secp256k1, legacy
  SIGHASH_ALL, Script interpreter (P2PKH, bare multisig, CLTV, CSV,
  resource limits), in-memory UTXO set, transaction validation (finality,
  BIP68, coinbase maturity 100, fees, block connection with
  no-commit-on-failure). OP_RETURN stays rejected.
- **M5 — Mining & mempool (2026-11-05):** coinbase (BIP34 height, reward
  validation), mempool (1 sat/vB min relay fee, fee-rate order, 300 MB
  cap, 14-day expiry), block templates, nonce search with extraNonce
  rollover.
- **M6 — P2P wire protocol (2026-11-06):** wire framing + message codecs,
  SipHash-2-4, compact blocks, peer scoring + bans (100 = ban, 24 h
  doubling to 30 days), address manager, loopback + TCP transports, node
  with handshake, headers-first sync, tx/block relay, full block
  validation.
- **M7 — SQLite chain store (2026-11-06):** single-file SQLite store
  (WAL + synchronous=FULL, schema v1): blocks, UTXO, tip, peers, bans;
  atomic `ApplyBlock`; pruning; load-time integrity checks; node restart
  restores chain + UTXO. (Reorg undo was NOT in M7 — added in M11.)
- **M8 — Wallet library (2026-11-06):** BIP39 (official English wordlist,
  24 words default), BIP32 HD keys (Oeeoo xprv/xpub versions), BIP44
  `m/44'/65535'/…`, gap limit 20, balance scan, coin selection, build +
  sign payments, watch-only (xpub), encrypted wallet file (PBKDF2 200k +
  AES-256-GCM). Honest finding: a universal `75Pk…` prefix is impossible
  with one version byte — dropped by owner decision.
- **M9 — Founder vesting (2026-11-06):** 10 × 100,000 tranches (launch +
  9 calendar anniversaries, Feb 29 → Feb 28), genesis pays exactly
  1,000,000 to the founder 2-of-3 multisig vault, vault-floor
  founder-spend rule (reorg-safe), genesis coinbase exempt from
  100-block maturity.
- **M10 — RPC & Stratum (2026-11-06):** dependency-free JSON module,
  JSON-RPC 2.0 over HTTP (127.0.0.1:64776, cookie auth, 28 methods),
  Stratum v1 mining server (127.0.0.1:64777, subscribe/authorize/notify/
  submit, vardiff; a share meeting network difficulty becomes a real
  block).
- **M11 — Audit prep (2026-11-06):** reorg support (BlockUndo, schema v2,
  migration 1 → 2, adopt only on strictly greater cumulative work), full
  chain re-validation from genesis (`VerifyChainFromStore`), deterministic
  mutation fuzzing (~6,000 inputs, no crash), `docs/SECURITY_REVIEW.md` +
  `docs/AUDIT_PACKAGE.md`. The audit itself is still external and has
  not happened.
- **M12 — Mainnet prep (2026-11-06):** runnable binaries (`oeeoo-node`,
  `oeeoo-genesis`, `oeeoo-cli`), config file + CLI, graceful shutdown;
  genesis tool (founder 2-of-3 multisig, parallel midstate mining);
  `docs/MAINNET_LAUNCH.md` launch checklist. A fresh data directory
  refuses to start without a genesis file — the node never invents one.
- **Launch support (2026-11-06, post-M12):** DNS seed resolution
  (`src/network/dnsseed.*`, empty by default — no hostnames invented)
  and the reproducible-build script (`scripts/reproducible-build.sh`).
- **M13 — Audit gap closures (2026-11-06):** full self-audit (three
  independent inspections); dynamic script-aware dust (182 sats P2PKH /
  304 sats 2-of-3 multisig at 1 sat/vB); opt-in RBF (BIP125-style);
  mempool eviction + enforced 14-day expiry; fee estimator +
  `estimatesmartfee` (29 RPC methods); low-S signatures (BIP62); JSON
  64-level depth cap; thread-per-connection RPC (max 32); deep-verify
  default ON; genesis checkpoint + identity check; peer cap 125
  enforced; handshake hardening; compact PoW check + full-block
  fallback; AddrMan cap 1,000 + IP validation. Verdict: **NOT READY**.
- **M14 — Node wallet integration (2026-11-06):** `oeeoo-node` loads a
  wallet file LOCKED (`--wallet`); `createwallet` / `walletpassphrase` /
  `walletlock` RPC (32 methods); locked = watch-only (balance visible,
  no spend, no new address); passphrase NEVER a CLI flag / config value /
  logged; wrong passphrase → error -14. Fixed: JSON whitespace-after-`:`
  rejection; wallet had NO coinbase-maturity handling (immature coinbases
  now excluded from balance, reported as `immature_balance`). Verdict:
  **NOT READY**.
- **M15 — P2P hardening & completeness (2026-11-06):** inbound peer IPs
  recorded (bans survive reconnect); AddrMan defences (per-/16 netgroup
  cap 64, 30-day expiry, failure counting, removed at 3); DNS resolver
  10 s timeout; orphan pool (Core's `TxOrphanage` model: max 100 / 5 MB,
  20-minute expiry, never relayed); compact-relay limits (in-flight 5
  per peer, getblocktxn depth 10, one retry then full-block `getdata`,
  2 s block-stall disconnect); peer eviction at the 125-peer cap with a
  documented ranking. Verdict: **NOT READY**.
- **M16 — RPC & mining completeness (2026-11-06):** `getchaintips` /
  `getblockstats` / `getchaintxstats`; real template fees (integer sats)
  and real sigops in `getblocktemplate` (`sigoplimit` 20000 kept as a
  template POLICY limit only, not consensus); wallet-tx sidecar journal
  (`<wallet>.txjournal`, JSON-lines — payload v2 rejected, wallet file
  format unchanged); `listtransactions` / `gettransaction` /
  `walletpassphrasechange`; RPC load control (Core-style worker pool 16
  threads + 64-deep queue, queue-full → HTTP 500, NO per-client rate
  limiting); bans keyed by **IP alone** (M15 follow-up fix); /16
  bucketing accepted permanently for v0.1.0; Stratum V2 deferred past
  launch; multi-wallet deferred to the future. 36 callable methods +
  `help`. Verdict: **NOT READY**.
- **M17 — Persistence & robustness (2026-11-06):** mempool persistence
  (`mempool.dat`, Oeeoo JSON-lines format, caps 100,000 entries / 64 MiB,
  atomic save, re-validation on load; `savemempool` / `importmempool`
  RPC, 38 callable methods + `help`); reorg depth 100 proven (undo
  correctness + mempool re-adding at the coinbase-maturity bound);
  crash-injection tests (SIGKILL mid-burst and mid-shutdown recover);
  disk-full behaviour (startup refuses loudly; mid-chain failure rolls
  back, never corrupts — and is now loud in the log via a
  `node_main` stderr warning); profiling (`oeeoo_bench`: block validation
  ~1.3 ms, mempool insertion ~0.58 ms/tx, RPC latency ~1 ms); soak run 2
  accepted at 1 h 44 m (401 blocks, 27 txs, 0 failures — NOT a clean
  2 h run; both runs ended by VM restarts). MAJOR finding: the node
  could NOT open an outbound P2P connection (carried as M18 #1).
  Verdict: **NOT READY**.
- **M18 — Fuzzing & security hardening (2026-11-06):** outbound P2P
  dialing FIXED (dial loop `TryDialPeers`, `--addnode` flag + config key,
  `addnode` RPC add/remove/onetry, outbound cap 10 enforced — audit
  finding A1 closed; DNS-seed results are now actually dialed); 7
  coverage-guided libFuzzer harnesses (tx, block, script, JSON, P2P
  framing, compact blocks, wallet file; 34-seed corpus; 7/7 fuzz ctest
  passed) + live P2P/RPC/Stratum fuzzing + differential tests against an
  independent Python reference; 5 real bugs found and fixed, all
  non-consensus (R1 HIGH — unauthenticated remote crash via malformed
  HTTP `Content-Length`; R2 — wallet loader heap-buffer-overflow; R3 —
  Stratum unbounded session buffer; R4 latent — `IntFromString`
  unguarded `stod`; R5 — heap-use-after-free in `Node::OnBytes`); ASan
  soak re-run (~107 min, 374 blocks, 19 txs, 0 sanitizer errors — on
  the M17 binary + bridge, not the dialing binary); full M13 audit
  re-run, verdict **NOT READY**. 39 callable methods + `help`. Verdict:
  **NOT READY**.
- **M19 — Mainnet launch readiness (2026-11-07, DONE):** final
  verification (clean-room build from a fresh checkout of `d0a73f3` +
  full suite 205 sections + ASan/UBSan + cppcheck 0 findings +
  reproducible build + E2E node/wallet/Stratum + soak on the dialing
  binary) and release prep (these notes, the `MAINNET_LAUNCH.md` /
  `RELEASE.md` final passes, the decisions list, and the verdict
  evidence table). No code changes in M19. Verdict: **NOT READY**.
  Local tag `v0.1.0` (local-only).

## Finalized policy (unchanged — do not change without the governance process)

- Max supply **21,000,000 OEEOO** (20,000,000 mining + 1,000,000 founder);
  rewards 50 / 25 / 12.5 / 6.25 / 3.125, halving every 210,000 blocks,
  0 subsidy from block 940,001.
- PoW: SHA-256d, 10-minute target, retarget every 2,016 blocks (4x clamp),
  mainnet PoW limit `0x1d00ffff`, max block size 1,000,000 bytes.
- Port **64775**, magic `e2 d8 04 d6`, launch addresses legacy P2PKH only
  (version byte `0x0f`, 34-char, `7` prefix); no SegWit/Taproot/bech32.
- Founder vesting: 10 × 100,000, launch + 9 anniversaries (Feb 29 →
  Feb 28), 2-of-3 multisig vault.
- Direct mainnet / NO public testnet (spec F24); internal testing only.
- Genesis message: "THINK ABOUT GOD ? IS ONLY ONE ALLAH".

## Known limitations (carried from M13–M18 — nothing hidden)

1. **No independent security audit.** M13 and the M18 re-run are
   self-reviews by the same agent that wrote the code. Mainnet must
   not launch until an independent audit is complete
   (`docs/AUDIT_PACKAGE.md` + `docs/SECURITY_REVIEW.md`).
2. **No approved mainnet genesis.** No launch time, no approved founder
   public keys, no genesis hash, no hardcoded mainnet checkpoint. The
   checkpoint mechanism (`--genesis-checkpoint`) is built and tested;
   the value itself is an owner decision that does not exist yet.
3. **RBF 100-replacement cap implemented but NOT test-covered**
   (M13 §B "NOT VERIFIED", carried through M18 finding A6).
4. **RBF descendants unreachable in practice.** Validation rejects
   unconfirmed inputs, so the replacement code's descendant handling is
   defense-in-depth that cannot be exercised through `AddTransaction`
   (M13 §B/§D).
5. **Orphan promotion needs a confirmed parent.** Only block arrival
   promotes an orphan; the `"tx"`-handler promotion call site cannot
   fire in practice (M15 §C.2.5, left as is by user-delegated decision
   in M16). An Oeeoo/Core behavioural difference, not a spec violation.
6. **`submitblock` returns success while chain persistence fails.**
   Since M17 the failure is loud in the node log (stderr warning, once
   per distinct error) and the store rolls back instead of corrupting,
   but RPC callers still see success (M17 §A.4/§G, carried in M18).
7. **Disk-full behaviour verified for the SQLite store path only.**
   Other I/O failures (wallet save, journal save, mempool save) warn
   but are not crash-tested (M17 §G, M18 §D).
8. **Test suite is not safe to run in parallel.** It shares fixed `/tmp`
   paths (and some fixed ports) across concurrent runs; a parallel run
   during M18 produced 665 CHECK failures — all artifacts, serial
   re-run passes 100%. Run it serially (M17 §C.2.8, M18 §D).
9. **Stale peer entries linger after a connection dies.** Observed in
   the M18 soak (node A's peer count grew 1→5 while B stayed at 1). A
   peer-timeout is a hardening follow-up, not implemented (M18 §G/A7).
10. **Spec §20 item 14 — the historical 0.0001 OEEOO fee figure — has
    no explicit reconciliation note** against the dynamic-fee policy
    (M18 finding A3; a one-line note in DECISION_LOG item 9 would
    close it). It is NOT a fixed consensus fee.
11. **Spec §20 item 16 — hardcoded fallback peer list — NOT
    IMPLEMENTED** (M18 finding A4; documented in
    `src/network/addrman.h:9`). With no DNS seeds either, a fresh node
    with an empty AddrMan learns no peers unless dialed (the M18
    dialing fix addresses the dialing half, not the fallback list).
12. **No distinct testnet identity.** Test and mainnet nodes share the
    magic, port, protocol version, and user agent, so they peer with
    each other and reject each other's blocks only at consensus. The
    only guard is the testing-mode banner (M13 §D, carried through M18).
13. **No per-client RPC rate limiting.** RPC is a Core-style worker pool
    (16 threads) + bounded queue (64 deep, queue-full → HTTP 500).
    Mitigated by localhost-only binding (127.0.0.1:64776); do not expose
    the port (M13 §D, M16 §A.5).
14. **Stratum `mining.authorize` accepts any credentials.** Acceptable
    only because Stratum binds localhost (127.0.0.1:64777). Never expose
    the port; if Stratum ever listens on a non-loopback address,
    `mining.authorize` must gain real credential checking first
    (M13 §D, documented as localhost-only by design in M16).
15. **Ban keys are not persisted across restarts.** `ChainStore` has
    `PutBan`/`GetBans` but no load path is wired into the node. If a
    load path is ever added it must pass stored keys through
    `BanManager::BanKey` (IP-only), or old stored `"ip:port"` keys
    would not match (M16 §A.6/§D).
16. **`getblockstats` cannot price a coin whose origin is not in a
    stored block** (fees are recomputed by replaying the block's own
    ancestry from stored blocks; test-only UTXO injections cannot be
    priced). In production every coin originates in a block (M16 §A.1).
17. **Pre-journal sends are not recoverable.** A wallet file with no
    sidecar starts with an empty journal; history is rebuilt from
    `ScanUtxo` (receives) on next rescan — sends made before the
    journal existed cannot be reconstructed (M16 §A.3).
18. **Reorg across pre-v2 blocks fails closed.** Reorg undo exists only
    from schema v2 (M11); blocks stored under schema v1 cannot be
    undone (M15 §D, carried).
19. **Fee estimator returns the minimum with no data.** Before 24 blocks
    are recorded, `estimatesmartfee` returns 1 sat/vB — the safe
    direction (never overestimates), but it is not an estimate
    (M13 §D).
20. **Fuzzing covers parsers, not the live peer/RPC/Stratum state
    machines** (the live fuzzing harnesses in M18 cover the live
    paths with mutated inputs, but they are deterministic test
    harnesses, not coverage-guided state-machine fuzzing — M15 §D,
    M18 §A.2).
21. **ASan RSS creep is not a leak proof.** RSS rose ~0.6–0.9 MB/min
    per node under ASan in both soaks; accepted as ASan-quarantine
    behaviour. A non-ASan long run to distinguish a real leak has not
    been done (M17 §G, M18 §B.6).
22. **Soak evidence is partial by VM lifetime.** The M17 soak was
    accepted at 1 h 44 m (not a clean 2 h run — both attempts ended by
    VM restarts); the M18 soak ran in 3 legs (~107 min total) on the
    M17 binary + Python P2P bridge, NOT on the dialing binary. **The
    M19 soak closes that gap**: ~1 h 56 m on the M18 final ASan binary
    with real P2P dialing (440 blocks, ~30 txs, 0 failures, 0
    sanitizer errors) — also NOT a clean 2 h run (killed ~3 min before
    its deadline by a VM restart), accepted under the M17 honest rule
    (M17 §A.5, M18 §A.5/§G, M19 §A.2).
23. **AddrMan has no per-source limits.** Per-/16 netgroup cap (64),
    30-day expiry, and failure counting exist since M15, but there is
    still no per-source limit and AS-map is NOT implemented — /16
    bucketing is the accepted permanent defence for v0.1.0 (M13 §D,
    M15 §A.2, M16 §A.7).
24. **Compact relay simplifications (pre-existing, documented):**
    `getblocktemplate` sigops are now real (M16), but `getblocktxn`
    still uses absolute indexes and compact relay has the documented
    depth/in-flight limits (M13 §D).
25. **The M18 soak leg-1 kill is unidentified.** All four soak
    processes died within ~1 min (~21:31 UTC, SIGTERM-like, not a
    crash, no OOM evidence); the killer was not identified (M18
    finding A8).
26. **Fuzz corpus in the build dir accumulates libFuzzer-added inputs**
    across runs (213 files in `build-m18b/corpus/script` vs the 34
    checked-in seeds); a re-configure resets it to the 34 checked-in
    seeds (M18 §D).
27. **One pre-existing clang-only warning** (not from any milestone's
    scope): `tests/test_milestone14.cpp:400: unused variable
    'nodeMutex'` — seen only in the Clang build (M18 §C.3).
28. **This VM's DNS resolver answers EVERY hostname** (wildcard proxy).
    A seed hostname that "resolves" here may not resolve on a real
    network; the node only attempts connections, and peer scoring
    drops bad ones (M13 §D).

## Toolchain

- g++ **13.3**, CMake **3.28**, OpenSSL **3.0.13**, SQLite **3.45.1**,
  cppcheck **2.13.0** (all milestones).
- Clang **18.1.3** (fuzzers only, M18).
- Same-machine, two-directory reproducibility only; cross-environment
  reproducibility is NOT claimed.

## CHECKSUMS

Clean-room reproducible build from the `d0a73f3` checkout
(SOURCE_DATE_EPOCH=1794015825, `logs/milestone19-cleanroom-log.txt`):

- oeeoo-coin-0.1.0.tar.gz:
  `a6d7f12a1cc447ea0ba9339b0280c9eac1223fc4c1cd581ce66f683e3bf1c4a5`
- oeeoo-node:
  `5f6c08d7319850a0e3dd1d0c39d63c48ec9bd601b351617f5fbd69a268a315b3`
- oeeoo-genesis:
  `29345d45c528b5a8a39ce47891b546a3ab9b53d03770b87848d57e74377f5c19`
- oeeoo-cli:
  `6b7a71af340d018930e53498844f7cf0b67f48b3aabaaadad06395c505fac2bb`
- source commit: `d0a73f3` (M18 final; the code is unchanged since
  M18, so these binary hashes are identical to M18's).

The tagged-commit reproducible build (local tag `v0.1.0`) reproduces
these exact binaries; its tarball differs only via `SOURCE.txt`
naming the tagged commit — the tagged tarball hash is recorded in
`docs/RELEASE.md` (Milestone 19 verification).

## What is NOT in this release

- No mainnet genesis block, no launch, no GitHub push (the
  repo exists and is verified empty:
  https://github.com/OeeooCoin/Oeeoo-Coin-Blockchain-By-Ahmad —
  code push ONLY after ALL milestones (M19) complete, per the user's
  decision). Local tag `v0.1.0` exists (local-only; no remote, no
  push, no publication).
- No independent security audit.
- No DNS seed domains, no release signing, no Stratum V2, no
  multi-wallet, no modern address formats (SegWit/Taproot) — see
  `docs/MILESTONE19.md` §G.
