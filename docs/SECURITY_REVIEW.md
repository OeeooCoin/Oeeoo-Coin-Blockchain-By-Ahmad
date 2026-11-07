# Oeeoo Coin — Security Review (self-review, Milestone 11)

**This is NOT an independent audit.** It is the project's own
threat model, invariants, and known gaps, written so an external
auditor can start from a true picture instead of discovering it.
Nothing here should be read as "the code is safe". The
independent audit (Milestone 12 / launch prep) has now happened
once (report OOC-SA-001, 2026-11-07; its findings are fixed in
Milestone 20) — but the auditor has not confirmed the fixes, and
a second cryptographic/consensus audit is still required before
any mainnet.

## Threat model

Adversaries, in order of exposure:

1. **Remote peers** — any internet host can connect to port
   64775 and send arbitrary P2P messages. They can lie about
   blocks, transactions, headers, addresses, and versions. The
   node must never trust a peer: every block and transaction is
   validated from scratch before it touches the chain or the
   UTXO set, and misbehavior is scored and banned (100 points =
   ban, doubling bans to 30 days).
2. **Miners** — a miner can build any block that passes
   consensus. The consensus rules (subsidy schedule, coinbase
   validation, founder vesting, difficulty, MTP, 2h-future) are
   the only thing standing between a miner and new coins. They
   are tested with exact vectors (see the test suite), but they
   have not been reviewed by an independent cryptographer.
3. **RPC clients** — the RPC server listens on 127.0.0.1 only
   (port 64776) and requires cookie auth. A local process with
   the cookie can spend the wallet. Never expose the RPC port,
   never share the cookie file.
4. **Stratum miners** — the Stratum server listens on
   127.0.0.1 only (port 64777). A miner who can connect can
   submit shares; a share that meets network difficulty becomes
   a real block paying the configured miner address.
5. **Wallet holder / operator** — the wallet file holds the
   mnemonic (encrypted with PBKDF2 200k + AES-256-GCM). Anyone
   with the file AND the passphrase owns the funds. The mnemonic
   must never enter chat, code, logs, or the cloud VM's shell
   history.
6. **Disk / crash** — the chain store is SQLite (WAL +
   synchronous=FULL). Every block application is one atomic
   transaction (block + UTXO changes + undo record + tip). A
   crash mid-transaction leaves the last good state. The undo
   table makes reorgs atomic too.

## Invariants the code must never break

- **Money supply**: exactly 20,000,000 mined + 1,000,000
  founder = 21,000,000 total. Integer arithmetic only
  (`COIN = 100,000,000`); no floating point for money. The
  subsidy schedule is verified exactly in the tests.
- **Coinbase**: at most subsidy + fees (under-claim allowed,
  over-claim rejected); height push matches the block height;
  founder vesting enforced (vault floor rule; fees count
  against vesting).
- **No double spend**: a transaction's inputs must exist in the
  UTXO set at validation time; a block that spends the same
  coin twice is rejected; a reorg restores spent coins exactly.
- **Chain selection**: greatest cumulative valid proof of work
  only. Invalid blocks never become the tip, however much work
  they claim.
- **Difficulty**: retarget every 2,016 blocks, clamped 4x;
  bits must equal the expected value for the height.
- **Time**: Median-Time-Past for finality and locktimes; no
  block more than 2 hours in the future.
- **Keys**: secp256k1/ECDSA via OpenSSL; BIP39/BIP32/BIP44;
  the wallet never reuses a derivation path for two different
  purposes; gap limit 20.

## Attack surfaces (where to look first)

- `src/network/` — message framing, header parsing, inventory,
  compact blocks, peer scoring, address handling. Fuzzed at the
  parser level (Milestone 11) and now coverage-guided
  (Milestone 18: 7 libFuzzer harnesses, 34-seed corpus) plus
  live P2P fuzzing of the peer state machine (mutated
  version/block/tx/addr/cmpctblock, raw byte streams,
  framing attacks) — which found and fixed the OnBytes
  use-after-free (M18B-R5).
- `src/transactions/`, `src/script/` — transaction validation,
  script interpreter (resource limits enforced: op count,
  stack size, push size). OP_RETURN is rejected at launch.
- `src/mining/` — block templates, coinbase construction,
  nonce search (extraNonce rollover).
- `src/mempool/` — fee-rate ordering, 300 MB cap, 14-day
  expiry, RBF (opt-in). No orphan transaction pool (a tx with
  missing parents is rejected, not stored).
