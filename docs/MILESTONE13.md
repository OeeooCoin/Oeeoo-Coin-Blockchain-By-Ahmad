# Milestone 13 — Complete Consensus, Mainnet Finalization, Security & Release Readiness Audit (DONE 2026-11-06)

This milestone was an audit, not a feature build: three independent
inspections of the whole repository (consensus/supply/vesting/difficulty/
block size; mempool/wallet/database/RPC/Stratum; genesis/network/docs/
license), followed by closing every gap that could be closed without
changing an approved decision. Everything the brief asked to verify is
reported below as MATCH / MISMATCH / FIXED / NOT IMPLEMENTED /
NOT VERIFIED / REQUIRES DECISION. Nothing was hidden: the bugs found —
including two in code written during this milestone itself — are listed
in "Bugs found and fixed (disclosed)".

## A. Milestone summary

**Inspected:** the full repository — all of `src/`, all of `tests/`,
every doc and milestone report, the license, the reproducible-build
script, and the git history (the repo is now a real Git checkout; see
§14).

**Verdict: NOT READY for mainnet.** The code matches the finalized
consensus parameters where they are implemented, and every fixable gap
found by the audit is now closed. But the brief's own rule decides the
verdict: *"If Mainnet genesis parameters are still not approved,
Mainnet must remain NOT READY."* The genesis timestamp, founder keys,
and genesis hash are unapproved, and the independent security audit is
still external and pending. No release was published or tagged.

**What changed, in one paragraph:** mempool expiry is now actually
enforced (it existed but was never called); opt-in RBF (BIP125-style)
is implemented; dust is dynamic and script-aware (182 sats P2PKH /
304 sats 2-of-3 multisig at 1 sat/vB) and the hardcoded 546-sat wallet
limit is gone; the mempool has eviction and a configurable cap
(default exactly 300 MB); a fee estimator over the last 24 blocks
feeds the new `estimatesmartfee` RPC (29 methods); signatures are
low-S only (BIP62); the JSON parser has a 64-level depth limit; RPC
is thread-per-connection (max 32) instead of sequential; the node
deep-verifies by default and refuses a genesis file or checkpoint
that does not match the stored chain; the peer cap is enforced; the
handshake rejects old protocol versions and self-connections; compact
blocks need valid proof of work before any transaction is fetched,
pending compact entries expire, and a failed compact relay falls
back to a full-block request; the address manager is capped at 1,000
entries with IP validation and randomized advertisement; a real
use-after-free in the loopback transport is fixed; the reproducible
build runs from a clean Git checkout and verifies the archive too.

## B. Decision cross-check

Finalized decisions (spec + owner decisions) against the code:

