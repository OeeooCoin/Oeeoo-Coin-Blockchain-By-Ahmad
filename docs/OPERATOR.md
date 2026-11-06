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
- `--persistmempool` (default on): the mempool is saved to
  `<datadir>/mempool.dat` on clean shutdown and reloaded on the
  next start (every loaded transaction is re-validated against the
  current UTXO set; expired or invalid entries are dropped).
  `--persistmempool=0` / `--no-persistmempool` opts out of the
  automatic save/load. `savemempool` (RPC) dumps the mempool on
  demand even when persistence is off; `importmempool` loads a
  mempool file into the live pool; `getmempoolinfo` reports a
  `loaded` flag (true once the startup load attempt finished).
- **Outbound peering (Milestone 18): the node dials out.** On every
  poll the node runs a dial loop over its address book (filled by
  DNS seeds, `addr` messages from peers, and `--addnode` / the
  `addnode` RPC), opening outbound connections up to 10 peers
  (plus the 125-peer total cap). A banned address is never dialed;
  the node never dials its own listen address or a peer it is
  already connected to. A failed dial counts against the address
  (3 failures and it is dropped as dead), and dial passes are
  throttled to one every 5 seconds, so a dead address is not spun
  on. When the address book empties out entirely, the node
  re-asks its configured DNS seeds (at most once per 10 minutes).
- `--addnode <ip:port>` (repeatable; config key `addnode`): a
  manually added peer. It goes into the address book, so the dial
  loop dials it at startup and keeps it. A malformed `--addnode`
  refuses startup loudly. The `addnode` RPC does the same at
  runtime: `addnode <ip:port> add|remove|onetry` (Bitcoin-shaped;
  `remove` also disconnects a live peer at that address, `onetry`
  dials once without adding the address to the book).
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
- Resource limits: a fixed pool of 16 RPC worker threads fed
  from a 64-deep work queue (Core's defaults; tune with the
  `rpcthreads` / `rpcworkqueue` config keys or the
  `--rpcthreads` / `--rpcworkqueue` flags). When the queue is
  full a request is answered with HTTP 500 ("RPC work queue is
  full") and the server keeps running. Requests are capped at
  64 KB of headers and 4 MB of body; JSON nesting is capped at
  64 levels (deeper payloads are rejected, not parsed). There is
  no per-client rate limit — Core has none either; the bound is
  the pool + queue. Do not expose the port.
- No RPC method returns a private key, mnemonic, seed, or xprv.
  There is no `dumpprivkey` / `dumpwallet`. The wallet methods
  return only addresses, balances, and transaction ids. When a
  wallet is attached with `--wallet` (see `docs/WALLET.md`),
  `getbalance` / `listunspent` / `getwalletinfo` /
  `listtransactions` / `gettransaction` work while locked
  (watch-only history); `getnewaddress` / `sendtoaddress` refuse
  while locked, and `walletpassphrasechange` requires the wallet
  to be unlocked.
- The RPC method list (36 callable methods plus `help` itself,
  including the new `getchaintips`, `getblockstats`,
  `getchaintxstats`, `listtransactions`, `gettransaction`, and
  `walletpassphrasechange`) is printed by `oeeoo-cli help`.

## Stratum security

- Stratum binds **127.0.0.1 only** (port 64777) and starts only when
  `--miner-address` is set. `mining.authorize` accepts any credentials
  — that is safe only because the port is localhost-only: it is
  reachable solely from the machine the node runs on, so there is no
  remote miner to authenticate. Never expose port 64777 beyond
  localhost. If you run a miner, run it on the same machine as the
  node and point it at `127.0.0.1:64777`. If Stratum ever listens on a
  non-loopback address, `mining.authorize` must gain real credential
  checking first. See `docs/MINING.md`.

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
  empty address book learns peers from `--dns-seed` entries you
  configure, from `--addnode` peers, or from peers that connect to
  it (their `addr` messages advertise further addresses, which
  the dial loop then dials).
- Bans are keyed by IP alone and are checked on inbound
  connections too, so a banned peer cannot reconnect from a new
  ephemeral port (see `docs/MILESTONE16.md`). Bans are NOT
  persisted across restarts: a restart clears the ban list.
