# Oeeoo Coin (OEEOO)

Independent decentralized Proof-of-Work blockchain, Bitcoin-style.
C++ implementation, MIT License. Planned repository:
`OeeooCoin-blockchain-by-Ahmad` (not yet created — see Decision Log).

**Status: Phase 1 + Milestones 1–19 DONE (205 test
sections, ASan/UBSan clean, cppcheck 0 findings, reproducible build
verified from a clean Git checkout, local tag `v0.1.0`). IMPLEMENTED +
COMPILED + UNIT-TESTED on this machine, with runnable binaries (oeeoo-node,
oeeoo-genesis, oeeoo-cli). NOT production-ready. NO mainnet. NO
genesis block yet. No independent security review or audit has been
done. Verdict after the Milestone 13 audit: NOT READY (Milestones 14–18
do not change it; the Milestone 18 audit re-run confirms it). Launch checklist: docs/MAINNET_LAUNCH.md.
Roadmap to Bitcoin-level (M14–M19): docs/ROADMAP_BITCOIN_LEVEL.md.**

## Finalized policy (do not change without community process)

- Max supply: **21,000,000 OEEOO** — 20,000,000 mining + 1,000,000 founder
- 1 OEEOO = 100,000,000 smallest units (`COIN`); integer arithmetic only
- Consensus: Proof of Work, SHA-256d, 10-minute target block time
- Reward schedule: 50 / 25 / 12.5 / 6.25 / 3.125 OEEOO per era of
  210,000 blocks, except the last era (840,001–940,000, 100,000 blocks);
  **0 subsidy from block 940,001**
- Max block size: 1 MB; difficulty retarget every 2,016 blocks
- Mainnet port: 64775; launch addresses: legacy P2PKH only
- Founder: 1,000,000 OEEOO, 10 × 100,000 tranches, yearly vesting,
  2-of-3 multisig wallet

## What Milestone 2 adds (2026-11-05)

- `src/serialize/` — VarInt (CompactSize) + little-endian rules
- `src/transactions/` — transaction serialization, txid, structural checks
- `src/blockchain/merkle.*` — Bitcoin-style Merkle root
- `src/crypto/` — Base58Check, HASH160 (RIPEMD-160+SHA-256), P2PKH addresses
- `docs/MILESTONE2.md` — vectors and scope

## What Milestone 3 adds (2026-11-05)

- `src/consensus/uint256.*` — 256-bit integer (no Boost needed)
- `src/consensus/difficulty.*` — nBits/target, PoW check, retarget, block proof
- `src/blockchain/block.*` — full block (header + txs), structural checks
- `src/blockchain/chain.*` — chain index, validation, cumulative work, reorg
- `src/blockchain/genesis.h` — genesis params structure (**not finalized**)
- `docs/MILESTONE3.md` — vectors and scope

## What Milestone 4 adds (2026-11-05)

- `src/crypto/ecdsa.*` — ECDSA secp256k1 (sign/verify, key generation)
- `src/transactions/sighash.*` — legacy SIGHASH_ALL
- `src/script/script.*` — Script interpreter (P2PKH, multisig, CLTV, CSV)
- `src/utxo/coins.*` — in-memory UTXO set
- `src/transactions/validation.*` — finality, BIP68, maturity, fees, block connection
- `docs/MILESTONE4.md` — rules and scope

## What Milestone 5 adds (2026-11-05)

- `src/mining/miner.*` — coinbase, block templates, nonce search, reward validation
- `src/mempool/mempool.*` — validated mempool, fee-rate order, expiry
- `src/script/script.*` — script numbers + BIP34 coinbase height
- `docs/MILESTONE5.md` — rules and scope

## What Milestone 6 adds (2026-11-06)

