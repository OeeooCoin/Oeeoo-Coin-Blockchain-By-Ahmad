# Oeeoo Coin — Milestone 15: P2P Hardening & Completeness

Status: DONE 2026-11-06. All 134 prior sections still pass;
21 new sections added (155 total), ALL CHECKS PASSED, ctest 100%.
Build warning-free; cppcheck 0 findings; ASan/UBSan clean (see §F).
Built in three parallel streams (AddrMan + DNS; orphan pool; node
integration) plus a coordinator fix to one M13 test.

The M13 verdict stands: **NOT READY** (genesis unapproved, no
independent audit). M15 closes the biggest *network* gaps (inbound
peer IPs, AddrMan defences, DNS timeout, orphan pool, compact-relay
limits, peer eviction); it does not change launch readiness.

## A. Summary

### 1. Inbound peer IPs (`src/network/transport.*`, `src/network/node.*`)

- `TransportMessage` gains `remoteAddress`. `TcpTransport` stores
  `{peerId, fd, address}` per peer; `accept()` records `"ip:port"`
  via `sockaddr_in` + `inet_ntop`; `Poll` fills `msg.remoteAddress`;
  `Connect` stores the dialed address. `LoopbackTransport::Send`
  sets `remoteAddress = "loopback:" + address_` (the sender's
  address).
- `Node::OnBytes(peerId, bytes, remoteAddress = "")`: a new inbound
  peer registers as `peer.address = remoteAddress` (fallback: the
  peerId). **The ban check now runs before registration** — a banned
  inbound peer is refused on reconnect. `Node::Poll` passes
  `msg.remoteAddress` through.

### 2. AddrMan (`src/network/addrman.*`, `src/network/protocol.h`)

- Source-netgroup bucketing: `NetgroupOf(ip)` returns the /16
  (`"10.20"` for `10.20.3.4`), `""` for an invalid address.
  `kMaxPerNetgroup = 64` — Oeeoo's own choice (64 is ~6% of the
  1,000-entry table), documented NOT a Core constant. The cap is
  enforced in `Add` (a 65th entry from the same /16 is refused).
- Entry expiry: `kAddrExpirySeconds = 30 * 24 * 3600` (Core's 30-day
  horizon) + `RemoveExpired(nowSeconds)` (wrap-safe unsigned
  subtraction; strict `>` boundary — an exactly-30-day-old entry
  survives; future-dated entries wrap to a huge age and are dropped,
  documented).
