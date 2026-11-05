# Milestone 12 — Mainnet Prep (DONE 2026-11-06)

The last milestone of the 12. The code is now a runnable system:
a node binary, a genesis generator, and an RPC client — plus the
launch checklist in `docs/MAINNET_LAUNCH.md`.

## What was built

### oeeoo-node (src/app/node_main.cpp)
- Reads `oeeoo.conf` (key = value) and CLI flags; CLI overrides
  the file. Flags: `--datadir`, `--port`, `--rpcport`,
  `--stratumport`, `--miner-address`, `--genesis-file`,
  `--founder-script`, `--pow-limit-bits` (testing only),
  `--deep-verify`, `--maxpeers`, `--help`, `--version`.
- Opens the SQLite chain store (`<datadir>/chain.db`), loads the
  chain (with optional deep verification), and refuses to start a
  fresh chain without a genesis file: "Generate one with
  oeeoo-genesis and pass --genesis-file <path>."
- Vesting parameters are resolved before the load, in order:
  genesis file > `--founder-script` > the stored genesis block
  (its coinbase output, when it pays exactly 1,000,000).
- Starts P2P listening, the JSON-RPC server (cookie auth), and
  the Stratum server when a miner address is given.
- Graceful shutdown on SIGINT/SIGTERM or the RPC `stop` method.

### oeeoo-genesis (src/app/genesis_main.cpp + src/app/genesis.*)
- Takes 3 compressed secp256k1 public keys, builds the founder
  2-of-3 multisig script, and mines the genesis block paying
  exactly 1,000,000 OEEOO to that vault, with the spec-fixed
  message "THINK ABOUT GOD ? IS ONLY ONE ALLAH".
- Parallel nonce search (threads split the nonce space; the
  first valid nonce wins). The genesis file (JSON) holds the
  serialized block, hash, Merkle root, nonce, time, bits,
  founder script, and message. `ReadGenesisFile` fully verifies
  a file: deserialization, hash, proof of work, and the founder
  rule (ValidateGenesisCoinbase).
- The tool never sees a private key — only public keys.

### oeeoo-cli (src/app/cli_main.cpp + src/app/cli.*)
- `oeeoo-cli [--datadir] [--rpcport] <method> [params-json]`.
  Reads the cookie, POSTs to the local RPC server, prints the
  JSON response, exits non-zero on an RPC error.

### Mining speed (midstate optimization)
- The first version of the genesis miner hashed every nonce with
  the EVP-based Sha256d: ~0.15 MH/s per core on this VM — a
  mainnet-difficulty genesis (2^32 hashes) would take ~6 hours.
- The miner now uses a SHA-256 midstate: the first 64-byte block
  of the header is compressed once and copied per nonce, so each
  nonce costs 2 compressions instead of 3 (~2x faster on this
  VM; much faster on a machine whose OpenSSL uses SHA-NI).
- Measured on this VM: a real mainnet-difficulty genesis is
  still not practical here (hours). It must be mined on a
  faster machine at launch, or the difficulty decision revisited
  by the user before launch (see MAINNET_LAUNCH.md).

### Testing-only pow-limit flag
- `--pow-limit-bits` lets a local test node accept a test-difficulty
  genesis (e.g. 207fffff). Default is the mainnet limit
  (0x1d00ffff). This is a local-testing convenience (like
  Bitcoin's -regtest), NOT a mainnet option: mainnet always uses
  the default.

## Bugs found and fixed (disclosed)

1. **Console hash doubling (genesis_main.cpp).** The printed
   genesis hash was 128 hex chars: `genesis.GetHash().begin()`
   and `genesis.GetHash().end()` were called on two different
   temporaries, so the vector spanned both 32-byte arrays.
   The genesis FILE was always correct (it uses a single call).
   Fixed by printing `genesis.header.GetHashHex()`.
2. **Test expectations vs. M11 duplicate rule (test_genesis.cpp).**
   The test expected a second genesis broadcast to fail, but
   M11 made duplicates a no-op success. Fixed the test to assert
   the no-op (tip unchanged). Also fixed a wrong expectation:
   2-of-2 multisig is valid, not an error.
3. **Node banner buffered (node_main.cpp).** The startup banner
   never appeared in a redirected log until shutdown (block
   buffering). Fixed with an explicit flush.

## End-to-end verification (this VM, 2026-11-06)
- `oeeoo-genesis` (test bits 207fffff, 3 random pubkeys):
  hash `2ee2bc4bee136c8bc30e22b298c9b5f9ed883b96c0afdfcb0db5d537c8323faa`,
  nonce 5, founder vault 1,000,000 OEEOO.
- `oeeoo-node --genesis-file ... --pow-limit-bits 207fffff`:
  "Genesis accepted", height 0, vesting enabled.
- `oeeoo-cli getblockcount` → 0; `getbestblockhash` → the
  genesis hash; `getblockchaininfo` → chain "main", blocks 0.
- Restart without the genesis file: the stored chain loads,
  `getblockcount` → 0 again.
- Fresh data dir without a genesis file: the node refuses to
  start (correct).
- A mainnet-bits (0x1d00ffff) genesis was started and killed:
  the node correctly rejects a test-bits genesis when the
  pow limit is the mainnet default ("genesis bits must equal
  the proof-of-work limit") — the difficulty enforcement works.

## Tests
- New: test_config (4 sections), test_genesis (3 sections),
  test_cli (1 section). Total: **101 sections, ALL CHECKS PASSED**
  (ctest 100%).

## Known limitations
- No public testnet (by design); no DNS seeds, no fallback peers
  (unresolved spec items — the operator must add them before
  launch, see MAINNET_LAUNCH.md).
- No release signing yet (custody is the user's); no reproducible
  build script yet (documented in MAINNET_LAUNCH.md).
- The independent security audit has NOT happened (M11 was prep
  only). No mainnet without it.
- The genesis block is NOT finalized: the test genesis above is
  a demonstration. The real one is generated at launch.
