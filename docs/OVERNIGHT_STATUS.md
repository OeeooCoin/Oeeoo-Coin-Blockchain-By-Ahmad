# Overnight Status — Oeeoo Coin, 2026-11-06 (night shift ~03:10 → morning 08:00 PKT)

## DONE

- **Roadmap `docs/ROADMAP_BITCOIN_LEVEL.md` (M14–M19) — DONE.** Written
  first, then refined with a Bitcoin Core comparison (web research +
  code inspection). Corrections applied: Core has NO per-client RPC
  rate limiting (thread/work-queue bounds instead); reorg-100 /
  crash-injection / soak are Oeeoo robustness targets, not verified
  Core features; `listtransactions`/`gettransaction` need a wallet-tx
  journal (M16, format decision pending); Stratum V2 is a spec
  requirement deferred pending a user decision; no descriptor wallets,
  no `encryptwallet` mapping. Commits `6ab59e6`, `0fc8bc8`.
- **Milestone 14 — node wallet integration — DONE.** `--wallet` flag +
  `wallet` config key; wallet loads LOCKED (Bitcoin model);
  `walletpassphrase` / `walletlock` / `createwallet` RPC (32 methods
  total); `getbalance` / `getnewaddress` / `sendtoaddress` /
  `listunspent` / `getwalletinfo` live through a real running node.
  Passphrase NEVER a CLI flag / config value / logged. Wallet file
  format unchanged (PBKDF2-HMAC-SHA512 200k + AES-256-GCM); saves
  atomic (tmp + rename). Commits `a7033d3`, `09c29f7`.
- **Bugs found by the M14 E2E and FIXED (disclosed in
  `docs/MILESTONE14.md` §C):**
  1. JSON parser rejected whitespace after `:` / `,` ("bad number") —
     `src/rpc/json.cpp`; regression tests in `tests/test_rpc.cpp`.
  2. **Coinbase maturity in the wallet (M8-era bug):** the wallet had
     NO maturity handling — `getbalance` counted immature coinbases
     as spendable and coin selection could pick one, so the spend was
     rejected by consensus. Fixed: `ScanUtxo(utxo, nextHeight)`
     applies the exact consensus rule (100 blocks; genesis coinbase
     exempt); immature funds are excluded from balance/unspent and
     reported as `immature_balance` in `getwalletinfo`.
     Commit `7e32311`. New test section 7.
  3. Test-side fixes: "zoo" mnemonic helper (23 → 24 words),
     `getnewaddress` expectation (second address, not first), the
     unlock-deadline second-boundary race, the M8 tamper-test flake
     (fixed `0xFF` write → XOR), the `LoadFile` missing-file contract.
