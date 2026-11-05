# Milestone 7 — Database (2026-11-06)

Status: **DONE — implemented, compiled, unit-tested** (ctest 100%, ALL CHECKS PASSED, 67 sections).

## What was built

- `src/db/database.h/.cpp` — thin RAII SQLite wrapper: open/create,
  `journal_mode=WAL`, `synchronous=FULL`, prepared statements,
  transaction guard (auto-rollback unless committed), schema
  version via `PRAGMA user_version`.
- `src/db/chainstore.h/.cpp` — ChainStore, one SQLite file
  (`oeeoo-chain.db`): tables `meta`, `blocks` (hash, height, header,
  cumulative work, body, time), `utxo`, `peers`, `bans`. Schema
  version 1.
  - `ApplyBlock`: block + UTXO effects + tip move in ONE
    transaction — a crash can never leave a block without its UTXO
    updates.
  - `PruneBlocks`: drops block BODIES below a height; headers and
    UTXO stay (pruned node can still validate and serve headers).
  - Load-time integrity: stored bodies are re-checked (hash matches
    key, Merkle root + CheckBasic); stored headers are re-verified
    on index rebuild (parent linkage, expected bits, MTP, PoW, and
    recomputed cumulative work must match the stored work).
- `src/blockchain/chain.*` — added `GetChainWork` and `AddIndexEntry`
  (load-only index rebuild from stored headers, with the checks
  above; no 2-hour future rule on load, since the wall clock has
  moved on).
- `src/network/node.*` — `AttachStore` (accepted blocks persist
  atomically) and `LoadFromStore` (chain index + UTXO + unpruned
  bodies rebuilt at startup). A store failure never un-accepts a
  block; it is reported via `LastStoreError()` and the node re-syncs
  the gap from peers after restart.

## Schema decision (spec item: DB schema was unresolved)

SQLite was chosen over LevelDB/RocksDB because it is a single
file, needs no server, gives atomic multi-table transactions, and
is available on the build machine (3.45.1). The schema is versioned
(`user_version = 1`) so later milestones can migrate. This is an
engineering decision, not a spec change — the spec never fixed a
database engine.

## Known limitations (honest)

- **Reorg undo is NOT implemented.** ApplyBlock only moves the
  chain forward. If the tip reorgs to a competing branch, the
  store keeps the old branch's blocks (good) but the UTXO set is
  not rolled back to the fork point (bad). Undo records for
  reorg-safe UTXO updates are planned work (Milestone 11/12
  scope). A node that reorgs must currently reindex to be safe.
- **No full re-validation from genesis on load.** Load verifies
  structure, PoW, bits, MTP and work — it does not re-run every
  transaction's script from genesis. A full reindex is an audit
  step (Milestone 11).
- Mempool, in-flight sync state and peer connections are NOT
  persisted (by design — Bitcoin behaves the same for the mempool).

## Tests

`ctest` 100% passed; `./build/oeeoo_tests` → ALL CHECKS PASSED
(67 sections). Full command log: `logs/milestone7-test-log.txt`.
Covered: schema + tip meta, block/UTXO round-trip, node restart,
atomicity (rollback discards / commit keeps), pruning, corruption
detection (tampered body and tampered work both rejected), peers
and bans persistence, fork-block retention after a reorg.
