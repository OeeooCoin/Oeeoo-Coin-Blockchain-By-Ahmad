# Oeeoo Coin — Milestone 16: RPC & Mining Completeness

Status: DONE 2026-11-06. All 155 prior sections still pass;
16 new sections added (171 total), ALL CHECKS PASSED, ctest 100%.
Build warning-free; cppcheck 0 findings; ASan/UBSan clean (see §F).
Built in three parallel streams (RPC chain methods + template;
wallet journal; load control + ban key) plus coordinator
integration (test wiring, node_main config wiring, docs).

The M13 verdict stands: **NOT READY** (genesis unapproved, no
independent audit). M16 closes the biggest *RPC* gaps (chain
introspection, real template fees/sigops, wallet history, load
control) and the M15 follow-up ban-key fix; it does not change
launch readiness.

## A. Summary

### 1. RPC chain methods (`src/blockchain/chain.*`, `src/rpc/rpcserver.cpp`)

- `ChainState::GetChainTips()` (NEW): returns every tip — a block
  that is no other block's parent — as `ChainTip {hash, height,
  branchlen, status}`. Verified first that the index retains
  stale-branch tips (it does: `vIndex` + `byHash_` hold every
  validated block). `branchlen` = tip height minus the fork-point
  height where its ancestry first meets the active chain (0 for the
  active tip); `status` = `"active"` (greatest-work tip) /
  `"valid-fork"` (all others). Sorted height-desc, hash-hex
  tiebreak. Documented in code: every index entry was fully
  validated, so no headers-only vs full-body distinction is
  tracked.
- `getchaintips` → array of {height, hash, branchlen, status}.
- `getblockstats` (hash param; error -5 unknown) → {height, hash,
  txs, size, totalfee (OEEOO double), avgfeerate (sats/vB double,
  fees / non-coinbase bytes, 0 when none), time, mediantime}.
  Fees are recomputed by **replaying the block's own ancestry
  from genesis through stored blocks** into a fresh UTXO view —
  the node's current view is the tip's view and cannot price older
  blocks; this is documented in code. Medi­antime is computed on
  the block's *own* ancestry, so side branches report their own
  value. No SegWit `sw*` fields (there is no SegWit). A missing
  input during replay is an internal -32603 (blocks were validated
  at accept time). Limitation, documented: fees are recomputed
  from stored blocks only — a coin whose origin is not in a block
  (e.g. test-only UTXO injection) cannot be priced; in production
  every coin originates in a block, so this is correct there.
- `getchaintxstats` (optional `nblocks`, default 2016 per Core) →
  {time, txcount (whole-chain total, Core's definition),
  window_block_count, window_final_block_height, window_tx_count,
  window_interval, txrate}; stored blocks only, no UTXO walk.
- All three methods are in the `help` list. The RPC surface is
  now **36 callable methods + `help`** (37 names listed by
  `help`; `getblock` shares a dispatch block with
  `getblockheader`, `getrawtransaction` with
  `decoderawtransaction`, `gettransaction` with
  `listtransactions`).

### 2. Real template sigops + fees (`src/script/script.*`, `src/mining/miner.*`, `src/rpc/rpcserver.cpp`)

- `CountSigOps(script)` (NEW, script.*): OP_CHECKSIG /
  OP_CHECKSIGVERIFY = 1 each; OP_CHECKMULTISIG / VERIFY = exact
  pubkey count when the *whole* script parses as a bare multisig
  (`OP_m <pushes> OP_n OP_CHECKMULTISIG[VERIFY]`, m≤n≤20), else
  20 (conservative, documented). Unparsable scripts fall back to
  a raw byte scan (same weights), never silently 0. No P2SH rule
  (no P2SH at launch).
- `CountSigOps(tx)`: sum over outputs' scriptPubKeys + inputs'
  scriptSigs (P2PKH scriptSigs are push-only → 0); coinbase
  inputs are skipped (coinbase scriptSig is miner data, not a
  script).
- `BlockTemplate` gains `txFees` (satoshis, parallel to
  non-coinbase `vtx[1..]`), recorded in `AssembleBlock` from each
  tx's validation fee — never recomputed in the RPC layer.
- `getblocktemplate`: `fee` is now the real integer satoshis
  from `txFees` (Bitcoin's unit here, documented — NOT OEEOO),
  `sigops` is the real `CountSigOps(tx)`. The documented
  "sigops always 0" simplification is gone.
- `sigoplimit: 20000` is kept, with a code comment recording the
  M16 decision: it is a **POLICY limit for templates only — NOT
  consensus, NOT Core's 80,000** (a weight-era SegWit number).
  No consensus sigop limit was added anywhere in
  `src/consensus/`.

### 3. Wallet transaction journal — sidecar file (`src/wallet/txjournal.*` NEW, `src/wallet/wallet.*`, `src/wallet/nodewallet.*`)

- **Format (decided, user-delegated): SIDECAR file.**
  `<wallet-path>.txjournal`, JSON-lines, one record per line,
  keyed by txid with latest-wins merge (never duplicated).
  Write discipline: **full rewrite via tmp file + fsync +
  rename on every save** (the same atomic pattern as the wallet
  file; no append path exists). Load is fail-closed on a
  malformed line; a missing sidecar loads as an empty journal,
  not an error. The wallet file format is UNCHANGED (payload v1,
  PBKDF2-HMAC-SHA512 200k + AES-256-GCM, verified).
- **Migration rule (documented in the header):** a wallet file
  with NO sidecar starts with an empty journal; history is
  rebuilt from `ScanUtxo` (receives) on next rescan — **sends
  made before the journal existed are NOT recoverable** (a UTXO
  scan cannot reconstruct them).
- Record: {txid, category ("send"/"receive"), amount (sats,
  signed per Bitcoin convention — negative for sends excluding
  fee), fee (sats, sends only), time (unix, block time when
  confirmed else time first seen), blockheight (-1 when
  unconfirmed), address, label ("")}. Confirmations are computed
  at query time from tip height, NOT stored.
- `Wallet` gains `WalletTxRecord` + `RecordTx()` (upsert by
  txid) / `ListTxRecords()` / `SetTxRecords()` + a dirty flag.
  `ScanUtxo` records a "receive" per funding txid (multi-output
  txs aggregated into one record; immature coinbases recorded
  too), with time = the funding block's time from
  `Coin.blockTime` — no chain lookup needed.
- `NodeWallet`: sidecar loads alongside the wallet file in
  `LoadFile`/`Unlock` (missing = empty, corrupt = fail load);
  `SaveIfDirty()` also persists the journal **even while
  locked** (the journal holds no secrets, and locked rescans
  discover receives that must survive restart); `Lock()`
  persists the journal first. The sidecar path derives from the
  wallet path — never a separate config value.
- **Scope (honest):** no mempool-acceptance hook in the Node
  (too invasive for M16) — receives appear once confirmed via
  rescan, sends appear immediately. After a reorg, a confirmed
  record's `blockhash` (looked up by height on the current main
  chain) may point at a different block; no
  abandon-transaction handling in v0.1.0. A journal save after a
  successful broadcast is best-effort (a journal I/O failure
  must not turn a sent tx into an error). `include_watchonly`
  is accepted and ignored (no watch-only distinction in the
  journal).
- `sendtoaddress` records a "send" after broadcast (amount
  negative excl. fee, fee separate, unconfirmed, time =
  `node_.Now()`).

### 4. `listtransactions` / `gettransaction` / `walletpassphrasechange` (`src/rpc/rpcserver.cpp`)

- `listtransactions` params: (account="*", count=10, skip=0,
  include_watchonly=false) — Bitcoin-shaped. Returns
  newest-first array of {account:"", address, category, amount
  (OEEOO double, Bitcoin convention), fee (OEEOO double, sends
  only), confirmations, blockhash ("" when unconfirmed),
  blockindex (-1), blocktime (0 when unconfirmed), txid, time,
  timereceived}. Confirmations computed from the node's tip
  height at call time.
- `gettransaction` param: txid (hex). Returns {amount, fee
  (sends), confirmations, blockhash, blockindex, blocktime,
  txid, time, timereceived, details:[...]} or error -5
  "Invalid or non-wallet transaction id" when unknown.
- **Decided and documented: history works while locked**
  (receives *and* sends — the journal holds no secrets, so
  locked = readable history, not spending).
- `walletpassphrasechange` params: [oldPassphrase,
  newPassphrase]. `NodeWallet::ChangePassphrase(old, new,
  error)`: requires unlocked, verifies the old passphrase by
  decrypting (wrong old → error **-14** "incorrect passphrase",
  never distinguished from a corrupt file, same as Unlock),
  re-encrypts atomically with the new passphrase, updates the
  in-memory passphrase. Refuses when no wallet is loaded (-13)
  and when locked (-13). Empty new passphrase → -8. No
  `walletfile.*` change was needed (reused `LoadWalletFile` +
  the existing atomic save). **Passphrase NEVER a CLI flag /
  config value / logged** — same rule as M14
  `walletpassphrase`; grep-verified (only a comment in
  config.h states the rule).

### 5. RPC load control (`src/rpc/rpcserver.*`, `src/app/config.*`, `src/app/node_main.cpp`)

- Replaced thread-per-connection (detached thread,
  `kMaxConnections = 32`, now removed) with a **fixed worker
  pool fed from a bounded queue, Core-style. No per-client rate
  limiting** — Core has none, and neither does Oeeoo.
- Defaults match Core: **16 worker threads, 64-deep queue**.
  New API: `SetMaxThreads(int)` (takes effect at next
  `Start()`), `SetWorkQueueSize(int)` (takes effect
  immediately), `MaxThreads()`, `WorkQueueSize()`,
  `WorkQueueDepth()`, `ActiveConnections()` (all setters clamp
  to ≥ 1).
- **Queue-full behaviour (exact):** `AcceptLoop` enqueues via
  `TryEnqueueConnection()`; when the queue is full it answers
  **`HTTP/1.1 500 Internal Server Error`** with body
  `{"jsonrpc":"2.0","id":null,"error":{"code":-32603,"message":"RPC work queue is full"}}`
  and closes the connection immediately; the server keeps
  running. The queue is bounded by a mutex + condition
  variable; it can never exceed `workQueueSize_`.
- Config: `rpcthreads` / `rpcworkqueue` as both config-file
  keys and `--rpcthreads` / `--rpcworkqueue` CLI flags (ranges
  1–1024 / 1–100000), plus `--help` text. `node_main.cpp`
  wires them into the server (two lines after the `RpcServer`
  construction, before `Start()`); until that wiring the node
  ran the compiled-in defaults (16/64) — the wiring is part of
  this milestone, not a gap.
- Preserved behaviour verified by the suite: cookie auth,
  POST-only, batches, notifications, 4 MB body cap, `stop`.
  Clean shutdown: `Stop()` closes the listen socket, joins the
  accept thread, closes still-queued sockets, `shutdown()`s any
  socket a worker is blocked on (tracked in `activeFds_`), then
  joins all workers. A worker that pops a fd after shutdown is
  set abandons it instead of handling it, so the shutdown sweep
  cannot miss a fd (this race is handled explicitly — see §C.1.2).
- `docs/OPERATOR.md` updated: the RPC resource-limits paragraph
  now describes the pool + queue (16/64, queue-full → HTTP
  500) instead of the old 32-connection cap; the method count
  is updated; the stale "wallet methods return -13 because no
  wallet is attached" line is corrected (a wallet IS attached
  when `--wallet` is configured, since M14).

### 6. IP-only ban key (`src/network/peer.*`, `src/network/node.*`) — M15 follow-up fix

- **Single choke point: `BanManager::BanKey(address)`**
  (static, in `peer.h`). `Ban` / `IsBanned` / `Unban` all
  normalize through it, so the rule cannot be bypassed by any
  caller. `node.cpp` call sites (`ConnectTo`, `Misbehave`,
  `OnBytes`) needed no logic change — only comments noting the
  normalization.
- **BanKey rules:** `"1.2.3.4:56789"` → `"1.2.3.4"`;
  `"loopback:<name>"` stays as-is (stable test identifiers, no
  port); an address with no port stays as-is; anything that is
  not a valid IPv4 `ip:port` (including IPv4-mapped forms like
  `::ffff:1.2.3.4:1234`, malformed strings, non-numeric /
  out-of-range ports) stays as-is — no new parsing, only the
  existing `ParseIPv4` check plus a numeric-port check.
  `Peer.address` keeps the full `"ip:port"` for
  display/diagnostics; only the ban key is IP-only.
- M15 test update (`tests/test_milestone15.cpp`, ban-key
  assertions only): the real-TCP ban test now reconnects from a
  **new ephemeral port** (fresh `Node` + fresh `TcpTransport`)
  and asserts the ban still holds (`PeerCount()==0`,
  `Bans().Size()==1`); loopback-test comment corrected to the
  new rule.
- Note (not a live bug): `ChainStore::PutBan` / `GetBans`
  persist ban keys as raw strings and no load path is wired
  into the node, so nothing re-bans from stored `"ip:port"`
  keys today. If a ban-persistence load path is ever added, it
  must pass stored keys through `BanManager::BanKey` (or
  re-ban via `Ban()`), or old stored keys would not match
  IP-only lookups.

### 7. AS-map + orphan promotion + Stratum authorize (decided, documented)

- **AS-map: accepted /16 bucketing permanently for v0.1.0**
  (one-line note in `src/network/addrman.h` next to the
  existing NOT-implemented comment); AS-map deferred
  post-launch.
- **Orphan promotion: left as is** (block arrival only). No
  code change; the defense-in-depth call site stays.
- **Stratum `mining.authorize` accepting any credentials:
  documented as localhost-only by design** (the Stratum server
  binds 127.0.0.1 only, port 64777). No auth change. Text
  added to `docs/MINING.md` and `docs/OPERATOR.md`: never
  expose port 64777 beyond localhost; if Stratum ever listens
  on a non-loopback address, `mining.authorize` must gain real
  credential checking first.

## B. Decision cross-check

- No finalized spec decision was changed (supply 21M, rewards,
  P2PKH-only launch, vesting schedule spec §6, port 64775, magic
  `e2 d8 04 d6`, direct mainnet / NO public testnet per spec F24).
- Decisions implemented in M16 (user-delegated 2026-11-06
  14:55 PKT, "jo behtar hain karo" — all 7 open decisions):
  1. `walletpassphrasechange` → implemented in M16 (§A.4).
  2. Wallet-tx journal → **sidecar file** (§A.3); payload v2
     rejected (migrating the encrypted wallet format is too
     risky).
  3. Multi-wallet → FUTURE, post-M19 / post-launch. Not in M16.
  4. Stratum V2 → DEFERRED past launch (spec §13.1 requires it
     eventually; V1 works; Core itself only added experimental
     V2 around v30). Not in M16.
  5. Inbound ban key → **IP alone** (§A.6).
  6. AS-map → accept /16 bucketing permanently for v0.1.0 (§A.7).
  7. Orphan promotion → LEAVE AS IS (§A.7).
  8. RPC load control → Core-style thread count + bounded work
     queue (queue-full → HTTP 500), NO per-client rate limiting
     (§A.5).
  9. Sigops counted for real in getblocktemplate; the existing
     20000 enforced as **RPC/template POLICY limit only — NOT
     consensus**, and NOT Core's 80,000 (§A.2).
  10. Stratum authorize-any documented as localhost-only by
      design (§A.7).
- Every numeric constant M16 introduces is marked in code as
  either Core's model value or Oeeoo's own choice:
  - Core's model: RPC pool 16 threads / 64 queue;
    `getchaintxstats` default window 2016 blocks.
  - Oeeoo's own choice: `sigoplimit` 20000 (kept, policy only);
    multisig sigop fallback 20 for non-bare-multisig scripts;
    journal write discipline (full rewrite via tmp + rename);
    `walletpassphrasechange` wrong-old-passphrase error -14.
  - Deliberate deviations, documented in code:
    `getblockstats` replays ancestry from stored blocks (§A.1);
    journal history works while locked (§A.4); journal save
    after broadcast is best-effort (§A.3).

## C. Warning report

### C.1 Bugs found and fixed (disclosed)

1. **REAL BUG — `gettransaction` dangling pointer (Stream B's
   new code, found by the test suite, FIXED by Stream B before
   delivery).** `src/rpc/rpcserver.cpp` did `for (const
   WalletTxRecord& rec : w->ListTxRecords()) { … found = &rec;
   … }`, but `Wallet::ListTxRecords()` returns
   `std::vector<WalletTxRecord>` **by value** — the temporary
   died at the end of the range-for, so `found` dangled and the
   later use (`recordToJson(*found)`) read freed memory. The
   RPC worker died with `std::length_error` in
   `Json::Json(const char*)` inside section `m16b:
   listtransactions/gettransaction RPC round-trip`, aborting the
   full suite in Stream A's build. Fixed by keeping the records
   in a local vector for the block's lifetime (the same pattern
   `listtransactions` already used). Never shipped in a broken
   state.
2. **WorkerLoop locking bug (Stream C's new code, caught in
   review before testing).** The first draft held the queue
   mutex across `HandleConnection`, which would have serialized
   all workers and blocked `Stop()`. Fixed by releasing the
   queue lock before handling, plus the shutdown-flag check
   (§A.5). The delivered code is the fixed version; the test
   suite (including the blocked-worker shutdown test) passes on
   it.
3. **Stream B edit-anchor mistake (test-side, reverted
   immediately).** One `muse.edit` anchor matched
   `sendrawtransaction`'s `BroadcastTx` before
   `sendtoaddress`'s; reverted in the next edit and re-applied
   with a unique anchor. No harm shipped.
4. **Environment: `libsqlite3-dev` and `cppcheck` were missing
   AGAIN after a VM replacement** (the same loss as M9/M15).
   The first clean build configured, then failed mid-build on
   `sqlite3.h: No such file or directory`, and the background
   build process was killed (SIGTERM) during the environment
   shift. Both packages were restored from the .debs already in
   the apt cache (`/var/cache/apt/archives`:
   libsqlite3-dev + libsqlite3-0 3.45.1-1ubuntu2.9,
   libtinyxml2-10, cppcheck 2.13.0-2ubuntu3, python3-pygments)
   via `sudo dpkg -i` in seconds — no download, no repo files
   touched. The clean build was then re-run from scratch and
   passed. **Lesson recorded:** on a fresh VM, check
   `ls /usr/include/sqlite3.h` and `which cppcheck` BEFORE the
   first build.

### C.2 Behavioural findings (not code bugs — disclosed)

5. **M15 real-TCP test flakiness is environmental, not a
   regression.** Stream A's pristine-HEAD baseline check
   (throwaway worktree of `3a6eed9`) showed 4 CHECKS FAILED in
   the M15 real-TCP test because `Listen(46479)` failed in that
   sandbox — the test binds a fixed port, so a leftover listener
   from an earlier run collides with it. The same test passed
   in every M16 run (normal suite twice, ASan suite, Stream C's
   standalone run), so it is intermittent here, not a code
   regression. No code change made.
6. **Concurrent-edit build breaks between streams (reported,
   not fought).** Stream C's first `build-c` link failed on
   Stream B's not-yet-complete test file, and Stream A's first
   `build-a` link failed before Stream B added `txjournal.cpp`
   to CMakeLists. Both resolved by re-configure + rebuild once
   the other stream landed; no code changes were needed. The
   coordinator's clean build is the authoritative run.

### C.3 Warnings (compiler / static analysis)

- None. Clean build at `-Wall -Wextra`: 0 warnings, 0 errors.
  cppcheck `--enable=warning,performance,portability`: 0 findings
  across all 83 files.

## D. Security report

Gaps closed by M16 (see also `docs/SECURITY_REVIEW.md`):

- **RPC load control is now bounded**: 16 worker threads + a
  64-deep queue; a flood of connections gets HTTP 500 instead
  of unbounded thread growth. Previously a thread-per-connection
  server capped only at 32 connections, with no queue bound.
- **Inbound bans now survive any reconnect**: keyed by IP
  alone, so a TCP peer reconnecting from a new ephemeral port
  is still refused. Previously the `"ip:port"` key gave a
  banned peer a fresh key on every reconnect.
- **Template fees and sigops are real**: `getblocktemplate`
  no longer reports 0 for both. Miners (and the node itself)
  now see the true fee and sigop cost of every template
  transaction.
- **Wallet history exists**: `listtransactions` /
  `gettransaction` answer from a sidecar journal that holds no
  secrets and is readable while locked; the wallet file format
  is untouched, so no migration risk to encrypted funds.
- **Passphrase rotation exists**: `walletpassphrasechange`
  re-encrypts atomically; a wrong old passphrase is
  indistinguishable from a corrupt file.

Gaps that remain (unchanged from M15 unless noted):

- No independent audit (external, still required before mainnet).
- No distinct testnet identity (test/mainnet share magic, port,
  version, userAgent — only the testing-mode banner guards).
- `Stratum mining.authorize` accepts any credentials
  (localhost-only mitigation, now documented in MINING.md +
  OPERATOR.md).
- RBF descendants unreachable in practice; 100-replacement cap
  untested.
- Disk-full behaviour not verified.
- Fuzzing covers parsers, not the live peer/RPC/Stratum state
  machines.
- Reorg undo tested to depth 2; reorg across pre-v2 blocks fails
  closed.
- AS-map NOT implemented — /16 bucketing is now the accepted
  permanent defence for v0.1.0 (§A.7).
- Orphan promotion via the `"tx"`-handler call site cannot fire
  in practice — block arrival is the real trigger (unchanged).
- `getblockstats` cannot price a coin whose origin is not in a
  stored block (§A.1); pre-journal sends are not recoverable
  (§A.3).
- Ban keys are not persisted across restarts (ChainStore has
  PutBan/GetBans but no load path is wired into the node);
  if a load path is ever added it must pass stored keys through
  `BanManager::BanKey` (§A.6).

## E. Files changed

Commit `d0c644b` on top of M15's `3a6eed9`:

`git diff --stat 3a6eed9..d0c644b`: 28 files, +3092/−37.

| File | Change |
|---|---|
| `src/blockchain/chain.h` / `chain.cpp` | `ChainTip` + `GetChainTips()` |
| `src/script/script.h` / `script.cpp` | `CountSigOps(script)` + `CountSigOps(tx)` |
| `src/mining/miner.h` / `miner.cpp` | `BlockTemplate.txFees` (per-tx satoshis) |
| `src/rpc/rpcserver.h` / `rpcserver.cpp` | getchaintips / getblockstats / getchaintxstats; real template fee + sigops; listtransactions / gettransaction / walletpassphrasechange; worker pool + bounded queue (HTTP 500 when full); help list updated |
| `src/rpc/rpcserver.h` | `SetMaxThreads` / `SetWorkQueueSize` / `MaxThreads` / `WorkQueueSize` / `WorkQueueDepth` / `ActiveConnections` |
| `src/wallet/txjournal.h` / `txjournal.cpp` | NEW — sidecar journal (`<wallet>.txjournal`, JSON-lines, tmp + rename) |
| `src/wallet/wallet.h` / `wallet.cpp` | `WalletTxRecord`, `RecordTx` / `ListTxRecords` / `SetTxRecords`; `ScanUtxo` records receives |
| `src/wallet/nodewallet.h` / `nodewallet.cpp` | Sidecar load/save alongside the wallet file; `ChangePassphrase` |
| `src/network/peer.h` / `peer.cpp` | `BanManager::BanKey` (IP-only normalization) |
| `src/network/node.h` / `node.cpp` | Ban-call-site comments (normalization is in `BanManager`) |
| `src/network/addrman.h` | AS-map /16-accepted note |
| `src/app/config.h` / `config.cpp` | `rpcthreads` / `rpcworkqueue` keys + CLI flags + help text |
| `src/app/node_main.cpp` | Wire `config.rpcThreads` / `config.rpcWorkQueueSize` into the server |
| `CMakeLists.txt` | `src/wallet/txjournal.cpp` in `oeeoo_core`; `tests/test_milestone16a/b/c.cpp` in the test sources |
| `tests/test_main.cpp` | `TestMilestone16A/B/C()` declared + called |
| `tests/test_milestone15.cpp` | Ban-key assertions updated to the IP-only rule |
| `tests/test_milestone16a.cpp` | NEW — 6 sections (chain methods + template) |
| `tests/test_milestone16b.cpp` | NEW — 6 sections (journal + wallet RPC) |
| `tests/test_milestone16c.cpp` | NEW — 4 sections (load control + ban key) |

## F. Test results

Toolchain: g++ 13.3, CMake 3.28, OpenSSL 3.0.13, SQLite 3.45.1,
cppcheck 2.13.0. Full log: `logs/milestone16-test-log.txt`.

- Clean build (coordinator re-ran from scratch):
  `rm -rf build && cmake -S . -B build` exit 0;
  `cmake --build build -j4` exit 0, **0 warnings, 0 errors**.
- Full suite: `./build/oeeoo_tests` exit 0 — **171 sections, ALL
  CHECKS PASSED** (run twice). `ctest --test-dir build` — **100%
  tests passed, 0 failed out of 1** (34.45 s).
- ASan/UBSan build (`-fsanitize=address,undefined
  -fno-sanitize-recover=all`, `detect_leaks=0`, no `stdbuf` —
  `stdbuf` breaks the ASan binary at startup): full suite exit 0,
  ALL CHECKS PASSED, **0 sanitizer errors**.
- cppcheck `--enable=warning,performance,portability
  --suppress=missingIncludeSystem -Isrc src tests`: exit 0,
  **0 findings** (83/83 files checked).
- Section arithmetic: 155 (M15) + 6 (chain/template) + 6
  (journal/wallet RPC) + 4 (load control/ban key) = **171**.
- New sections (16):
  - `m16a: CountSigOps P2PKH, multisig, fallback` (P2PKH=1,
    2-of-3=3, lone CHECKMULTISIG=20, mixed=21, coinbase skip)
  - `m16a: getchaintips incl forked tip` (genesis-only → 1
    active tip; fork tying main at height 3 → active branchlen
    0, fork `valid-fork` branchlen 2)
  - `m16a: getblockstats hand-computed values` (101-block
    block-funded chain: block 101 fee exactly 1.0 OEEOO,
    avgfeerate = 1e8/spend-bytes, size = 80+1+coinbase+spend,
    mediantime kTime+96·600; coinbase-only block avgfeerate 0;
    side-branch block stats + own mediantime; unknown hash → -5)
  - `m16a: getchaintxstats window math` (102 blocks / 103 txs;
    nblocks=2 → window 3 txs, txcount still 103; nblocks=0 → -8)
  - `m16a: getblocktemplate real sigops and fees` (P2PKH spend:
    fee exactly 100000000 sats as integer, sigops exactly 1,
    coinbasevalue 51 COIN)
  - `m16a: getblocktemplate bare multisig sigops` (2-of-3
    output spend: sigops exactly 3, fee 1 COIN)
  - `m16b: tx journal round-trip (sidecar file)`
  - `m16b: ScanUtxo populates receive records`
  - `m16b: listtransactions/gettransaction RPC round-trip`
  - `m16b: walletpassphrasechange` (success + wrong-old
    rejection -14 + new passphrase unlocks, old fails)
  - `m16b: journal survives lock/unlock cycle`
  - `m16b: migration — wallet without a sidecar`
  - `m16c: rpc work queue full returns HTTP 500, server stays
    alive`
  - `m16c: rpc pool shutdown is clean with a blocked worker`
  - `m16c: ip-only ban key survives a reconnect from a new
    ephemeral port (real TCP)`
  - `m16c: loopback bans still work`
- Reproducible build: `./scripts/reproducible-build.sh` from the
  final commit — **REPRODUCIBLE: all three binaries are
  byte-identical across two builds** (checksums in §F.1 below;
  recorded at delivery time in the test log).

### F.1 Reproducible-build checksums

Run from commit `d0c644b` (SOURCE_DATE_EPOCH=1793961737):

```
./scripts/reproducible-build.sh
# SOURCE_DATE_EPOCH=1793961737
# REPRODUCIBLE: all three binaries are byte-identical across two builds
# REPRODUCIBLE: archive is byte-identical across two builds
# release package: oeeoo-coin-0.1.0.tar.gz
#   e40d060612e8fdfd48d1d3f6f4196d4735189d1779d09b17422ff7f48a69783f  oeeoo-coin-0.1.0.tar.gz
#   a7d0d880d39d5008b38f4f659e533374fb7d05a5400d58ca2348fcb3cf3f6f5f  oeeoo-node
#   110168c17be7c8cb51c0b0bc9103f2189e2108e82ff07cca02f949e2ca8ace82  oeeoo-genesis
#   a80b3c6f0677814b3ee181f4f5b6bb9bee85f56171d71b964c621a79b8323136  oeeoo-cli
```

`oeeoo-cli` is unchanged from M14/M15, so its hash matches M14's
and M15's. The binary hashes above are exact for this milestone's
source. The tarball hash is for the release package built from
`d0c644b`; a rebuild from a later docs-only commit differs only in
`release-0.1.0/SOURCE.txt` (the commit-hash line).

Confirmation run from the docs commit `1f21b81`
(SOURCE_DATE_EPOCH=1793962495): **REPRODUCIBLE — binaries
byte-identical, archive byte-identical**; all three binary hashes
identical to the run above; tarball
`5b7ff2a6e8dea7fccc6895fc94d43b97bc61a818f6fc0252c1eeb9326cef878b`
(differs from the `d0c644b` tarball only via SOURCE.txt).

## G. Outstanding decisions (carried + new)

All 7 previously open decisions were DECIDED by user delegation
2026-11-06 14:55 PKT (see §B) — none of them is open anymore.
Carried from M14/M15 (still open, unchanged):

- None. The M14 morning decisions (walletpassphrasechange,
  journal format, multi-wallet, Stratum V2) and the M15 decisions
  (ban key, AS-map, orphan promotion) are all decided and
  implemented/documented in M16.

New from M16: none. (If a future milestone needs one, it goes
here.)

Standing owner items (unchanged): founder keys offline (user
reports all 3 pairs generated + secured on their mobile, single
device, user's accepted risk — user-reported, NOT verified);
independent audit (give the auditor `docs/AUDIT_PACKAGE.md` +
`docs/SECURITY_REVIEW.md`); GitHub repo — **created by the user
2026-11-06, verified empty:
https://github.com/OeeooCoin/Oeeoo-Coin-Blockchain-By-Ahmad —
code push ONLY after ALL milestones (M19) complete, per the user's
decision**; launch time + genesis approval; 4 DNS seed domains;
release-signing custody. Founder addresses are NOT needed for M16
— needed only at genesis time (M19/launch): the 3 PUBLIC keys
(66 hex each) for `oeeoo-genesis --pubkey1/2/3`; private keys
NEVER to be shared.

## H. Verdict

**NOT READY** — unchanged from M13/M14/M15. Genesis unapproved,
no independent security audit, no mainnet. M16 closes the RPC
and mining completeness scope of the roadmap; the next milestone
(M17, persistence & robustness) is the next step toward the full
M14–M19 roadmap.