- **Funded manual E2E — DONE (was a stretch goal).** A small Python
  miner (`/tmp/oeeoo-m14-miner.py`, test difficulty only — refuses any
  other bits) mined 101 blocks via `getblocktemplate`/`submitblock`
  against a test genesis: at tip 101 the wallet showed balance 100.0
  (coinbases 1 and 2 mature for block 102) and `immature_balance`
  4950.0; `sendtoaddress` 10 OEEOO to a second wallet address
  returned a txid (first run `0280e14d…5a93a`; the regenerated test
  log's run `e6e814b3616c9f6a8c9f9f7f3ba3c518be3395a0ea6890a0ef8c4b7ab80430cc`)
  and the mempool grew to size 1. This run is what found bug 2 above.
- **Tests — DONE: 134 sections (127 prior + 7 new), ALL CHECKS PASSED,
  ctest 100%, cppcheck 0 findings, ASan/UBSan clean.** Full log:
  `logs/milestone14-test-log.txt` (675 lines, incl. both E2E
  transcripts; the createwallet test mnemonic is redacted). Commit
  `62cfc6f`.
- **Verify page updated** — `~/workspace/oeeoo-coin-verify.html` §19
  (M14) + §20 (134 sections) incl. the maturity fix and the funded
  send.
- **Docs updated** — `docs/MILESTONE14.md`, `docs/WALLET.md`
  (node-wallet section + maturity), `docs/DECISION_LOG.md` (M14
  entry), `README.md` (134 sections).

## PARTIAL / STILL OPEN (not M14's scope — see the roadmap)

- `listtransactions` / `gettransaction` — NOT IMPLEMENTED. The wallet
  library keeps no transaction history (UTXO scan only). Needs a
  wallet-tx journal + a format decision (payload v2 vs sidecar).
  Planned for M16.
- `walletpassphrasechange` (passphrase rotation) — NOT IMPLEMENTED.
  Small follow-up; include in M15 or M16, or leave as follow-up.
- Multi-wallet (`loadwallet` / `unloadwallet` / `listwallets`) —
  NOT IMPLEMENTED. Single `--wallet` is the M14 simplification.
- Stratum V2 — NOT IMPLEMENTED. Spec §13.1 requires Stratum V1 *and*
  V2; only V1 exists. Deferred pending an explicit user decision.
- M13 carry-overs (unchanged): RBF 100-replacement cap untested; RBF
  descendants unreachable in practice; disk-full behaviour unverified;
  modern address formats (SegWit/Taproot) REQUIRE DECISION and stay
  out of M14–M19.

## FAILED

- Nothing failed in M14. (One false start while verifying: `stdbuf`
  breaks the ASan binary at startup — its LD_PRELOAD puts libstdbuf
  before libasan. Run ASan WITHOUT `stdbuf`; the normal build is
  unaffected. Recorded in `docs/MILESTONE14.md` §F.)

## Test results (from `~/workspace/oeeoo-coin/`)

- `rm -rf build && cmake -S . -B build && cmake --build build -j4` —
  exit 0, warning-free.
- `./build/oeeoo_tests` — **134 sections, ALL CHECKS PASSED**.
- `ctest --test-dir build --output-on-failure` — **100% tests passed,
  0 tests failed out of 1**.
- `cppcheck --enable=warning,performance,portability
  --suppress=missingIncludeSystem -Isrc src tests` — **0 findings**.
- ASan/UBSan: build-asan (Debug, `-fsanitize=address,undefined
  -fno-sanitize-recover=all`), `ASAN_OPTIONS=detect_leaks=0
  ./build-asan/oeeoo_tests` — **134 sections, ALL CHECKS PASSED, no
  sanitizer errors**.
- Reproducible build (from the final commit `62cfc6f`,
  SOURCE_DATE_EPOCH=1793919946): **REPRODUCIBLE — all three binaries
  byte-identical across two builds; archive byte-identical.**
  oeeoo-node `5cc2616b5637affb323891fa1f324ffc3d591855969dee697754a82f032ea88e`,
  oeeoo-genesis `77d89e07432e08dfa1b8759e3023e48905c46e62a8b2fbfa3d6007c554e7c502`,
  oeeoo-cli `a80b3c6f0677814b3ee181f4f5b6bb9bee85f56171d71b964c621a79b8323136`,
  tarball `oeeoo-coin-0.1.0.tar.gz`
  `b3a0bb49080d8f0dc0585c3367d0fa858ce4e32d6d875588346bf918754f731f`.
  (No release published or tagged.)

## Commits (local git repo, working tree clean except this file)

- `6ab59e6` Roadmap to Bitcoin-level (M14–M19)
- `0fc8bc8` Refine roadmap with Core comparison research
- `a7033d3` Milestone 14: node wallet integration (locked-by-default)
- `09c29f7` Milestone 14: docs
- `5a347db` Milestone 14: test log (133 sections — superseded by the
  final log below)
- `7e32311` Milestone 14: coinbase maturity in the wallet
- `62cfc6f` Milestone 14: test log, final (134 sections, funded E2E)

## Verdict

**NOT READY** — unchanged from M13. M14 closes the biggest functional
gap (the node now has a working wallet, proven by a funded send), but
mainnet readiness still requires the owner's decisions: genesis
approval and an independent security audit. No mainnet, no release,
no tag.

## Decisions needed from the user in the morning

1. `walletpassphrasechange` — include in M15 or M16, or leave as a
   follow-up?
2. Wallet-tx journal format for `listtransactions`/`gettransaction`
   (M16): payload v2 vs sidecar file?
3. Multi-wallet support — future, or schedule it?
4. Stratum V2 — defer past launch (spec §13.1 requires it), or
   schedule it? Needs an explicit decision.
5. The standing owner items (unchanged): generate the 3 founder key
   pairs OFFLINE; commission the independent security audit; create
   the GitHub repo `OeeooCoin-blockchain-by-Ahmad` and push (local
   git has 11 commits ready to push); decide the launch time +
   approve the genesis parameters; register the 4 DNS seed domains;
   release-signing custody.

## Deliverables (local paths — uploads BLOCKED, see below)

- Verify page: `~/workspace/oeeoo-coin-verify.html` (§19 M14 + §20,
  134 sections, repro line).
- Test log: `~/workspace/oeeoo-coin/logs/milestone14-test-log.txt`
  (675 lines; committed as `62cfc6f`).
- Code zip: `~/workspace/oeeoo-coin-milestone14.zip` (1,192,949 bytes,
  196 files incl. the release tarball `oeeoo-coin-0.1.0.tar.gz`,
  SHA-256 `b3a0bb49080d8f0dc0585c3367d0fa858ce4e32d6d875588346bf918754f731f`).

**Uploads did NOT happen.** The upload command
(`/opt/hatch/bin/remote-storage upload-file --path …`) needs an
egress approval, and the approval timed out while the user was
asleep — the tool reported `approval_expired` / "egress approval
timed out for this invocation. The action was not performed."
Nothing was uploaded. To finish delivery in the morning, re-run the
same three upload commands (the approval card may still be pending
for the user to approve, or a fresh approval will be requested).
No file needs to be rebuilt: all three deliverables are final.

## First step for the morning

1. Re-run the three uploads above (approve the egress card when it
   appears) and give the user the three fresh links.
2. Read `docs/ROADMAP_BITCOIN_LEVEL.md` M15, then start M15 (P2P
   hardening & completeness).
