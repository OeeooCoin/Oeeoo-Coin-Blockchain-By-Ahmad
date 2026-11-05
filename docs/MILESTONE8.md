# Milestone 8 — Wallet (2026-11-06)

Status: **DONE — implemented, compiled, unit-tested** (ctest 100%, ALL CHECKS PASSED, 72 sections).

## What was built

- `src/wallet/bip39.h/.cpp` — BIP39 mnemonics. The wordlist is the
  official BIP39 English list (2048 words, embedded in
  `bip39_words.h`; source text kept at
  `src/wallet/bip39_english.txt`, SHA-256
  `2f5eed53…b24dbda`). 24 words by default (256-bit entropy).
  Seed = PBKDF2-HMAC-SHA512, 2048 iterations, salt
  "mnemonic" + passphrase — exactly the BIP39 rule, so an Oeeoo
  mnemonic restores in any BIP39 tool.
- `src/wallet/bip32.h/.cpp` — BIP32 HD keys: master key, hardened
  and normal derivation, neutered (public-only) derivation, path
  parsing (`m/44'/65535'/0'/0/0`), extended-key serialization.
- `src/wallet/wallet.h/.cpp` — Wallet: BIP44 layout
  `m/44'/65535'/account'/change/index`, gap limit 20, balance and
  UTXO scanning, coin selection, build + sign payments, watch-only
  (xpub) mode.
- `src/wallet/walletfile.h/.cpp` — encrypted wallet file:
  PBKDF2-HMAC-SHA512 (200,000 iterations) + AES-256-GCM. A wrong
  passphrase fails the GCM tag check; a tampered file fails too.
- `src/utxo/coins.*` — added `CoinsView::GetAllCoins` (needed for
  wallet scanning).

## Decisions (recorded, not spec changes)

- **Coin type 65535** (BIP44 path) is provisional and UNREGISTERED
  (spec item; the code uses it, nothing claims it is assigned).
- **Extended-key version bytes are Oeeoo-specific** (NOT Bitcoin's
  xprv/xpub), so an Oeeoo key cannot be mistaken for a Bitcoin key:
  private `0x0488C3E4` → prefix `xq6p…`, public `0x0488B7E4` →
  prefix `xpyE…`.
- **Dust rule is provisional policy** (spec §10.3 unresolved):
  change below 546 sats is added to the fee; payments below 546
  sats are refused. Not consensus.
- **Coin selection**: smallest UTXOs first (keeps large coins
  intact). Documented policy, not consensus.
- **Wallet max fee rate 100 sat/vB** (spec policy, not consensus);
  the default fee rate for a payment is 1 sat/vB.

## Honest finding — the "75Pk" address prefix

The spec asked for a "75Pk" prefix as a target to test, not assume.
Tested and found: version byte `0x0f` gives 34-character P2PKH
addresses starting with "7", but the second character depends on
the key's hash — with 10 random keys the prefixes were
7L, 7N, 7K, 78, 7C, 7H, 79, 7Q… "75" appears only for some keys
(the M2 example `75PkvD7AKStxvaDSyQH9gtC9kTStqPnPxV` was found by
brute-forcing a hash for that prefix). A universal "75Pk" prefix
for EVERY address is mathematically impossible with one version
byte; it would need a per-address vanity grind, which breaks
standard BIP44 recovery. The version byte is correct; the prefix
is per-address. No code change hides this.

## Tests

`ctest` 100% passed; `./build/oeeoo_tests` → ALL CHECKS PASSED
(72 sections). Full command log: `logs/milestone8-test-log.txt`.
Covered: 7 BIP39 mnemonic vectors + 2 seed vectors (independently
computed in Python), BIP32 official test vector 1 (all 6 paths:
priv/chain/pub), neutered derivation, hardened-from-public
rejection, serialize/parse round-trips, wallet addresses (first
address `7L88G1Not1Gv8nmci9mchhoN9XgbXTXqX5` cross-checked with an
independent pure-Python secp256k1 implementation), balance, gap
limit, build + sign a payment (validated against the UTXO set),
fee cap, dust and watch-only refusals, encrypted wallet file
(round-trip, wrong passphrase, tampering, missing file).

## Security notes

- The wallet holds private keys in memory and NEVER logs them.
- The mnemonic/seed/xprv must never enter chat, code, or logs.
- The encrypted wallet file protects the mnemonic at rest, but
  whoever holds the file AND the passphrase owns the funds.
- No audit has been done. This wallet is not yet safe for real funds.
