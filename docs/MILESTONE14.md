# Oeeoo Coin — Milestone 14: Node Wallet Integration

Status: DONE 2026-11-06 (overnight). All 127 prior sections still pass;
7 new sections added (134 total), ALL CHECKS PASSED, ctest 100%.
Build warning-free; cppcheck 0 findings; ASan/UBSan clean (see §F).
Includes a late M14 fix found by the funded E2E: coinbase maturity
in the wallet (see §C.7) — without it a mined wallet could not spend
at all in practice.

The M13 verdict stands: **NOT READY** (genesis unapproved, no
independent audit). M14 closes the biggest *functional* gap (the node
had no wallet at all); it does not change launch readiness.

## A. Summary

`oeeoo-node` v0.1.0 had a complete wallet *library* (M8) but the node
itself loaded no wallet: the five RPC wallet methods returned error
-13 ("no wallet attached"). M14 wires the wallet into the node on the
Bitcoin model:

- `--wallet <path>` CLI flag + `wallet` config key
  (`src/app/config.*`). A relative path resolves against the data
  directory. Empty = no wallet (old behaviour, error -13 preserved).
- `src/wallet/nodewallet.*` (NEW): `NodeWallet` owns the file path, a
  `Wallet`, lock state, and the unlock deadline.
  - `LoadFile(path)`: an existing file is verified to parse as a
    wallet file (without the passphrase) and loads **LOCKED**. A
    missing file is remembered — the node starts, and the wallet is
    created later with the `createwallet` RPC.
  - `CreateFile(path, passphrase, mnemonicOut)`: fresh 24-word
    mnemonic, saved encrypted, returns the mnemonic ONCE. Fails if
    the file exists or the passphrase is empty. Left LOCKED.
  - `Unlock(passphrase, timeoutSeconds)`: decrypts, restores the
    wallet, caches the account xpub + indices for the locked
    watch-only view. Wrong passphrase → "incorrect passphrase".
    Timeout 0 = until `walletlock`.
  - `Lock()`: persists the indices first, then wipes the mnemonic,
    keys, and file passphrase from memory (`OPENSSL_cleanse`) and
    leaves a watch-only wallet (neutered keys) behind.
  - `SaveIfDirty()`: re-encrypts only when the indices moved, via a
    temporary file + rename, so a crash mid-save cannot corrupt the
    wallet file. The format is unchanged.
  - Auto-relock: `IsLocked()` / `UnlockedUntil()` / `GetWallet()`
    re-lock lazily when the deadline passes.
- `src/wallet/wallet.*`: small accessors the wrapper needs —
  `GetNextExternal()` / `GetNextChange()` / `SetNextIndices(ext,
  change)` and `WipeSecrets()` (cleanses the mnemonic + private keys,
  then neuters the keys so the wallet stays usable as watch-only).
  Plus a behaviour fix found by the funded E2E (see §C.7):
  **coinbase maturity in the wallet.** `ScanUtxo` now takes the
  next-block height and applies the consensus maturity rule
  (COINBASE_MATURITY = 100; the genesis coinbase at height 0 is
  exempt, exactly as in `src/transactions/validation.cpp`):
  immature coinbases are NOT in `unspent_`/`balance_` — they are
  tracked separately as `immatureBalance_` — so neither `getbalance`
  nor coin selection can ever see them as spendable. `WalletUtxo`
  gained `height` + `isCoinbase`; `getwalletinfo` reports
  `immature_balance`.
- `src/rpc/rpcserver.*`: `AttachNodeWallet(NodeWallet*)` (the plain
  `AttachWallet(Wallet*)` stays for tests). Three new methods:
  `createwallet`, `walletpassphrase`, `walletlock`. The five existing
  methods go through the node wallet when one is attached:
  `getbalance` / `listunspent` / `getwalletinfo` work while locked
  (watch-only view); `getnewaddress` / `sendtoaddress` refuse while
  locked (error -13, "wallet is locked") because deriving an address
  or spending advances the file's indices, which can only be
  persisted while the passphrase is in memory. `getwalletinfo.
  unlocked_until` reports the real deadline (0 = locked).
  `listunspent[].spendable` is false while locked. `getblocktemplate`
  refuses to derive a miner address from a locked wallet (already
  in place; now reachable). Help list grows to 32 methods.
  `sendtoaddress` persists the (possibly advanced) change index
  BEFORE broadcasting, so a save failure cannot strand a broadcast
  transaction with an unpersisted change address.
