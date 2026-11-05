# Oeeoo Coin — Mainnet Launch Checklist

The code is ready (Milestones 1–12). Launch is a human process:
keys, decisions, and an audit that no program can do for you.
Work through this list in order. Nothing here starts the chain
by itself.

## 1. Founder keys (offline, before anything else)
- Generate THREE secp256k1 key pairs on an OFFLINE machine
  (never on a machine with internet, never in this VM).
- Each key holder keeps their private key in a separate, safe
  place (paper + encrypted backup). The 2-of-3 vault needs any
  two of the three to spend.
- Keep only the three PUBLIC keys (compressed, 66 hex chars).
- The 3 backups the spec requires are the three holders'
  backups — not three copies in one place.

## 2. Independent security audit (do this FIRST, before launch)
- Hire an independent auditor (or audit firm) for the code in
  `~/workspace/oeeoo-coin/` (or the GitHub repo, step 3).
- Give them `docs/AUDIT_PACKAGE.md` and `docs/SECURITY_REVIEW.md`.
- Fix every finding they raise. Re-run the full test suite
  after each fix.
- Do NOT launch mainnet without the audit. Milestone 11 was
  audit PREPARATION only — it is not an audit.

## 3. GitHub repository
- Create the repository (planned name:
  `OeeooCoin-blockchain-by-Ahmad`). Verify the name is free.
- Push the code, the docs, and the test logs.
- Tag the launch commit (e.g. `v0.1.0`).
- The VM workspace is not a safe home for the code — GitHub is.

## 4. Genesis block (in the open)
- Pick the launch time (UTC) and announce it beforehand.
- Run, in public (a recorded session, or a published command):
  `oeeoo-genesis --pubkey1 <hex> --pubkey2 <hex> --pubkey3 <hex>
  --time <unix> --out genesis.json`
- This mines at the mainnet difficulty (0x1d00ffff, ~2^32 hashes).
  On a fast machine with several cores it takes minutes; on a
  slow machine, hours. Do NOT lower the difficulty to make it
  faster without a public, recorded decision — the difficulty is
  part of the chain's identity.
- Publish `genesis.json` (hash, Merkle root, nonce, founder
  script) so anyone can verify it with
  `oeeoo-node --genesis-file genesis.json` (it refuses a bad file).
- The genesis block pays exactly 1,000,000 OEEOO to the founder
  2-of-3 vault. Verify the founder script matches the three
  public keys from step 1.

## 5. Network: seeds and peers
- The code supports DNS seeds (`dnsseed = ...` in oeeoo.conf,
  or `--dns-seed`), but the seed list is EMPTY by default — no
  domains are invented. Before launch, register 4 community DNS
  seed domains and configure them (see `docs/RELEASE.md`).
- Run at least 2–3 seed nodes yourself (different machines),
  and publish their addresses so the first miners can connect.
- The P2P port is 64775; the RPC port is 64776 (127.0.0.1 only);
  the Stratum port is 64777 (127.0.0.1 only).

## 6. Release
- Build the release with `./scripts/reproducible-build.sh
  <version>` — it builds twice, checks the binaries are
  byte-identical, and creates the release tarball + checksums.
- Sign the release (the signing key is yours — keep it offline).
- Publish the binaries + SHA-256 checksums + the signature.
- Record the exact toolchain versions (g++ 13.3, CMake 3.28,
  OpenSSL 3.0.13, SQLite 3.45.1).

## 7. Launch day
- Start the first node with the published genesis file:
  `oeeoo-node --datadir <dir> --genesis-file genesis.json`
- Verify with `oeeoo-cli getblockchaininfo` (height 0, the
  published genesis hash).
- Announce the genesis hash, the seed nodes, and the repo.
- Mining starts at block 1 (50 OEEOO reward, 10-minute blocks).

## 8. After launch
- Watch the first retarget (block 2,016). The difficulty must
  move by at most 4x in either direction.
- The founder vault unlocks 100,000 at launch and 100,000 per
  anniversary — enforced by the protocol, not by trust.
- Never change the supply, the rewards, or the genesis. Any
  consensus change needs the governance process (95% signaling
  over 2,016 blocks + 2,016-block activation delay).

## What is NOT decided yet (spec's unresolved items)
- DNS seed domains and operators (step 5).
- Release-signing custody (step 6).
- Reproducible builds (step 6).
- SLIP-0044 coin type (code uses provisional 65535).
- The exact launch time (step 4).
