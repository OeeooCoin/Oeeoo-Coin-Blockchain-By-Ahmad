# Milestone 2 — Cryptography and Serialization (DONE 2026-11-05)

## What was built

- **VarInt (CompactSize)** — `src/serialize/varint.*`: minimal encoding
  (1/3/5/9 bytes); decoding rejects non-minimal encodings and
  truncated input.
- **Little-endian rules** — `src/serialize/endian.h`: the single place
  byte order lives. All integers in blocks/transactions are LE.
- **Transactions** — `src/transactions/transaction.*`: version, inputs
  (prev hash, index, scriptSig, sequence), outputs (value, scriptPubKey),
  lockTime; deterministic serialization; txid = SHA256d(serialization);
  structural `CheckBasic()` (non-empty vin/vout, MoneyRange outputs,
  output total within max supply, no duplicate inputs, size ≤ 1 MB).
  Script execution and signature checks are Milestone 4, not here.
- **Merkle tree** — `src/blockchain/merkle.*`: Bitcoin-style pairing,
  odd hash duplicated, 1 leaf = the leaf, empty = all-zero root.
- **Base58Check** — `src/crypto/base58.*`: encode/decode with checksum
  verification and invalid-character rejection.
- **HASH160** — `src/crypto/hash160.*`: RIPEMD-160(SHA-256) via OpenSSL
  EVP + legacy provider (no hand-rolled RIPEMD-160).
- **P2PKH addresses** — `src/crypto/address.*`: version byte `0x0f`
  + HASH160, Base58Check. Full BIP32/BIP39 wallet is Milestone 8.

## Verified test vectors (independently computed with Python)

- Tx serialization (92 bytes, version 1, 1-in/1-out, 50 OEEOO):
  txid `9ac2b1bac217553b606a716a3917d3a65b25b4ea2f08ccc1bc2534a5736be8c5`
- Merkle roots for leaves SHA256d("tx0")…: 1 leaf `ba7b78fe…`,
  2 leaves `f7ed2a56…`, 3 leaves `e7238981…`, 5 leaves `dbbb0e2d…`
- Addresses (version `0x0f`): hash160 `11…11` →
  `74dHhhkwZiebrbdmmb5wEtZvMGUwwWBGpT`; hash160
  `197a166292b7c3d067d0aed78e74c4a883d1465c` →
  `75PkvD7AKStxvaDSyQH9gtC9kTStqPnPxV` (the requested `75Pk` prefix,
  achieved); HASH160 of compressed pubkey `02`+`0x22`×32 →
  `4377a5acd66dc5cb67148a24818d1e51fa183bd2` →
  `79DnNiKicRvs6S7DakR4Bp3xsNa9LF46J6`
- VarInt boundaries: 252=`fc`, 253=`fdfd00`, 65535=`fdffff`,
  65536=`fe00000100`, 4294967295=`feffffffff`

Note: not every `0x0f` address starts with `75` — the prefix depends
on the hash160 too. `75Pk` is achievable (vector above), which is what
the specification asked to test.

## Test result

`ctest` → 100% tests passed. `./build/oeeoo_tests` → ALL CHECKS PASSED
(9 sections: amount, subsidy, crypto, block, varint, transaction,
merkle, address + rejections).