- `src/app/node_main.cpp`: loads/registers the wallet, attaches it
  to the RPC server, saves + locks on shutdown (an I/O failure at
  shutdown prints a warning instead of being swallowed), prints a
  `wallet:` line in the banner.
- `docs/WALLET.md`: node-wallet section rewritten (locked model,
  `createwallet` / `walletpassphrase` / `walletlock`, timeouts,
  balance-while-locked, `getnewaddress`-while-locked refusal, BIP39
  passphrase must be empty for node wallets — the file passphrase is
  a different secret). The "no wallet integration" note is removed.

**The passphrase rule (hard constraint, Bitcoin model):** the wallet
passphrase is NEVER a CLI flag and NEVER stored in a file the node
reads silently (it would be visible in `ps` / on disk). It travels
only over localhost RPC (`walletpassphrase`, cookie-authenticated),
lives in memory only while unlocked, and is wiped on lock. The node
never logs it.

**Error code -14:** "wallet passphrase incorrect" (Bitcoin's code).
Verified unused anywhere in `src/` or `tests/` before use.

## B. Decision cross-check

| Item | Status | Evidence |
|---|---|---|
| Wallet file format unchanged (PBKDF2-HMAC-SHA512 200k + AES-256-GCM, magic `OEEOOWALLET1`, payload v1) | MATCH | `tests/test_milestone14.cpp` §"wallet file format unchanged": a NodeWallet-created file reads back through the plain `LoadWalletFile` API with the same magic/payload, and a plain-API file loads through the node wallet. |
| Passphrase never a CLI flag / config value / logged | MATCH | No `--wallet-passphrase` flag, no `passphrase` config key; `grep` clean. Passphrase travels only via `walletpassphrase` RPC. |
| Wallet loads LOCKED; `walletpassphrase` unlocks in memory for a timeout; `walletlock` re-locks | MATCH | `src/wallet/nodewallet.*`; tests §load/lock/unlock + §timeout auto-relock. |
| BIP44 account 0 only; BIP39 passphrase empty for node wallets | MATCH | `nodewallet.h` header contract; `Unlock` restores with `""`. |
| Locked = watch-only view (balance visible; no spend, no new address) | MATCH | Tests §load/lock/unlock + §RPC end to end (balance 50.0 while locked; `getnewaddress`/`sendtoaddress` → -13). |
| `getbalance`/`getnewaddress`/`sendtoaddress`/`listunspent`/`getwalletinfo` live through a real node | MATCH | Tests §RPC end to end over real HTTP + manual E2E (see §F). |
| Coinbase maturity in the wallet (M8-era gap, found by the funded E2E) | FIXED | `ScanUtxo` ignored maturity entirely: `getbalance` counted immature coinbases as available and coin selection could pick one, so the spend was then rejected by consensus ("coinbase output not yet mature"). Now immature coinbases are excluded from balance/unspent and reported as `immature_balance` (see §C.7). |
| `createwallet` (not in the original draft, needed: no wallet CLI exists) | FIXED (added) | Without it the feature would be unusable — no other way to create a wallet file. Mnemonic returned ONCE over localhost RPC. |
| `LoadFile` on a missing file | FIXED | The header contract said "remember the path"; the implementation returned an error. Fixed to match the contract (needed for `createwallet` to be reachable). |
| `listtransactions` / `gettransaction` | REQUIRES DECISION (M16) | The wallet library keeps no transaction history (UTXO scan only). Needs a wallet-tx journal + a format decision (payload v2 vs sidecar) — see `docs/ROADMAP_BITCOIN_LEVEL.md` M16. |
| `walletpassphrasechange` (passphrase rotation) | NOT IMPLEMENTED (follow-up) | Oeeoo files are created already-encrypted; only rotation is missing. Small, but out of the M14 scope as assigned. |
| Multi-wallet (`loadwallet`/`unloadwallet`/`listwallets`) | NOT IMPLEMENTED (future) | Single `--wallet` is the M14 simplification, stated in the roadmap. |
| Descriptor wallets | NOT IMPLEMENTED (by design) | Oeeoo is deliberately BIP39/44 P2PKH (spec §7); Core's descriptor model does not apply. |
| M13 carry-overs (RBF 100-replacement cap untested; RBF descendants unreachable; disk-full unverified) | NOT VERIFIED (unchanged) | Still open; see M13 §G. |

## C. Warning report

- Compiler (`-Wall -Wextra`): the build is warning-free (verified by
  rebuilding the four touched translation units and grepping the
  output — no warnings, no errors).
- cppcheck 2.13.0 (`--enable=warning,performance,portability
  --suppress=missingIncludeSystem -Isrc src tests`): **0 findings**
  (0 errors, 0 warnings, 0 style notes).
- Bugs found and fixed during M14 (disclosed):
  1. **Test bug, `tests/test_milestone14.cpp` (first run, 13
     failures):** three test-side errors, all fixed in the tests,
     none in production code — (a) the "zoo" mnemonic helper had 23
     words, not 24 (hand-counted string); (b) the RPC test expected
     `getnewaddress` to return the *first* address, but the wallet's
     scan had already moved the index past the funded address (index
     0) — the correct expectation is the *second* address (deriving
     index 0 again would be address reuse); (c) the CreateFile
     "already exists" check used a NodeWallet that already had a
     wallet loaded, so it failed with "a wallet is already loaded"
     instead. All three fixed; the suite then passed.
  2. **Flaky test, `tests/test_milestone14.cpp:301`:** the unlock
     deadline check (`until <= before + 60`) raced a second boundary
     — PBKDF2 takes ~100 ms, so the clock could tick between the
     `before` reading and the unlock's own clock read, failing the
     check. Fixed by bounding the deadline by both readings
     (`until <= after + 60`).
  3. **Flaky test, pre-existing M8 bug, `tests/test_wallet.cpp:402`:**
     the tamper test wrote a fixed `0xFF` byte at offset 30 of the
     wallet file — offset 30 is a *random nonce byte*, so the write
     was a no-op (and the test failed) whenever the byte was already
     `0xFF` (p = 1/256 per run). Found because the suite failed once
     in ~7 runs during M14 stability testing. Fixed by reading the
     byte and XOR-ing it (`byte ^= 0xFF`), which always changes it.
  4. **LoadFile / header contract mismatch** (see §B): the
     implementation refused a missing file while its own header
     comment promised to remember the path. Fixed in
     `src/wallet/nodewallet.cpp`.
  5. **Design note, not a bug:** a file truncated in the middle of
     the ciphertext is *not* refused by the load probe — the probe
     cannot distinguish a decryption failure caused by a wrong
     passphrase from one caused by truncation, and must treat both
     as "locked". It fails closed at unlock (the GCM tag check
     rejects it). Tested explicitly (§"wallet file format unchanged").
  6. **Real bug, M10-era, found by the M14 E2E and FIXED
     (`src/rpc/json.cpp`):** the JSON parser rejected whitespace
     after `:` in objects and after `,` in arrays — a value starting
     with a space fell through to the number parser ("bad number").
     `oeeoo-cli walletpassphrase '["pass", 60]'` failed while the
     same call without the space worked. Any real-world client
     (Python `json.dumps`, curl examples) emits those spaces, so the
     RPC server would have rejected normally-formatted requests. The
     M10 tests never caught it because `Json::Dump` emits no spaces
     and the hand-written test vectors had none either. Fixed by
     adding `SkipWs()` after `:` and after `,`; regression tests in
     `tests/test_rpc.cpp` (spaced object/array/full-request shapes).
  7. **Real bug, M8-era, found by the funded E2E and FIXED
     (`src/wallet/wallet.*`, `src/rpc/rpcserver.cpp`):** the wallet
     had NO coinbase-maturity handling anywhere. `ScanUtxo` added
     every wallet-owned UTXO to `unspent_`/`balance_`, and
     `CreateTransaction` selected smallest-first with no maturity
     filter — so on a mined chain `getbalance` reported immature
     coinbase funds as spendable, and a spend would almost always
     pick an immature coin (101 equal 50-OEEOO coinbases, only 1
     mature) and be rejected by consensus validation ("coinbase
     output not yet mature") instead of failing cleanly in the
     wallet. Fixed: `ScanUtxo(utxo, nextHeight)` applies the exact
     consensus rule (next block height − coin height ≥ 100; height-0
     genesis coinbase exempt); immature coinbases are excluded from
     balance/unspent and tracked as `immatureBalance_` (reported by
     `getwalletinfo` as `immature_balance`); `CreateTransaction`
     scans with its own `height` parameter, so the cut is exactly
     the consensus rule. The funded E2E proves it: at tip 101 the
     wallet reported balance 100.0 (coinbases 1 and 2 mature for
     block 102) and `immature_balance` 4950.0 (99 coinbases), and
     the 10-OEEOO send succeeded. Regression tests in the new §7
     (`tests/test_milestone14.cpp`): boundary checks at next heights
     50 / 104 / 105, the genesis exemption, and a 55-OEEOO payment
     that fails at height 50 and succeeds at height 105.

## D. Security report

- **Medium — mnemonic over RPC:** `createwallet` returns the 24-word
  mnemonic in the RPC response, and `walletpassphrase` carries the
  passphrase in a JSON-RPC request. Both travel only over localhost
  (127.0.0.1) behind cookie auth, the same boundary Bitcoin uses.
  The operator must write the mnemonic down immediately — the node
  never stores it. Anyone who can read the cookie file (or the
  operator's terminal) can create a wallet and read its mnemonic.
  Mitigation is the existing localhost-only + cookie-auth boundary;
  no new exposure is introduced beyond what RPC already had.
- **Low — balance visible while locked:** by design (watch-only
  view). A locked wallet still reveals addresses and balances to
  anyone with RPC access. Same as Bitcoin's watch-only behaviour.
- **Low — misleading unlock error on a truncated file:** `Unlock`
  reports "incorrect passphrase" for a truncated/corrupt ciphertext
  (see §C.5). Fail-closed, but the message can mislead an operator
  into thinking the passphrase is wrong. Documented here; a future
  improvement could distinguish the cases (e.g. a file-integrity
  check at load).
- **Informational — passphrase lifetime:** the file passphrase lives
  in `NodeWallet::filePassphrase_` while unlocked and is cleansed on
  `Lock()` and on timeout re-lock. The mnemonic is cleansed after
  each decrypt/restore. The RPC request string holding the
  passphrase is freed by the JSON layer; no explicit cleanse of the
  HTTP buffer is done (same as Bitcoin's handling of the RPC
  passphrase in memory).
- No consensus, supply, difficulty, vesting, or network change in
  M14. No new network-facing surface: the wallet is reachable only
  through the existing localhost RPC.

## E. Files changed

| Path | Change |
|---|---|
| `src/wallet/nodewallet.h` / `.cpp` | NEW (M14 core). LoadFile missing-file contract fixed (see §C.4). |
| `src/wallet/wallet.h` / `.cpp` | `GetNextExternal()` / `GetNextChange()` / `SetNextIndices()` / `WipeSecrets()` accessors (already present from the M14 start; no behaviour change). **Plus the §C.7 maturity fix:** `WalletUtxo` gained `height` + `isCoinbase`; `ScanUtxo(utxo, nextHeight)` excludes immature coinbases from balance/unspent and tracks `immatureBalance_`; `CreateTransaction` scans with its `height` parameter. |
| `src/app/config.h` / `.cpp` | `--wallet <path>` flag + `wallet` config key; usage text. |
| `src/rpc/rpcserver.h` / `.cpp` | `AttachNodeWallet`, three new methods, five existing methods routed through the node wallet, `listunspent[].spendable` reflects lock state, `sendtoaddress` persists before broadcast, error -14 for a wrong passphrase. **Plus §C.7:** the three `ScanUtxo` calls pass `chain.GetTipHeight() + 1`; `getwalletinfo` reports `immature_balance`. |
| `src/rpc/json.cpp` | Whitespace fix: `SkipWs()` after `:` in objects and after `,` in arrays (see §C.6). |
| `tests/test_rpc.cpp` | Regression tests for the whitespace fix (spaced object/array/full-request shapes). |
| `src/app/node_main.cpp` | Wallet load/register, attach, banner line, save + lock on shutdown. |
| `tests/test_milestone14.cpp` | NEW. 7 sections (the 7th is the §C.7 maturity section). Call sites updated for the new `ScanUtxo` signature. |
| `tests/test_main.cpp`, `CMakeLists.txt` | Register the new test file + `nodewallet.cpp`. |
| `tests/test_wallet.cpp` | Tamper-test flake fixed (XOR instead of fixed `0xFF`); `ScanUtxo` call sites updated for the new signature. |
| `docs/ROADMAP_BITCOIN_LEVEL.md` | Written first (M14–M19), then refined with the Bitcoin Core comparison research (see the commit messages). |
| `docs/WALLET.md` | Node-wallet section rewritten; "no wallet integration" note removed. |
| `docs/MILESTONE14.md` | This file. |

## F. Test results

Toolchain: g++ 13.3, CMake 3.28, OpenSSL 3.0.13, SQLite 3.45.1,
cppcheck 2.13.0.

Commands (from `~/workspace/oeeoo-coin/`):

- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` — configure OK.
- `cmake --build build -j4` — exit 0, warning-free.
- `./build/oeeoo_tests` — **134 sections, ALL CHECKS PASSED**
  (127 prior + 7 new M14 sections). Stability: the suite was run 8
  times consecutively after the first fixes — all 8 runs passed; the
  final code (with the §C.7 maturity fix) was re-verified with a
  clean build + full suite + ctest + cppcheck + ASan/UBSan run
  (see `logs/milestone14-test-log.txt`).
- `ctest --test-dir build --output-on-failure` — **100% tests
  passed, 0 tests failed out of 1**.
- `cppcheck --enable=warning,performance,portability
  --suppress=missingIncludeSystem -Isrc src tests` — exit 0,
  **0 findings**.
- ASan/UBSan: build-asan (Debug, `-fsanitize=address,undefined
  -fno-sanitize-recover=all`) + full suite with `detect_leaks=0` —
  exit 0, 134 sections, ALL CHECKS PASSED, no AddressSanitizer or
  UBSan errors. (One false start while verifying: `stdbuf` breaks
  the ASan binary at startup — its LD_PRELOAD puts libstdbuf before
  libasan ("ASan runtime does not come first in initial library
  list"). The ASan run must be done WITHOUT `stdbuf`; the normal
  build is unaffected. Recorded here so the next run doesn't
  re-diagnose it.)

New M14 sections (all in `tests/test_milestone14.cpp`):

1. `m14: node wallet load/lock/unlock lifecycle` — missing file
   remembered, corrupt file refused, wrong/empty passphrase
   rejected, correct passphrase unlocks with a deadline, first
   address is the known M8 address `7L88G1Not1Gv8nmci9mchhoN9XgbXTXqX5`,
   locked watch-only balance stays visible (50 OEEOO), timeout 0 =
   until walletlock, indices persist across lock/unlock with no
   address reuse.
2. `m14: unlock timeout auto-relocks` — 1-second unlock expires
   after a 2-second sleep; re-unlock works.
3. `m14: locked wallet cannot spend, unlocked can` — locked
   `CreateTransaction` refused ("watch-only"); unlocked builds and
   broadcasts (mempool size 1).
4. `m14: RPC wallet methods end to end` — real `RpcServer` +
   `NodeWallet` over HTTP: locked balance 0.0 (empty view before
   first unlock), `getnewaddress`/`sendtoaddress`/`getblocktemplate`
   refused while locked (-13), wrong passphrase → -14, unlock →
   balance 50.0, `getnewaddress` returns the second address,
   `listunspent` spendable, `getblocktemplate` coinbasevalue 50
   OEEOO, `sendtoaddress` returns a 64-hex txid and the mempool
   grows to 1, `walletlock` → balance still visible, spendable
   false; a server with no wallet returns -13 everywhere.
5. `m14: createwallet RPC creates a fresh wallet` — empty passphrase
   → -8, fresh 24-word mnemonic returned once with a warning, the
   mnemonic restores the same wallet (first address matches), second
   createwallet → -13.
6. `m14: wallet file format unchanged` — NodeWallet-created file
   reads back through the plain API (magic `OEEOOWALLET1`, same
   mnemonic/indices), wrong passphrase fails, a plain-API file loads
   through the node wallet, a header-truncated file is refused at
   load, a ciphertext-truncated file fails closed at unlock.
7. `m14: coinbase maturity in the wallet` — a coinbase at height 5
   (50 OEEOO) + a regular coin at height 5 (10 OEEOO): at next
   heights 50 and 104 the balance is 10 OEEOO and the immature
   balance is 50 OEEOO; at height 105 the coinbase matures (balance
   60, immature 0); a height-0 genesis coinbase is mature at height
   1; a 55-OEEOO payment fails ("insufficient funds") at height 50
   and builds (2 inputs) at height 105.

Manual E2E (test values only, recorded in `logs/milestone14-test-log.txt`):
`oeeoo-node` with a freshly mined test genesis (same 3 M12 test
pubkeys, time 1793923200, bits 207fffff; single-threaded mining
found nonce 0, hash
`47bcc8f4a711c2d4905ac04baed7434b6a39f33dacc2ae376670cebaecf9aa8b` —
nonce 0 was already valid; M12's nonce 5 was just the first found by
the parallel miner) + `--pow-limit-bits 207fffff` + `--wallet
wallet.dat` (relative path, resolved against the data dir): banner
shows `wallet: /tmp/oeeoo-m14-e2e-data/wallet.dat (not created yet —
use createwallet RPC)`; `getblockcount` → 0; `getwalletinfo` before
any wallet → -13 "wallet not loaded"; `oeeoo-cli createwallet`
returns a 24-word mnemonic + warning; `getwalletinfo` (locked,
never unlocked) → balance 0, `unlocked_until` 0; `walletpassphrase`
with a wrong passphrase → -14 "incorrect passphrase"; with the
right one (spaced JSON, after the §C.6 fix) → "wallet unlocked";
`getwalletinfo` → `unlocked_until` 1793922150 (now + 3600);
`getnewaddress` ×2 → two different addresses
(`7FVX6n39k4fr6Hq8NALw3cPADDuAYVC9vy`,
`761VUSbqcBHUprmRzfjuKpH6vRqMYfBe3A`); `getblocktemplate` →
`coinbasevalue` 5000000000, height 1, no error; `sendtoaddress` →
-13 "cannot build transaction: insufficient funds" — the wallet has
no funds on this chain (the test genesis pays the founder vault,
whose M12 test keys were discarded), so the manual send cannot be
funded this way; the funded send between two wallet addresses is
proven by the RPC test (§4 above: funded UTXO, real server, txid
returned, mempool size 1). `walletlock` → "wallet locked";
`getwalletinfo` → `unlocked_until` 0. Restart without
`--genesis-file`: chain loads with deep-verify ON, banner shows
`wallet: … (locked)`.

**Funded manual E2E (DONE — the stretch goal was completed):** a
small Python miner (`/tmp/oeeoo-m14-miner.py`, test difficulty only
— it refuses any other bits value) mined 101 blocks via
`getblocktemplate`/`submitblock` against the same test genesis: it
reconstructs the coinbase exactly as `AssembleBlock` does (BIP34
height push, no extra data) and solves the header PoW in Python
(`CheckProofOfWork` compares hash and target as big-endian bytes).
At tip 101 `getwalletinfo` reported balance 100.0 (coinbases 1 and 2
mature for block 102) and `immature_balance` 4950.0 (99 coinbases);
`sendtoaddress` to a second wallet address returned txid
`0280e14d7328305490fcf9aa6099d65e5c8e39d1ad1e4840cba0c1883795a93a`
and the mempool grew to size 1 (225 bytes). This run is what found
the §C.7 maturity bug (its first attempt failed the balance
assertion the author had written from a wrong expectation — the
wallet's 100.0 was correct: two coinbases are mature at tip 101,
not one — and the send itself then proved the fix). The miner
script is a one-off E2E harness, not a shipped tool; the test log
carries the full transcript.

## G. Outstanding decisions (carried + new)

Carried from M13 (unchanged — still the owner's): independent security
audit; genesis timestamp + founder 2-of-3 keys + genesis hash
approval; GitHub repo `OeeooCoin-blockchain-by-Ahmad`; 4 DNS seed
domains/operators; release-signing custody; SLIP-0044 coin-type
registration (65535 provisional); modern address formats (SegWit/
Taproot — out of scope, P2PKH-only launch stands).

New from M14 (for the morning): `walletpassphrasechange` — include
in M15 or M16, or leave as a follow-up?; wallet-tx journal format
for `listtransactions`/`gettransaction` (M16, payload v2 vs sidecar);
multi-wallet support (future, not M14–M16).

## H. Verdict

**NOT READY** — unchanged from M13. M14 closes the biggest functional
gap (the node now has a working wallet), but mainnet readiness still
requires the owner's decisions: genesis approval and an independent
security audit. No mainnet, no release, no tag.
