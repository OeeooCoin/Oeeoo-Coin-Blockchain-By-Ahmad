# Oeeoo Coin — Wallet

Status: wallet **library complete and tested** (Milestone 8, re-verified in
Milestone 13). The node binary (`oeeoo-node` v0.1.0) does **not** yet load
a wallet file: there is no `--wallet` flag and no wallet command-line tool,
so the RPC wallet methods (`getbalance`, `getnewaddress`, `sendtoaddress`,
`listunspent`, `getwalletinfo`) return error `-13` ("no wallet attached")
under `oeeoo-node` until a wallet-loading integration is built. This
document describes the wallet as implemented, so that integration — and
recovery — can be done correctly. Do not present the wallet as usable
through the node until that integration exists.

## What the wallet is

- **Legacy P2PKH only at launch.** No SegWit, no Taproot, no bech32.
  Addresses are Base58Check with version byte `0x0f`: 34 characters
  starting with `7`. (The `75Pk` prefix idea was dropped by owner
  decision in Milestone 8 — the second character varies with the key
  hash, so no universal `75` prefix exists.)
- **Keys:** secp256k1 / ECDSA, low-S signatures only (BIP62, added in
  Milestone 13). High-S signatures are rejected at verification.
- **Recovery phrase:** BIP39, **24 words by default**. The seed is
  PBKDF2-HMAC-SHA512, 2,048 iterations. Restore accepts the standard
  12/15/18/21/24-word lengths. An optional BIP39 passphrase creates a
  completely different wallet — the passphrase is part of the secret,
  not a password you can reset.
- **Derivation:** BIP32 + BIP44 path `m/44'/65535'/account'/change/index`.
  Coin type **65535 is provisional and unregistered** (SLIP-0044
  registration is an unresolved decision). Gap limit 20. Address reuse
  is allowed. Watch-only wallets can be restored from an account xpub.
- **Fees (policy, not consensus):** the wallet's default maximum fee
  rate is 100 sat/vB. The dust limit is dynamic and script-aware
  (Milestone 13): 182 sats for a P2PKH output at 1 sat/vB, 304 sats
  for a bare 2-of-3 multisig output. The old hardcoded 546-sat wallet
  limit was removed in Milestone 13.
- **Coin selection:** smallest UTXOs first (documented policy, not
  consensus).

## Wallet file format

`src/wallet/walletfile.*`. One encrypted file holds the mnemonic:

```
magic    "OEEOOWALLET1" (12 bytes)
salt     16 random bytes
nonce    12 random bytes
cipher   AES-256-GCM ciphertext
tag      16-byte GCM tag
```

- Key: PBKDF2-HMAC-SHA512(passphrase, salt, **200,000 iterations**, 32 bytes).
- Payload (v1): version, mnemonic, next external index, next change index.
- All randomness comes from `RAND_bytes` (OpenSSL).
- Corruption is rejected: wrong passphrase fails, any tampered byte
  fails the GCM tag check, and a truncated file is rejected by the
  size checks (all three tested in Milestones 8 and 13).
- **The file holds your mnemonic. Anyone with the file AND the
  passphrase owns the funds. Anyone with the file alone does not —
  but treat the file as a secret anyway.**

## Backup — do this before receiving funds

1. Write the 24 words down **on paper** (or stamp them on metal), in
   order, numbered 1–24. Check every word against the BIP39 English
   wordlist spelling — one wrong letter is a different wallet.
2. Keep **three copies in three separate safe places**, offline.
   The founder rule (3 offline backups) is the right model for any
   serious wallet.
3. If you set a BIP39 passphrase, write it down separately from the
   words — losing it loses the funds exactly as surely as losing the
   words.
4. Never type the words into a website, a chat, a screenshot, or a
   cloud note. Never store them in this repository, in a git commit,
   or in a backup that leaves your control. **Private keys and
   recovery phrases must never enter chat, code, or the cloud VM.**
5. Back up the encrypted wallet file too, but remember: the words
   are the real backup. The file is only a convenience copy.

## Restore

1. Take the 24 words (and the passphrase, if one was set) to any
   BIP39-compatible wallet, or to the Oeeoo wallet library.
2. Use derivation path `m/44'/65535'/account'/change/index` with coin
   type 65535. A standard BIP39 tool that does not let you set a
   custom coin type will derive the wrong addresses — this is the
   most common restore mistake. Verify the first address matches the
   one you recorded before sending funds.
3. The wallet scans ahead 20 unused addresses (gap limit) on each
   chain (external/change). Funds on an address beyond a 20-address
   unused gap will not be found by the scan — if you used more than
   20 consecutive fresh addresses, tell the restoring wallet to scan
   further.
4. Watch-only restore from an account xpub shows balances and
   addresses but cannot spend. Spending needs the mnemonic (or the
   private keys) — there is no way to spend from an xpub alone.

## Verification (test vectors)

- BIP39: `abandon`×11 + `about` (128-bit zero entropy),
  `zoo`×11 + `wrong` (128-bit one entropy), `abandon`×23 + `art`
  (256-bit zero entropy), `zoo`×23 + `vote` (256-bit one entropy),
  plus the `legal winner…` and `letter advice…` vectors.
  See `tests/test_wallet.cpp`.
- BIP32 test vector 1 (seed `000102…0f`): master private
  `e8f32e72…`, chain `873dff81…`. See `tests/test_wallet.cpp`.
- First address of the all-zero 24-word mnemonic:
  `7L88G1Not1Gv8nmci9mchhoN9XgbXTXqX5`.
- Wallet file: wrong passphrase, tampered byte, and truncation are
  all rejected (tests in `tests/test_wallet.cpp` and
  `tests/test_milestone13.cpp`).

## What is NOT decided yet

- SLIP-0044 coin-type registration (65535 is provisional).
- Modern address formats (SegWit/Taproot): explicitly NOT approved
  for launch; P2PKH only. Any change needs an owner decision.
- Node wallet integration (loading a wallet file in `oeeoo-node`,
  wallet CLI): not built yet.
