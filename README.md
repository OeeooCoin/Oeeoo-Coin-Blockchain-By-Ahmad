# Oeeoo Coin (OEEOO)

Independent decentralized Proof-of-Work blockchain, Bitcoin-style.
C++ implementation, MIT License. Planned repository:
`OeeooCoin-blockchain-by-Ahmad` (not yet created — see Decision Log).

**Status: Phase 1 + Milestones 2–12 DONE (all 12 milestones). IMPLEMENTED +
COMPILED + UNIT-TESTED on this machine, with runnable binaries
(oeeoo-node, oeeoo-genesis, oeeoo-cli). NOT production-ready.
NO mainnet. NO genesis block yet. No independent security review
or audit has been done. Launch checklist: docs/MAINNET_LAUNCH.md.**

## Finalized policy (do not change without community process)

- Max supply: **21,000,000 OEEOO** — 20,000,000 mining + 1,000,000 founder
- 1 OEEOO = 100,000,000 smallest units (`COIN`); integer arithmetic only
- Consensus: Proof of Work, SHA-256d, 10-minute target block time
- Reward schedule: 50 / 25 / 12.5 / 6.25 / 3.125 OEEOO per era of
  210,000 blocks, except the last era (840,001–940,000, 100,000 blocks);
  **0 subsidy from block 940,001**
- Max block size: 1 MB; difficulty retarget every 2,016 blocks
- Mainnet port: 64775; launch addresses: legacy P2PKH only
- Founder: 1,000,000 OEEOO, 10 × 100,000 tranches, yearly vesting,
  2-of-3 multisig wallet

## What Milestone 2 adds (2026-11-05)

- `src/serialize/` — VarInt (CompactSize) + little-endian rules
- `src/transactions/` — transaction serialization, txid, structural checks
- `src/blockchain/merkle.*` — Bitcoin-style Merkle root
- `src/crypto/` — Base58Check, HASH160 (RIPEMD-160+SHA-256), P2PKH addresses
- `docs/MILESTONE2.md` — vectors and scope

## What Milestone 3 adds (2026-11-05)

- `src/consensus/uint256.*` — 256-bit integer (no Boost needed)
- `src/consensus/difficulty.*` — nBits/target, PoW check, retarget, block proof
- `src/blockchain/block.*` — full block (header + txs), structural checks
- `src/blockchain/chain.*` — chain index, validation, cumulative work, reorg
- `src/blockchain/genesis.h` — genesis params structure (**not finalized**)
- `docs/MILESTONE3.md` — vectors and scope

## What Milestone 4 adds (2026-11-05)

- `src/crypto/ecdsa.*` — ECDSA secp256k1 (sign/verify, key generation)
- `src/transactions/sighash.*` — legacy SIGHASH_ALL
- `src/script/script.*` — Script interpreter (P2PKH, multisig, CLTV, CSV)
- `src/utxo/coins.*` — in-memory UTXO set
- `src/transactions/validation.*` — finality, BIP68, maturity, fees, block connection
- `docs/MILESTONE4.md` — rules and scope

## What Milestone 5 adds (2026-11-05)

- `src/mining/miner.*` — coinbase, block templates, nonce search, reward validation
- `src/mempool/mempool.*` — validated mempool, fee-rate order, expiry
- `src/script/script.*` — script numbers + BIP34 coinbase height
- `docs/MILESTONE5.md` — rules and scope

## What Milestone 6 adds (2026-11-06)

- `src/network/protocol.*` — wire framing + message codecs
- `src/network/siphash.*`, `src/network/compact.*` — SipHash-2-4, compact blocks
- `src/network/peer.*`, `src/network/addrman.*` — scoring/bans, addresses
- `src/network/transport.*`, `src/network/node.*` — loopback + TCP, the node
- `docs/MILESTONE6.md` — rules and scope

## What Milestone 7 adds (2026-11-06)

- `src/db/database.*`, `src/db/chainstore.*` — SQLite wrapper + chain store
- `src/blockchain/chain.*` — `AddIndexEntry` (load-time index rebuild)
- `src/network/node.*` — `AttachStore` / `LoadFromStore`
- `docs/MILESTONE7.md` — rules, schema decision, known gaps

## What Milestone 8 adds (2026-11-06)

- `src/wallet/bip39.*`, `src/wallet/bip39_words.h`, `src/wallet/bip39_english.txt` — BIP39 mnemonics (official wordlist)
- `src/wallet/bip32.*` — BIP32 HD keys (Oeeoo xprv/xpub versions)
- `src/wallet/wallet.*` — wallet: BIP44, gap limit 20, balance, payments
- `src/wallet/walletfile.*` — encrypted wallet file (PBKDF2 + AES-256-GCM)
- `docs/MILESTONE8.md` — rules, decisions, the honest "75Pk" finding

## What Milestone 9 adds (2026-11-06)

