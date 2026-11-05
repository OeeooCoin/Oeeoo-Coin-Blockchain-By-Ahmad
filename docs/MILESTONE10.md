# Milestone 10 — RPC + Stratum (2026-11-06)

Status: **DONE — implemented, compiled, unit-tested** (ctest 100%, ALL CHECKS PASSED, 81 sections).

## What was built

- `src/rpc/json.*` — a minimal dependency-free JSON value, parser
  and serializer (null/bool/int/double/string/array/object, strict
  parsing incl. `\u` escapes and surrogate pairs). No nlohmann/json
  on the VM, so the node carries its own.
- `src/rpc/rpcserver.*` — Bitcoin-style JSON-RPC 2.0 over HTTP on
  127.0.0.1, default port **64776** (P2P is 64775). Single requests
  and batches; POST only. Auth is HTTP Basic with a cookie file
  (`<dataDir>/.cookie`, `__cookie__:<random hex>`, mode 0600),
  exactly like Bitcoin's cookie auth; the cookie can be disabled
  for tests. Methods (28): help, stop, getblockchaininfo,
  getblockcount, getbestblockhash, getblockhash, getblock,
  getblockheader, getdifficulty, gettxout, gettxoutsetinfo,
  getmempoolinfo, getrawmempool, getnetworkinfo, getpeerinfo,
  getconnectioncount, getmininginfo, getblocktemplate, submitblock,
  sendrawtransaction, getrawtransaction, decoderawtransaction,
  getbalance, getnewaddress, listunspent, sendtoaddress,
  getwalletinfo. Amounts are OEEOO as JSON numbers (exact for all
  valid amounts: total supply 2.1e15 sats < 2^53).
- `src/rpc/stratum.*` — Stratum v1 mining server on 127.0.0.1,
  default port **64777**: mining.subscribe / mining.authorize /
  mining.set_difficulty / mining.notify / mining.submit, jobs
  rebuilt when the tip moves (or every 60s), basic vardiff
  (doubles above 12 shares/min, halves below 2, floor 0.01). A
  share that also meets the network difficulty is assembled into a
  real block and submitted to the chain.
- `src/blockchain/merkle.*` — `MerkleBranch` (sibling hashes per
  level, bottom to top) for Stratum jobs.
- `src/blockchain/chain.*` — `GetNextBits` (next block's difficulty
  incl. retargets), `GetBlockHashAtHeight`, `GetMedianTimePastAtHeight`.
- `src/network/node.*` — `GetBlock` (full block by hash) and
  `GetPeers` (snapshot for getpeerinfo).

## Decisions (recorded, not spec changes)

- **Ports:** RPC 64776, Stratum 64777 — adjacent to the P2P port
  64775, recorded here (the spec did not fix them).
- **Oeeoo hash byte order, everywhere.** Hashes are compared and
  displayed in their internal byte order (big-endian for PoW).
  Stratum's `prevhash` is the same hex `getblockhash` returns —
  no byte reversal anywhere. This differs from Bitcoin's display
  reversal and is documented so miners are written to it.
- **RPC is local-only** (127.0.0.1). No remote RPC in this
  milestone; exposing it is a deployment decision for M12.
- **getblocktemplate uses the tip's bits via GetNextBits**, so it
  is correct at retarget boundaries too.
- **getblocktemplate sigops are reported as 0**: the script
  module has no sigop counter yet (documented simplification).
- **Thread safety:** Node is not thread-safe; both servers
  serialize all node access through an optional shared mutex
  (the tests share one). Run one server per node, or share the
  mutex — never let two threads touch the node unguarded.
- **Stratum vardiff is basic** (60s windows, x2 / /2). Production
  pools tune this; the mechanism is in place.

## Bugs found and fixed (disclosed)

- **Test bug: Node built with mainnet difficulty.** The RPC and
  Stratum tests constructed `Node node;` (default pow limit
  0x1d00ffff) but mined the genesis at test bits 0x207fffff, so
  the genesis was rejected and the first RPC call dereferenced a
  missing result. Fixed by constructing `Node node(kTestBits)`;
  `RpcResult` in the test was also hardened against a missing
  result (a crash became a CHECK failure).
- **Test bug: coinbase maturity.** The RPC test funded the wallet
  by mining block 1 to it, then tried to spend it at height 2 —
  a coinbase needs 100 blocks. Fixed by funding the wallet with a
  regular (non-coinbase) UTXO instead.

## Tests

`ctest` 100% passed; `./build/oeeoo_tests` → ALL CHECKS PASSED
(81 sections). Full command log: `logs/milestone10-test-log.txt`.
Covered: JSON round-trips/escapes/surrogates/numbers/error cases;
every RPC method end-to-end over real HTTP (blockchain, mempool,
network, mining, raw tx, wallet, batch, unknown method, bad
params, help, stop); cookie auth (no auth → 401, wrong cookie →
401, right cookie → 200, cookie file on disk matches); Stratum
subscribe/authorize/notify, a mined share at low difficulty that
becomes a real block paying the miner, deterministic rejections
(unknown job, bad extranonce2, bad ntime), and a low-difficulty
share rejected at difficulty 1.0.

## Known limitations

- No TLS on RPC/Stratum (local-only by design in this milestone).
- No `getchaintips` / `getblockstats` / `getrawtransaction`
  by txid index across pruned history (it scans the mempool and
  the stored chain; pruned blocks are not found).
- Stratum has no `mining.set_extranonce` and no extranonce
  subscription (rejected with a clear error).
- The RPC server handles one connection at a time in its accept
  thread (sequential); concurrent clients queue. Fine for a
  node operator, not for a public service.
