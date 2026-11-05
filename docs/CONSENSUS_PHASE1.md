# Consensus — Phase 1 (what is implemented)

## Money

- `Amount` = `int64_t`, smallest units. `COIN = 100,000,000`.
- `MAX_MONEY = 2,100,000,000,000,000` (21M OEEOO). `MoneyRange()`
  rejects negatives and anything above `MAX_MONEY`.
- `OeeooToUnits()` / `AddMoney()` use `__int128` intermediates and
  fail instead of overflowing. No floating point anywhere for money.

## Subsidy — `GetBlockSubsidy(height)`

| Height | Subsidy |
|--------|---------|
| 0 (genesis) | 0 |
| 1–210,000 | 50 OEEOO (5,000,000,000) |
| 210,001–420,000 | 25 OEEOO (2,500,000,000) |
| 420,001–630,000 | 12.5 OEEOO (1,250,000,000) |
| 630,001–840,000 | 6.25 OEEOO (625,000,000) |
| 840,001–940,000 | 3.125 OEEOO (312,500,000) |
| 940,001+ | 0 |

Total: 10,500,000 + 5,250,000 + 2,625,000 + 1,312,500 + 312,500
= **20,000,000 OEEOO exactly**. Verified two ways in tests
(era arithmetic and a per-block sum over heights 1–940,000).

## Hashing

- `SHA256d(data) = SHA256(SHA256(data))`, via OpenSSL EVP.
- Test vectors (independently computed with Python hashlib):
  `SHA256d("") = 5df6e0e2761359d30a8275058e299fcc0381534545f55cf43e41983f5d4c9456`
  `SHA256d("abc") = 4f8b42c22dd3729b519ba6f68d2da7cc5b2d606d05daed5ad5128cc03e6c6358`

## Block header

- Fields: version (int32), prev hash (32 B), Merkle root (32 B),
  time (uint32), nBits (uint32), nonce (uint32).
- Serialization: little-endian integers, hashes in stored order,
  exactly 80 bytes, deterministic on every platform.
- `GetHash() = SHA256d(serialization)`. Hex is shown in raw byte
  order in Phase 1; the display convention is an open item (Decision
  Log #16) to lock with the genesis parameters before mainnet.
- This is a header only — full blocks, transactions, Merkle tree,
  difficulty validation, and chain selection are Milestones 2–3.

## Network identity (Phase 1, calculated)

- Magic bytes (candidate): `e2 d8 04 d6` — first 4 bytes of
  SHA-256("Oeeoo Coin Mainnet Network Magic").
- Port 64775. P2PKH version byte `0x0f` (verified `75` prefix,
  34-char addresses, `75Pk` achievable).