- Failure counting: `kMaxFailures = 3` (Oeeoo's choice) +
  `RecordFailure(ip, port)` (removes the entry at 3) and
  `RecordSuccess(ip, port)` (resets the count to 0); both return
  false for unknown addresses. `GetCandidates` deliberately does
  NOT filter failures 1–2 (documented).
- `AddrEntry` gains `int failures = 0;` in protocol.h. It is NOT
  part of the wire format — `SerializeAddr` / `ParseAddr` carry only
  time/services/ip/port, so parsed entries always have
  `failures == 0`.
- **AS-map: NOT implemented** (documented in addrman.h) — it needs
  an external data file plus an operator decision. See §G.

### 3. DNS seeds (`src/network/dnsseed.*`)

- Resolver timeout: `kDnsSeedTimeoutMs = 10000` (Oeeoo's own
  choice, NOT a Core constant) + a defaulted `timeoutMs` parameter
  on `ResolveSeed` (existing callers get the timeout with zero
  changes). The lookup runs on a worker thread via `std::thread` +
  `std::promise` (not `std::async`, whose future destructor would
  block); on timeout the error contains `"timed out"`; the worker
  is detached and a late result is discarded (documented).
- Test seam: `SeedResolver`
  (`std::function<SeedLookupResult(const std::string&)>`) + public
  `ResolveSeedWithResolver(...)`.

### 4. Orphan transaction pool (`src/mempool/orphan.h/.cpp` — NEW)

Core's `TxOrphanage` model:

- `kMaxOrphans = 100` (Core's model), `kOrphanExpirySeconds =
  20 * 60` (Core's model), `kMaxOrphanBytes = 5 MB` (Oeeoo's
  choice: 100 × 50 KB largest plausible tx).
- Per-tx limit reuses `MAX_BLOCK_SIZE` (1,000,000 — the limit
  `Transaction::CheckBasic()` / `Deserialize()` already enforce;
  an exactly-1 MB tx is accepted, edge tested).
- API: `explicit OrphanPool(maxOrphans = kMaxOrphans, maxBytes =
  kMaxOrphanBytes); bool Add(tx, peerId, nowSeconds, error); bool
  Contains(txid); bool Get(txid, tx); bool Remove(txid); void
  RemoveExpired(nowSeconds); std::size_t PurgePeer(peerId);
  std::vector<Hash256> GetOrphansDependingOn(prevTxHash); Size();
  Bytes(); MaxOrphans(); MaxBytes();`
- `Add` rejects duplicates (`"transaction already in orphan
  pool"`), a tx over `MAX_BLOCK_SIZE`, and a tx larger than the
  byte cap alone. When full (count OR bytes) it evicts the OLDEST
  by addedTime (ties broken by smallest txid, deterministic) and
  never evicts the tx being added. `RemoveExpired` drops entries
  where `nowSeconds >= addedTime + 1200` computed in 64-bit (a
  deliberate deviation from the mempool's 32-bit pattern,
  documented). `GetOrphansDependingOn` returns txids sorted. The
  prevout index is maintained on every add/remove/expire/purge
  path (empty index rows erased). Constructor parameters exist
  only so tests can use small caps; production must use defaults.
- The pool does NO validation itself and NEVER relays orphans (no
  advertisement API, documented).
- Orphan trigger (verified against `src/transactions/validation.cpp`):
  `ValidateTransaction` fails with `"missing input"` (input loop)
  or `"sequence lock: missing input"` (CheckSequenceLocks for a
  v≥2 tx whose sequence locks reference an unknown coin).

### 5. Compact relay (`src/network/node.*`)

- Per-peer in-flight limit: `kMaxCompactInFlightPerPeer = 5`
  (Oeeoo's own choice). A 6th cmpctblock from a peer →
  `Misbehave(+20)`, ignored.
- Depth limits: `kMaxBlockTxnIndexes = 10` (matches Core's
  getblocktxn depth of 10 — a starting value, not gospel). Missing
  > 10 indexes → full-block `getdata` directly via the new
  `SendBlockRequest` helper, no pending entry.
- `getblocktxn` retry: `PendingCompact` gains `retries`. The
  `blocktxn` handler is restructured: a first failure re-sends
  `getblocktxn` with the new missing list and keeps the entry,
  **saving the already-received txs back into it** (otherwise the
  retry could never reconstruct); a second failure erases the entry
  and falls back to full-block `getdata`.
- Block-stalling disconnect: `Peer::{awaitingBlock, blockRequestAt}`,
  set by `SendBlockRequest` / `SendBlockTxnRequest` (and the
  headers-handler batch getdata, tracked as its first block);
  cleared on `block` / `cmpctblock` / `blocktxn` arrival for that
  hash (any outcome — the request was answered). `Poll` checks every
  call: `Now() - blockRequestAt >= 2` (`kBlockStallTimeoutSeconds
  = 2`, Core's default) → disconnect with **no** misbehavior
  points. **Deliberate deviation, documented in code:** no Core
  64 s backoff — it only matters if the node kept requesting from
  the same peer, which it does not after a disconnect. A frozen
  `SetTime` cannot trigger stalls (the difference stays 0 until the
  clock advances).

### 6. Peer eviction when full (`src/network/peer.h`, `src/network/node.cpp`)

- New `Peer` fields: `connectedAt`, `lastTxTime`, `lastBlockTime`,
  `lastPongTime`, `pingRttMs` (`UINT32_MAX` = unknown),
  `lastPingSentMs` (steady-clock ms via a `SteadyMs()` helper);
  ping send records it, the `pong` handler (previously a no-op)
  parses the nonce and sets RTT / `lastPongTime` on match.
- `EvictInboundPeerFor(newAddress)` with the ranking documented in
  a code comment: inbound-only; excludes (a) the lowest known ping,
  (b) tx/block senders in the last 60 s, (c) the longest-connected
  peer; among the rest evicts the largest /16 netgroup (via
  `AddressManager::NetgroupOf`), tie-break most-recently-connected,
  then highest peerId string. Wired in `OnBytes` at the cap;
  `ConnectTo` still refuses at the cap (eviction is inbound-only).

### 7. Node wiring — orphan + AddrMan (`src/network/node.*`)

- `Node` gains `OrphanPool orphans_` + `OrphanCount()` /
  `Orphans()` accessors, and `PendingCompactCount()` for tests.
- `"tx"` handler: a `"missing input"` / `"sequence lock: missing
  input"` validation failure → orphan pool, no misbehavior, no
  relay (success or failure); any other failure keeps
  `Misbehave(+10)`.
- `ProcessOrphansFor(parentTxid)`: re-validates via
  `GetOrphansDependingOn`, promotes to the mempool + `RelayTxInv`
  + recursion on success, leaves on missing-input failure, drops
  on any other failure; **capped at 100 orphans per call**
  (documented). Called in the `"tx"` handler, `BroadcastTx`, at
  the end of `AcceptBlockDirect` after UTXO adoption, and at the
  end of `AcceptBlockReorg` on the adopted path only (looping the
  non-coinbase txids of the side branch).
- `DisconnectPeer` purges the peer's orphans (covers
  ban-disconnects). The 60 s `Poll` sweep now also runs
  `orphans_.RemoveExpired` and `addrman_.RemoveExpired`.
- `ConnectTo` failure → `RecordFailure` (via a guarded
  `ParseIpPort` helper; loopback names are never recorded,
  documented); `verack` handshake completion → `RecordSuccess`
  for `peer.address`.

## B. Decision cross-check

- No finalized spec decision was changed (supply 21M, rewards,
  P2PKH-only launch, vesting schedule spec §6, port 64775, magic
  `e2 d8 04 d6`, direct mainnet / NO public testnet per spec F24).
- Every numeric constant M15 introduces is marked in code and in
  §A above as either Core's model value or Oeeoo's own choice:
  - Core's model: orphan max 100, orphan expiry 20 min,
    AddrMan expiry 30 days, block-stall timeout 2 s, getblocktxn
    depth 10.
  - Oeeoo's own choice: per-netgroup cap 64, AddrMan max failures
    3, DNS timeout 10 s, orphan byte cap 5 MB, compact in-flight
    limit 5 per peer, eviction 60 s recency window.
  - Deliberate deviations, documented in code: no 64 s stall
    backoff (§A.5); orphan `RemoveExpired` uses 64-bit arithmetic
    where the mempool uses 32-bit (§A.4); `GetCandidates` does not
    filter failures 1–2 (§A.2).
- AS-map documented as NOT implemented (needs an external data
  file + operator decision) — see §G.
- Nothing in M15 touches consensus, money, vesting, the wallet,
  RPC, or Stratum.

## C. Warning report

### C.1 Bugs found and fixed (disclosed)

1. **M13 test conflict with the new netgroup cap — found by
   Stream A, fixed by the coordinator (commit `91b82cf`).**
   `tests/test_milestone13.cpp:874–875` filled the AddrMan table to
   1,000 using IPs `10.0.<i/256>.<i%256>` — all in a single /16 —
   so the new per-netgroup cap correctly stopped it at 64 (+ the
   earlier `1.2.3.4` = 65) and 2 CHECKs failed. The test was
   wrong, not the cap: the fill loop now spreads across netgroups
   (`"10." + (i/64) + "." + (i%64) + ".1"`, 32 netgroups × 64
   each). Suite green again at 140 sections after the fix.
2. **Uninitialized `Hash256 awaitingBlock` — real bug in Stream
   B's new code, found by the test suite before delivery.**
   Declared without an initializer in `peer.h`, so as a
   `std::array` member it held indeterminate bytes; the stall
   sweep then disconnected every peer on its first `Poll` (21
   test failures, all node handshakes). Fixed with `Hash256
   awaitingBlock{};` plus a comment explaining why the
   initializer matters. Never shipped in a broken state.
3. **Two test-side issues in Stream C's standalone driver** (no
   production-code bugs in the orphan pool): the byte-cap test
   used the same timestamp for every tx so the txid tie-break
   fired — the test was wrong, the pool was correct; the driver
   fixture sizes were 63 bytes not ~193 — driver arithmetic
   fixed.
4. **Environment: `libsqlite3-dev` and `cppcheck` were missing
   on this VM** (replaced since M9/M10). `apt-get update` hung
   >10 min and was killed; both were restored via direct .deb
   downloads from azure.archive.ubuntu.com in seconds
   (libsqlite3-0 + libsqlite3-dev 3.45.1-1ubuntu2.9, libtinyxml2-10,
   cppcheck 2.13.0-2ubuntu3, python3-pygments). No repo files
   touched by this.

### C.2 Behavioural findings (not code bugs — disclosed)

5. **Orphan promotion requires a *confirmed* parent.**
   `ValidateTransaction` and `Mempool::AddTransaction` check only
   the confirmed UTXO set (M13: "validation rejects unconfirmed
   inputs"), so a parent that merely enters the mempool can never
   promote its orphan; only block arrival (via
   `AcceptBlockDirect` / `AcceptBlockReorg`) can. The `"tx"`-handler
   `ProcessOrphansFor` call site is therefore defense-in-depth
   that cannot fire in practice. The E2E test delivers the parent
   in a mined block, which is the honest way to exercise it. (In
   Core the mempool accepts unconfirmed inputs, so the tx-arrival
   path works there — an Oeeoo/Core behavioural difference, not a
   spec violation.)
6. **TCP bans are keyed by `"ip:port"`, and the port is
   ephemeral.** A ban survives a reconnect when the source address
   is stable (loopback names, or a peer re-dialing the same port),
   but a TCP peer reconnecting from a new ephemeral port gets a
   fresh key. The loopback test proves ban-survives-reconnect; the
   TCP test proves `accept()` records the real IP (inbound peer
   address is `"127.0.0.1:<port>"`, never `"tcp:N"`). No code change
   made — this follows the brief's explicit `"ip:port"` address
   design and matches how outbound bans already work. Whether
   inbound bans should be keyed by IP alone is an open decision
   (§G).

### C.3 Warnings (compiler / static analysis)

- None. Clean build at `-Wall -Wextra`: 0 warnings, 0 errors.
  cppcheck `--enable=warning,performance,portability`: 0 findings
  across all 79 files.

## D. Security report

Gaps closed by M15 (see also `docs/SECURITY_REVIEW.md`):

- **Inbound peer IPs are now recorded** — bans on inbound peers
  survive reconnects (with the ephemeral-port caveat in §C.2.6).
- **AddrMan now has anti-fill defences**: per-/16 netgroup cap
  (64), 30-day expiry, failure counting with removal at 3.
  Previously a single source could fill the whole 1,000-entry
  table with stale entries that never expired.
- **DNS seeds can no longer hang startup**: a 10 s deadline on a
  worker thread; a hanging resolver is cut off and the node
  continues with the other seeds.
- **Orphan pool is bounded**: max 100 orphans / 5 MB, 20-minute
  expiry, per-peer purge on disconnect, never relayed — it cannot
  become a memory DoS vector.
- **Compact relay is bounded**: per-peer in-flight limit 5, depth
  limit 10, one retry then full-block fallback, stalling peers
  disconnected after 2 s.
- **A full node no longer refuses all new connections**: an
  existing inbound peer is evicted by a documented ranking that
  protects the best peers (lowest ping, recent senders,
  longest-connected).

Gaps that remain (unchanged from M13/M14 unless noted):

- No independent audit (external, still required before mainnet).
- No distinct testnet identity (test/mainnet share magic, port,
  version, userAgent — only the testing-mode banner guards).
- No per-client RPC rate limiting (only the 32-connection cap).
- `Stratum mining.authorize` accepts any credentials
  (localhost-only mitigation).
- `getblocktemplate` sigops always 0; getblocktxn absolute indexes.
- RBF descendants unreachable in practice; 100-replacement cap
  untested.
- Disk-full behaviour not verified.
- Fuzzing covers parsers, not the live peer/RPC/Stratum state
  machines.
- Reorg undo tested to depth 2; reorg across pre-v2 blocks fails
  closed.
- AS-map NOT implemented (§A.2) — AddrMan bucketing is by /16
  netgroup only, so a single /16 hosting many distinct peers is
  still one bucket.
- Orphan promotion via the `"tx"`-handler call site cannot fire in
  practice (§C.2.5) — block arrival is the real trigger.

## E. Files changed

Commits (all on top of M14's `14fa2bd`):

- `8740e9a` — orphan pool core (Stream C)
- `335d7ec` — AddrMan + DNS (Stream A)
- `91b82cf` — coordinator fix: M13 AddrMan test for the
  per-netgroup cap
- `ee2c4ca` — node integration (Stream B)

`git diff --stat 14fa2bd..ee2c4ca`: 18 files, +2103/−67.

| File | Change |
|---|---|
| `src/mempool/orphan.h` / `orphan.cpp` | NEW — OrphanPool (TxOrphanage model) |
| `src/network/addrman.h` / `addrman.cpp` | Netgroup cap 64, 30-day expiry, failure counting, AS-map NOT implemented |
| `src/network/dnsseed.h` / `dnsseed.cpp` | 10 s resolver timeout on a worker thread; `SeedResolver` test seam |
| `src/network/transport.h` / `transport.cpp` | `TransportMessage.remoteAddress`; `accept()` records real IP; loopback sets `"loopback:" + address_` |
| `src/network/node.h` / `node.cpp` | Inbound IP storage + pre-registration ban check; eviction; compact limits + retry + stall disconnect; orphan wiring; AddrMan RecordFailure/RecordSuccess |
| `src/network/peer.h` | Ping/RTT + block-request tracking fields; `awaitingBlock{}` initializer (see §C.1.2) |
| `src/network/protocol.h` | `AddrEntry.failures` (not wire format) |
| `CMakeLists.txt` | `src/mempool/orphan.cpp` in `oeeoo_core`; `tests/test_milestone15.cpp` in the test sources |
| `tests/test_main.cpp` | `TestMilestone15()` declared + called after `TestMilestone14` |
| `tests/test_milestone15.cpp` | NEW — 15 sections (6 orphan-pool + 9 node) |
| `tests/test_dnsseed.cpp` | 3 new sections (slow-resolver timeout, injected fast resolver, localhost resolve) |
| `tests/test_network.cpp` | 3 new sections (netgroup cap, expiry, failure counting) |
| `tests/test_milestone13.cpp` | Fill loop spreads across /16s (§C.1.1) |

## F. Test results

Toolchain: g++ 13.3, CMake 3.28, OpenSSL 3.0.13, SQLite 3.45.1,
cppcheck 2.13.0. Full log: `logs/milestone15-test-log.txt`.

- Clean build (coordinator re-ran from scratch):
  `rm -rf build && cmake -S . -B build` exit 0;
  `cmake --build build -j4` exit 0, **0 warnings, 0 errors**.
- Full suite: `./build/oeeoo_tests` exit 0 — **155 sections, ALL
  CHECKS PASSED**. `ctest --test-dir build` — **100% tests passed,
  0 failed out of 1**.
- ASan/UBSan build (`-fsanitize=address,undefined
  -fno-sanitize-recover=all`, `detect_leaks=0`, no `stdbuf` —
  `stdbuf` breaks the ASan binary at startup): full suite exit 0,
  ALL CHECKS PASSED, **0 sanitizer errors**.
- cppcheck `--enable=warning,performance,portability
  --suppress=missingIncludeSystem -Isrc src tests`: exit 0,
  **0 findings** (79/79 files checked).
- Section arithmetic: 134 (M14) + 6 (AddrMan/DNS) + 6 (orphan
  pool) + 9 (node integration) = **155**. The brief said "140
  sections" as the pre-B baseline; the actual pre-change suite at
  HEAD was 146 (134 + 6 AddrMan/DNS + 6 orphan-pool, the latter
  registered by Stream B), and all 146 pre-existing sections still
  pass.
- New sections (21):
  - `network: addrman netgroup cap`
  - `network: addrman expiry`
  - `network: addrman failure counting`
  - `dnsseed: slow resolver times out` (injected 1 s sleeper,
    200 ms deadline, elapsed asserted 100–5000 ms)
  - `dnsseed: injected fast resolver still works`
  - `dnsseed: normal resolve still works (localhost)`
  - `m15: orphan pool bounds: 100 max, 101st evicts oldest`
  - `m15: orphan pool expiry at 20 minutes`
  - `m15: parent-arrival lookup via the prevout index`
  - `m15: purge on disconnect removes only that peer's orphans`
  - `m15: byte-cap eviction and oversized rejection`
  - `m15: duplicate add and get/remove round-trip`
  - `m15: inbound ban survives reconnect (loopback)`
  - `m15: inbound ban survives reconnect (real TCP)`
  - `m15: eviction picks the worst peer and protects the best`
  - `m15: compact in-flight limit`
  - `m15: getblocktxn depth limit`
  - `m15: getblocktxn retry then full block`
  - `m15: block stalling disconnect`
  - `m15: orphan end-to-end via two nodes`
  - `m15: addrman expiry and failure recording via the node`
- Reproducible build: `./scripts/reproducible-build.sh` from the
  final commit — **REPRODUCIBLE: all three binaries are
  byte-identical across two builds** (checksums in §F.1 below;
  recorded at delivery time in the test log).

### F.1 Reproducible-build checksums

Run from commit `ee2c4ca` (SOURCE_DATE_EPOCH=1793955896):

```
./scripts/reproducible-build.sh
# SOURCE_DATE_EPOCH=1793955896
# REPRODUCIBLE: all three binaries are byte-identical across two builds
# REPRODUCIBLE: archive is byte-identical across two builds
# release package: oeeoo-coin-0.1.0.tar.gz
#   3c27ee4bef349ab9d76f620d6fe37ebcd733950da6a55adfce69f0cc22840747  oeeoo-coin-0.1.0.tar.gz
#   3f2675dad2c9a8aea9f01d933164715f49af5a03f92538ad458e5d1bb9128ce9  oeeoo-node
#   52b29e3893e1f805f2bf82d0441eb4e09c5b3818217dd541207886c06c8130f1  oeeoo-genesis
#   a80b3c6f0677814b3ee181f4f5b6bb9bee85f56171d71b964c621a79b8323136  oeeoo-cli
```

The binary hashes above are exact for this milestone's source
(unchanged since `ee2c4ca`). The tarball hash is for the release
package built from `ee2c4ca`; a rebuild from a later docs-only
commit differs only in `release-0.1.0/SOURCE.txt` (the commit-hash
line). `oeeoo-cli` is unchanged from M14, so its hash matches
M14's.

## G. Outstanding decisions (carried + new)

Carried from M14 (still open, unchanged):

1. `walletpassphrasechange` — M15, M16, or follow-up?
2. Wallet-tx journal format for `listtransactions` /
   `gettransaction` (M16): payload v2 vs sidecar?
3. Multi-wallet — future or schedule?
4. Stratum V2 — defer past launch (spec §13.1 requires it) or
   schedule?

New from M15:

5. **Inbound bans: key by IP alone, or keep `"ip:port"`?**
   (§C.2.6) A TCP peer reconnecting from a new ephemeral port gets
   a fresh ban key today. Keying inbound bans by IP alone would
   make bans survive any reconnect, at the cost of banning a whole
   NAT address. Needs a user decision before any change.
6. **AS-map for AddrMan** — implement (needs an external AS data
   file + operator decision on its source/update) or accept /16
   netgroup bucketing as the permanent defence?
7. **Orphan promotion on mempool-parent arrival** — leave as is
   (block arrival is the only trigger, §C.2.5) or accept
   unconfirmed inputs into validation so the `"tx"`-handler path
   can fire (a consensus-adjacent change — do NOT do casually)?

Standing owner items (unchanged): founder keys offline (user
reports all 3 pairs generated + secured on their mobile, single
device, user's accepted risk — user-reported, NOT verified);
independent audit (give the auditor `docs/AUDIT_PACKAGE.md` +
`docs/SECURITY_REVIEW.md`); GitHub repo — **created by the user
2026-11-06, verified empty:
https://github.com/OeeooCoin/Oeeoo-Coin-Blockchain-By-Ahmad —
code push ONLY after ALL milestones (M19) complete, per the user's
decision**; launch time + genesis approval; 4 DNS seed domains;
release-signing custody.

## H. Verdict

**NOT READY** — unchanged from M13/M14. Genesis unapproved, no
independent security audit, no mainnet. M15 closes the P2P
hardening scope of the roadmap; the next milestone (M16, wallet
transactions) is the next step toward the full M14–M19 roadmap.
