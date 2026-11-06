# Oeeoo Coin — Mining

## Consensus facts a miner needs

- Proof of work: SHA-256d (double SHA-256), 80-byte header.
- Block time target: 600 seconds. Difficulty retargets every 2,016
  blocks (Bitcoin-style, 4x clamp).
- Rewards: 50 OEEOO per block for blocks 1–210,000, then 25, 12.5,
  6.25, 3.125, ending at block 940,000. Total mining issuance is
  exactly 20,000,000 OEEOO. Coinbase maturity: 100 blocks.
- Maximum block size: 1,000,000 bytes. Maximum block weight is the
  same as the size (no witness discount — P2PKH only).
- Minimum relay fee: 1 sat/vB. The coinbase pays subsidy + collected
  fees; over-claiming is rejected on every validation path.
- Mainnet PoW limit bits: `0x1d00ffff`. Test difficulty (`0x207fffff`)
  is a testing-only value — never mine or accept it on a real chain.

## Solo mining through the node (getblocktemplate)

1. Build and start the node (see `docs/BUILD_GUIDE.md`, "Running the
   node"). The node must have a chain — at minimum the genesis block.
2. Ask the node for a block template over RPC (cookie auth, see
   `docs/OPERATOR.md`):
   ```
   oeeoo-cli getblocktemplate
   ```
   The template is built from the mempool in fee-rate order, re-validated
   against a working UTXO view, and capped at 1 MB. The coinbase pays
   the `coinbaseaddress` you pass, or the attached wallet's new address
   (no wallet is attached to `oeeoo-node` v0.1.0 — see `docs/WALLET.md` —
   so pass `coinbaseaddress` explicitly).
3. Mine the template: find a nonce whose header hash meets the target.
   The built-in miner (`src/mining/miner.*`) is a CPU miner: it searches
   nonces, rolls the extraNonce when the nonce space is exhausted, and
   builds the coinbase with a BIP34 height push. Submit the solved block
   with:
   ```
   oeeoo-cli submitblock <block-hex>
   ```
   `null` means accepted; a `"rejected: <error>"` string means the block
   failed validation.
4. **Honest performance note:** on the reference VM (4 vCPUs, ~1.5 GHz)
   the midstate miner manages ~0.17 MH/s single-threaded. At the
   mainnet limit (`0x1d00ffff`, ~2^32 hashes per block) that is **hours
   per block** — mine the real genesis and run mainnet mining on a
   faster machine, or with more hardware. Do not claim a laptop can
   mine mainnet competitively.

## Pooled mining (Stratum)

The node speaks Stratum v1 on **127.0.0.1:64777** (localhost only).

`mining.authorize` accepts any username and password. This is by
design, not an oversight: the Stratum server binds **127.0.0.1
only** (port 64777) and is reachable only from the local machine,
so there is no remote party to authenticate. Never expose port
64777 beyond localhost — do not port-forward it, do not bind it to
a public interface, and do not put the node on a network where
untrusted local users can reach it, because anyone who can reach
the port can mine as the node. If Stratum ever listens on a
non-loopback address, `mining.authorize` must gain real credential
checking first.

1. Start the node with a miner address — Stratum stays off without one:
   ```
   oeeoo-node --miner-address <your-P2PKH-address> ...
   ```
   The address receives the coinbase of every block this node finds.
2. Point the miner at `127.0.0.1:64777` (same machine only — Stratum
   binds localhost and `mining.authorize` accepts any credentials, which
   is safe only because it is not reachable from the network; never
   expose the port with a tunnel or a proxy).
3. Protocol flow:
   - `mining.subscribe` → the server replies with an extranonce1
     (4 random bytes, hex) and an extranonce2 size of 4 bytes.
   - `mining.authorize` → always accepted (localhost only, see above).
   - `mining.set_difficulty` → the server sends the starting
     difficulty (default 1.0) and adjusts it (vardiff): more than 12
     shares in 60 s doubles it, fewer than 2 halves it (floor 0.01).
   - `mining.notify` → 9 parameters: job id, previous block hash,
     coinbase1, coinbase2, Merkle branches, version, nbits, ntime,
     clean-jobs flag. **Hash byte order is Oeeoo internal order
     everywhere — the `prevhash` in notify is the same hex that
     `getblockhash` returns (no Bitcoin-style display reversal).**
   - `mining.submit` → job id, extranonce2 (exactly 8 hex chars),
     ntime (must be within `[job.ntime, job.ntime + 7200]`), nonce.
     Low-difficulty shares are rejected; a share that meets network
     difficulty is assembled into a real block and broadcast.
4. The server rebuilds the job when the chain tip changes, or every
   60 seconds. The previous job is kept briefly so in-flight shares
   for it are still judged.

## Mining checklist (mainnet)

- [ ] Genesis block is the approved, published one (hash set as the
      `--genesis-checkpoint` for every operator).
- [ ] Node runs at the mainnet PoW limit (`0x1d00ffff`) — if the
      banner prints `*** WARNING: TESTING MODE ***`, stop: that node
      is not on mainnet.
- [ ] Miner address is a P2PKH address you control (version byte
      `0x0f`, `7…` prefix).
- [ ] Coinbase matures 100 blocks before it can be spent.
- [ ] Stratum is reachable only from the mining machine itself.
