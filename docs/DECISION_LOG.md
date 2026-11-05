# Oeeoo Coin Decision Log

Permanent record. Finalized decisions are policy — they change only
through the community governance process (Section 14 of the master
specification). Unresolved items are engineering work, not silent
policy changes. Nothing here claims a value is final until it is
calculated, tested, and marked VERIFIED.

## Finalized decisions (from the master specification)

| # | Decision | Value |
|---|----------|-------|
| F1 | Ticker | OEEOO |
| F2 | Type | Independent PoW blockchain, NOT a token |
| F3 | Language | C++ (CMake) |
| F4 | Hashing | SHA-256 / SHA-256d |
| F5 | License | MIT |
| F6 | Max supply | 21,000,000 OEEOO |
| F7 | Founder allocation | 1,000,000 OEEOO (inside the 21M) |
| F8 | Mining allocation | 20,000,000 OEEOO |
| F9 | Precision | 8 decimals, COIN = 100,000,000, integer only |
| F10 | Block time target | 10 minutes (600 s) |
| F11 | Difficulty retarget | every 2,016 blocks |
| F12 | Reward schedule | 50 / 25 / 12.5 / 6.25 / 3.125 OEEOO; 0 from 940,001 |
| F13 | Last subsidy block | 940,000 |
| F14 | Max block size | 1 MB |
| F15 | Coinbase maturity | 100 blocks |
| F16 | Mainnet port | 64775 |
| F17 | Peer limits | 125 total / 10 outbound |
| F18 | Mempool | 300 MB default, 14-day expiry |
| F19 | Min relay fee | 1 sat/vB; wallet default max 100 sat/vB |
| F20 | Launch addresses | Legacy P2PKH only (no SegWit/Taproot at launch) |
| F21 | Genesis message | "THINK ABOUT GOD ? IS ONLY ONE ALLAH" (genesis only) |
| F22 | Founder vesting | 10 × 100,000 OEEOO, yearly, 2-of-3 multisig |
| F23 | Chain selection | Greatest cumulative valid PoW |
| F24 | Mainnet strategy | Direct mainnet (no public testnet); internal testing only |
| F25 | Governance signalling | 95% over 2,016 blocks, 2,016-block activation delay |
| F26 | Emergency authority | Temporary founder authority only; cannot change supply, create coins, reverse txs, or override consensus |

## Verified in Phase 1 (calculated, not invented)

| # | Item | Result | How verified |
|---|------|--------|--------------|
| V1 | SHA-256d test vectors | `SHA256d("")=5df6e0e2…`, `SHA256d("abc")=4f8b42c2…` | Python hashlib, then C++ test |
| V2 | Subsidy boundaries | All 12 spec boundaries pass | C++ test, per-block sum 1..940,000 |
| V3 | Total mining issuance | Exactly 20,000,000 OEEOO | Era arithmetic + per-block sum |
| V4 | Network magic candidate | SHA-256("Oeeoo Coin Mainnet Network Magic") = `e2d804d6…` → magic bytes `e2 d8 04 d6` | Python hashlib |
| V5 | P2PKH version byte | `0x0f` is the ONLY version byte (of 256, 3,000 random payloads each) giving a `75` prefix; addresses are 34 chars | Python Base58Check brute force |
| V6 | `75Pk` prefix achievable | Example format: `75PkvD7AKStxvaDSyQH9gtC9kTStqPnPxV` (34 chars, found after 44,590 random payloads with version `0x0f`) | Python brute force |

Note on V6: that example string is a **format example only** — its
payload was random, it has no known private key and holds no funds.
Real addresses come from real keys in Milestone 8.

## Unresolved engineering items (do NOT invent final values)

1. SLIP-0044 coin type — provisional dev id 65535 in code, clearly
   marked NOT registered; register officially before mainnet.
2. Genesis block — timestamp, nonce, Merkle root, block hash: NOT
   finalized, NOT generated. Created only after implementation is
   verified (Milestone 12).
3. Genesis difficulty `nBits = 0x1d00ffff` — candidate from the spec,
   to be verified against the final genesis mining.
4. DNS seed domains + operators (need 4 independent, geographic spread).
5. Hardcoded fallback peer list.
6. Script opcode list + resource costs (must be specified before
   consensus script implementation).
