# Milestone 6 — P2P Networking (2026-11-06)

Status: **DONE — implemented, compiled, unit-tested** (ctest 100%, ALL CHECKS PASSED, 59 sections).

## What was built

- `src/network/protocol.h/.cpp` — wire framing: magic `e2 d8 04 d6`,
  12-byte command, uint32 length, 4-byte SHA256d checksum; max
  message 2 MB. Payload codecs: version, inv/getdata/notfound,
  ping/pong, getheaders/headers, addr, sendcmpct, reject.
- `src/network/siphash.*` — SipHash-2-4 (BIP152 short IDs), verified
  against the published reference vector (empty input, key
  00..0f → `0x726fdb47dd0e0e31`).
- `src/network/compact.*` — compact blocks (BIP152-style): header +
  nonce + 6-byte short IDs + prefilled txs; getblocktxn/blocktxn
  round-trip; reconstruction from the mempool.
- `src/network/peer.*` — peer state (handshake, compact support,
  ping, known inventory) and BanManager: score >= 100 bans the
  peer; first ban 24h, doubling per offence, capped at 30 days.
- `src/network/addrman.*` — address manager (manual peers + `addr`
  messages). DNS seeds do NOT exist yet (spec item 15) — no seed
  hostnames are invented; the list starts empty.
- `src/network/transport.*` — transport abstraction: LoopbackTransport
  (in-process, deterministic, used by tests) and TcpTransport (real
  POSIX sockets, non-blocking accept via select).
- `src/network/node.*` — the node: version/verack handshake,
  headers-first sync (getheaders → headers → getdata), tx validation
  + mempool + inv relay, full block validation (structure, PoW,
  difficulty, transactions, coinbase reward) + relay (cmpctblock to
  compact peers, inv to others), confirmed txs leave the mempool.
- `src/blockchain/chain.*` — added `GetBlockHeader` / `GetPrevHash`
  (needed to serve getheaders).

## Bugs found and fixed during this milestone (disclosed)

1. **Coinbase mis-detection (real bug, from Milestones 4–5).**
   `ComputeBlockFees` and `ConnectBlockTransactions` treated a
   transaction as coinbase only when `vin` was EMPTY. A real
   coinbase has ONE input (null outpoint), so both functions tried
   to validate it as a normal spend and failed — no real block
   could be accepted. Fixed with a proper `IsCoinbase()` rule
   (exactly one input, zero prev hash, index 0xffffffff) in
   `src/transactions/transaction.*`, used in both places. The
   Milestone 4 test that built an empty-vin "coinbase" was
   corrected to a real coinbase, and a regression test was added
   in `tests/test_mining.cpp`.
2. **Compact-block tx count.** ReconstructBlock derived the tx
   count from shortIds + prefilled; after merging a blocktxn
   answer into prefilled the count was wrong (out-of-bounds
   short-ID read). Fixed by storing an explicit `txCount` in the
   compact block (wire format updated) and treating prefilled as
   authoritative; the header's Merkle root still anchors integrity.
3. **getheaders hashStop cut the walk short.** The stop hash ended
   the backward walk before the locator was reached, so the peer
   answered nothing. The stop limit is now applied AFTER the walk.

## Rules locked here

- A block whose parent is unknown is NOT punished — the node asks
  for headers instead (we may simply be behind).
- Invalid blocks (bad PoW, bad bits, bad transactions, over-claimed
  reward) earn 100 misbehavior points = an immediate ban.
- Blocks are accepted only as tip extensions in this milestone
  (deep reorg handling belongs to the persistence milestone).
- No orphan pool: a transaction whose inputs are unknown is
  rejected (+10), not stored.
- getblocktxn uses ABSOLUTE indexes (not BIP152 differential
  encoding) — documented simplification of our own protocol.

## Not done (later milestones)

- P2P (this milestone) is done at the protocol/node level; a
  runnable node binary / CLI is Milestone 12 (mainnet prep).
- Database persistence (M7), wallet (M8), founder vesting
  enforcement (M9), RPC/Stratum (M10), audit (M11).
- DNS seeds / fallback peers: still unresolved (spec items 15–16),
  never invented.
- Peer eviction when > 125 peers: not implemented (no cap
  enforcement yet).

## Tests

`ctest` 100% passed; `./build/oeeoo_tests` → ALL CHECKS PASSED
(59 sections). Full command log: `logs/milestone6-test-log.txt`.
End-to-end tests: 3-node tx relay, compact block relay, fresh-node
headers-first sync, compact missing-tx round-trip, invalid-block
ban, real TCP handshake on localhost.
