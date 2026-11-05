# Milestone 5 — Mining (2026-11-05)

Status: **DONE — implemented, compiled, unit-tested** (ctest 100%, ALL CHECKS PASSED, 45 sections).

## What was built

- `src/script/script.*` — added `EncodeScriptNum`, `DecodeScriptNum`
  and `GetCoinbaseHeight` (BIP34 height reading).
- `src/mining/miner.h/.cpp` — coinbase construction (BIP34: scriptSig
  starts with the block height; 2..100 bytes), coinbase validation
  (single input, zero prev hash, height match, total ≤ subsidy +
  fees — under-claim allowed, over-claim rejected), block-reward
  check, fee computation for a block, block-template assembly, nonce
  search with extraNonce rollover.
- `src/mempool/mempool.h/.cpp` — minimal mempool: validated entries
  only, 1 sat/vB minimum relay fee, duplicate/conflict rejection,
  fee-rate ordering (exact integer comparison), 300 MB default cap,
  14-day expiry.
- Block templates: mempool transactions in fee-rate order, validated
  one by one against a working UTXO copy, within the 1 MB block
  limit; coinbase pays subsidy + collected fees to the miner.

## Bug found and fixed during this milestone (disclosed)

`GetCoinbaseHeight` first parsed the whole coinbase scriptSig as
script. That fails once the extraNonce's raw bytes are appended,
because the parser reads them as a (truncated) push. Fixed by
reading only the first push — the rest of a coinbase scriptSig is
arbitrary miner data by design (Bitcoin rule).

## Rules locked here

- A coinbase may claim less than subsidy + fees, never more.
- Nonce search bumps the coinbase extraNonce (and recomputes the
  Merkle root) when the 32-bit nonce range is exhausted.
- Mempool entries must validate against the UTXO set at entry time;
  a transaction that becomes invalid later is skipped at template
  time (defensive path; not reachable through the mempool's own
  conflict rules in this milestone).

## Not done (later milestones)

- P2P networking (M6), database persistence (M7), wallet (M8),
  founder vesting enforcement (M9), RPC/Stratum (M10), audit (M11),
  genesis finalization (M12).
- RBF replacement and mempool eviction policy: not implemented.
- Stratum V1/V2 pool interfaces: Milestone 10.
- ChainState does not yet call CheckBlockReward on new blocks — that
  wiring belongs to full block connection (Milestone 7).

## Tests

`ctest` 100% passed; `./build/oeeoo_tests` → ALL CHECKS PASSED
(45 sections). Full command log: `logs/milestone5-test-log.txt`.
End-to-end test: a mined template block is accepted by ChainState.
