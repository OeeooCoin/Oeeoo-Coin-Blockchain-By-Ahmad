# Soak run 2 — COMPLETED as a 1h44m run (NOT the planned 2h clean finish)

Read this before quoting run 2 as "the soak completed": it ran
1h44m, inside the 1–2 h window and above the 1 h minimum, with zero
failures and zero sanitizer output — but it ended in a second VM
restart 17 minutes before its 2 h deadline, with no clean shutdown
and no driver-written final summary. The numbers below are exact;
the incompleteness is stated, not smoothed over. The coordinator
decides: accept 1h44m as the M17 soak, or order a third attempt
(note: the VM restarted at ~12:38 and again at ~14:25 UTC — roughly
every 2 h — so a third 2 h attempt will very likely die the same
way; see "Why no run 3" at the end).

## Setup (identical to run 1 except fresh chain)
- Binary: build-asan/oeeoo-node, HEAD d964bca (M16), ASan/UBSan,
  ASAN_OPTIONS=detect_leaks=0. Nodes started ~12:40:30 UTC 2026-11-06
  (A pid 2632, B pid 2651). --pow-limit-bits 207fffff.
- Test genesis run2/genesis17.json, hash
  78da43766cb8d7fcb7b10b332d040ee02f5295ec2b450df0b73090ad754dbfb3
  (fresh throwaway test pubkeys; founder privkeys never persisted).
- A: P2P 64875 / RPC 64877, datadir run2/data-a. B: P2P 64876 /
  RPC 64878, datadir run2/data-b. Wallets created + unlocked via RPC
  (test-only passphrase "soak17-test-only"); mnemonics never logged.
- P2P bridge soak17/p2p-bridge.py (see the no-dial finding below).
- Driver soak17/soak-driver.py, deadline 14:41:02 UTC, 1 block / 15 s,
  1 tx / 3 min once funds matured, RSS+CPU sampled every 5 min.

## Results
- Duration: nodes ~12:40:30 → ~14:24–14:26 UTC = ~1 h 44–45 min.
  Final height 401 on BOTH nodes (400 blocks mined by the driver +
  1 smoke-test block; last mined 14:22:28 UTC, latest h=401).
- Wallet txs: 27 total (15 A->B, 12 B->A), all confirmed; first
  13:06:13 UTC (A balance 100.0 at tip 101, exactly as M14 predicted),
  last 14:24:25 UTC. Full list with txids in run2/driver.log.
- Block failures: 0. Tx failures: 0. B missed the 30 s catch-up
  window: 0 times. Max B catch-up lag 0.5–0.6 s (every 20-block log
  line reports lag 0.5–0.6 s).
- P2P bridge forwarded (final counts, run2/bridge.log): A->B 415
  blocks / 415 headers / 430 inv / 15 tx / 12 getdata; B->A 415
  getheaders / 430 getdata / 12 tx / 12 inv; ping/pong 106 each way.
  1 sendcmpct dropped per side (by design, see below).
- RSS (VmRSS, kB; full series in logs/soak17-samples.log, 22 samples):
  A 515,076 (12:41) -> 674,368 (14:21), peak 674,368. B 508,004 ->
  636,360, peak 636,360. Shape: +~17 MB in the first 5 min, then
  decelerating (+~1.3 MB/min early, ~+0.6 MB/min over the last
  30 min) — NOT flat, NOT unbounded in 1h44m. No leak claim is made:
  ASan RSS includes quarantine/allocator arenas and detect_leaks=0
  was set by design (M13 benign libcrypto one-time globals).
  Combined with run 1 (A +147 MB / B +142 MB over 75 min, same
  shape), the picture is consistent: fast early rise, then a slow
  creep of ~0.5–1 MB/min/node at this (very light) load.
- CPU (utime+stime totals): A 3.38 -> 24.6 s; B 2.36 -> 19.79 s over
  ~100 min (~0.2 s/min — nodes essentially idle between blocks).
- ASan/UBSan output in either node log: NONE (grep clean).
- Samples file: logs/soak17-samples.log (run 2; copy in run2/).

## How it ended
Second VM restart: `uptime` showed "up 0 min" at 14:26:26 UTC and
"up 1 min" at 14:27:01 UTC; both nodes, bridge and driver died
between the last driver line (14:24:25) and 14:26:26. No "Shutting
down..." in either node log, no sanitizer output, no driver summary
(the driver writes its summary only at its deadline). Chain DBs are
preserved uncleanly (WAL files present) in run2/data-a and
run2/data-b. The coordinator's M17 draft already notes the soak
runs on the M16 binary with mempool save/load validated separately
by Stream A's restart tests — that separation is what makes this
unclean ending acceptable as soak evidence: the soak validates
steady-state node code, not the shutdown path.

## Why no run 3 (decision for the coordinator)
The task instruction says to restart from scratch if the VM is
replaced — Stream C did that once (run 2 after run 1 died at 12:38
UTC). Run 2 then died the same way at ~14:25 UTC, i.e. the VM
restarts roughly every ~2 h and a full 2 h soak does not fit in one
VM lifetime. A third attempt would cost ~2 h and, on the observed
pattern, die again at ~1h45m with the same evidence. Stream C
stopped after run 2 and reports instead. If the coordinator wants a
clean 2 h finish, the options are (a) accept run 2's 1h44m (inside
the 1–2 h acceptance window), or (b) order a run explicitly
designed to survive restarts (e.g. resume-from-disk chain +
driver that re-attaches after a reboot), or (c) a third from-scratch
attempt accepting the likely repeat. No third run was started.