| Decision | Status | Evidence |
|---|---|---|
| Max supply 21,000,000 = 20M mining + 1M founder | MATCH | `GetBlockSubsidy` eras sum to exactly 20,000,000 over 940,000 blocks (long-range simulation test); `CheckBlockReward` caps every coinbase at subsidy + fees (now with an explicit `__int128` checked addition and a MAX_MONEY rejection) |
| Rewards 50/25/12.5/6.25/3.125, halving every 210,000 | MATCH | Boundary tests at 210000/210001, 420000/420001, 630000/630001, 840000/840001, 940000/940001; subsidy is 0 after 940,000 |
| Block time 600 s, retarget every 2,016 blocks, 4x clamp | MATCH | Difficulty vectors re-verified (see M3); retarget tests incl. clamps |
| Max block size 1,000,000 bytes | MATCH | Enforced on every path (block, template, P2P 2 MB message cap); boundary tests at exactly max / max+1 pre-date M13 |
| 10-min target / mainnet PoW limit `0x1d00ffff` | MATCH | Default in config and genesis tool; genesis bits must equal the limit at acceptance; a test-bits genesis is rejected at the default limit (verified end-to-end) |
| Port 64775, magic `e2 d8 04 d6`, P2PKH `0x0f` ("7" prefix) | MATCH | No collision with Bitcoin on any identifier; protocol version 70015 is shared by number only (magic differs) |
| Founder vesting: 10 × 100,000, launch + 9 anniversaries, Feb 29 → Feb 28 | MATCH (spec §6) | Implemented in M9, re-verified in M13. Note: the M13 brief §5 says vesting terms "must remain UNDECIDED until explicitly approved" — the spec §6 schedule is the owner's finalized decision and was NOT changed; any change needs the owner's explicit approval |
| Legacy P2PKH only at launch | MATCH | No SegWit/Taproot/bech32 anywhere in `src/` |
| Min relay fee 1 sat/vB | MATCH | Exact integer compare in mempool; `getmempoolinfo` now also reports `mempoolminfee` |
| Mempool 300 MB default, 14-day expiry | FIXED | Cap was enforced but not configurable and had no eviction; expiry existed but was never called in production. Now: `--maxmempool` / `maxmempool` config (default exactly 300 MB), eviction by lowest fee rate, `Node::Poll` sweeps expired entries every 60 s, `AssembleBlock` never mines an expired entry |
| Opt-in RBF | FIXED | Was explicitly "later milestone"; conflicts were rejected outright. Now BIP125-style: every conflicting entry must signal (nSequence < 0xfffffffe), replacement fee and fee rate must both beat every replaced entry, no new unconfirmed inputs, at most 100 replaced |
| Dynamic script-aware dust | FIXED | Was a hardcoded 546-sat wallet policy only. Now `src/transactions/dust.*`: threshold = (output size + input size) × rate, input size script-aware (P2PKH 107, multisig 1 + m×74); enforced in mempool and wallet |
| Dynamic fee estimation over last 24 blocks | FIXED | Did not exist. Now `src/mempool/feeestimator.*` (exact-integer median, minimum 1 sat/vB, 24-block window) + `estimatesmartfee` RPC |
| Wallet: BIP39 24 words, BIP32, BIP44 `m/44'/65535'/…` | MATCH | Vectors re-verified (see `docs/WALLET.md`); 24-word default; restore accepts 12/15/18/21/24 words (standard BIP39) |
| Wallet encryption: PBKDF2-HMAC-SHA512 200k iters + AES-256-GCM | MATCH | 16 B salt, 12 B nonce, 16 B tag; wrong passphrase / tampered byte / truncation all rejected (truncation test added in M13) |
| Keys never logged / never exposed via RPC | MATCH | No `dumpprivkey`/`dumpwallet`/xprv/mnemonic/seed method; wallet methods return only addresses, balances, txids |
| SQLite: WAL + synchronous=FULL, schema versioning, atomicity | MATCH | Schema v2 + undo table; `TransactionGuard` wraps Apply/Disconnect/Prune; migration 1→2 tested |
| Pruning safety | MATCH | Bodies NULLed below height, `prune_height` recorded; deep verify fails loudly on pruned history (documented limit) |
| RPC binds 127.0.0.1, cookie auth mandatory | MATCH | 32 random bytes, mode 0600, constant-time compare; auth-off exists only in tests |
| Stratum share validation | MATCH | extranonce2 exactly 8 hex chars; ntime window `[job, job+7200]`; low-difficulty shares rejected; network-difficulty share becomes a real block; vardiff present |
| Genesis: deterministic construction, verified fields | FIXED | `ReadGenesisFile` now verifies hash, time, bits, nonce, Merkle root, and message (before M13 it verified only hash/time/PoW/founder output — the M12 report's "fully verifies" was an overstatement, corrected here). `CheckGenesisIdentity` refuses a file or checkpoint that disagrees with the stored chain |
| No silent different genesis on an existing data dir | FIXED | Was silently ignored as a chain while its founder script was still adopted for vesting (a real misconfiguration hole). Now refused with an explicit error |
| Peer cap 125 | FIXED | Existed only as a constant/config value; nothing consumed it. Now `Node::SetMaxPeers`, enforced on outbound `ConnectTo` and inbound registration (this also bounds `TcpTransport::Poll`'s `FD_SETSIZE` exposure: 125 < 1024) |
| Compact relay + full-block fallback | FIXED | The fallback did not exist (a failed `blocktxn` was silently dropped). Now requests the full block with `getdata`. Also new: PoW is checked before any transaction is fetched; pending entries expire after 120 s |
| Modern address formats (SegWit/Taproot) | REQUIRES DECISION | The brief's table lists "Legacy + modern formats" but also warns not to assume SegWit was approved. The spec says P2PKH only at launch. Nothing implemented |
| Genesis timestamp / hash / founder keys | REQUIRES DECISION | Unapproved; no mainnet genesis exists; no hardcoded mainnet checkpoint exists (the `--genesis-checkpoint` mechanism is built and tested, waiting for the approved value) |
| DNS seed domains / operators | REQUIRES DECISION | Mechanism built, list empty; no hostnames invented |
| SLIP-0044 coin type | REQUIRES DECISION | Code uses provisional 65535 (unregistered) |
| Release-signing custody | REQUIRES DECISION | Reproducible package exists; signing is undecided |

**NOT VERIFIED (honest limits):** RBF descendant handling is
defense-in-depth — descendants are unreachable in practice because
validation rejects unconfirmed inputs, so the code exists but the
path cannot be exercised through `AddTransaction`; the 100-replacement
cap is implemented but not test-covered. Disk-full behaviour cannot be
simulated honestly in a test here. Crash recovery is tested by
abandoning a store without closing it (it loads correctly), not by
killing the process mid-write. ECDSA low-S is now enforced and tested
(50 sign/verify rounds + a hand-built high-S rejection); dedicated
SIGHASH_ALL vectors are covered by the new test (locktime and output
changes invalidate the signature).

## C. Warning report (every old and new warning)

Tool versions: g++ 13.3, CMake 3.28, cppcheck 2.13.0, OpenSSL 3.0.13,
SQLite 3.45.1.

| Warning | Cause | Fix / justification |
|---|---|---|
| Compiler `-Wall -Wextra`: 0 warnings | — | Normal build and ASan build are warning-free (verified on the final code) |
| cppcheck `eraseDereference` (error) in `src/network/node.cpp:484` | **Real bug in new M13 code:** the compact full-block fallback read `pit->second.peerId` after `pendingCompact_.erase(pit)` — use-after-erase | Fixed: the peer id is copied into a local (`compactPeer`) before the erase; the fallback uses the copy. Found by cppcheck, disclosed in "Bugs found" below |
| cppcheck style notes, `src/network/transport.h:51,76` (virtual `Close()` called from destructors) | Historical M11 notes | Fixed by design, not suppression: non-virtual `CloseLoopback()` / `CloseTcp()` do the cleanup; destructors call those directly; the virtual `Close()` delegates to the same functions. The notes are gone |
| cppcheck `nullPointerRedundantCheck` ×8 in test files (`test_cli.cpp`, `test_rpc.cpp`, `test_stratum.cpp`) | The `CHECK` macro counts failures and continues (does not abort), so cppcheck correctly noted a possible null dereference after a failed CHECK | Fixed properly: an early `if (x == nullptr) return;` guard now follows each CHECK. Test code only; no production impact |
| cppcheck `passedByValue` in `tests/test_node.cpp:90` | Test helper `Pump` took the node vector by value | Fixed: `const std::vector<Node*>&` |
| cppcheck `containerOutOfBounds` in `tests/test_verify.cpp:157,159` | Same CHECK-macro pattern (`coins[0]` after `CHECK(!coins.empty())`) | Fixed with an early-return guard |
| `tests/test_fuzz.cpp:57` `-Wstringop-overflow` (vector self-insert, M11-era) | Inserting a vector into itself — undefined behaviour | Already fixed in launch support (copy into a temporary vector first); verified gone |
| `system()` return-value warnings in RPC tests (M10-era) | Unchecked `system()` return | Fixed in M12 (`CHECK(== 0)`); verified gone |
| `86400 * 365 * 5` signed-int overflow (M9/M10-era, flagged by the brief §5) | 32-bit overflow in a test constant | Fixed in M12 (unsigned literals); M13 verified no `86400 * 365` pattern remains in `src/` or `tests/` |
| CMake `CMAKE_C_FLAGS was unused` warning (reproducible build) | The project has no C sources | Fixed: the flag was removed from `scripts/reproducible-build.sh` (comment records why) |
| `gzip: warning: GZIP environment variable is deprecated` | `GZIP=-n` in the reproducible-build script | Fixed: `tar -I 'gzip -n'` instead of the environment variable; the warning is gone from the M13 run |
| **Final cppcheck result on the final code: 0 errors, 0 warnings, 0 style notes** (`--enable=warning,performance,portability --suppress=missingIncludeSystem -Isrc src tests`) | — | Nothing was suppressed to make the report clean |

## D. Security report

**Critical**
- **No independent security audit.** This milestone is a self-review
  by the same agent that wrote the code; it is not an audit. Mainnet
  must not launch until an independent audit is complete (give the
  auditor `docs/AUDIT_PACKAGE.md` + `docs/SECURITY_REVIEW.md`).
- **No approved mainnet genesis.** There is no authenticated genesis
  hash for any node to check against. The checkpoint mechanism
  (`--genesis-checkpoint`) is built and tested; the value itself is
  an owner decision that does not exist yet.

**High**
- **No distinct testnet identity.** Test and mainnet nodes share the
  magic, port, protocol version, and user agent, so they peer with
  each other and reject each other's blocks only at consensus. The
  only guard is the testing-mode banner (M13: the node now prints an
  explicit warning whenever the PoW limit is not the mainnet limit).
  A distinct testnet magic/port is a design decision for the owner.
- **No per-client RPC rate limiting.** The sequential accept loop
  (one slow client blocked all RPC) is fixed — RPC is now
  thread-per-connection with a 32-connection cap — but there is still
  no per-client rate limit. Mitigated by localhost-only binding; do
  not expose the port.
- **Inbound peer IPs are not recorded.** `accept()` discards the
  peer address and bans are keyed by ephemeral transport ids
  (`tcp:3`, …), so a ban on an inbound peer does not survive a
  reconnect. Outbound bans work correctly.
- **Stratum `mining.authorize` accepts any credentials.** Acceptable
  only because Stratum binds localhost (127.0.0.1:64777). Never expose
  the port, or any machine on the network could mine to your address.

**Medium**
- **JSON parser depth limit added (was unbounded recursion).** A
  deeply nested payload inside the 4 MB body cap could previously
  stack-overflow the RPC server. Now capped at 64 levels. Severity
  was bounded by localhost-only binding.
- **RBF descendant path is untestable in practice.** Descendants
  cannot exist because validation rejects unconfirmed inputs; the
  replacement code handles them as defense-in-depth, and the
  100-replacement cap has no test. The reachable RBF paths (signal
  check, fee and fee-rate checks, no-new-unconfirmed-inputs) are
  tested.
- **Disk-full behaviour is not verified.** No honest simulation exists
  in this environment. WAL + `synchronous=FULL` gives durability in
  principle; a full disk will surface as a store error, not silent
  corruption.
- **DNS resolution has no timeout.** A hanging resolver blocks node
  startup. Seeds are queried only when the address manager is empty.
- **Fee estimator returns the minimum with no data.** Before 24
  blocks are recorded, `estimatesmartfee` returns 1 sat/vB. That is
  the safe direction (never overestimates), but it is not an estimate.

**Low**
- `getblocktemplate` reports sigops as 0 (documented simplification,
  pre-existing).
- Compact relay `getblocktxn` uses absolute indexes (documented
  simplification, pre-existing).
- The height-0 genesis coinbase is exempt from the 100-block maturity
  rule (by design — the founder vault must be spendable under the
  vesting rules, not the maturity rule).
- `AddrMan` has a 1,000-entry cap and IP validation now, but no
  per-source limits and no expiry of stale entries.

**Informational**
- ASan with leak detection reports 6,513 bytes in 117 allocations,
  all "indirect leak" frames inside libcrypto (the legacy provider's
  one-time process-lifetime globals, triggered by
  `hash160.cpp`'s `EnsureLegacyProvider`). Benign by design: the
  provider is intentionally never freed. Zero direct leaks from
  project code.
- This VM's DNS resolver answers every hostname (wildcard proxy).
  A seed hostname that "resolves" here may not resolve on a real
  network; the node only attempts connections, and peer scoring
  drops bad ones.

## Bugs found and fixed (disclosed)

1. **Use-after-erase in the new compact fallback (M13, mine).**
   `src/network/node.cpp`: the `blocktxn` handler erased the pending
   entry and then read `pit->second.peerId` for the full-block
   fallback. Found by cppcheck (`eraseDereference`). Fixed by copying
   the peer id before the erase. The code was never delivered in this
   state — it was caught before the final test run.
2. **Loopback transport use-after-free (pre-existing, found by
   subagent C).** `~LoopbackTransport` removed itself from the
   registry, but other transports' `Link` entries still held a raw
   pointer to it; a later `Send()` dereferenced a destroyed object.
   Fixed: `CloseLoopback()` (used by both `Close()` and the
   destructor) now purges every link pointing at the closing
   transport from all registered transports and deregisters it, so a
   closed transport is unreachable for new connections too. Tested
   both with and without an explicit `Close()`.
3. **`SerializeAddr` count bug (pre-existing, found by subagent C).**
   The count prefix was written before unparseable IPs were skipped,
   so the count could exceed the entries written. Fixed: serialize
   first, then write the true count.
4. **`ParseAddr` accepted any 16-byte address field.** It formatted
   bytes 12–15 as an IPv4 address without checking the IPv4-mapped
   prefix. Fixed: the 10 zero bytes + `0xffff` prefix is now required;
   anything else rejects the message.
5. **`TcpTransport::Connect` could throw.** `std::stoi` on a
   non-numeric port threw instead of failing cleanly. Fixed with
   try/catch → `false`.
6. **M12 documentation overstatement.** `docs/MILESTONE12.md` said
   "`ReadGenesisFile` fully verifies a file" — it did not verify the
   Merkle root, bits, nonce, or message fields. `ReadGenesisFile` now
   verifies all of them (8 tamper cases tested), and this report
   corrects the record.
7. **README contradictions (found by subagent C).** The README still
   claimed version byte `0x0f` was "the only version byte producing
   a `75` prefix" and `75Pk…` "achievable" — false, and it
   contradicted the owner's Milestone 8 decision to drop `75Pk`.
   Fixed. The README's "What is NOT done yet" section still listed
   Milestones 2–12 as not done — fixed. `docs/MAINNET_LAUNCH.md`
   listed reproducible builds as "NOT decided" although the script
   exists and was verified — fixed. `oeeoo-genesis`'s usage comment
   listed a `--message` flag that does not exist — fixed.

## E. Files changed

**New files**
- `src/transactions/dust.h`, `src/transactions/dust.cpp` — dynamic
  script-aware dust thresholds.
- `src/mempool/feeestimator.h`, `src/mempool/feeestimator.cpp` —
  fee estimator over the last 24 blocks.
- `tests/test_milestone13.cpp` — 21 sections (16 original + 5 for the
  subagent-C follow-up fixes).
- `docs/WALLET.md` — wallet recovery, backup, restore, watch-only,
  encryption (incl. the honest note that `oeeoo-node` v0.1.0 does not
  yet load a wallet file).
- `docs/MINING.md` — solo (getblocktemplate) and Stratum mining.
- `docs/THIRD_PARTY.md` — OpenSSL (Apache-2.0), SQLite (public
  domain), BIP39 wordlist (MIT).
- `docs/OPERATOR.md` — node setup, RPC security, backup/restore,
  known limitations.
- `.gitignore` — build dirs, release artefacts, data dirs, databases.

**Changed files**
- `src/mempool/mempool.h`, `src/mempool/mempool.cpp` — RBF, dust
  enforcement, eviction, configurable cap, `GetEntry`, `SetMaxBytes`.
- `src/network/node.h`, `src/network/node.cpp` — fee estimator,
  expiry sweep, pending-compact expiry, peer cap, genesis hash
  accessor, handshake checks (min version, self-nonce), random node
  nonce, compact PoW check, full-block fallback (incl. the
  use-after-erase fix).
- `src/network/transport.h`, `src/network/transport.cpp` —
  destructor refactor (`CloseLoopback`/`CloseTcp`), loopback link
  purge + deregistration, `stoi` guard.
- `src/network/addrman.h`, `src/network/addrman.cpp` — 1,000-entry
  cap, IPv4 validation, randomized `GetAddr`.
- `src/network/protocol.h`, `src/network/protocol.cpp` — `ParseIPv4`
  made shared, `SerializeAddr` true count, `ParseAddr` prefix check.
- `src/crypto/ecdsa.cpp` — BIP62 low-S signing + high-S rejection.
- `src/wallet/wallet.h`, `src/wallet/wallet.cpp` — hardcoded 546-sat
  dust limit removed; dynamic threshold used instead.
- `src/rpc/json.cpp` — 64-level nesting limit.
- `src/rpc/rpcserver.h`, `src/rpc/rpcserver.cpp` — thread-per-
  connection (max 32), `maxmempool`/`mempoolminfee` in
  `getmempoolinfo`, new `estimatesmartfee` method (29 methods total).
- `src/consensus/vesting.cpp` — `AnniversaryTime` clamps at
  `UINT32_MAX` instead of wrapping.
- `src/mining/miner.cpp` — explicit `__int128` checked coinbase
  addition (MAX_MONEY rejection); template reserves the exact
  coinbase size.
- `src/consensus/consensus.h` — stale "75" comment corrected.
- `src/app/config.h`, `src/app/config.cpp` — `deepVerify` default
  **true**, `--no-deep-verify`, `genesischeckpoint` /
  `--genesis-checkpoint`, `maxmempool` / `--maxmempool`.
- `src/app/genesis.h`, `src/app/genesis.cpp` — hardened
  `ReadGenesisFile` (Merkle root, nonce, bits, message) + new
  `CheckGenesisIdentity`.
- `src/app/node_main.cpp` — genesis identity check, peer cap +
  mempool cap applied, testing-mode banner warning.
- `src/app/genesis_main.cpp` — usage comment corrected (no
  `--message` flag).
- `scripts/reproducible-build.sh` — no `CMAKE_C_FLAGS`, archive
  built reproducibly twice and compared, `gzip -n` via `-I`,
  `SOURCE.txt` with the git commit.
- `CMakeLists.txt`, `tests/test_main.cpp` — new sources and test file.
- `tests/test_config.cpp` — default `deepVerify` updated + new M13
  options section (incl. a fixed test bug: wrong `argc`).
- `tests/test_cli.cpp`, `tests/test_rpc.cpp`, `tests/test_stratum.cpp`,
  `tests/test_verify.cpp`, `tests/test_node.cpp` — cppcheck guards /
  const-reference fix.
- `README.md` — status line, 75Pk correction, "NOT done yet" fixed.
- `docs/MAINNET_LAUNCH.md` — reproducible builds marked done;
  `docs/SECURITY_REVIEW.md` — deep-verify default and light-load
  trust model; `docs/RELEASE.md` — M13 checksums; `docs/DECISION_LOG.md`
  — M13 entry.

## F. Test results

Toolchain: g++ 13.3, CMake 3.28, OpenSSL 3.0.13, SQLite 3.45.1,
cppcheck 2.13.0.

```
cmake -S . -B build && cmake --build build -j4
ctest --test-dir build
# 100% tests passed, 0 tests failed out of 1

./build/oeeoo_tests
# [test] ALL CHECKS PASSED (127 sections, exit 0)
```

New in M13 (21 sections in `tests/test_milestone13.cpp`): dust
thresholds (P2PKH 182 / 2-of-3 multisig 304 at 1 sat/vB); RBF
(signal / fee / fee-rate / non-signaling rejection / dust);
eviction under pressure; expiry enforced by the node; fee estimator
(median 20, 24-block window, out-of-order ignored); long-range supply
simulation (940,000 blocks sum to exactly 20,000,000 mining +
1,000,000 founder = 21,000,000); 8 genesis-file tamper cases;
vesting at extreme times (clamped at `UINT32_MAX`); low-S (50 rounds
+ high-S rejection); JSON depth limit; wallet truncation; crash
recovery (store never closed); genesis identity (file vs store vs
checkpoint); peer cap; transport close; SIGHASH_ALL vector;
handshake rejects old versions and self-connections; compact header
without PoW ignored + pending entries expire; AddrMan cap / IP
validation / addr serialization; loopback close purges peer links
(incl. destroy-without-close); TCP connect rejects malformed
addresses.

```
# ASan + UBSan (final code):
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=0 ./build-asan/oeeoo_tests
# [test] ALL CHECKS PASSED (127 sections, exit 0)
# No AddressSanitizer errors, no UBSan runtime errors.
# (With detect_leaks=1: 6,513 bytes in 117 allocations, all indirect
#  leaks inside libcrypto's one-time legacy-provider globals — benign,
#  see §D Informational.)

cppcheck --enable=warning,performance,portability \
  --suppress=missingIncludeSystem -Isrc src tests
# 0 errors, 0 warnings, 0 style notes

./scripts/reproducible-build.sh   (from a clean Git checkout)
# SOURCE_DATE_EPOCH=1793915486
# REPRODUCIBLE: all three binaries are byte-identical across two builds
# REPRODUCIBLE: archive is byte-identical across two builds
# release package: oeeoo-coin-0.1.0.tar.gz
#   95047a0642a0242de32608dceddef21963eeccb49d6ece74507fb119a7f3726d
# oeeoo-node    bb88f13f8d178e1e236d34ff8ab9f8c0dc392b5f972106a72c598708efe672cf
# oeeoo-genesis 4468f4653254015b48bb647c66e7ba99cd0d113402facbc66fcc8e57b91fc7c6
# oeeoo-cli     59cd1105741ab025722067a66e2140cfdad42ad57e431e8c2b3aa3e492f7b246
# source commit: f866618bb3f1f0dbebe7ecebd7e48d917e5a9ac6
# (Same-machine, two-directory reproducibility only. Cross-environment
#  reproducibility is NOT claimed. No release published or tagged.)
```

End-to-end node check (test values only, 2026-11-06): `oeeoo-node`
with the M12 test genesis file + `--pow-limit-bits 207fffff` starts,
prints the `*** WARNING: TESTING MODE ***` banner, and answers
`getblockcount` → 0, `getbestblockhash` → the genesis hash;
`getmempoolinfo` reports `maxmempool: 314572800` and
`mempoolminfee: 1e-08`; `estimatesmartfee` returns
`{blocks: 0, feerate: 1e-08}`. Restart without the genesis file loads
the chain with deep verification on. A `--genesis-checkpoint` of 64
zeros refuses startup (exit 1, both hashes named in the error). The
default PoW limit refuses the test-bits genesis
("genesis bits must equal the proof-of-work limit").

## G. Outstanding decisions (owner's — not guessed)

1. Genesis parameters: launch time, founder public keys (3, offline),
   and the resulting genesis hash. Then set the published hash as the
   `--genesis-checkpoint` value for operators.
2. Independent security audit (external) — required before mainnet.
3. GitHub repository "OeeooCoin-blockchain-by-Ahmad" — create and push
   (a local git repo with two commits now exists: `4757b2b` and
   `f866618`).
4. DNS seed domains and operators (4 community seeds).
5. SLIP-0044 coin-type registration (65535 is provisional).
6. Release-signing custody.
7. Modern address formats (SegWit/Taproot): explicitly undecided;
   P2PKH only at launch.
8. Node wallet integration (loading a wallet file in `oeeoo-node`,
   wallet CLI): not built yet; `oeeoo-node` v0.1.0 has no wallet.
9. Distinct testnet identity (magic/port): undecided; test and
   mainnet currently share them, guarded only by the testing-mode
   banner.
10. Vesting terms: the spec §6 schedule stands as finalized; the
    brief §5's "UNDECIDED" caution is noted. Any change needs the
    owner's explicit approval.

## H. Launch readiness verdict

**NOT READY.**

The code is in its best audited state so far: 127 test sections pass,
ASan/UBSan are clean, cppcheck reports zero findings, the build is
reproducible from a clean Git checkout, and every gap the audit found
that could be closed without changing an approved decision is closed.
But two facts decide the verdict and neither is in the code's power to
fix: the mainnet genesis parameters are not approved, and no
independent security audit has happened. Per the brief §18, that is
NOT READY — not a judgement on the code, a statement about what is
still missing. The owner's remaining steps are unchanged: founder keys
offline, independent audit, GitHub repo, launch-time decision + real
genesis on a faster machine, DNS seeds, release signing.