7. Dynamic dust formula (script-aware, not one hardcoded value).
8. Fee-estimation parameters (24-block window per spec).
9. Historical 0.0001 OEEOO transfer-fee figure vs. dynamic-fee policy
   — reconcile before implementation; it is NOT a fixed consensus fee.
10. Peer scoring / progressive ban durations.
11. Custom database schema + performance strategy.
12. Stratum V1/V2 integration details.
13. Authenticated RPC transport + permissions.
14. Release-signing key generation, custody, rotation (2-of-3).
15. Exact dependency versions + reproducible-build environment.
16. Block-header hash display convention (raw byte order in Phase 1;
    lock before mainnet).
17. Founder vesting data structure + reorg-safe activation transitions.
18. GitHub repository `OeeooCoin-blockchain-by-Ahmad` — availability
    NOT yet verified; do not claim it exists until created.

## Changelog

- 2026-11-05 — Phase 1 created: foundation, subsidy, SHA-256d,
  block header, tests passing (see README for the exact run).
- 2026-11-05 — Milestone 2 DONE: VarInt, transaction serialization +
  txid, Merkle tree, Base58Check, HASH160, P2PKH addresses; all tests
  pass (see docs/MILESTONE2.md for vectors).
- 2026-11-05 — PROJECT RENAMED: EarthCoin (EARTH) → Oeeoo Coin (OEEOO).
  Everything else unchanged (supply, rewards, timings, prefix target,
  genesis message). Network magic recalculated for the new name:
  SHA-256("Oeeoo Coin Mainnet Network Magic") = e2d804d6… → magic
  bytes e2 d8 04 d6. Planned repo renamed to
  "OeeooCoin-blockchain-by-Ahmad" (not yet created/verified).
- 2026-11-05 — SUPPLY CLARIFICATION (user question): there is NO
  "2,000,000" anywhere in the project. The figure shown in Phase 1/2
  outputs, "subsidy total issuance = 20,000,000 OEEOO", is the MINING
  total only. Total supply = 20,000,000 mining + 1,000,000 founder =
  21,000,000 OEEOO (tested: MINING + FOUNDER == MAX_MONEY). The
  browser check page was updated to show the full equation so the two
  numbers can never be confused again.
- 2026-11-05 — Milestone 3 DONE: full block, difficulty (nBits/target/
  retarget/block proof, incl. a fixed 256-bit overflow in retarget
  arithmetic), chain index + cumulative work + reorg, Median-Time-Past
  and future-time rules, genesis params structure (NOT finalized).
  All tests pass (see docs/MILESTONE3.md for vectors).
- 2026-11-05 — Milestone 4 DONE: ECDSA secp256k1, legacy SIGHASH_ALL,
  Script interpreter (P2PKH, bare multisig, CLTV, CSV, resource
  limits), in-memory UTXO set, transaction validation (finality,
  BIP68, coinbase maturity 100, fees, block connection with
  no-commit-on-failure). OP_RETURN stays rejected (no arbitrary-data
  outputs). All tests pass (see docs/MILESTONE4.md).
- 2026-11-05 — Milestone 5 DONE: coinbase (BIP34 height, reward
  validation, under-claim allowed / over-claim rejected), mempool
  (1 sat/vB min relay fee, fee-rate order, 300 MB cap, 14-day
  expiry), block templates, nonce search with extraNonce rollover.
  Fixed during testing: GetCoinbaseHeight now reads only the first
  push (coinbase scriptSig tail is arbitrary miner data). All tests
  pass (see docs/MILESTONE5.md).
- 2026-11-06 — Milestone 6 DONE: P2P wire protocol (magic e2 d8 04 d6,
  port 64775), SipHash-2-4, compact blocks, peer scoring + bans
  (100 = ban, 24h doubling to 30 days), address manager, loopback +
  TCP transports, node with handshake, headers-first sync, tx/block
  relay, full block validation. Fixed during testing: (1) real
  coinbase detection — IsCoinbase() replaces the wrong empty-vin
  test in fee computation and block connection (no real block could
  be accepted before this fix); (2) compact-block txCount stored
  explicitly; (3) getheaders hashStop applied after the walk.
  All tests pass (see docs/MILESTONE6.md).