- `src/network/protocol.*` — wire framing + message codecs
- `src/network/siphash.*`, `src/network/compact.*` — SipHash-2-4, compact blocks
- `src/network/peer.*`, `src/network/addrman.*` — scoring/bans, addresses
- `src/network/transport.*`, `src/network/node.*` — loopback + TCP, the node
- `docs/MILESTONE6.md` — rules and scope

## What Milestone 7 adds (2026-11-06)

- `src/db/database.*`, `src/db/chainstore.*` — SQLite wrapper + chain store
- `src/blockchain/chain.*` — `AddIndexEntry` (load-time index rebuild)
- `src/network/node.*` — `AttachStore` / `LoadFromStore`
- `docs/MILESTONE7.md` — rules, schema decision, known gaps

## What Milestone 8 adds (2026-11-06)

- `src/wallet/bip39.*`, `src/wallet/bip39_words.h`, `src/wallet/bip39_english.txt` — BIP39 mnemonics (official wordlist)
- `src/wallet/bip32.*` — BIP32 HD keys (Oeeoo xprv/xpub versions)
- `src/wallet/wallet.*` — wallet: BIP44, gap limit 20, balance, payments
- `src/wallet/walletfile.*` — encrypted wallet file (PBKDF2 + AES-256-GCM)
- `docs/MILESTONE8.md` — rules, decisions, the honest "75Pk" finding

## What Milestone 9 adds (2026-11-06)

- `src/consensus/vesting.*` — vesting schedule, calendar math, founder-spend rule, genesis rule
- `src/mining/miner.*`, `src/transactions/validation.*`, `src/mempool/mempool.*`, `src/blockchain/chain.*`, `src/network/node.*` — vesting enforcement wired through
- `docs/MILESTONE9.md` — rules, decisions, known gaps

## What Milestone 10 adds (2026-11-06)

- `src/rpc/json.*` — minimal dependency-free JSON (value, parser, serializer)
- `src/rpc/rpcserver.*` — JSON-RPC 2.0 over HTTP (127.0.0.1:64776, cookie auth, 28 methods)
- `src/rpc/stratum.*` — Stratum v1 mining server (127.0.0.1:64777, subscribe/authorize/notify/submit, vardiff)
- `src/blockchain/merkle.*` — MerkleBranch for Stratum jobs; `src/blockchain/chain.*` — GetNextBits, GetBlockHashAtHeight, GetMedianTimePastAtHeight; `src/network/node.*` — GetBlock, GetPeers
- `docs/MILESTONE10.md` — method list, decisions, known gaps

## What Milestone 11 adds (2026-11-06)

- `src/transactions/validation.*` — BlockUndo, BuildBlockUndo, DisconnectBlockTransactions
- `src/db/chainstore.*` — schema v2 + undo table + DisconnectBlock + GetUndo (migration 1 → 2)
- `src/network/node.*` — reorg-aware AcceptBlock (fork-point disconnect, adopt only on more work)
- `src/blockchain/verify.*` — VerifyChainFromStore (full re-validation from genesis)
- `tests/test_reorg.cpp`, `tests/test_verify.cpp`, `tests/test_fuzz.cpp` — reorg, deep verification, mutation fuzzing
- `docs/MILESTONE11.md`, `docs/SECURITY_REVIEW.md`, `docs/AUDIT_PACKAGE.md` — audit prep docs

## What Milestone 12 adds (2026-11-06) — mainnet prep

- `src/app/node_main.cpp` — the runnable `oeeoo-node` binary (config file + CLI, store, P2P + RPC + Stratum, graceful shutdown)
- `src/app/genesis.*` + `src/app/genesis_main.cpp` — the `oeeoo-genesis` tool (founder 2-of-3 multisig, parallel midstate mining, genesis JSON file)
- `src/app/cli.*` + `src/app/cli_main.cpp` — the `oeeoo-cli` RPC client
- `src/app/config.*` — `oeeoo.conf` parser + CLI arguments
- `tests/test_config.cpp`, `tests/test_genesis.cpp`, `tests/test_cli.cpp`
- `docs/MILESTONE12.md`, `docs/MAINNET_LAUNCH.md` — the launch checklist (audit, repo, keys, seeds, release)

