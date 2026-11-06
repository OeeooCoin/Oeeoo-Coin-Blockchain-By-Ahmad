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

## Milestone 14 (2026-11-06, overnight) — node wallet integration
- Roadmap first: docs/ROADMAP_BITCOIN_LEVEL.md (M14–M19) was written
  before implementation, then refined with a Bitcoin Core comparison
  (web research + code inspection). Corrections applied: Core has NO
  per-client RPC rate limiting (thread/work-queue bounds instead);
  reorg-100 / crash-injection / soak are Oeeoo robustness targets,
  not verified Core features; listtransactions/gettransaction need a
  wallet-tx journal (M16, format decision pending); Stratum V2 is a
  spec requirement deferred pending a user decision; no descriptor
  wallets, no encryptwallet mapping (files are created encrypted).
- Node wallet (src/wallet/nodewallet.*): --wallet flag + wallet
  config key; relative paths resolve against the data directory.
  Wallet loads LOCKED; walletpassphrase unlocks in memory for a
  timeout (0 = until walletlock); walletlock re-locks. The passphrase
  is NEVER a CLI flag or a config value and is never logged; it
  travels only over localhost RPC and is wiped (OPENSSL_cleanse) on
  lock. Wrong passphrase -> error -14 (Bitcoin's code, verified
  unused before use).
- Locked = watch-only: getbalance / listunspent / getwalletinfo work
  while locked (balance visible after the first unlock; empty before
  it, because the keys live inside the encrypted file);
  getnewaddress / sendtoaddress refuse while locked (-13). The
  wallet file format is unchanged (PBKDF2-HMAC-SHA512 200k +
  AES-256-GCM); saves are atomic (tmp file + rename). Node wallet
  uses BIP44 account 0 only and an empty BIP39 passphrase. Single
  wallet per node.
- createwallet RPC added (not in the original draft): without it the
  feature would be unusable, since no wallet CLI exists. Returns the
  24-word mnemonic ONCE over localhost RPC; the node never stores it.
- LoadFile contract fixed: the header promised a missing file would
  be remembered for later createwallet, but the implementation
  returned an error. Now matches the header.
- JSON parser bug found by the M14 E2E and FIXED (src/rpc/json.cpp):
  whitespace after ':' in objects and after ',' in arrays was
  rejected ("bad number"). Any real-world client (Python json.dumps,
  curl examples) emits those spaces, so normally-formatted RPC
  requests would have been rejected. M10's tests never caught it
  (Json::Dump emits no spaces). Regression tests added in
  tests/test_rpc.cpp.
- Coinbase-maturity bug found by the funded M14 E2E and FIXED
  (src/wallet/wallet.*, src/rpc/rpcserver.cpp): the wallet had NO
  maturity handling — ScanUtxo counted immature coinbases as
  spendable and coin selection could pick one, so the spend was
  rejected by consensus instead of failing cleanly in the wallet.
  ScanUtxo now takes the next-block height and applies the exact
  consensus rule (100 blocks; genesis coinbase exempt); immature
  coinbases are excluded from balance/unspent and reported as
  immature_balance in getwalletinfo. Proven by the funded E2E: at
  tip 101 the wallet showed balance 100.0 / immature_balance 4950.0
  and the 10-OEEOO send succeeded (txid
  0280e14d7328305490fcf9aa6099d65e5c8e39d1ad1e4840cba0c1883795a93a).
  Regression tests added (new M14 section 7).
- Flaky tests found and fixed during M14 stability runs:
  tests/test_wallet.cpp tamper test wrote a fixed 0xFF at offset 30
  (a random nonce byte) — a no-op with p = 1/256; now XORs the byte.
  tests/test_milestone14.cpp unlock-deadline check raced a second
  boundary (PBKDF2 ~100 ms); now bounded by both clock readings.
  (Also: stdbuf breaks the ASan binary at startup — its LD_PRELOAD
  puts libstdbuf before libasan. Run ASan without stdbuf.)
- Verdict: NOT READY (unchanged from M13 — genesis unapproved, no
  independent audit). M14 closes the biggest functional gap; it does
  not change launch readiness.
