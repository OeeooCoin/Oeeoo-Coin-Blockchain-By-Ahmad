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

## Milestone 17 (2026-11-06) — Persistence & robustness

- Mempool persistence (Bitcoin's `mempool.dat` model, Oeeoo's own
  JSON-lines format): `<datadir>/mempool.dat`, tx + fee + time
  only (never the UTXO set); caps 100,000 entries / 64 MiB,
  highest fee rate first; save via `<path>.new` + fsync + rename
  on clean shutdown and on demand (`savemempool` RPC); reload on
  start re-validates every tx against the current UTXO set and
  drops expired (>14d) / invalid entries; `importmempool`
  returns {loaded, dropped_expired, dropped_invalid} (Oeeoo
  choice, not Core's null); `loaded` flag in `getmempoolinfo`;
  default persist ON with `--persistmempool=0` /
  `--no-persistmempool` opt-out (Core-compatible semantics).
  `savemempool` works even with persistmempool=0 (opt-out
  disables only the automatic load/save). RPC surface is now
  38 callable methods + `help`.
- Reorg depth "100" is Oeeoo's own target, NOT a Bitcoin Core
  number: 100 equals coinbase maturity, so a 100-deep reorg is
  the deepest that can strand matured coinbase spends. Test
  only — no production bug was found; no production fix made.
- Crash-injection + soak are Oeeoo robustness work, NOT Core
  parity (no named Core crash-injection test could be verified;
  the soak is an Oeeoo target, not a verified Core feature).
- Disk-full surfacing (coordinator decision, per Stream B's
  recommendation): the node_main loop warns to stderr whenever
  Node::LastStoreError() is set (once per distinct error) —
  before M17 the failure was silent at runtime (submitblock
  kept returning success, rollback visible only after
  restart). submitblock STILL returns success while
  persistence fails; the warning is the loudness. Changing
  that is a semantic decision for a later milestone.
- Soak acceptance (coordinator decision): run 2 (1 h 44 m, 401
  blocks, 27 txs, 0 failures, no sanitizer output) is accepted
  as the M17 soak — inside the 1–2 h window, above the 1 h
  minimum. Both runs were ended by VM restarts (~every 2 h),
  not clean shutdowns; a third attempt would very likely die
  the same way. Across both runs: 3 h 02 m, 695 blocks,
  35 txs, 0 failures, 0 sanitizer reports. Soak ran on the M16
  ASan binary (d964bca) — steady-state code is what it
  validates; mempool save/load is covered by Stream A's
  restart tests on the final binary.
- RSS creep under ASan (~0.6–0.9 MB/min/node late in the run)
  accepted as ASan-quarantine behaviour in M17; a non-ASan
  long run to distinguish a real leak is a possible M18
  follow-up, not required.
- MAJOR finding (verified by the coordinator with grep): the
  shipped node cannot open an outbound P2P connection — no
  --addnode, no addnode RPC, no dial loop; Node::ConnectTo is
  called only from tests. Two stock nodes never peer; DNS
  seeds only fill AddrMan, nothing dials them. Inbound
  connections work. NOT fixed in M17 (feature-sized change
  outside M17's scope); carried as the #1 decision for M18
  (coordinator recommends adding it to M18's scope).
- Bugs found and fixed (disclosed in docs/MILESTONE17.md §C.1):
  (1) mempool dump wrote the txid twice (two different
  temporaries) — loader dropped every entry; fixed by naming
  the txid in a local first. (2) Disk-full persistence failure
  was silent at runtime — fixed with the node_main stderr
  warning, now asserted by the disk-full test. (3) Latent
  fseek(NULL) SIGSEGV in tests/test_wallet.cpp after a failed
  fopen (CHECK does not abort) — fixed with an early return;
  the only other fopen in the suite already had the guard.
  (4) Stream B test-design bugs (vesting params in deep
  verify; deep-verifying test-injected UTXOs) — fixed before
  delivery. (5) Bench harness double-spend in the 5-tx
  sensitivity run — harness bug, node correctly rejected it;
  fixed with a mature-unspent queue. (6) Soak driver deadlock
  on wallet traffic (waited for B's balance while only A
  could fund B) — fixed with a fallback sender.
- Environment: no restore needed in M17 — libsqlite3-dev and
  cppcheck were present at the start (unlike M9/M15/M16).
- Verdict: NOT READY (unchanged — genesis unapproved, no
  independent audit). Next is M18 (fuzzing & security
  hardening).
- New docs: docs/MILESTONE17.md; docs/SECURITY_REVIEW.md,
  docs/OPERATOR.md updated; bench harness src/bench/bench.cpp
  (oeeoo_bench); profiling in logs/milestone17-profiling.txt;
  soak evidence in soak17/ + logs/soak17-samples*.log.
- Final state: 182 test sections (171 + 11 new), ALL CHECKS
  PASSED, ctest 100%, ASan/UBSan clean, cppcheck 0 findings.
  Toolchain: g++ 13.3, CMake 3.28, OpenSSL 3.0.13, SQLite
  3.45.1, cppcheck 2.13.0.

## Milestone 18 (2026-11-06) — Fuzzing & security hardening

- Outbound P2P dialing is IN M18 (parent-agent decision on the
  user's standing delegation — the M17 #1 finding, launch-blocking):
  dial loop `Node::TryDialPeers()` in `Node::Poll` (one pass per
  dial interval; dials AddrMan addresses; respects bans and the
  125-peer cap; never dials self or an already-connected peer;
  failed dial counts against the address — 3 failures and it is
  dropped as dead). `--addnode` CLI flag + config key
  (repeatable); `addnode` RPC, Bitcoin-shaped (add / remove /
  onetry; onetry is an immediate explicit connection). DNS-seed
  results are actually dialed (QueryDnsSeeds fills AddrMan; the
  loop dials from it). RPC surface is now 39 callable methods +
  `help`. docs/OPERATOR.md now documents dialing (the M17
  peering-limitation note is replaced).
- Outbound cap → 10 (audit finding A1 CLOSED): DECISION_LOG F17
  ("125 total / 10 outbound") was only half-implemented in HEAD
  — the 10 half had no `kMaxOutbound|maxOutbound` anywhere in
  `src/`. Now `maxOutboundPeers_ = 10` (default) +
  SetMaxOutboundPeers/MaxOutboundPeers/OutboundPeerCount,
  enforced in the dial loop, with a test.
- Fuzzer engine → **libFuzzer (Clang 18.1.3)**. Toolchain check
  first (roadmap requirement): at M18 start the VM had NO
  Clang, no cppcheck, no libsqlite3-dev; apt was locked for
  100+ min, so Clang 18.1.3 + compiler-rt (libFuzzer) +
  cppcheck 2.13.0 + libsqlite3-dev 3.45.1 were installed from
  the Ubuntu 24.04 archive by direct .deb download + dpkg -i.
  AFL++ was not needed; no fallback was required. The VM was
  replaced mid-M18 (toolchain + /tmp wiped); the boot-time
  os-intent replay restored the toolchain. llvm-symbolizer is
  NOT installed (not in the apt index) — coverage is reported
  as libFuzzer feature counts, not percentages. The .deb cache
  + reinstall script live at ~/workspace/m18b-debs/ and
  ~/workspace/m18b-toolchain-reinstall.sh (outside the repo).
- Fuzzing: `src/fuzz/` — 7 libFuzzer harnesses (tx, block,
  script, JSON, P2P framing, compact blocks, wallet file) +
  `tests/fuzz/corpus/` (34 seed files, generated by
  `tests/fuzz/gen_corpus.cpp`); `CMakeLists.txt` —
  `OEEOO_BUILD_FUZZERS` option, 7 ctest entries (fixed seeds,
  bounded iterations). Corpus-copy rule: CMake copies seeds
  into the build dir at configure time; never run a fuzzer
  directly against `tests/fuzz/corpus/` (libFuzzer writes new
  inputs back and polluted the checked-in seeds with 600+
  stray files during development). Live fuzzing harnesses in
  `tests/test_milestone18b.cpp` (mutated P2P messages / raw
  byte stream / JSON-RPC / Stratum mining.submit into real
  nodes, deterministic, ASan/UBSan). Core's caveat applies: a
  fuzzer cannot produce valid PoW/signature txs/blocks, so the
  P2P libFuzzer target stops at parsing; deeper net-processing
  coverage comes from the live harness.
- Differential tests (defined, not vague): (a) node vs an
  independent pure-stdlib Python reference
  (`tests/diff_reference.py`, written from the spec / Bitcoin
  conventions, NOT translated from C++) — subsidy, difficulty
  retarget, vesting, Merkle, Base58Check; (b) two-node
  agreement on identical inputs; (c) block-acceptance
  equivalence between the node and the standalone test
  harness. All pass.
- Soak acceptance (coordinator decision): the M18 soak ran in
  3 legs (~107 min total, height 0→374, 19 wallet txs, 0
  sanitizer errors) on the M17 binary + the Python P2P bridge
  because dialing was not integrated when the soak started;
  leg 3 (47.8 min, 187 blocks, 13 txs, 0 failures, clean
  shutdown) is the reported leg. A soak on the dialing binary
  is a possible M19 follow-up, not claimed here. RSS creep
  under ASan accepted again (same shape/magnitude as M17).
- Bugs found and fixed (disclosed in docs/MILESTONE18.md §C.1):
  R1 (HIGH) — unauthenticated remote crash via malformed HTTP
  Content-Length (unguarded std::stoul before auth → exception
  escaped the worker thread → std::terminate → whole node
  aborts); fixed with a strict digits-only parse, saturating
  at 4 MB+1, 400 response. R2 — wallet loader heap-buffer-
  overflow (16 + mnemonicLen uint32 wrap, 0xFFFFFFF0 → 0 →
  std::string::assign read ~4 GB out of a 16-byte buffer);
  fixed with a size_t comparison. R3 — Stratum session buffer
  unbounded (20 MB held for a 20 MB stream); connection now
  dropped when the unterminated tail exceeds 1 MB (deliberately
  above the longest legitimate skipped line, 65536 — the first
  fix capped at 65536 and the 70 KB-line test caught it). R4
  (latent, no callers) — Json::IntFromString called std::stod
  unguarded inside its own catch block; fixed defensively.
  R5 — heap-use-after-free in Node::OnBytes (Peer& held across
  Misbehave() calls that can ban + erase the peer, then
  peer.recvBuffer.clear() on the freed Peer; trigger: two
  bad-magic messages, 50 + 50); fixed with the peers_.find
  guard at all three Misbehave sites. Coordinator fix —
  fuzz_script ctest flake: the 2,000 runs complete cleanly,
  then LeakSanitizer fires at process exit on the known benign
  libcrypto one-time leak and libFuzzer writes a 0-byte
  "crash" artifact; fixed by setting
  ASAN_OPTIONS=detect_leaks=0 on the 7 fuzz ctest entries
  (same documented practice as the main suite). All findings
  are non-consensus; no consensus code was changed in M18.
- Audit re-run (Stream C, full draft in
  soak18/M18C-AUDIT-DRAFT.md): the full 18-section M13 brief
  re-run against M14–M17 changes; verdict NOT READY
  (unchanged). New findings A1–A8 (A1 and A5 FIXED in M18;
  A2 is a process finding — a concurrent-edit franken-build
  aborted under UBSan; A3/A4/A6 carried; A7/A8 are soak
  observations). No consensus-touching issue found.
- Environment: VM replaced twice during M18 (once mid-fuzzing
  — toolchain + /tmp wiped, a fuzz run at ~17k/20k killed;
  once during the coordinator's reproducible-build run — the
  build was restarted and the log moved to logs/ so a
  replacement cannot wipe it). libsqlite3-dev + cppcheck +
  Clang 18 were restored from the cached .debs (the cached
  libllvm18 was truncated, 22.0 MB vs 27.5 MB real —
  re-downloaded intact).
- Verdict: NOT READY (unchanged — genesis unapproved, no
  independent audit). Next is M19 (mainnet launch readiness).
- New docs: docs/MILESTONE18.md; docs/SECURITY_REVIEW.md,
  docs/AUDIT_PACKAGE.md, docs/OPERATOR.md, README.md updated;
  soak evidence in soak18/ + logs/soak18-samples.log.
- Final state: 205 test sections (182 + 23 new), ALL CHECKS
  PASSED, ctest 100%, ASan/UBSan clean, cppcheck 0 findings,
  fuzz ctest 7/7 passed. Toolchain: g++ 13.3, CMake 3.28,
  OpenSSL 3.0.13, SQLite 3.45.1, cppcheck 2.13.0, Clang 18.1.3
  (fuzzers only).

## Milestone 19 (2026-11-07) — Mainnet launch readiness (final verification + release prep)

- No code changes in M19 — verification + release prep only. No
  consensus change, no spec change, no new policy. Every decision
  below is about verification method, release form, or the verdict;
  all REQUIRES DECISION items are listed with owners in
  docs/MILESTONE19.md §G (21 OPEN + 16 CLOSED).
- Clean-room method (honest): fresh directory
  `~/workspace/oeeoo-coin-cleanroom/`, fresh git checkout of HEAD
  `d0a73f3`, fresh build dirs, fresh E2E datadir/genesis/wallet.
  Reused: the git repo as checkout source, the system toolchain
  (g++ 13.3, CMake 3.28, OpenSSL 3.0.13, SQLite 3.45.1, cppcheck
  2.13.0, Clang 18.1.3), and the M17/M18 soak-driver mining pattern.
  NOT a fresh VM — this VM cannot be replaced on demand, and it was
  in fact replaced three times during Stream A (~08:16, ~10:14,
  ~12:33 UTC) and once during Stream B (~10:14 UTC). Interrupted
  runs were restarted, never claimed complete.
- Soak acceptance (M19 leg 2, ~1 h 56 m): accepted under the M17
  honest rule (inside the 1–2 h window, above the 1 h minimum).
  Killed ~3 min before its 2 h deadline by a VM restart — NOT
  claimed as a clean 2 h run. This closes decisions item B5 (soak
  on the dialing binary) — the soak ran on the M18 final ASan binary
  with real P2P dialing (`--addnode`, no bridge): peers 2/2 at all
  29 samples, 440 blocks, ~30 wallet txs, 0 failures, 0 sanitizer
  errors.
- Version → **stays 0.1.0** unless the owner says otherwise. No
  version string in code was changed. Local git tag `v0.1.0` created
  on the final M19 commit (local-only — no remote configured,
  nothing pushed, nothing published).
- Release notes → `docs/RELEASE_NOTES.md` (final): M1–M19, finalized
  policy, 28 known limitations, toolchain, M19 checksums (clean-room
  run: node `5f6c08d7…`, genesis `29345d45…`, cli `6b7a71af…`,
  tarball `a6d7f12a…`; binary hashes identical to M18's — the code is
  unchanged since M18). The tagged-commit run reproduces the same
  binaries; its tarball differs only via SOURCE.txt (recorded in
  docs/RELEASE.md).
- RELEASE.md backfill: M14–M17 verification entries were never
  recorded there (only M13 was) — added in M19 from the milestone
  reports (M14 §F/OVERNIGHT_STATUS.md, M15 §F.1, M16 §F.1, M17 §F.1).
- Verdict → **NOT READY** (unchanged — genesis unapproved, no
  independent audit). Evidence table in docs/MILESTONE19.md §H
  (E1 + E2 decide it; E3–E8 support the code but do not change it).
- Environment: VM replaced three times during Stream A (toolchain +
  /tmp wiped each time; reinstalled once from cached .debs via
  `~/workspace/m18b-toolchain-reinstall.sh`) and once during Stream
  B (killing leg 2 and erroring Stream B's agent before close-out;
  the coordinator verified the soak data on disk and wrote the
  closing summary). No run's evidence depends on a killed run.
- Final state: 205 test sections, ALL CHECKS PASSED, ctest 100%,
  ASan/UBSan clean, cppcheck 0 findings, fuzz ctest 7/7 passed
  (M18, unchanged). Toolchain: g++ 13.3, CMake 3.28, OpenSSL
  3.0.13, SQLite 3.45.1, cppcheck 2.13.0, Clang 18.1.3 (fuzzers
  only).

## Milestone 20 (2026-11-07) — Audit remediation (OOC-SA-001)

- Audit report OOC-SA-001 received 2026-11-07 (user pasted it;
  "Crypto Coin Blockchain Security Auditor"). Verdict: NOT READY
  FOR MAINNET. Three findings, all independently verified against
  the code by the parent agent before M20 started, and all FIXED
  in M20: OOC-001 CRITICAL (multiple coinbase / unauthorized
  inflation — CheckBasic had no coinbase-count/first rule,
  CheckBlockReward validated only vtx[0], ConnectBlockTransactions
  minted EVERY IsCoinbase tx's outputs); OOC-002 HIGH
  (submitblock returned success while persistence failed —
  already documented in M17); OOC-003 MEDIUM (wallet file
  permissions not 0600 — fopen("wb") with no chmod).
- OOC-001 fix: exactly one coinbase, at vtx[0], enforced at
  three layers (Block::CheckBasic, CheckBlockReward,
  ConnectBlockTransactions — a second coinbase is a HARD
  REJECTION, never a mint) on every acceptance path (normal,
  reorg, full-chain/deep verification, genesis). Live two-node
  demonstration: a malicious peer's valid-PoW, valid-Merkle
  multi-coinbase block is rejected and its sender banned
  (docs/MILESTONE20.md §F.3). This is the auditor's release
  condition for OOC-001 — demonstrated, logged.
- OOC-002 fix: DURABLE ACCEPTANCE SEMANTICS — persist BEFORE
  committing any in-memory state, in both acceptance paths.
  Header validation + work come from a trial copy of the chain
  index (ChainState::AddBlock on a copy — no API exists to remove
  a block again, so validate-then-roll-back is not possible);
  ApplyBlock runs first (one atomic SQLite transaction); only
  then does the real index + UTXO + mempool + undo commit. A
  persistence failure returns false + "failed to persist
  accepted block: ..." with punish == false, changing NO
  in-memory state. The reorg path has a best-effort restore
  (disconnect applied side blocks, re-apply disconnected main
  blocks from saved undos); honest limit: under a sustained I/O
  failure the restore itself fails and the store is left at an
  intermediate but VALID chain state. Trial-copy cost: O(chain)
  per acceptance, accepted for exactness. submitblock now returns
  an error on persistence failure. This CLOSES open item C7.
- OOC-003 fix: wallet files are created owner-only (0600) —
  open(O_WRONLY|O_CREAT, 0600) + fchmod(0600) + fstat verify +
  truncate-after-verify, mode fixed BEFORE any byte is written;
  atomic tmp+rename save verifies the final mode 0600; the tx
  journal gets the same treatment (privacy-sensitive). POSIX
  only; Windows ACLs out of scope.
- Network identity separation — APPROVED by the user 2026-11-07
  ~21:28 PKT ("Haan, M20 mein alag kar do"). Testnet gets its
  own magic (d9 73 08 c2 = first 4 bytes of SHA-256("Oeeoo Coin
  Testnet Network Magic"), same derivation as mainnet — verified
  with python3 hashlib), default ports 64779/64780/64781, user
  agent "/OeeooCoin:0.1.0(testnet)/". Mainnet identity UNCHANGED
  (magic e2 d8 04 d6, ports 64775/64776/64777, agent
  "/OeeooCoin:0.1.0/"). Selection: --testnet / --network testnet
  (CLI) or network=testnet (config file); default is mainnet;
  explicit ports always win; invalid values rejected loudly.
  This CLOSES open REQUIRES DECISION item B2. It is a network
  identity only — NOT a public testnet, consensus unchanged.
- Relayed coinbase guard (found during M20): a coinbase-shaped
  tx relayed as "tx" used to land in the orphan pool (it can
  never validate there); now rejected outright (+10
  misbehavior), before the orphan branch.
- Close-out fixes (found by the close-out coordinator's
  file-by-file diff review + re-verification, NOT by the first
  coordinator): (1) node_main banner double-temporary UB —
  ActiveNetworkMagic() returns by value, and the banner took
  .begin()/.end() from two different temporaries: normal build
  printed the magic doubled (e2d804d6e2d804d6), ASan build
  aborted at startup (stack-buffer-overflow, READ of size 20),
  which broke every real-node spawn in the first ASan run
  (m17b/m18a cascade). Fixed by capturing the magic once in a
  local. (2) m17b disk-full observation stale under M20
  semantics — new OBSERVED (M20) branch (a submit rejected
  upfront, height at the durable height, no rollback needed).
- Audit report's §7 note recorded as a NEW open item (A8): a
  separate external cryptographic and consensus audit remains
  necessary before launch. The audit blocker (A2) is PARTLY
  cleared (report received, findings fixed) but NOT closed —
  the auditor has not confirmed the fixes.
- Version → **stays 0.1.0**. No version string changed; local
  tag v0.1.0 stays on M19's 9252086 (not moved, no new tag).
- Verdict → **NOT READY** (unchanged — genesis unapproved, the
  M13 §18 rule decides it alone; audit confirmation + second
  audit still pending). Evidence table in docs/MILESTONE20.md
  §H (E1 + E2 decide it; E3–E8 support the code but do not
  change it).
- Final state: 230 test sections (205 prior + 25 new: m20a 8,
  m20b 7, m20c 5, m20d 5), ALL CHECKS PASSED, ctest 100%
  (281.93 s), ASan/UBSan clean (re-run after the banner fix —
  the first ASan run was killed by close-out after the §C.1
  diagnosis and is superseded), cppcheck 0 findings (102/102
  files), reproducible build verified (see docs/MILESTONE20.md
  §F.2). Toolchain: g++ 13.3, CMake 3.28, OpenSSL 3.0.13,
  SQLite 3.45.1, cppcheck 2.13.0, Clang 18.1.3 (fuzzers only).
- Environment: VM replaced twice during close-out (builds
  SIGTERMed, re-run via setsid nohup); a user XMRig miner
  (student-mining plan) ran on the VM through close-out (4
  threads, ~1.8 GB RSS, not touched) and slowed every run.
- Untracked `audit-outreach-draft.md` (repo root, Roman-Urdu
  audit outreach note) assessed in docs/MILESTONE20.md §G.5:
  price ranges unverified estimates; audit-package link/expiry
  correct; OSTIF route plausible; "no audit yet" premise now
  stale for OOC-SA-001. Left untracked, NOT committed.
