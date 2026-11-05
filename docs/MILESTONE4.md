# Milestone 4 — Transactions and UTXO (2026-11-05)

Status: **DONE — implemented, compiled, unit-tested** (ctest 100%, ALL CHECKS PASSED, 40 sections).

## What was built

- `src/crypto/ecdsa.h/.cpp` — ECDSA on secp256k1 via OpenSSL:
  key generation, compressed public keys, DER signatures, verification,
  HASH160 of public keys. (OpenSSL 3.0 deprecates the low-level EC API;
  the calls are still correct — deprecation warnings are silenced for
  this file only.)
- `src/transactions/sighash.h/.cpp` — legacy sighash, **SIGHASH_ALL
  only**. Other hash types are rejected by the interpreter.
- `src/script/script.h/.cpp` — Script interpreter, Bitcoin-style subset:
  pushdata, OP_0..OP_16, OP_1NEGATE, OP_NOP, OP_VERIFY, OP_DROP,
  OP_DUP, OP_HASH160, OP_EQUAL, OP_EQUALVERIFY, OP_CHECKSIG,
  OP_CHECKSIGVERIFY, OP_CHECKMULTISIG, OP_CHECKMULTISIGVERIFY,
  OP_CHECKLOCKTIMEVERIFY (CLTV), OP_CHECKSEQUENCEVERIFY (CSV).
  Standard builders: P2PKH and bare multisig (scriptPubKey/scriptSig).
  Limits: script ≤ 10,000 bytes, stack ≤ 1,000 elements, element ≤ 520
  bytes, ≤ 201 counted opcodes, multisig ≤ 20 pubkeys, scriptSig must
  be push-only.
- `src/utxo/coins.h/.cpp` — in-memory UTXO set: outpoint → coin
  (output, creation height, creation block time, coinbase flag).
- `src/transactions/validation.h/.cpp` — `IsFinalTx` (nLockTime),
  BIP68 sequence locks, `ValidateTransaction` (input existence,
  coinbase maturity 100 blocks, value balance, fee, per-input script
  verification), `ConnectBlockTransactions` (validates and applies a
  block's transactions in order, commits only if all succeed;
  coinbase outputs enter the set without validation — coinbase
  construction/reward checks are Milestone 5).

## Rules locked here

- A transaction with nLockTime = H is final from height **H+1**
  (strict less-than, Bitcoin rule) — the first test run caught my own
  wrong expectation here; the implementation follows Bitcoin.
- BIP68: height locks need block height **greater than**
  coin height + masked sequence; time locks need MTP **at or after**
  coin block time + masked × 512s. Version < 2 or the disable flag
  turns the lock off.
- CLTV: tx lockTime must be ≥ the script value, same type
  (height vs time), and the input sequence must not be final. CSV:
  tx version ≥ 2, same type flag, masked sequence value ≤ masked
  input sequence. Neither opcode pops its value.
- CHECKMULTISIG consumes the dummy element (must be empty) and
  matches signatures to pubkeys in order.
- OP_RETURN is rejected: arbitrary-data outputs stay disabled
  (spec §8.3). Conditionals, arithmetic and OP_CODESEPARATOR are not
  supported in this milestone.

## Not done (later milestones)

- Coinbase construction and reward validation (M5).
- P2P networking (M6), database persistence of the UTXO set (M7),
  wallet (M8), founder vesting enforcement (M9), RPC/Stratum (M10),
  audit (M11), genesis finalization (M12).
- Dust rule: still unresolved (spec §10.3 / item 12) — no dust check
  in validation yet.
- Sighash types other than SIGHASH_ALL: not implemented.

## Tests

`ctest` 100% passed; `./build/oeeoo_tests` → ALL CHECKS PASSED
(40 sections). Full command log: `logs/milestone4-test-log.txt`.
Real ECDSA signatures are used throughout the script and validation
tests (keys generated in-test, never stored).