- 2026-11-06 — Milestone 7 DONE: SQLite chain store (single file,
  WAL + synchronous=FULL, schema version 1): blocks, UTXO, tip,
  peers, bans; atomic ApplyBlock; pruning; load-time integrity
  checks (hash, Merkle, PoW, bits, MTP, recomputed work); node
  restart restores chain + UTXO. Schema decision: SQLite chosen
  (single file, atomic transactions, available 3.45.1) — spec item
  "DB schema" was unresolved; this is an engineering decision, not
  a spec change. Known gap: reorg undo NOT implemented — after a
  reorg the stored UTXO set is stale until reindex (documented in
  docs/MILESTONE7.md). All tests pass (see docs/MILESTONE7.md).
- 2026-11-06 — Milestone 8 DONE: wallet — BIP39 (official English
  wordlist, 24 words default), BIP32 HD keys, BIP44
  m/44'/65535'/account'/change/index, gap limit 20, balance scan,
  coin selection, build + sign payments, watch-only (xpub), encrypted
  wallet file (PBKDF2 200k + AES-256-GCM). Decisions: coin type 65535
  provisional/unregistered; Oeeoo extended-key versions
  (xprv 0x0488C3E4 -> "xq6p", xpub 0x0488B7E4 -> "xpyE"); dust 546 sats
  and max fee 100 sat/vB are policies, not consensus. HONEST FINDING:
  "75Pk" prefix is NOT universal — version byte 0x0f gives 34-char
  addresses starting with "7", the 2nd char depends on the key's
  hash (verified: 7L/7N/7K/78/...); "75Pk" needs a per-address vanity
  grind, which breaks BIP44 recovery, so it is not done. All tests
  pass (see docs/MILESTONE8.md).
- 2026-11-06 — User decision: DROP "75Pk" entirely; keep version
  byte 0x0f as the best available secure choice (distinct "7"
  prefix, no collision with Bitcoin "1"/Litecoin "L"/Dogecoin "D",
  prevents cross-chain confusion).
- 2026-11-06 — Milestone 9 DONE: founder vesting enforced —
  10 x 100,000 tranches (launch + 9 calendar anniversaries, Feb 29
  -> Feb 28), genesis pays exactly 1,000,000 to the founder 2-of-3
  multisig vault, founder-spend rule is a vault floor
  (vault >= unvested amount, read from the UTXO set + block time,
  reorg-safe); fees count against vesting; genesis coinbase exempt
  from 100-block maturity (genesis cannot be reorged); vesting
  parameters are chain config, founder keys are the user's
  launch-time responsibility. All tests pass (see docs/MILESTONE9.md).
