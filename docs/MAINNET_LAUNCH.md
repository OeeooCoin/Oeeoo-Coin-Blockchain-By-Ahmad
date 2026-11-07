# Oeeoo Coin — Mainnet Launch Checklist

**Milestone status: Milestones 1–18 DONE (205 test sections, ASan/UBSan
clean, cppcheck 0 findings, reproducible build verified from a clean
Git checkout). Milestone 19 (final verification + release prep) is IN
PROGRESS. The code is NOT launch-ready: verdict NOT READY (genesis
unapproved + no independent audit — see `docs/M19-VERDICT-DRAFT.md`).
Launch is a human process: keys, decisions, and an audit that no
program can do for you. Work through this list in order. Nothing here
starts the chain by itself.

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
  audit PREPARATION only — it is not an audit. Milestone 13 (and the
  Milestone 18 re-run of its 18-section brief) were self-reviews by
  the same agent that wrote the code — also not audits.

## 3. GitHub repository
- The repository EXISTS, created by the user 2026-11-06 and verified
  empty: `https://github.com/OeeooCoin/Oeeoo-Coin-Blockchain-By-Ahmad`.
  Code push ONLY after ALL milestones (M19) complete, per the user's
  decision — do not push before M19 is done.
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
- Since Milestone 18 the node can actually dial out: a dial loop
  (`Node::TryDialPeers`) dials AddrMan addresses (including DNS-seed
  results), `--addnode <ip:port>` (repeatable) adds manual peers, and
  the `addnode` RPC adds/removes/retries them; outbound peers are
  capped at 10 of the 125 total. A fresh node with an empty AddrMan
  still learns no peers unless dialed or seeded — there is no
  hardcoded fallback peer list (spec §20 item 16, not implemented).
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
  OpenSSL 3.0.13, SQLite 3.45.1, cppcheck 2.13.0; Clang 18.1.3 for
  the fuzzers only).

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
- Genesis parameters: launch time, founder public keys, genesis hash
  (step 4) — unapproved; no mainnet genesis exists.
- DNS seed domains and operators (step 5).
- Release-signing custody (step 6).
- SLIP-0044 coin type (code uses provisional 65535).
- The exact launch time (step 4).
- Modern address formats (SegWit/Taproot) — explicitly undecided;
  P2PKH only at launch.
- Distinct testnet identity (magic/port) — test and mainnet currently
  share them, guarded only by the testing-mode banner.
- Hardcoded fallback peer list (spec §20 item 16) — not implemented.
- Spec §20 item 14 (historical 0.0001 OEEOO fee figure) — no explicit
  reconciliation note against the dynamic-fee policy.

Reproducible builds are DONE, not undecided: `scripts/reproducible-build.sh`
builds twice from a clean Git checkout and verifies byte-identical
binaries and archive (see `docs/RELEASE.md`; verified again in
Milestone 13).
