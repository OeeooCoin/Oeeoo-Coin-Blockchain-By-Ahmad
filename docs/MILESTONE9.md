# Milestone 9 — Founder vesting (2026-11-06)

Status: **DONE — implemented, compiled, unit-tested** (ctest 100%, ALL CHECKS PASSED, 76 sections).

## What was built

- `src/consensus/vesting.h/.cpp` — the vesting module:
  - UTC calendar math (days-from-civil / civil-from-days) and
    anniversary computation. Feb 29 launch → Feb 28 in non-leap
    years (spec §6).
  - `VestedAmount`: 100,000 at launch, +100,000 per calendar
    anniversary, capped at 1,000,000 at the 9th anniversary.
  - `CheckFounderSpend`: after a transaction, the founder vault
    must still hold at least the UNVESTED amount
    (`vault >= 1,000,000 - vested(blockTime)`). The check reads
    only the UTXO set + the block time, so it is reorg-safe: a
    reorg that removes a founder spend restores the vault.
  - `ValidateGenesisCoinbase`: with vesting enabled, the genesis
    coinbase must be exactly one output paying exactly
    FOUNDER_ALLOCATION to the founder's 2-of-3 multisig script,
    and its timestamp must equal the launch time (when set).
- `src/mining/miner.*` — `CheckBlockReward` allows the genesis
  coinbase to claim the founder allocation (instead of the
  subsidy, which is 0 at height 0) when vesting is enabled.
- `src/transactions/validation.*` — `ConnectBlockTransactions`
  enforces the genesis rule and the founder-spend rule for every
  transaction in a block (including the coinbase). Genesis
  coinbase outputs are exempt from the 100-block coinbase
  maturity: the genesis block can never be reorged away, and the
  founder vault must be spendable at launch (see Decisions).
- `src/mempool/mempool.*` — `AddTransaction` rejects founder
  spends that violate vesting (clock = current time).
- `src/blockchain/chain.*` — `ChainState` carries the vesting
  parameters; when the launch time is unset, the genesis block's
  timestamp is adopted as the launch time.
- `src/network/node.*` — `Node::SetVestingParams` forwards the
  parameters to the chain, block acceptance, and mempool paths.

## Decisions (recorded, not spec changes)

- **Enforcement by vault floor, not by tracking spends.** The
  rule is a balance floor on the founder script's UTXOs. No extra
  state, no spend history — reorg-safe by construction. A
  transaction that does not touch founder coins is never affected,
  and a chain whose genesis never funded the vault is not bricked.
- **Fees count against vesting.** A founder spend of 100,000 with
  a 1,000-sat fee moves 100,000.00001 out of the vault, so it
  exceeds a vested 100,000. The founder can spend at most
  vested − fee. (Found and fixed in testing: the first "valid"
  spend failed exactly on this.)
- **Genesis coinbase maturity exemption.** Mined coinbases still
  wait 100 blocks (M4 test updated to use a height-1 coinbase).
  Only the height-0 coinbase is exempt, because the genesis
  block is the chain's identity and cannot be reorged.
- **Founder script is a chain parameter, not code.** The 2-of-3
  multisig script comes from the founder's real keys at launch.
  Tests use throwaway keys only. No founder key ever enters code,
  logs, or prompts.
- **Address prefix (user decision 2026-11-06):** "75Pk" dropped.
  Version byte 0x0f is kept as the best available secure choice:
  distinct "7" prefix, no collision with Bitcoin ("1"), Litecoin
  ("L") or Dogecoin ("D") — prevents cross-chain confusion.

## Tests

`ctest` 100% passed; `./build/oeeoo_tests` → ALL CHECKS PASSED
(76 sections). Full command log: `logs/milestone9-test-log.txt`.
Covered: calendar math (leap years incl. 2000/1900/2100,
round-trips, 10 anniversaries of 2026-11-06, Feb 29 → Feb 28),
schedule (before launch / at launch / between / at 9th / capped),
genesis founder output + spend rule end-to-end on a Node
(over-spend rejected in mempool and in a block, 1-of-3 signature
rejected by the script, valid spend accepted, early second spend
rejected, post-anniversary spend accepted, exact vault balances
after each step), wrong genesis rejected (wrong script, wrong
amount, wrong timestamp), vesting-disabled chain unchanged.

## Known limitations

- The founder's 2-of-3 multisig keys do not exist yet — they are
  the user's launch-time responsibility (with 3 offline backups,
  per spec §6). The code only enforces the rules around whatever
  script is configured.
- The M7 reorg-undo gap still applies to the persistent store:
  the vesting rule itself is reorg-safe (it reads the live UTXO
  view), but after a reorg the stored UTXO set is stale until
  reindex (documented in MILESTONE7.md).
