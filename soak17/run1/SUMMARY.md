# Soak run 1 — PARTIAL, ended by VM restart (NOT a completed run)

Do NOT quote this as the M17 soak result. The completed soak is run 2.

## What happened
- Nodes started 2026-11-06 11:21 UTC (A pid 18204, B pid 18259),
  ASan binary build-asan/oeeoo-node, HEAD d964bca (M16),
  --pow-limit-bits 207fffff, test genesis run1/genesis17.json
  (hash 1442bf92abbc0e4bbdc5e46bca656b7dd1bb92a901880c0b0c08bba835016956).
- Driver leg 1: 11:24:22–12:08:05 UTC (killed deliberately to fix a
  tx-alternation deadlock in the driver — see below). Leg 2 (fixed
  driver, same original deadline): 12:08:46–12:39:09 UTC.
- 12:38 UTC the VM RESTARTED (systemd start 12:38, `uptime` 1 min at
  12:39:41). Both nodes, the P2P bridge and the driver died with it.
  No clean shutdown, no final summary from the driver, no ASan/UBSan
  output. Per the task instructions the soak restarts FROM SCRATCH
  (run 2) — this run's 1h18m does NOT count as the completed soak.

## Results while it ran (all from run1/ logs)
- Final height 294 on BOTH nodes, in sync at death (last sample
  12:39:09 UTC). 293 blocks mined by the driver + 1 smoke-test block.
- Wallet txs: 8 total (5 A->B, 3 B->A), all confirmed; txids in
  run1/driver.log. First tx 11:49:43 UTC, once coinbases matured
  (A balance 100.0 at tip 101, exactly as M14 predicted).
- Block failures: 0. Tx failures: 0. B missed the 30 s catch-up
  window: 0 times. Max B catch-up lag ~0.6 s.
- P2P bridge forwarded (final counts): A->B 294 blocks / 294 headers /
  299 inv / 5 tx; B->A 294 getheaders / 299 getdata / 3 tx / 3 inv.
  ping/pong 76 each way. 1 sendcmpct dropped per side (by design).
- RSS (VmRSS, kB): A 498,364 (11:24) -> 645,820 (12:39), peak
  651,612 (12:29). B 487,996 -> 629,868, peak ~629,868. Shape:
  +~60 MB in the first 5 min, then ~+10–20 MB per 5 min, then
  flat-ish 634–651 MB for the last 30 min (consistent with ASan
  quarantine filling toward its cap, not unbounded growth — but
  unproven over 2 h, which is what run 2 must show).
- CPU (utime+stime totals): A 4.14 -> 16.45 s; B 2.63 -> 12.69 s
  over 75 min (~0.16 s/min — nodes essentially idle between blocks).
- ASan/UBSan output in node logs: NONE (grep clean).
- Samples: run1/soak17-samples.log (19 samples, 5-min cadence;
  includes a duplicated header line at the leg-2 restart).

## Driver bug found and fixed during run 1 (disclosed)
The first driver version deadlocked wallet traffic: after the first
A->B tx it waited forever for B's balance to reach 2.0 before
sending B->A, while B could only be funded by A. From 11:52 to
12:07 every tx attempt logged "tx skipped: B balance 1.00". Fixed
in soak-driver.py (try the intended sender, fall back to the other
side, skip only if neither can pay); leg 2 then sent in both
directions (A->B 12:19, B->A 12:22, and 5 more). Nodes were NOT
restarted for this fix — only the driver was, keeping the original
deadline. The fix is in the run-2 driver from the start.
