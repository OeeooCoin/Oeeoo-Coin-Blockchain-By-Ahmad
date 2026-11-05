# Build Guide — Oeeoo Coin Phase 1

## Requirements

- C++17 compiler (g++ 11+ / Clang 14+)
- CMake 3.16+
- OpenSSL development libraries (`libssl-dev` on Debian/Ubuntu)
- SQLite3 development libraries (`libsqlite3-dev` on Debian/Ubuntu,
  Milestone 7 — chain store; runtime library alone is NOT enough,
  the header is required)

## Linux (tested here: Ubuntu 24.04, g++ 13.3, CMake 3.28, OpenSSL 3.0.13, SQLite 3.45.1)

```bash
sudo apt update && sudo apt install build-essential cmake libssl-dev libsqlite3-dev
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected result: `100% tests passed, 0 tests failed`.

To run the test binary directly with verbose output:

```bash
./build/oeeoo_tests
```

## Running the node (Milestone 12)

The build also produces three binaries: `oeeoo-node`,
`oeeoo-genesis`, `oeeoo-cli`.

```bash
# 1. Generate a genesis block (3 founder public keys, test difficulty):
./build/oeeoo-genesis --pubkey1 <hex> --pubkey2 <hex> --pubkey3 <hex> \
  --time 1793923200 --bits 207fffff --threads 2 --out genesis.json
# 2. Start the node (test pow limit matches the test genesis):
./build/oeeoo-node --datadir oeeoo-data --genesis-file genesis.json \
  --pow-limit-bits 207fffff
# 3. Ask it something:
./build/oeeoo-cli getblockcount
./build/oeeoo-cli getblockchaininfo
```

Mainnet uses the default pow limit (0x1d00ffff) — never pass
`--pow-limit-bits` for a real chain. A fresh data directory
refuses to start without a genesis file. See
`docs/MAINNET_LAUNCH.md` for the launch checklist.

## Android / Termux (initial mobile development)

Install from an official distribution source:

```bash
pkg install clang cmake ninja git openssl
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Notes:

- Termux is for initial development and testing only. A Termux build
  is NOT equivalent to a production Linux node build; release builds
  must be reproduced on a controlled Linux environment.
- Never put private keys, passwords, or recovery phrases in Termux,
  in a cloud dev environment, or in an AI prompt.
- If mobile resources become insufficient, move the build to a
  desktop/cloud Linux machine — same C++, same protocol, no changes
  to the finalized decisions.

## What a passing build proves — and what it does not

- Proves: the code compiles, the subsidy math is exact, the hash
  vectors match, header serialization is deterministic.
- Does NOT prove: security, consensus correctness of unbuilt modules,
  or production readiness. Independent review and audit are still
  required (Milestone 11) before any mainnet launch.
