# Third-Party Licenses

Oeeoo Coin v0.1.0 depends on exactly two third-party libraries at
runtime, plus its build tooling. Everything else in the repository is
original code under the project's MIT license (see `LICENSE`).

## Runtime dependencies

### OpenSSL — Apache License 2.0

- What it is used for: SHA-256 / SHA-256d, RIPEMD-160 (legacy
  provider), secp256k1 ECDSA signing and verification, HMAC-SHA512
  (BIP32/BIP39), PBKDF2, AES-256-GCM (wallet file), `RAND_bytes`
  (all secure randomness).
- Version verified with: OpenSSL 3.0.13 (system package).
- License: Apache License, version 2.0.
  Full text: https://www.openssl.org/source/license.html
  (the `LICENSE` file shipped with the OpenSSL source distribution).
- Note: RIPEMD-160 is only available through OpenSSL's legacy
  provider, which the code loads explicitly
  (`src/crypto/hash160.cpp`). That is a deliberate, documented use of
  a deprecated algorithm for the HASH160 address construction — it
  does not affect the security of the signatures.

### SQLite — Public Domain

- What it is used for: the chain store (`src/db/*`) — blocks,
  headers, UTXO set, undo records, peers, bans. WAL mode with
  `synchronous=FULL`.
- Version verified with: SQLite 3.45.1 (system package, `libsqlite3-dev`).
- License: public domain. The SQLite authors dedicate the code to the
  public domain (the "blessing" in the SQLite source:
  "May you do good and not evil. May you find forgiveness for
  yourself and forgive others. May you share freely, never taking
  more than you give."). No attribution is required; this note is
  kept for completeness.
  See https://sqlite.org/copyright.html

### BIP39 English wordlist — MIT (bitcoin/bips)

- What it is used for: the 2,048-word English mnemonic wordlist
  (`src/wallet/bip39_english.txt`, embedded as
  `src/wallet/bip39_words.h`). Its SHA-256 is
  `2f5eed53a4727b4bf8880d8f3f199efc90e58503646d9ff8eff3a2ed3b24dbda`.
- Source: `bitcoin/bips`, `bip-0039/english.txt`. The bitcoin/bips
  repository is MIT-licensed.

## Build-time tooling (not shipped in the binaries)

- **CMake** (3.28 verified) — BSD 3-Clause license.
- **A C++ compiler** (g++ 13.3 verified) — GCC, GPLv3 with the
  runtime library exception. The compiled binaries are not GPL
  encumbered by the compiler.
- **cppcheck** (2.13.0, static analysis only) — GPLv3. It analyzes
  the code; none of its code ships in Oeeoo Coin.
- **git** (reproducible-build provenance only) — GPLv2.

## What is NOT a dependency

- No Boost, no LevelDB/RocksDB, no external JSON library, no
  external test framework. The JSON parser, the test harness, the
  256-bit integer arithmetic, and the SipHash implementation are
  original code in this repository.
- No vendored copies of third-party code are shipped except the
  BIP39 wordlist above.
