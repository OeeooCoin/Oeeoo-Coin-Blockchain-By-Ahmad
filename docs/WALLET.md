# Oeeoo Coin — Wallet

Status: wallet **library complete and tested** (Milestone 8, re-verified in
Milestone 13) and **integrated into the node** (Milestone 14):
`oeeoo-node` loads a wallet file with `--wallet <path>`, and the RPC
wallet methods work end to end (see "Node wallet" below). Without
`--wallet`, the RPC wallet methods return error `-13` ("wallet not
loaded"), as before.

## Node wallet (Milestone 14)

Start the node with a wallet file:

```
oeeoo-node --datadir <dir> --genesis-file <genesis.json> \
    --wallet wallet.dat
```

A relative `--wallet` path resolves against the data directory. The
banner prints a `wallet:` line: `<path> (locked)` when a file is
loaded, `<path> (not created yet — use createwallet RPC)` when the
path does not exist yet.

**Locked by default (Bitcoin model).** The wallet loads LOCKED. The
passphrase is NEVER a CLI flag and NEVER stored in a file the node
reads silently (it would be visible in `ps` / on disk). It travels
only over localhost RPC, lives in memory only while unlocked, and is
wiped on lock. The node never logs it.

| RPC method | What it does |
|---|---|
| `createwallet ["<passphrase>"]` | Creates a new wallet file at the `--wallet` path (fails if a wallet is already loaded, or if the file exists). Returns the 24-word mnemonic **once** plus a warning — write it down immediately; the node never stores it. The wallet is left LOCKED. Needs a `--wallet` path configured, otherwise error -13. |
| `walletpassphrase ["<passphrase>", <timeout-seconds>]` | Unlocks the wallet in memory for `timeout` seconds (`0` = until `walletlock`). Wrong passphrase → error **-14** ("incorrect passphrase"). |
| `walletlock []` | Locks immediately: persists the address indices, then wipes the mnemonic, keys, and passphrase from memory. |
| `getbalance []` | Works while locked (watch-only view). Before the wallet has been unlocked at least once since the node started, the locked view is empty (the keys live inside the encrypted file) and the balance reads 0. |
| `listunspent []` | Works while locked. Each entry has `spendable`: `false` while locked, `true` while unlocked. |
| `getwalletinfo []` | Works while locked. `unlocked_until` is the unix time the unlock expires, `0` when locked. `immature_balance` is the coinbase funds that are not spendable yet (see below). |
| `getnewaddress []` | **Refused while locked** (error -13, "wallet is locked"): deriving an address advances the file's indices, which can only be persisted while the passphrase is in memory. |
| `sendtoaddress ["<address>", "<amount>"]` | **Refused while locked** (error -13). While unlocked it builds, signs, and broadcasts; the change index is persisted *before* the transaction is broadcast, so a save failure cannot strand a broadcast transaction. |
| `getblocktemplate` | Without a `coinbaseaddress` parameter it derives a miner address from the wallet — refused while locked. |

Other node-wallet rules:

- **One wallet per node.** Only a single `--wallet` file is supported
  in M14; multi-wallet (`loadwallet` / `unloadwallet`) is future work.
- **BIP44 account 0 only.** The file format stores no account index.
- **The BIP39 passphrase of a node wallet is always empty.** The file
  passphrase is the only secret the node handles — it is a *different*
  secret from a BIP39 passphrase. A wallet created elsewhere with a
  non-empty BIP39 passphrase cannot be used by the node (unlock would
  derive the wrong keys).
- **Saves are atomic.** The file is written to `<path>.tmp` and then
  renamed over `<path>`, so a crash mid-save cannot leave a
  half-written wallet file. The format itself is unchanged (see
  below).
- **Unlock timeout auto-relocks.** When the deadline passes, the next
  use re-locks the wallet lazily; a locked wallet's balance stays
  visible (the account xpub is cached after the first unlock).
- **Coinbase maturity.** A mined coinbase output is spendable only
  after 100 blocks (the consensus rule; the genesis coinbase at
  height 0 is exempt). `getbalance` counts only mature funds;
  immature coinbase funds are reported separately as
  `immature_balance` in `getwalletinfo`, and coin selection never
  picks them — a payment that only immature funds could cover fails
  cleanly with "insufficient funds" instead of being rejected by
  consensus. (M8-era gap, found by the M14 funded E2E and fixed in
  M14; see `docs/MILESTONE14.md` §C.7.)
- **Shutdown** saves the indices and locks the wallet. A save failure
  at shutdown prints a warning instead of being swallowed.
- There is no `walletpassphrasechange` yet (passphrase rotation is a
  follow-up, not in M14) and no `listtransactions` / `gettransaction`
  yet (the wallet keeps no transaction history — that needs a tx
  journal, planned for M16).

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
- Wallet transaction history (`listtransactions` /
  `gettransaction`): needs a wallet-tx journal and a format decision
  (payload v2 vs sidecar) — planned for M16.
- Passphrase rotation (`walletpassphrasechange`) and multi-wallet
  support: follow-ups, not in M14.