- `src/rpc/` — JSON parser (strict, fuzzed), HTTP handling
  (4 MB body cap, 64 KB header cap, 64-level JSON nesting cap),
  cookie auth (constant-time compare), worker pool (16 threads)
  + bounded work queue (64 deep; queue-full → HTTP 500),
  39 callable methods + `help` (incl. `getchaintips`,
  `getblockstats`, `getchaintxstats`, `listtransactions`,
  `gettransaction`, `walletpassphrasechange`, `addnode`).
  A malformed `Content-Length` header could crash the whole
  node before auth (Milestone 18, M18B-R1) — now a strict
  digits-only parse with a 400 response.
- `src/rpc/stratum.*` — line-delimited JSON (64 KB line cap),
  job building, share validation, vardiff.
- `src/db/` — SQLite store (schema v2, undo table), PRAGMA
  journal_mode=WAL + synchronous=FULL.
- `src/wallet/` — BIP39/BIP32/BIP44, encrypted wallet file,
  coin selection (smallest UTXOs first), fee estimation.
- `src/mempool/feeestimator.*` — median fee rate over the last
  24 blocks (exact integer arithmetic, minimum 1 sat/vB).

## Known gaps (honest list)

- **Independent audit report OOC-SA-001 received (2026-11-07).**
  The first external audit found three findings — OOC-001
  CRITICAL (multiple coinbase), OOC-002 HIGH (submitblock
  success despite persistence failure), OOC-003 MEDIUM (wallet
  file permissions) — all FIXED in Milestone 20 with regression
  tests and a live two-node demonstration (§F.3 of
  `docs/MILESTONE20.md`). What is still missing: the auditor's
  confirmation that the fixes close the findings, and the
  separate external cryptographic/consensus audit the report's
  §7 says remains necessary. This document is still self-review;
  OOC-SA-001 was a source-level review (no live-network attack,
  no mainnet deployment).
- **Reorg across pre-v2 blocks fails closed** (no undo
  records for blocks stored under schema v1). It refuses
  rather than corrupting state, but a node with an old store
  cannot follow a deep reorg until it re-indexes.
- **Reorg undo is tested to depth 100** (Milestone 17: main
  chain 100 vs fork 101, undo correctness + mempool re-adding
  verified at the coinbase-maturity bound). Reorg across
  pre-v2 blocks still fails closed (see above).
- **Orphan transaction pool is now bounded** (Milestone 15:
  `src/mempool/orphan.*`, Core's `TxOrphanage` model — max 100
  orphans / 5 MB, 20-minute expiry, per-peer purge on disconnect,
  never relayed). A transaction whose parents are unknown goes to
  the pool instead of being rejected outright. Caveat: promotion
  needs a *confirmed* parent — validation rejects unconfirmed
  inputs, so the `"tx"`-handler promotion path cannot fire in
  practice; block arrival is the real trigger.
- **Peer cap (125) is now enforced** (Milestone 13: `Node::SetMaxPeers`,
  outbound + inbound). Previously unenforced — closed.
- **Dust is now dynamic and script-aware in the mempool and wallet**
  (Milestone 13: 182 sats P2PKH / 304 sats 2-of-3 multisig at
  1 sat/vB; the hardcoded 546-sat wallet policy was removed). It is
  still a mempool/wallet policy, not a consensus rule: a miner could
  include a dusty output in a block directly, and the block would be
  accepted. Dust in consensus validation remains an open decision.
- **RPC/Stratum are local-only, no TLS.** Exposing them needs
  a reverse proxy with TLS and is out of scope for the node.
  There is no per-client RPC rate limit (Core has none either);
  the bound is the worker pool (16 threads) + work queue
  (64 deep, queue-full → HTTP 500).
- **`getchaintips` / `getblockstats` / `getchaintxstats` now
  exist** (Milestone 16). `getblockstats` replays a block's
  ancestry from stored blocks to price fees — a coin whose
  origin is not in a stored block cannot be priced; pruned
  history cannot be re-validated (by design — bodies are
  required).
