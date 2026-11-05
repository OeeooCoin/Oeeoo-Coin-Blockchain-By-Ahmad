# Milestone 3 — Core Blockchain (2026-11-05)

Status: **DONE — implemented, compiled, unit-tested** (ctest 100%, ALL CHECKS PASSED, 27 sections).

## What was built

- `src/consensus/uint256.h/.cpp` — minimal 256-bit unsigned integer
  (LE uint32 limbs): compare, add, subtract, multiply/divide by uint32,
  exact 256/256 division (binary long division), big-endian bytes and
  hex. No Boost dependency (Boost is not installed on the build VM).
- `src/consensus/difficulty.h/.cpp` — compact `nBits` encoding/decoding
  (Bitcoin rules), proof-of-work check, Bitcoin-style retarget, block
  proof (chain work).
- `src/blockchain/block.h/.cpp` — full `Block` (header + transactions):
  serialization, deserialization, `CheckBasic` (size ≤ 1 MB, non-empty,
  Merkle root, no duplicate txids, per-tx basic validity).
- `src/blockchain/genesis.h` — Genesis parameters structure. The
  genesis block is **NOT finalized**: `finalized = false`, `time = 0`
  until launch. Message constant: "THINK ABOUT GOD ? IS ONLY ONE ALLAH".
- `src/blockchain/chain.h/.cpp` — in-memory `ChainState`: block index,
  validation (parent linkage, height, Median-Time-Past, 2-hour future
  rule, expected difficulty incl. retarget, proof of work), cumulative
  work, tip selection by greatest cumulative valid PoW (automatic
  reorg when a heavier branch appears).
- `src/transactions/transaction.*` — added an offset-based
  `Deserialize` overload so `Block::Deserialize` can parse transactions
  out of a block buffer. The original whole-buffer overload now
  delegates to it; all Milestone 2 vectors still pass unchanged.

## Conventions locked here

- Target and hash are compared as **big-endian** 256-bit numbers
  (byte 0 most significant): PoW holds iff `hash <= target`.
- `ChainState` takes an optional proof-of-work limit (default: mainnet
  `0x1d00ffff`). Tests pass an easy limit (`0x207fffff`) so blocks can
  be mined in microseconds. Production must use the default.
- Consensus never reads the wall clock: `AddBlock` takes `nowSeconds`
  as a parameter, so tests are deterministic.
- Coinbase rules (first tx must be a coinbase, reward checks) are
  **Milestone 5**, not here. `Block::CheckBasic` is structural only.

## Verified vectors (all computed independently in Python first)

- Bits → target: `0x1d00ffff` → `00000000ffff0000…`,
  `0x1b0404cb` → `00000000000404cb…`, `0x207fffff` → `7fffff0000…`,
  `0x1c00ffff` → `0000000000ffff00…`; all round-trip exactly.
- Invalid bits rejected: sign bit set, zero mantissa, exponent > 32.
- PoW boundary: hash == target passes, hash == target + 1 fails.
- Retarget (mainnet limit): exact 1,209,600s → `0x1d00ffff` unchanged;
  604,800s → `0x1c7fff80`; 302,400s → `0x1c3fffc0`; 100,000s (clamped)
  → `0x1c3fffc0`; 4,838,400s and 10,000,000s (clamped) → `0x1d00ffff`.
- Retarget (test limit): 604,800s → `0x203fffff`; 302,400s → `0x201fffff`.
- Block proof: `0x1d00ffff` → `…000100010001` (4,295,032,833);
  `0x207fffff` → 2.
- Chain: genesis + 5 blocks, tip height 5, work 12; a heavier fork
  (7 blocks, work 14) reorgs the tip to height 6.
- Rejections tested: bad PoW, wrong bits, unknown parent, duplicate
  block, zero-time genesis, non-zero-prev genesis, future timestamp
  (> now + 2h), timestamp at/below Median-Time-Past, Merkle tamper,
  duplicate txid, empty block, oversize block (> 1 MB).
- Retarget at height 2016, chain-level: exact-timespan chain keeps
  `0x207fffff`; fast chain (+300s/block, timespan 604,500s) requires
  `0x203ff7de` — old bits rejected, new bits accepted.

## Bug found and fixed during this milestone (disclosed)

Retarget arithmetic first multiplied the target by the timespan and
then divided — with the easy test target (~2^255) that product
overflows 256 bits and the code clamped to the limit, giving wrong
bits. Fixed by decomposing the division (`target = q*T + r`,
`newTarget = q*actual + (r*actual)/T`), which is mathematically
identical and never overflows for valid inputs. The mainnet case was
never affected (targets ≤ 2^224), but the fix is in the code for all
limits. See `difficulty.cpp` comments.

## Not done (later milestones)

- Coinbase construction and reward validation (M5).
- Transaction/UTXO validation, scripts, signatures (M4).
- P2P networking (M6), database persistence (M7), wallet (M8),
  founder vesting enforcement (M9), RPC/Stratum (M10), audit (M11),
  genesis finalization (M12).

## Tests

`ctest` 100% passed; `./build/oeeoo_tests` → ALL CHECKS PASSED
(27 sections). Full command log: `logs/milestone3-test-log.txt`.
