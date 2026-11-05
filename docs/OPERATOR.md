# Oeeoo Coin — Node Operator Guide

## Running the node

Build first (see `docs/BUILD_GUIDE.md`). Then:

```
oeeoo-node --datadir oeeoo-data --genesis-file genesis.json
```

- `--datadir` (default `oeeoo-data`): the chain database
  (`chain.db`), the RPC cookie (`.cookie`), and the config file live here.
- A **fresh data directory refuses to start without a genesis file**:
  generate one with `oeeoo-genesis` (see `docs/MAINNET_LAUNCH.md` step 4)
  and pass `--genesis-file <path>`. On restart the chain is loaded from
  the store and the genesis file is not needed — but if you pass one, it
  must match the stored genesis exactly or the node refuses to start.
- `--genesis-checkpoint <64-hex>`: the published mainnet genesis hash.
  The node refuses to start if the genesis file (or the stored genesis)
  does not equal the checkpoint. Set this to the approved, published
  hash on every mainnet node.
- `--pow-limit-bits` is a **testing-only** flag (default `0x1d00ffff`,
  the mainnet limit). Passing any other value prints
  `*** WARNING: TESTING MODE — … This node is NOT on Mainnet. ***`
  in the banner. A node showing that warning is not on mainnet.
- `--maxpeers` (default 125): maximum peer connections (inbound +
  outbound). `--maxmempool <MB>` (default 300): mempool size cap.
- `--no-deep-verify`: skips the full chain replay at startup. **The
  default is to deep-verify**: on every start the node replays the
  chain from the genesis block and compares the resulting UTXO set
  with the stored one, entry by entry. That is what catches a tampered
  UTXO table (a light load only checks for duplicate coins). Use
  `--no-deep-verify` only on very tall chains where you accept trusting
  the stored UTXO table, and say so when you do.
- `--dns-seed <hostname>` (repeatable): DNS seeds to query when the
  address manager is empty. **The list is empty by default — no seed
  hostnames are invented.** A failing seed never stops the others.
- `--miner-address <P2PKH>`: enables the Stratum server (see
  `docs/MINING.md`). Without it, Stratum stays off.
- Full options: `oeeoo-node --help`. A config file (`--config`, or
  `oeeoo.conf` in the data directory) takes the same keys; see
  `src/app/config.cpp` for the exact key names.

## RPC security — read this before opening anything

- RPC binds **127.0.0.1 only** (port 64776 by default). It is not
  reachable from the network, and there is no flag to change that.
  **Do not put a proxy, tunnel, or port forward in front of it**
  unless you understand that you are then responsible for its
  authentication.
- **Cookie authentication is mandatory.** On startup the node writes
  32 random bytes (hex) to `<datadir>/.cookie` with mode `0600`.
  Every RPC call must present it as HTTP Basic auth
  (`__cookie__:<hex>`). `oeeoo-cli` reads the cookie file itself.
  There is no username/password login, no default password, and no
  unauthenticated mode in the node binary (the auth-off switch exists
  only in the test suite).
- There is **no TLS** on RPC. On localhost that is the intended
  design; over any network it would expose the cookie.
- Resource limits: at most 32 concurrent RPC connections; requests
  are capped at 64 KB of headers and 4 MB of body; JSON nesting is
  capped at 64 levels (deeper payloads are rejected, not parsed).
  There is no per-client rate limit beyond the connection cap — do
  not expose the port.
- No RPC method returns a private key, mnemonic, seed, or xprv.
  There is no `dumpprivkey` / `dumpwallet`. The wallet methods
  return only addresses, balances, and transaction ids — and under
  `oeeoo-node` v0.1.0 they return error `-13` because no wallet is
  attached (see `docs/WALLET.md`).
- The RPC method list (29 methods in v0.1.0, including the new
  `estimatesmartfee`) is printed by `oeeoo-cli help`.

## Stratum security

- Stratum binds **127.0.0.1 only** (port 64777) and starts only when
  `--miner-address` is set. `mining.authorize` accepts any credentials
  — that is safe only because the port is localhost-only. Never expose
  it. See `docs/MINING.md`.

## Backup and restore

- **Chain database:** stop the node first, then copy the whole data
  directory (`chain.db` plus its `-wal`/`-shm` files if present).
  Never copy `chain.db` while the node is running — SQLite WAL mode
  means a live copy can be inconsistent. To restore, put the copy
  back and start the node; the default deep verification replays the
  chain and reports any mismatch. A pruned node cannot be fully
  revalidated (bodies below the prune height are gone by design);
  the verifier fails loudly on pruned history instead of silently
  passing it.
- **Wallet file:** back up the encrypted wallet file and — more
  importantly — the 24 words on paper (see `docs/WALLET.md`). The
  words are the real backup.
- **Founder keys:** the 3 founder private keys must live offline,
  in 3 separate places, and must never enter this repository, a chat,
  or the cloud VM. Only the 3 public keys are needed to build the
  genesis block.
- **What to do after a crash:** just restart the node. The store is
  WAL + `synchronous=FULL` with atomic block application, so a crash
  mid-block cannot leave a half-applied block. If the node reports a
  corrupt store, restore from backup — do not hand-edit the database.
  (Crash-recovery and unclean-shutdown behaviour are tested in
  `tests/test_milestone13.cpp`: a store that is never closed still
  loads correctly.)

## Known limitations an operator must know

- No independent security audit has been done. Do not run mainnet
  until one is complete (see `docs/MAINNET_LAUNCH.md`).
- The genesis parameters are not finalized or approved. Until they
  are, every node is a test node.
- There is no distinct testnet identity: test and mainnet nodes share
  the magic, port, protocol version, and user agent. They peer with
  each other and reject each other's blocks only at consensus. The
  testing-mode banner is the only guard — read it.
- DNS seeds do not exist yet (list is empty). A new node with an
  empty address book learns peers only from `--dns-seed` entries you
  configure or from peers you connect to manually.
- Inbound peers are identified by their transport id, not their IP,
  so a ban on an inbound peer does not survive a reconnect.