- `src/consensus/vesting.*` — vesting schedule, calendar math, founder-spend rule, genesis rule
- `src/mining/miner.*`, `src/transactions/validation.*`, `src/mempool/mempool.*`, `src/blockchain/chain.*`, `src/network/node.*` — vesting enforcement wired through
- `docs/MILESTONE9.md` — rules, decisions, known gaps

## What Milestone 10 adds (2026-11-06)

- `src/rpc/json.*` — minimal dependency-free JSON (value, parser, serializer)
- `src/rpc/rpcserver.*` — JSON-RPC 2.0 over HTTP (127.0.0.1:64776, cookie auth, 28 methods)
- `src/rpc/stratum.*` — Stratum v1 mining server (127.0.0.1:64777, subscribe/authorize/notify/submit, vardiff)
- `src/blockchain/merkle.*` — MerkleBranch for Stratum jobs; `src/blockchain/chain.*` — GetNextBits, GetBlockHashAtHeight, GetMedianTimePastAtHeight; `src/network/node.*` — GetBlock, GetPeers
- `docs/MILESTONE10.md` — method list, decisions, known gaps

## What Milestone 11 adds (2026-11-06)

- `src/transactions/validation.*` — BlockUndo, BuildBlockUndo, DisconnectBlockTransactions
- `src/db/chainstore.*` — schema v2 + undo table + DisconnectBlock + GetUndo (migration 1 → 2)
- `src/network/node.*` — reorg-aware AcceptBlock (fork-point disconnect, adopt only on more work)
- `src/blockchain/verify.*` — VerifyChainFromStore (full re-validation from genesis)
- `tests/test_reorg.cpp`, `tests/test_verify.cpp`, `tests/test_fuzz.cpp` — reorg, deep verification, mutation fuzzing
- `docs/MILESTONE11.md`, `docs/SECURITY_REVIEW.md`, `docs/AUDIT_PACKAGE.md` — audit prep docs

## What Milestone 12 adds (2026-11-06) — mainnet prep

- `src/app/node_main.cpp` — the runnable `oeeoo-node` binary (config file + CLI, store, P2P + RPC + Stratum, graceful shutdown)
- `src/app/genesis.*` + `src/app/genesis_main.cpp` — the `oeeoo-genesis` tool (founder 2-of-3 multisig, parallel midstate mining, genesis JSON file)
- `src/app/cli.*` + `src/app/cli_main.cpp` — the `oeeoo-cli` RPC client
- `src/app/config.*` — `oeeoo.conf` parser + CLI arguments
- `tests/test_config.cpp`, `tests/test_genesis.cpp`, `tests/test_cli.cpp`
- `docs/MILESTONE12.md`, `docs/MAINNET_LAUNCH.md` — the launch checklist (audit, repo, keys, seeds, release)

## Launch support (2026-11-06, after Milestone 12)

- `src/network/dnsseed.*` — DNS seed resolution (IPv4, max 100 per seed); `Node::QueryDnsSeeds` fills the AddrMan; config `dnsseed` / CLI `--dns-seed` (repeatable, empty by default — no hostnames invented)
- `scripts/reproducible-build.sh` — builds twice, verifies byte-identical binaries, creates the release tarball + checksums
- `docs/RELEASE.md` — reproducible builds and DNS seeds; `tests/test_dnsseed.cpp`

## What Phase 1 contains

- `src/consensus/` — consensus constants, integer money type, subsidy
  schedule (`GetBlockSubsidy`), total-issuance verification
- `src/crypto/` — SHA-256 / SHA-256d via OpenSSL EVP
- `src/blockchain/` — 80-byte block header, deterministic little-endian
  serialization, header hash
- `tests/` — automated tests: money arithmetic, every subsidy boundary,
  exact 20,000,000 OEEOO total issuance, SHA-256d test vectors,
  block-header serialization round-trip
- `docs/DECISION_LOG.md` — finalized decisions vs. unresolved items
  (SLIP-0044, genesis parameters, DNS seeds, script opcodes, …)

## Build and test (Linux)

```bash
sudo apt install build-essential cmake libssl-dev   # Debian/Ubuntu once
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: `100% tests passed, 0 tests failed`. See `docs/BUILD_GUIDE.md`
for Termux / Android notes.

## Verified Phase 1 calculations (not invented)

- Network magic candidate: SHA-256("Oeeoo Coin Mainnet Network Magic")
  = `e2d804d6…`, first 4 bytes `e2 d8 04 d6` (see `src/consensus/consensus.h`)
- P2PKH version byte `0x0f` verified by brute-force search: it is the
  only version byte producing a `75` prefix, addresses are 34 chars,
  and `75Pk…` is achievable (format example in the Decision Log —
  that example is a random payload, not a real funded address).

## What is NOT done yet

Genesis block (hash/nonce/timestamp not finalized), transactions/UTXO,
script, mempool, P2P, database, wallet, mining, RPC, founder vesting,
Stratum, governance signalling. Those are Milestones 2–12.