- **Fuzzing now covers parsers AND the live peer/RPC/Stratum
  state machines** (Milestone 18): 7 coverage-guided libFuzzer
  harnesses (tx, block, script, JSON, P2P framing, compact
  blocks, wallet file; 34-seed corpus, fixed seeds, CI-time)
  + live fuzzing harnesses (mutated P2P messages / raw byte
  streams / JSON-RPC / Stratum `mining.submit` into real
  nodes, deterministic, ASan/UBSan). Five bugs were found and
  fixed by the fuzzing (M18B-R1–R5, all non-consensus):
  unauthenticated RPC crash via `Content-Length` (R1),
  wallet-loader heap overflow (R2), unbounded Stratum session
  buffer (R3), `IntFromString` unguarded `stod` (R4, latent),
  `OnBytes` use-after-free after a peer ban (R5). Differential
  tests cross-check the node against an independent Python
  reference (subsidy, difficulty, vesting, Merkle, Base58Check)
  plus two-node agreement and block-acceptance equivalence.
- **Deep verification is now the node default** (Milestone 13).
  A light load trusts the stored UTXO table (it checks only for
  duplicate coins and value ranges); a tampered UTXO is caught
  only by the deep replay. `--no-deep-verify` opts out — an
  operator who uses it accepts that trust model.
- **Test and mainnet now have distinct identities (Milestone
  20).** Testnet magic `d9 73 08 c2`, default ports
  64779/64780/64781, user agent `"/OeeooCoin:0.1.0(testnet)/"`;
  mainnet identity unchanged (magic `e2 d8 04 d6`, ports
  64775/64776/64777). Selected with `--testnet` / `--network
  testnet` / `network=testnet`; default is mainnet. A node on
  one network can never handshake with a node on the other (the
  magic check drops cross-network bytes). This closes the M13
  gap (open REQUIRES DECISION B2, user-approved 2026-11-07).
- **Inbound peer IPs are now recorded** (Milestone 15:
  `TransportMessage.remoteAddress`, `accept()` records the real
  IP), and bans are now keyed by **IP alone** (Milestone 16:
  `BanManager::BanKey` strips the ephemeral TCP port), so bans
  on inbound peers survive reconnects from any port. Ban keys
  are not persisted across restarts (no load path is wired into
  the node); if one is ever added it must pass stored keys
  through `BanManager::BanKey`.
- **AddrMan now has anti-fill defences** (Milestone 15): per-/16
  netgroup cap 64, 30-day entry expiry, failure counting with
  removal at 3. **AS-map is NOT implemented** — /16 bucketing is
  the accepted permanent defence for v0.1.0 (Milestone 16
  decision); a single /16 hosting many distinct peers is still
  one bucket.
- **Wallet history is a sidecar journal** (Milestone 16:
  `<wallet>.txjournal`, JSON-lines, atomic rewrite, no secrets
  inside, readable while locked). The wallet file format is
  untouched. Sends made before the journal existed are not
  recoverable; receives are rebuilt by rescan.
- **`getblocktemplate` now reports real per-tx fees (integer
  satoshis) and real sigops** (Milestone 16:
  `CountSigOps` — CHECKSIG = 1, bare multisig = pubkey count,
  20 fallback). `sigoplimit` 20000 is a template POLICY limit
  only, not consensus, and not Core's 80,000.
- **DNS seeds can no longer hang startup** (Milestone 15): a 10 s
  deadline on a worker thread; a hanging resolver is cut off.
- **Compact relay is now bounded** (Milestone 15): per-peer
  in-flight limit 5, getblocktxn depth limit 10, one retry then
  full-block fallback, stalling peers disconnected after 2 s.
- **A full node now evicts an inbound peer instead of refusing
  all new connections** (Milestone 15), with a documented ranking
  that protects the best peers.
- **RBF descendant handling is defense-in-depth** (unreachable in
  practice: validation rejects unconfirmed inputs) and the
  100-replacement cap is untested. (Milestone 13.)
- **Disk-full behaviour is verified** (Milestone 17:
  `RLIMIT_FSIZE` tests). Startup under a too-small limit
  refuses loudly (`disk I/O error`); a limit biting mid-chain
  rolls back to the last durable height (39 blocks rolled
  back in the test) — rollback, never corruption. The node
  now also warns to stderr as soon as the chain store fails
  (`chain store error — accepted blocks may not be durable`),
  once per distinct error. **Milestone 20 CLOSED the
  `submitblock` gap (OOC-002):** a failed persist is now
  REJECTED upfront (persist-before-commit in both acceptance
  paths) — `submitblock` returns an error and no in-memory
  state changes. The M17 tests re-run green under the new
  semantics (the disk-full observation now reads OBSERVED
  (M20)).
- **Mempool persistence is bounded and re-validated**
  (Milestone 17: `<datadir>/mempool.dat`, JSON-lines, caps
  100,000 entries / 64 MiB, atomic `.new` + rename save;
  every loaded tx is re-validated against the current UTXO
  set, expired/invalid entries are dropped).
