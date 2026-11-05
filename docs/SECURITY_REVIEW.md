# Oeeoo Coin — Security Review (self-review, Milestone 11)

**This is NOT an independent audit.** It is the project's own
threat model, invariants, and known gaps, written so an external
auditor can start from a true picture instead of discovering it.
Nothing here should be read as "the code is safe". The
independent audit (Milestone 12 / launch prep) is still required
before any mainnet.

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
  parser level (Milestone 11); the live peer state machine is
  not fuzzed yet.
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
  cookie auth (constant-time compare), thread-per-connection
  (max 32), all 29 methods (incl. `estimatesmartfee`).
- `src/rpc/stratum.*` — line-delimited JSON (64 KB line cap),
  job building, share validation, vardiff.
- `src/db/` — SQLite store (schema v2, undo table), PRAGMA
  journal_mode=WAL + synchronous=FULL.
- `src/wallet/` — BIP39/BIP32/BIP44, encrypted wallet file,
  coin selection (smallest UTXOs first), fee estimation.
- `src/mempool/feeestimator.*` — median fee rate over the last
  24 blocks (exact integer arithmetic, minimum 1 sat/vB).

## Known gaps (honest list)

- **No independent audit yet.** This document is self-review.
  (Milestone 13 was a self-audit, not an independent audit.)
- **Reorg across pre-v2 blocks fails closed** (no undo
  records for blocks stored under schema v1). It refuses
  rather than corrupting state, but a node with an old store
  cannot follow a deep reorg until it re-indexes.
- **Reorg undo is tested to depth 2** (main chain 3 vs fork
  4). Deeper reorgs use the same code path, but a 100-block
  reorg has not been exercised.
- **No orphan transaction pool**; a transaction whose parents
  are unknown is rejected outright.
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
  There is also no per-client RPC rate limit (only the
  32-connection cap).
- **No `getchaintips` / `getblockstats`**; pruned history
  cannot be re-validated (by design — bodies are required).
- **Fuzzing covers parsers, not the live peer/RPC/Stratum
  state machines.**
- **Deep verification is now the node default** (Milestone 13).
  A light load trusts the stored UTXO table (it checks only for
  duplicate coins and value ranges); a tampered UTXO is caught
  only by the deep replay. `--no-deep-verify` opts out — an
  operator who uses it accepts that trust model.
- **No distinct testnet identity.** Test and mainnet nodes share
  the magic, port, protocol version, and user agent. The only guard
  is the testing-mode banner. (Milestone 13.)
- **Inbound peer IPs are not recorded**, so bans on inbound peers
  do not survive a reconnect. (Milestone 13.)
- **RBF descendant handling is defense-in-depth** (unreachable in
  practice: validation rejects unconfirmed inputs) and the
  100-replacement cap is untested. (Milestone 13.)
- **Disk-full behaviour is not verified.** (Milestone 13.)

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
