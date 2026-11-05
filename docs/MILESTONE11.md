# Milestone 11 — Audit Prep (2026-11-06)

Status: **DONE — implemented, compiled, unit-tested** (ctest 100%, ALL CHECKS PASSED, 93 sections).

This milestone closes the two known correctness gaps from
Milestone 7 and prepares the codebase for an independent security
audit. It does NOT audit the code itself — an independent auditor
must still be hired by the user (see docs/AUDIT_PACKAGE.md).

## What was built

- `src/transactions/validation.*` — `BlockUndo` (every input a
  block consumed, with values, and every output it created),
  `BuildBlockUndo` (from the pre-block UTXO view) and
  `DisconnectBlockTransactions` (the exact inverse of connecting
  a block, fail-closed: added coins must still be present with
  the recorded values, spent coins must not already exist).
- `src/db/chainstore.*` — schema **v2** (migration 1 → 2 creates
  the `undo` table; fresh files are created at v2). `ApplyBlock`
  now takes the undo record and stores it atomically with the
  block, the UTXO changes, and the tip. New `DisconnectBlock`
  (reverses the tip block only, moves the tip to its parent) and
  `GetUndo`. Undo records are serialized in a strict,
  bounds-checked binary format.
- `src/network/node.*` — **reorg-aware `AcceptBlock`**: a block
  extending the tip takes the direct path (unchanged); a block on
  a side branch disconnects the tip back to the fork point on a
  working copy, validates and connects the side branch in order
  (with per-block Median-Time-Past computed from the fork
  point's ancestors plus the earlier side blocks), and adopts it
  only if it has strictly more cumulative work. Otherwise the
  block is stored as an inactive side branch. On a reorg the
  store follows (disconnect tip-first, then apply the side
  branch), and the mempool gets the disconnected transactions
  back (if still valid) while the loser's conflicting ones are
  dropped. Duplicate blocks are a no-op (previously they were
  re-validated and punished).
- `src/blockchain/verify.*` — `VerifyChainFromStore`: full
  re-validation from the genesis block — headers (parent, MTP,
  difficulty bits, proof of work, recomputed cumulative work),
  every transaction (fees, coinbase reward, founder vesting),
  and an exact comparison of the rebuilt UTXO set against the
  store's UTXO table. A pruned history fails loudly (bodies are
  required). `Node::LoadFromStore` gains a `deepVerify` flag;
  the light load (headers + tip + UTXO table) is unchanged.
  Vesting launch time is adopted from the stored genesis block
  on load, mirroring live acceptance.
- `tests/test_fuzz.cpp` — deterministic mutation fuzzing (fixed
  seed): 1,500 mutated transactions, 1,000 mutated blocks,
  500 mutated headers, 1,500 mutated JSON texts, 1,000 mutated
  P2P messages, 1,000 random scripts — none may crash the
  parser, validator, or interpreter.
- Static analysis: **cppcheck 2.13.0** on `src/` (warning,
  performance, portability): **0 errors, 0 warnings**, 2 style
  notes (see below) — after fixing 4 performance findings and
  2 compiler warnings the run itself surfaced (see below).

## Decisions (recorded, not spec changes)

- **Undo records are the reorg mechanism**, not re-indexing:
  every applied block stores exactly what it consumed and
  created, so a rollback is a single atomic store transaction.
- **The store only ever disconnects the tip** (fail-closed).
  A reorg is a sequence of tip disconnects, oldest side block
  first on the way back in.
- **A side branch is validated against the fork-point UTXO
  view**, never against the current tip's view (the old code
  validated side-branch blocks against the tip's view, which is
  wrong).
- **Equal work does not reorg** (strictly greater cumulative
  work only) — a tie keeps the first-seen chain.
- **Deep verification is opt-in at load** (`deepVerify`), not
  the default: it replays every block and is slower than the
  light load. Operators run it after a crash, a restore, or
  before an audit.

## Bugs found and fixed (disclosed)

- **Side-branch blocks were connected to the wrong UTXO set.**
  Before this milestone, `AcceptBlock` connected EVERY
  accepted block's transactions to the current tip's UTXO set —
  even a block on a side branch. A side-branch block could
  spend coins that only exist on the main chain (or fail to
  spend coins that only exist on its own branch), silently
  corrupting the UTXO set. Fixed by the reorg-aware path above.
- **Duplicate blocks were punished.** Re-accepting an already
  known block failed validation (its outputs already exist) and
  earned the sender misbehavior points. Now a duplicate is a
  no-op success.

## Static analysis findings (reviewed)

cppcheck 2.13.0, `--enable=warning,performance,portability`:
after the fixes below, 2 style notes remain, both **benign, no
change made**:
- `LoopbackTransport::~LoopbackTransport` and
  `TcpTransport::~TcpTransport` call the virtual `Close()` from
  the destructor. Dynamic dispatch is not used there, but both
  classes are leaf classes with no further subclasses — the call
  always resolves to the class's own `Close()`, which is the
  intended behavior (close the socket/registry entry on
  destruction). No bug.

Findings fixed in this milestone (the first cppcheck run
surfaced them; a second run confirms they are gone):
- `rpcserver.cpp`: `header = header.substr(0, headerEnd)` —
  ineffective self-assign; replaced with `header.resize(...)`.
- `stratum.cpp`: a response helper took its `Json` result by
  value (a copy per call); now `const Json&`.
- `wallet.cpp` (x2): `find` + `operator[]` insert replaced with
  a single `try_emplace`.
- Compiler warnings (pre-existing, found while re-running the
  build with warnings shown): unused `port` parameter in
  `LoopbackTransport::Listen` (name omitted) and an unused
  `Target target` variable in `getblockchaininfo` (removed).
  Four test-file warnings fixed too: an unused `Node&`
  parameter in a test helper, a signed-integer overflow in a
  vesting test constant (`2077920000 + 86400*365*5` exceeded
  INT_MAX — the wrapped value happened to be right, the
  arithmetic is now unsigned), two unchecked `system()` calls
  (now CHECKed), and an unused test helper. The build is
  warning-free again.

## Tests

`ctest` 100% passed; `./build/oeeoo_tests` → ALL CHECKS PASSED
(93 sections). Full command log: `logs/milestone11-test-log.txt`.
Covered: undo/disconnect restores the UTXO set exactly (incl. a
tampered undo record refused); store disconnect rolls the tip
back and deletes the undo record; a full node reorg (main chain
3 blocks vs fork 4 blocks — the fork wins, the store follows,
the loser's output is gone, the winner's is present, the
non-conflicting disconnected transaction returns to the
mempool, the conflicting one is dropped); deep verification of
a good 102-block chain; tampered UTXO set caught (light load
still passes — only deep verification catches it); pruned
history fails deep verification; empty store fails; ~6,000
fuzz mutations with no crash.

## Known limitations

- A reorg that reaches back before a restart needs the undo
  records in the store's `undo` table — present for every block
  applied since schema v2. A store migrated from v1 has NO undo
  records for its old blocks: a reorg across a pre-migration
  block fails with "no undo record" instead of corrupting state
  (fail-closed, documented). Re-indexing from genesis rebuilds
  them; see docs/AUDIT_PACKAGE.md.
- Deep verification requires unpruned block bodies.
- No fuzzing of the RPC/Stratum network paths yet (the parsers
  are fuzzed; the servers are not).
- The audit itself is external and has not happened.