- **Outbound dialing now works (Milestone 18 — the M17 MAJOR
  finding, FIXED):** dial loop in `Node::Poll` (AddrMan
  addresses, respects bans and the 125-peer cap), outbound cap
  10 enforced (finding A1 closed — DECISION_LOG F17's second
  half was NOT IMPLEMENTED in HEAD), `--addnode` CLI flag +
  config key, `addnode` RPC (add/remove/onetry), DNS-seed
  results actually dialed. Two stock nodes now peer via
  outbound dial (E2E-tested). The node can join the network.
- **Audit re-run (Milestone 18, `soak18/M18C-AUDIT-DRAFT.md`):**
  the full 18-section M13 brief re-run against M14–M17
  changes; verdict NOT READY (unchanged). One new
  decision-vs-code gap found (A1, 10 outbound — fixed in the
  same milestone); spec §20 item 14 (0.0001 fee figure) still
  has no reconciliation note (A3); fallback peer list still
  NOT IMPLEMENTED (A4); RBF 100-replacement cap still
  untested (A6); stale peer entries linger after a connection
  dies (A7 — peer-timeout follow-up); the leg-1 soak kill
  (~21:31 UTC, SIGTERM-like, not a crash, not a VM restart)
  remains unidentified (A8).

- **Milestone 19 (2026-11-07, final verification + release prep):**
  clean-room verification from a fresh checkout of `d0a73f3` — clean
  build 0 warnings, 205 sections ALL CHECKS PASSED, ctest 100%,
  ASan/UBSan clean, cppcheck 0 findings, E2E (106 blocks, 1.5 OEEOO
  send confirmed, restart survival, Stratum share → block 107),
  reproducible build (binaries identical to M18's); soak re-run on
  the M18 final ASan binary with real P2P dialing (~1 h 56 m, 440
  blocks, ~30 txs, 0 failures, 0 sanitizer errors) — **the
  soak-on-the-dialing-binary caveat is CLOSED**; no node bugs found;
  verdict NOT READY (unchanged). Full report `docs/MILESTONE19.md`;
  release notes `docs/RELEASE_NOTES.md`; local tag `v0.1.0`
  (local-only).

- **Milestone 20 (2026-11-07, audit remediation — OOC-SA-001):**
  the first independent audit's three findings are FIXED —
  **OOC-001** (multiple coinbase / unauthorized inflation:
  exactly one coinbase at `vtx[0]`, enforced at three layers —
  `CheckBasic`, `CheckBlockReward`, `ConnectBlockTransactions`
  — on every acceptance path, with a live two-node demonstration
  that a malicious multi-coinbase block is rejected and its
  sender banned); **OOC-002** (durable-acceptance semantics:
  persist-before-commit, `submitblock` returns an error on
  persistence failure); **OOC-003** (wallet files 0600 on
  creation, atomic save, and tx journal). Network identity
  separated (testnet magic `d9 73 08 c2`, ports 64779/64780/64781
  — mainnet unchanged). 230 sections, ALL CHECKS PASSED; ctest
  100%; ASan/UBSan clean; cppcheck 0 findings (102/102 files).
  Close-out found and fixed one bug in the first coordinator's
  code (node_main banner double-temporary UB: normal build
  printed the magic doubled, ASan build aborted at startup).
  Verdict NOT READY (unchanged — genesis unapproved; auditor
  confirmation + second crypto/consensus audit pending). Full
  report `docs/MILESTONE20.md`.

## Auditor checklist

1. Reproduce the build from a clean checkout
   (`docs/BUILD_GUIDE.md`) and run `ctest` — expect 100%.
2. Read `docs/MASTER_SPECIFICATION.md` (the spec, 26
   sections) and `docs/DECISION_LOG.md` (every engineering
   decision, marked as such when the spec left it open).
3. Run the deep verification on a synced store — the node does it
   by default at startup now (`config.deepVerify = true`); to test
   the light-load path explicitly, pass `--no-deep-verify` and
   compare.
4. Review the consensus-critical paths first: subsidy,
   coinbase validation, vesting, difficulty retarget,
   script interpreter, transaction validation, reorg/undo.
5. Review the trust boundaries: P2P input, RPC input,
   Stratum input, wallet file format, chain store.
6. Check the known gaps above — each is a place where the
   code is knowingly weaker than the spec's ambition.