- 2026-11-06 — Milestone 10 DONE: JSON-RPC server (Bitcoin-style,
  HTTP on 127.0.0.1:64776, cookie auth, 28 methods incl. wallet +
  getblocktemplate/submitblock) and Stratum v1 mining server
  (127.0.0.1:64777, subscribe/authorize/notify/submit, basic
  vardiff; a share meeting network difficulty becomes a real
  block). Ports 64776/64777 are engineering decisions (spec did
  not fix them). Oeeoo hash byte order is used everywhere —
  Stratum prevhash is the same hex getblockhash returns, no
  byte reversal (differs from Bitcoin's display reversal).
  Minimal dependency-free JSON module (no nlohmann/json on the
  VM). Node is not thread-safe: both servers serialize access
  through a shared mutex. All tests pass (see docs/MILESTONE10.md).
- 2026-11-06 — Milestone 11 DONE: audit prep. (1) Reorg
  support: undo records (BlockUndo) stored atomically with
  every applied block (schema v2, migration 1 -> 2); the node
  disconnects the tip back to the fork point and adopts a side
  branch only on strictly greater cumulative work; the store
  follows; the mempool gets non-conflicting disconnected txs
  back. Two real bugs fixed and disclosed: side-branch blocks
  were connected to the wrong (tip) UTXO set, and duplicate
  blocks were punished. (2) Full chain re-validation from
  genesis (VerifyChainFromStore): headers, every transaction,
  and an exact UTXO-set comparison; pruned history fails
  loudly; opt-in at load via deepVerify. (3) Deterministic
  mutation fuzzing (~6,000 inputs: tx, block, header, JSON,
  P2P message, script) — no crash. (4) cppcheck 2.13.0:
  0 errors, 0 warnings, 2 benign style notes (virtual Close()
  in transport destructors). (5) Audit package docs:
  SECURITY_REVIEW.md (threat model, invariants, known gaps)
  + AUDIT_PACKAGE.md. The audit itself is still external and
  has not happened. All tests pass (see docs/MILESTONE11.md).

## Milestone 12 (2026-11-06) — mainnet prep
- Runnable binaries: oeeoo-node (config file + CLI, store, P2P +
  RPC + Stratum, graceful shutdown), oeeoo-genesis (founder 2-of-3
  multisig, parallel mining, genesis JSON file), oeeoo-cli (RPC
  client). A fresh data directory refuses to start without a
  genesis file — the node never invents a genesis.
- Genesis mining uses a SHA-256 midstate (first 64-byte block
  compressed once, copied per nonce): ~2x the naive EVP path on
  this VM. A mainnet-difficulty genesis (2^32 hashes) is still
  hours on this VM — it must be mined on a faster machine, or the
  difficulty decision revisited by the user before launch.
- --pow-limit-bits is a TESTING-ONLY flag (like Bitcoin's
  -regtest). Mainnet always uses the default 0x1d00ffff.
- Bugs fixed+disclosed: (1) the genesis tool printed a doubled
  hash (two temporaries in one expression) — the genesis FILE
  was always correct; (2) test expectations vs the M11 duplicate
  no-op rule; (3) the node banner was buffered and invisible in
  redirected logs. End-to-end verified on this VM: genesis,
  node start, CLI calls, restart from store, fresh-dir refusal.
  All 12 milestones are DONE. The launch checklist is
  docs/MAINNET_LAUNCH.md. The independent audit has NOT happened.

## Launch support (2026-11-06, post-Milestone 12)
- DNS seeds: src/network/dnsseed.* (getaddrinfo, IPv4 only, max
  100 addresses per seed) + Node::AddDnsSeed/QueryDnsSeeds +
  config `dnsseed` / CLI `--dns-seed` (repeatable). The seed list
  is EMPTY by default — no hostnames invented (spec item 15).
  The node queries seeds at startup only when its AddrMan is
  empty. A failing seed is reported, never fatal.
- Reproducible build: scripts/reproducible-build.sh builds twice
  in different directories and compares SHA-256 hashes; creates
  the release tarball + checksums.txt. Documented in
  docs/RELEASE.md. Toolchain: g++ 13.3, CMake 3.28,
  OpenSSL 3.0.13, SQLite 3.45.1.
- Found in testing: this VM's DNS resolver answers EVERY name
  (wildcard proxy, 198.18.x.x), so ".invalid" is not a reliable
  failure case here — the test uses an over-long hostname
  instead. Caveat documented: a wildcard resolver can inject
  bogus addresses; the node only attempts connections, and peer
  scoring/bans drop bad ones.

## Milestone 13 (2026-11-06) — audit gap closures
- Audit method: three independent read-only inspections (consensus/
  supply/vesting/difficulty/block size; mempool/wallet/DB/RPC-Stratum;
  genesis/network/docs/license) + direct verification of the key
  findings. Full report: docs/MILESTONE13.md. Verdict: NOT READY
  (genesis unapproved, no independent audit).
- Mempool expiry (14 days) is now enforced by a 60-second sweep in
  Node::Poll; AssembleBlock never mines expired entries. (The
  mechanism existed since M5 but had zero production callers.)
- Opt-in RBF (BIP125-style): conflicting entries must signal
  (nSequence < 0xfffffffe); replacement fee AND fee rate must beat
  every replaced entry; no new unconfirmed inputs; at most 100
  replaced. Descendants are unreachable in practice (validation
  rejects unconfirmed inputs) — the code handles them as
  defense-in-depth; the 100-cap is untested.
- Dust is dynamic and script-aware (src/transactions/dust.*):
  threshold = (output size + script-aware input size) x rate.
  P2PKH 182 sats, bare 2-of-3 multisig 304 sats at 1 sat/vB. The
  hardcoded 546-sat wallet limit was removed. Dust is a mempool/
  wallet policy, NOT a consensus rule (a miner can still include a
  dusty output directly).
- Mempool eviction: lowest fee rate first; a worse-or-equal-rate tx
  never evicts a better one; eviction runs before any removal so a
  failed replacement leaves the pool untouched. Cap configurable
  (--maxmempool / maxmempool, default exactly 300 MB).
- Fee estimator: exact-integer median over the last 24 blocks,
  minimum 1 sat/vB, exposed as estimatesmartfee (29 RPC methods).
- Low-S signatures (BIP62): signing always produces low-S;
  verification rejects high-S. Malleability vector removed.
- JSON parser: 64-level nesting cap ("JSON nesting too deep").
- RPC: thread-per-connection (max 32) replaces the sequential
  accept loop (one slow client blocked all RPC). No per-client rate
  limit yet.
- Node deep-verifies by default (config.deepVerify = true);
  --no-deep-verify opts out and accepts the light-load trust model
  (stored UTXO table trusted; tampering caught only by deep replay).
- Genesis identity: --genesis-checkpoint (64 hex) + CheckGenesisIdentity
  refuse a file or checkpoint disagreeing with the stored chain.
  ReadGenesisFile now verifies merkleroot, nonce, bits, and message
  (M12's "fully verifies" was an overstatement — corrected).
- Testing-mode banner: the node prints an explicit WARNING whenever
  the PoW limit is not the mainnet 0x1d00ffff.
- Peer cap 125 now enforced (Node::SetMaxPeers, outbound + inbound).
- Handshake: peers below protocol version 70015 are refused;
  a peer echoing the node's own nonce is treated as a
  self-connection and dropped. The node nonce is now random
  (it was time-derived: two nodes started in the same second shared
  a nonce).
- Compact relay: the header's proof of work is checked before any
  transaction is fetched; pending entries expire after 120 s; a
  failed blocktxn now falls back to a full-block getdata request.
  (One real bug found by cppcheck in the new fallback code:
  use-after-erase of the pending iterator — fixed by copying the
  peer id first.)
- AddrMan: capped at 1,000 entries, IPv4 validation on Add,
  randomized GetAddr advertisement. SerializeAddr now writes the
  true count (unparseable entries skipped); ParseAddr requires the
  IPv4-mapped prefix.
- Loopback transport use-after-free fixed: Close()/destructor now
  purge every link pointing at the closing transport from all
  registered transports and deregister it (a closed transport is
  unreachable). TcpTransport::Connect no longer throws on a
  non-numeric port.
- Vesting: AnniversaryTime clamps at UINT32_MAX instead of wrapping
  for extreme launch times. Miner template reserves the exact
  coinbase size (the 250-byte estimate was ~15 bytes short for a
  multisig miner address). Coinbase addition is an explicit __int128
  checked addition with a MAX_MONEY rejection.
- Reproducible build: runs from a clean Git checkout (repo
  initialized in M13; commits 4757b2b, f866618); the archive is now
  verified reproducible too; CMAKE_C_FLAGS and GZIP warnings fixed.
  Verified byte-identical binaries + archive (see docs/RELEASE.md).
  No release published or tagged.
- Vesting schedule vs the brief's "UNDECIDED" flag: the spec section 6
  schedule (10 x 100,000, launch + 9 anniversaries) is the owner's
  finalized decision and was NOT changed; any change needs the
  owner's explicit approval.
- Modern address formats: REQUIRES DECISION (spec: P2PKH only at
  launch; the brief's "Legacy + modern formats" row conflicts with
  the spec's own warning not to assume SegWit was approved).
- Node wallet integration is NOT built: oeeoo-node v0.1.0 has no
  wallet; RPC wallet methods return -13. Documented in
  docs/WALLET.md.
- New docs: docs/WALLET.md, docs/MINING.md, docs/THIRD_PARTY.md,
  docs/OPERATOR.md. README 75Pk claim and stale "NOT done yet"
  section corrected; MAINNET_LAUNCH.md reproducible-builds line
  corrected; SECURITY_REVIEW.md gaps updated for M13.
- Final state: 127 test sections, ALL CHECKS PASSED, ctest 100%,
  ASan/UBSan clean, cppcheck 0 findings. Toolchain: g++ 13.3,
  CMake 3.28, OpenSSL 3.0.13, SQLite 3.45.1, cppcheck 2.13.0.