## Launch support (2026-11-06, after Milestone 12)

- `src/network/dnsseed.*` — DNS seed resolution (IPv4, max 100 per seed); `Node::QueryDnsSeeds` fills the AddrMan; config `dnsseed` / CLI `--dns-seed` (repeatable, empty by default — no hostnames invented)
- `scripts/reproducible-build.sh` — builds twice, verifies byte-identical binaries, creates the release tarball + checksums
- `docs/RELEASE.md` — reproducible builds and DNS seeds; `tests/test_dnsseed.cpp`

## What Milestone 13 adds (2026-11-06) — audit gap closures

- Dynamic script-aware dust, opt-in RBF, mempool eviction + expiry enforcement, fee estimator + `estimatesmartfee`, low-S signatures, JSON depth cap, thread-per-connection RPC (max 32), deep-verify default ON, genesis checkpoint + identity check, peer cap 125, handshake hardening, compact PoW check + full-block fallback, AddrMan cap + IP validation, transport close fixes, vesting clamp, exact template coinbase reserve
- `docs/MILESTONE13.md` (full audit report, verdict NOT READY), `docs/WALLET.md`, `docs/MINING.md`, `docs/THIRD_PARTY.md`, `docs/OPERATOR.md`
- `tests/test_milestone13.cpp` (21 sections)

## What Milestone 14 adds (2026-11-06, overnight) — node wallet integration

- `src/wallet/nodewallet.*` (NEW) — the node wallet: loads LOCKED, `walletpassphrase` / `walletlock` (Bitcoin model), atomic saves (tmp + rename), auto-relock on timeout; the passphrase is never a CLI flag or a config value
- `src/app/config.*` — `--wallet <path>` flag + `wallet` config key (relative paths resolve against the data directory)
- `src/rpc/rpcserver.*` — `createwallet` / `walletpassphrase` / `walletlock` (32 methods total); `getbalance` / `listunspent` / `getwalletinfo` work while locked (watch-only), `getnewaddress` / `sendtoaddress` refuse while locked; wrong passphrase → error -14
- `src/rpc/json.cpp` — whitespace fix: values after `:` / `,` now parse (real clients emit those spaces; the parser rejected them with "bad number")
- `src/wallet/wallet.*` — coinbase maturity in the wallet (M8-era gap found by the funded E2E): immature coinbases are excluded from balance/unspent and reported as `immature_balance` in `getwalletinfo`; the funded manual E2E (101 mined blocks, 10-OEEOO send between two wallet addresses) proves it
- `docs/ROADMAP_BITCOIN_LEVEL.md` (M14–M19), `docs/MILESTONE14.md`, `docs/WALLET.md` (node-wallet section rewritten)
- `tests/test_milestone14.cpp` (7 sections)

## What Milestone 15 adds (2026-11-06) — P2P hardening & completeness