- New docs: docs/ROADMAP_BITCOIN_LEVEL.md, docs/MILESTONE14.md;
  docs/WALLET.md node-wallet section rewritten (the "no wallet
  integration" note removed).
- Final state: 134 test sections (127 + 7 new), ALL CHECKS PASSED,
  ctest 100%, ASan/UBSan clean, cppcheck 0 findings. Toolchain:
  g++ 13.3, CMake 3.28, OpenSSL 3.0.13, SQLite 3.45.1, cppcheck 2.13.0.

## Milestone 15 (2026-11-06) — P2P hardening & completeness

- Inbound peer IPs recorded: TransportMessage gains remoteAddress;
  TcpTransport accept() records "ip:port" (sockaddr_in + inet_ntop);
  LoopbackTransport sets "loopback:" + address_. Node::OnBytes stores
  it as peer.address and ban-checks BEFORE registering, so inbound
  bans survive reconnects. (Caveat: TCP bans are keyed by "ip:port"
  and the port is ephemeral — a reconnect from a new ephemeral port
  gets a fresh key. Open decision: key inbound bans by IP alone?)
- AddrMan defences: per-/16 netgroup cap kMaxPerNetgroup = 64
  (Oeeoo's own choice, ~6% of the 1,000-entry table — NOT a Core
  constant); entry expiry kAddrExpirySeconds = 30 days (Core's
  horizon); failure counting kMaxFailures = 3 (Oeeoo's choice),
  entry removed at 3, reset on success. AS-map documented as NOT
  implemented (needs an external data file + operator decision).
- DNS seeds: resolver timeout kDnsSeedTimeoutMs = 10000 (Oeeoo's own
  choice, NOT a Core constant) on a worker thread via std::thread +
  std::promise (not std::async, whose future destructor would block);
  timeout error contains "timed out"; late results discarded.
- Orphan pool (NEW src/mempool/orphan.*): Core's TxOrphanage model —
  max 100 orphans (Core), 20-minute expiry (Core), byte cap 5 MB
  (Oeeoo's choice: 100 x 50 KB), per-tx limit reuses MAX_BLOCK_SIZE,
  never relayed, prevout-indexed, per-peer purge on disconnect,
  evicts oldest (txid tie-break) when full. RemoveExpired uses 64-bit
  arithmetic (deliberate deviation from the mempool's 32-bit pattern).
- Compact relay: in-flight limit 5 per peer (Oeeoo's own choice,
  Misbehave +20 at the cap); getblocktxn depth limit 10 (matches
  Core's depth); one retry then full-block getdata (received txs are
  saved back into the pending entry so the retry can reconstruct);
  block-stall disconnect after 2 s (Core's default) with NO
  misbehavior points and NO 64 s backoff (documented deviation: the
  backoff only matters if the node kept requesting from the same
  peer, which it does not after a disconnect).
- Peer eviction at the 125-peer cap (inbound only; ConnectTo still
  refuses): protects lowest known ping, tx/block senders in the last
  60 s, longest-connected peer; among the rest evicts the largest /16
  netgroup, tie-break most-recently-connected, then highest peerId.
  Ranking documented in a code comment in node.cpp.
- Orphan wiring: "tx" handler sends missing-input failures to the
  pool (no misbehavior, no relay); ProcessOrphansFor (capped at 100
  per call) runs after a mempool add, in BroadcastTx, and after block
  adoption (AcceptBlockDirect / adopted reorg path only).
  DisconnectPeer purges the peer's orphans; the 60 s Poll sweep also
  runs orphans_.RemoveExpired and addrman_.RemoveExpired. ConnectTo
  failure records an AddrMan failure; verack completion records a
  success.
- Behavioural finding (disclosed, not a bug): orphan promotion needs
  a CONFIRMED parent — validation rejects unconfirmed inputs, so a
  parent that merely enters the mempool can never promote its orphan;
  only block arrival can. The "tx"-handler ProcessOrphansFor call
  site is defense-in-depth that cannot fire in practice. (In Core the
  mempool accepts unconfirmed inputs, so the tx-arrival path works
  there — an Oeeoo/Core behavioural difference, not a spec violation.)
- Bugs found and fixed (disclosed in docs/MILESTONE15.md §C.1):
  (1) M13 AddrMan test filled the table from a single /16 — the new
  cap correctly stopped it at 64; the test now spreads across 32
  netgroups. (2) Uninitialized Hash256 awaitingBlock in peer.h held
  indeterminate bytes and the stall sweep disconnected every peer on
  its first Poll (21 test failures) — fixed with Hash256
  awaitingBlock{};. (3) Two test-side issues in Stream C's standalone
  driver (byte-cap tie-break, fixture sizes). (4) Environment:
  libsqlite3-dev and cppcheck were missing on this VM; apt-get update
  hung >10 min; restored via direct .deb downloads.
- GitHub repo status (user, 2026-11-06): the repo now EXISTS and is
  verified empty — https://github.com/OeeooCoin/Oeeoo-Coin-Blockchain-By-Ahmad
  — code push ONLY after ALL milestones (M19) complete, per the
  user's decision.
- Verdict: NOT READY (unchanged — genesis unapproved, no independent
  audit). M15 closes the P2P hardening scope; next is M16 (wallet
  transactions).
- New doc: docs/MILESTONE15.md; docs/SECURITY_REVIEW.md gaps updated.
- Final state: 155 test sections (134 + 21 new), ALL CHECKS PASSED,
  ctest 100%, ASan/UBSan clean, cppcheck 0 findings. Toolchain:
  g++ 13.3, CMake 3.28, OpenSSL 3.0.13, SQLite 3.45.1, cppcheck 2.13.0.

## Milestone 16 (2026-11-06) — RPC & mining completeness

- All 7 previously open decisions DECIDED by user delegation
  2026-11-06 14:55 PKT ("jo behtar hain karo"):
  (1) walletpassphrasechange → M16. (2) Wallet-tx journal →
  SIDECAR file (payload v2 rejected — migrating the encrypted
  wallet format is too risky; sidecar keeps the wallet file
  untouched). (3) Multi-wallet → FUTURE, post-M19/post-launch.
  (4) Stratum V2 → DEFER past launch (spec §13.1 requires it
  eventually; V1 works; Core itself only added experimental V2
  around v30). (5) Inbound ban key → IP ALONE (strip the port —
  "ip:port" defeats M15's IP-keyed bans since TCP ports are
  ephemeral). (6) AS-map → accept /16 bucketing permanently for
  v0.1.0, AS-map deferred post-launch. (7) Orphan promotion →
  LEAVE AS IS (block arrival only; accepting unconfirmed inputs
  into validation is consensus-adjacent and risky).
- M16 also decided: RPC load control = Core-style thread count +
  bounded work queue (queue-full → HTTP 500), NO per-client rate
  limiting (Core has none either); sigops counted for real in
  getblocktemplate, existing 20000 enforced as RPC/template
  POLICY limit only (NOT consensus, NOT Core's 80,000);
  Stratum authorize-any documented as localhost-only by design
  (binds 127.0.0.1).
- New methods: getchaintips (ChainState retains stale-branch
  tips in vIndex — verified before building), getblockstats
  (fees by replaying the block's ancestry from stored blocks;
  the node's current UTXO view cannot price older blocks),
  getchaintxstats (default window 2016), listtransactions /
  gettransaction (sidecar journal), walletpassphrasechange
  (wrong old → -14, never distinguished from a corrupt file).
  RPC surface now 36 callable methods + help.
- getblocktemplate: fee = real integer satoshis per tx
  (BlockTemplate.txFees, recorded in AssembleBlock),
  sigops = real CountSigOps (CHECKSIG 1, bare multisig pubkey
  count, 20 fallback for non-bare-multisig scripts; no P2SH
  rule — no P2SH at launch). The "sigops always 0"
  simplification is gone.
- Sidecar journal: `<wallet>.txjournal`, JSON-lines, keyed by
  txid (latest-wins merge), full rewrite via tmp + fsync +
  rename on every save; missing sidecar = empty journal, not an
  error; migration: receives rebuilt by ScanUtxo rescan, sends
  made before the journal existed are NOT recoverable. History
  works while locked (journal holds no secrets). No
  mempool-acceptance hook (receives appear once confirmed via
  rescan, sends immediately). Reorg: blockhash looked up by
  height on the current main chain.
- RPC load control: worker pool 16 threads + 64-deep queue
  (Core defaults), SetMaxThreads/SetWorkQueueSize (+ config
  keys rpcthreads/rpcworkqueue and CLI flags, wired in
  node_main.cpp); queue-full → HTTP 500
  {"code":-32603,"message":"RPC work queue is full"}; clean
  shutdown joins all workers (incl. a worker blocked on an
  idle client). Thread-per-connection (max 32) removed.
- Ban key: BanManager::BanKey is the single choke point — Ban /
  IsBanned / Unban all normalize through it. "loopback:<name>"
  stays as-is; non-IPv4 ip:port stays as-is. Peer.address keeps
  the full "ip:port" for display. Ban keys are NOT persisted
  across restarts (no load path wired in); a future load path
  must pass stored keys through BanKey.
- Bugs found and fixed (disclosed in docs/MILESTONE16.md §C.1):
  (1) gettransaction dangling pointer — ListTxRecords() returns
  by value, the code kept a pointer into the temporary; the RPC
  worker died with std::length_error; fixed with a local copy.
  (2) WorkerLoop first draft held the queue mutex across
  HandleConnection — would have serialized all workers and
  blocked Stop(); fixed in review. (3) Environment:
  libsqlite3-dev and cppcheck missing AGAIN after a VM
  replacement; restored from cached .debs via sudo dpkg -i
  (no download needed); first clean build was killed (SIGTERM)
  during the environment shift and was re-run from scratch.
- Verdict: NOT READY (unchanged — genesis unapproved, no
  independent audit). Next is M17 (persistence & robustness).
- New doc: docs/MILESTONE16.md; docs/SECURITY_REVIEW.md,
  docs/OPERATOR.md, docs/MINING.md updated.
- Final state: 171 test sections (155 + 16 new), ALL CHECKS
  PASSED, ctest 100%, ASan/UBSan clean, cppcheck 0 findings.
  Toolchain: g++ 13.3, CMake 3.28, OpenSSL 3.0.13, SQLite
  3.45.1, cppcheck 2.13.0.