- `src/network/transport.*` — `TransportMessage.remoteAddress`; `accept()` records the real peer IP (`"ip:port"` via `sockaddr_in` + `inet_ntop`); loopback sets `"loopback:" + address_`
- `src/network/node.*` — inbound peer IPs stored as `peer.address` with the ban check running *before* registration (bans survive reconnect); peer eviction at the 125-peer cap (documented ranking: protects lowest ping, recent tx/block senders, longest-connected; evicts largest /16 netgroup); compact-relay limits (in-flight 5 per peer, getblocktxn depth 10, one retry then full-block `getdata`, 2 s block-stall disconnect with no misbehavior points); orphan wiring (`ProcessOrphansFor`, per-peer purge on disconnect, 60 s expiry sweep)
- `src/mempool/orphan.h/.cpp` (NEW) — `OrphanPool` (Core's `TxOrphanage` model): max 100 orphans / 5 MB, 20-minute expiry, never relayed, prevout-indexed so a tx is validated when its parent arrives
- `src/network/addrman.*` — per-/16 netgroup cap 64, 30-day entry expiry, failure counting (removed at 3); AS-map documented as NOT implemented
- `src/network/dnsseed.*` — 10 s resolver timeout on a worker thread (hanging resolver no longer blocks startup)
- `docs/MILESTONE15.md` (full report, verdict NOT READY), `docs/DECISION_LOG.md`, `docs/SECURITY_REVIEW.md` (gaps closed)
- `tests/test_milestone15.cpp` (15 sections) + 3 AddrMan + 3 DNS sections in `tests/test_network.cpp` / `tests/test_dnsseed.cpp` — **155 sections total**
- Known caveats (disclosed in `docs/MILESTONE15.md` §C.2): orphan promotion needs a *confirmed* parent (mempool-parent arrival cannot promote); TCP bans are keyed by `"ip:port"` so a reconnect from a new ephemeral port gets a fresh key — both are open decisions (§G)

## What Milestone 16 adds (2026-11-06) — RPC & mining completeness

- `src/blockchain/chain.*` — `GetChainTips()` (every tip incl. stale branches, with branch length and status); `src/rpc/rpcserver.*` — `getchaintips` / `getblockstats` (fees by replaying the block's ancestry from stored blocks) / `getchaintxstats` (default window 2016); all three in the `help` list — **36 callable methods + `help`**
- `src/script/script.*` — `CountSigOps` (CHECKSIG = 1, bare multisig = pubkey count, 20 fallback; no P2SH rule); `src/mining/miner.*` — `BlockTemplate.txFees` (real per-tx satoshis); `getblocktemplate` now reports real `fee` (integer sats) and real `sigops` — the "sigops always 0" simplification is gone; `sigoplimit` 20000 kept as a template POLICY limit only (not consensus, not Core's 80,000)
- `src/wallet/txjournal.*` (NEW) — the wallet-tx sidecar journal: `<wallet>.txjournal`, JSON-lines, keyed by txid, full rewrite via tmp + fsync + rename; the wallet file format is unchanged (payload v1); `src/wallet/wallet.*` — `WalletTxRecord`, `ScanUtxo` records receives; `src/wallet/nodewallet.*` — sidecar load/save, `ChangePassphrase`
- `src/rpc/rpcserver.*` — `listtransactions` / `gettransaction` (history works while locked — the journal holds no secrets) and `walletpassphrasechange` (wrong old passphrase → error -14); passphrase never a CLI flag / config value / logged
- `src/rpc/rpcserver.*` — RPC load control: fixed worker pool (16 threads) + bounded work queue (64 deep, Core defaults); queue-full → HTTP 500; clean shutdown joins all workers; config keys `rpcthreads` / `rpcworkqueue` + CLI flags, wired in `src/app/node_main.cpp`; thread-per-connection (max 32) removed; **no per-client rate limiting** (Core has none either)
- `src/network/peer.*` — `BanManager::BanKey`: bans keyed by **IP alone** (M15 follow-up fix — the ephemeral TCP port no longer defeats bans); `src/network/addrman.h` — /16 bucketing accepted permanently for v0.1.0, AS-map deferred post-launch; orphan promotion left as is (block arrival only); Stratum `mining.authorize` documented as localhost-only by design (`docs/MINING.md`, `docs/OPERATOR.md`)
- `docs/MILESTONE16.md` (full report, verdict NOT READY), `docs/DECISION_LOG.md`, `docs/SECURITY_REVIEW.md`, `docs/OPERATOR.md`, `docs/MINING.md` updated
- `tests/test_milestone16a.cpp` (6 sections) + `tests/test_milestone16b.cpp` (6 sections) + `tests/test_milestone16c.cpp` (4 sections) — **171 sections total**
- Known caveats (disclosed in `docs/MILESTONE16.md` §C.2): `getblockstats` cannot price a coin whose origin is not in a stored block; sends made before the journal existed are not recoverable; ban keys are not persisted across restarts; the M15 real-TCP test is intermittently flaky in sandboxes (fixed-port bind collision), not a code regression

## What Milestone 17 adds (2026-11-06) — Persistence & robustness

- `src/mempool/mempooldump.*` (NEW) — mempool persistence (Bitcoin's `mempool.dat` model, Oeeoo's own JSON-lines format): `<datadir>/mempool.dat`, tx + fee + time only, caps 100,000 entries / 64 MiB, atomic `.new` + fsync + rename save; reload re-validates every tx against the current UTXO set and drops expired/invalid ones; `src/mempool/mempool.*` — `AddLoadedTransaction()` preserves the stored `addedTime` (no fresh 14 days)
- `src/network/node.*` + `src/app/node_main.cpp` — load after `LoadFromStore`, save on clean shutdown; `src/app/config.*` — `persistmempool` (default on) + `--persistmempool` / `--no-persistmempool` flags; `src/rpc/rpcserver.*` — `savemempool` (on-demand dump), `importmempool` (returns load counts), `loaded` flag in `getmempoolinfo` — **38 callable methods + `help`**
- `tests/test_reorg.cpp` — reorg depth **100** (main 100 vs fork 101, undo correctness + mempool re-adding + full store-vs-node UTXO equality verified; the "100" is Oeeoo's own target = coinbase maturity, not a Core number); `tests/test_milestone17b.cpp` (NEW) — SIGKILL mid-block-burst and mid-shutdown recovery, disk-full (`RLIMIT_FSIZE`) recovery (startup refuses loudly; mid-chain limit rolls back 39 blocks, never corrupts)
- `src/app/node_main.cpp` — **chain-store-error warning**: the main loop warns to stderr as soon as `LastStoreError()` is set (disk-full was silent at runtime before M17); `tests/test_wallet.cpp` — `fopen` NULL guard (a latent `fseek(NULL)` SIGSEGV found when parallel suite runs collided on a shared `/tmp` path)
- `src/bench/bench.cpp` (NEW, `oeeoo_bench`) — profiling on a 1,000-block synthetic chain: block validation ~1.3 ms avg (~770–800 blocks/s), mempool insertion ~0.58 ms/tx, RPC latency ~1 ms (`getblockcount` p50 ~0.65 ms) — measured, not estimated; full numbers in `logs/milestone17-profiling.txt`
- Soak: two ASan nodes, run 2 **1 h 44 m** (401 blocks, 27 wallet txs, 0 failures, no ASan/UBSan output) — accepted inside the 1–2 h window; both runs were ended by VM restarts (~every 2 h), not clean shutdowns; RSS rose ~159/128 MB (ASan quarantine, not claimed as a leak)
- `docs/MILESTONE17.md` (full report, verdict NOT READY), `docs/DECISION_LOG.md`, `docs/SECURITY_REVIEW.md`, `docs/OPERATOR.md` updated
- `tests/test_milestone17a.cpp` (7 sections) + `tests/test_milestone17b.cpp` (3 sections) + depth-100 reorg section — **182 sections total**
- Known caveats (disclosed in `docs/MILESTONE17.md` §C): **MAJOR — the node cannot open an outbound P2P connection** (no `--addnode`, no dial loop, `ConnectTo` is test-only; DNS seeds only fill AddrMan) — carried as the #1 decision for M18; `submitblock` still returns success while chain persistence fails (now loud in the log, silent to RPC callers); the test suite is not safe to run in parallel (shared fixed `/tmp` paths)

## What Milestone 18 adds (2026-11-06) — Fuzzing & security hardening

- `src/network/node.*` + `src/app/config.*` + `src/app/node_main.cpp` — **outbound P2P dialing (the M17 #1 finding, FIXED)**: `Node::TryDialPeers()` dial loop in `Poll()` (dials AddrMan addresses, respects bans and the 125-peer cap, never dials self or already-connected peers, drops a dead address after 3 failures); **outbound cap 10 enforced** (`maxOutboundPeers_`, finding A1 closed); `--addnode` CLI flag + config key (repeatable); `addnode` RPC (add/remove/onetry, Bitcoin-shaped) — **39 callable methods + `help`**; DNS-seed results are now actually dialed
- `src/fuzz/` (NEW) — **coverage-guided fuzzing with libFuzzer (Clang 18.1.3)**: 7 harnesses (tx, block, script, JSON, P2P framing, compact blocks, wallet file) + 34-seed corpus in `tests/fuzz/corpus/` (generated by `tests/fuzz/gen_corpus.cpp`); `CMakeLists.txt` — `OEEOO_BUILD_FUZZERS` option, 7 ctest entries (fixed seeds, bounded iterations); `tests/test_milestone18b.cpp` (NEW) — **live fuzzing harnesses** (mutated P2P messages / raw byte stream / JSON-RPC / Stratum `mining.submit` into real nodes, deterministic, ASan/UBSan)
- `tests/diff_reference.py` (NEW) + `tests/test_milestone18c.cpp` (NEW) — **differential tests**: node vs an independent pure-Python reference (subsidy, difficulty retarget, vesting incl. Feb 29, Merkle, Base58Check), two-node agreement on identical inputs, block-acceptance equivalence between the node and the standalone harness
- **Bugs found and fixed (disclosed in `docs/MILESTONE18.md` §C)**: R1 (HIGH) — unauthenticated remote crash via malformed HTTP `Content-Length` (`std::stoul` → `std::terminate`); R2 — wallet loader heap-buffer-overflow (length-field wrap, ~4 GB read); R3 — Stratum session buffer unbounded (dropped over 1 MB); R4 (latent) — `IntFromString` unguarded `stod` in its own catch; R5 — heap-use-after-free in `Node::OnBytes` (`Peer&` used after `Misbehave()` banned + erased it). All non-consensus — no consensus code changed
- Soak re-run under ASan/UBSan: 3 legs, ~107 min, **374 blocks, 19 wallet txs, 0 sanitizer errors** (ran on the M17 binary + bridge; dialing was not integrated when it started — a soak on the dialing binary is a possible M19 follow-up); audit re-run (full 18-section M13 brief, `soak18/M18C-AUDIT-DRAFT.md`) — **verdict NOT READY, unchanged**
- `docs/MILESTONE18.md` (full report), `docs/DECISION_LOG.md`, `docs/SECURITY_REVIEW.md`, `docs/AUDIT_PACKAGE.md`, `docs/OPERATOR.md` (dialing documented) updated
- `tests/test_milestone18a.cpp` (11 sections) + `tests/test_milestone18b.cpp` (6 sections) + `tests/test_milestone18c.cpp` (6 sections) — **205 sections total**
- Known caveats (disclosed in `docs/MILESTONE18.md` §C–D): RBF 100-replacement cap still untested; `submitblock` still returns success while chain persistence fails (loud in the log); the test suite is not safe to run in parallel; the M18 soak ran on the M17 binary + bridge, not the dialing binary; stale peer entries linger after a connection dies (peer-timeout follow-up)

## What Milestone 19 adds (2026-11-07) — Mainnet launch readiness (final verification + release prep)

- **No code changes** — M19 verifies and prepares. Clean-room verification (Stream A): fresh directory + fresh git checkout of `d0a73f3` + clean build (0 warnings) + full suite (**205 sections, ALL CHECKS PASSED**) + ctest 100% (208.63 s) + ASan/UBSan clean (0 sanitizer errors) + cppcheck 0 findings (98/98 files) + E2E (106 blocks mined, 1.5 OEEOO send confirmed in block 106, restart survival, Stratum share accepted → block 107) + reproducible build (binaries identical to M18's). Log: `logs/milestone19-cleanroom-log.txt` (1,058 lines)
- Soak re-run on the M18 final (dialing) binary (Stream B): **~1 h 56 m, 440 blocks, ~30 wallet txs, 0 block/tx failures, peers 2/2 at all 29 samples, 0 ASan/UBSan errors** — real P2P dialing via `--addnode`, no bridge. **Closes the M18 caveat** (soak on the dialing binary). NOT a clean 2 h run (killed ~3 min before its deadline by a VM restart) — accepted under the M17 honest rule. Log: `logs/milestone19-soak-log.txt`; driver + node logs in `soak19/`
- Release prep (Stream C + coordinator): `docs/RELEASE_NOTES.md` (M1–M19, finalized policy, 28 known limitations, toolchain, M19 checksums); `docs/MAINNET_LAUNCH.md` + `docs/RELEASE.md` final passes (M14–M17 verification entries backfilled); full REQUIRES DECISION list with owners (**21 OPEN + 16 CLOSED**, `docs/MILESTONE19.md` §G); verdict evidence table (§H)
- **Verdict: NOT READY** (unchanged — genesis unapproved, no independent audit). **Version stays 0.1.0.** Local git tag `v0.1.0` on the final M19 commit (local-only — no remote, nothing pushed, nothing published)
- `docs/MILESTONE19.md` (full report), `docs/RELEASE_NOTES.md`, `docs/DECISION_LOG.md`, `docs/SECURITY_REVIEW.md`, `docs/MAINNET_LAUNCH.md`, `docs/RELEASE.md`, README.md updated; verify page §29–30
- Environment honesty: the VM was replaced three times during Stream A and once during Stream B (killing the soak ~3 min before its deadline and Stream B's agent); interrupted runs were restarted, never claimed complete; nothing claimed beyond what was actually fresh (fresh directory/checkout/build dirs — NOT a fresh VM)

## What Phase 1 contains

- `src/consensus/` — consensus constants, integer money type, subsidy
  schedule (`GetBlockSubsidy`), total-issuance verification
- `src/crypto/` — SHA-256 / SHA-256d via OpenSSL EVP
- `src/blockchain/` — 80-byte block header, deterministic little-endian
  serialization, header hash
- `tests/` — automated tests: money arithmetic, every subsidy boundary,
  exact 20,000,000 OEEOO total issuance, SHA-256d test vectors,
  block-header serialization round-trip
- `docs/DECISION_LOG.md` — finalized decisions vs. unresolved items
  (SLIP-0044, genesis parameters, DNS seeds, script opcodes, …)

## Build and test (Linux)

```bash
sudo apt install build-essential cmake libssl-dev   # Debian/Ubuntu once
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: `100% tests passed, 0 tests failed`. See `docs/BUILD_GUIDE.md`
for Termux / Android notes.

## Verified Phase 1 calculations (not invented)

- Network magic candidate: SHA-256("Oeeoo Coin Mainnet Network Magic")
  = `e2d804d6…`, first 4 bytes `e2 d8 04 d6` (see `src/consensus/consensus.h`)
- P2PKH version byte `0x0f`: addresses are 34 chars and start with
  `7` (the second character varies with the key hash — a universal
  `75Pk…` prefix is mathematically impossible with one version
  byte, so the `75Pk` idea was dropped by owner decision in
  Milestone 8; the brute-forced `75Pk…` example in the Decision Log
  is a random payload, not a real funded address).

## What is NOT done yet

Genesis block (hash/nonce/timestamp not finalized), independent
security audit, GitHub repository, DNS seed domains, release signing,
and the mainnet launch itself. Milestones 1–13 (transactions/UTXO,
script, mempool, P2P, database, wallet, mining, RPC, founder
vesting, Stratum, audit prep, mainnet prep, M13 gap closures) are
DONE — see `docs/MILESTONE13.md`.
