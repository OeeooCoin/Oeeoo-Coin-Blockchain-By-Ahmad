#!/usr/bin/env python3
"""M17 soak driver: mine blocks on node A, relay them to node B over
the P2P bridge, send wallet transactions between the nodes, and sample
RSS/CPU every 5 minutes. Runs 2 hours, then stops both nodes cleanly
via RPC `stop` and writes a summary.

Nodes (already running, ASan binary build-asan/oeeoo-node, HEAD
d964bca, --pow-limit-bits 207fffff, test genesis soak18/genesis17.json):
  A: P2P 64875 RPC 64877 datadir soak18/data-a  PID in soak18/node-a.pid
  B: P2P 64876 RPC 64878 datadir soak18/data-b  PID in soak18/node-b.pid
Bridge: soak18/p2p-bridge.py (PID in soak18/bridge.pid).

Test-only wallet passphrase: "soak18-test-only" (throwaway test chain).
The createwallet mnemonic is NOT logged anywhere.
"""
import base64
import hashlib
import json
import logging
import os
import struct
import sys
import time
import urllib.request

BASE = "/home/hatch/workspace/oeeoo-coin/soak18"
LOGS = "/home/hatch/workspace/oeeoo-coin/logs"
PASSPHRASE = "soak18-test-only"
SOAK_SECONDS = 7200
BLOCK_INTERVAL = 15.0
TX_INTERVAL = 180.0
SAMPLE_INTERVAL = 300.0

LOG = logging.getLogger("soak-driver")
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
    handlers=[logging.FileHandler(os.path.join(BASE, "driver.log")),
              logging.StreamHandler(sys.stdout)],
    datefmt="%Y-%m-%d %H:%M:%S")


def sha256d(b):
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()


class Rpc:
    def __init__(self, name, port, datadir):
        self.name = name
        self.url = "http://127.0.0.1:%d/" % port
        with open(os.path.join(datadir, ".cookie")) as f:
            cookie = f.read().strip().split(":", 1)[1]
        token = base64.b64encode(("__cookie__:" + cookie).encode()).decode()
        self.auth = "Basic " + token
        self._id = 0

    def call(self, method, params=None):
        self._id += 1
        body = json.dumps({"jsonrpc": "2.0", "id": self._id,
                           "method": method, "params": params or []}).encode()
        req = urllib.request.Request(self.url, data=body, method="POST",
                                     headers={"Content-Type": "application/json",
                                              "Authorization": self.auth})
        with urllib.request.urlopen(req, timeout=30) as r:
            resp = json.loads(r.read().decode())
        if resp.get("error") is not None:
            raise RuntimeError("%s %s -> error %s" % (self.name, method, resp["error"]))
        return resp.get("result")


# --- Bitcoin-style helpers (mirror of src/, test difficulty only) ---
B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def b58decode(s):
    n = 0
    for ch in s:
        n = n * 58 + B58.index(ch)
    raw = n.to_bytes((n.bit_length() + 7) // 8, "big") if n else b""
    pad = 0
    for ch in s:
        if ch == "1":
            pad += 1
        else:
            break
    return b"\x00" * pad + raw


def p2pkh_script(address):
    raw = b58decode(address)
    assert len(raw) == 25, "bad address length %d" % len(raw)
    assert sha256d(raw[:21])[:4] == raw[21:], "bad address checksum"
    assert raw[0] == 0x0F, "unexpected version byte 0x%02x" % raw[0]
    return b"\x76\xa9\x14" + raw[1:21] + b"\x88\xac"


def encode_script_num(v):
    if v == 0:
        return b""
    out = bytearray()
    mag = v
    while mag > 0:
        out.append(mag & 0xFF)
        mag >>= 8
    if out[-1] & 0x80:
        out.append(0x00)
    return bytes(out)


def push_data(data):
    assert len(data) <= 75
    return bytes([len(data)]) + data


def varint(n):
    if n < 0xFD:
        return bytes([n])
    if n <= 0xFFFF:
        return b"\xfd" + struct.pack("<H", n)
    if n <= 0xFFFFFFFF:
        return b"\xfe" + struct.pack("<I", n)
    return b"\xff" + struct.pack("<Q", n)


def merkle_root(txids):
    cur = list(txids)
    while len(cur) > 1:
        nxt = []
        for i in range(0, len(cur), 2):
            left = cur[i]
            right = cur[i + 1] if i + 1 < len(cur) else cur[i]
            nxt.append(sha256d(left + right))
        cur = nxt
    return cur[0]


def mine_one_block(rpc, miner_addr):
    """One getblocktemplate -> solve -> submitblock round. Returns
    (height, solve_ms, tx_count) or raises."""
    t0 = time.time()
    tmpl = rpc.call("getblocktemplate", [miner_addr])
    if tmpl["bits"] != "207fffff":
        raise RuntimeError("refusing non-test bits: %s" % tmpl["bits"])
    height = tmpl["height"]
    script_sig = push_data(encode_script_num(height))
    cb = struct.pack("<I", 1) + varint(1)
    cb += b"\x00" * 32 + struct.pack("<I", 0xFFFFFFFF)
    cb += varint(len(script_sig)) + script_sig + struct.pack("<I", 0xFFFFFFFF)
    script = p2pkh_script(miner_addr)
    cb += varint(1) + struct.pack("<q", tmpl["coinbasevalue"])
    cb += varint(len(script)) + script + struct.pack("<I", 0)
    txids = [sha256d(cb)]
    tx_hexes = []
    for tx in tmpl["transactions"]:
        raw = bytes.fromhex(tx["data"])
        tx_hexes.append(tx["data"])
        txids.append(bytes.fromhex(tx["txid"]))
    root = merkle_root(txids)
    target = int(tmpl["target"], 16)
    header_prefix = (struct.pack("<I", tmpl["version"])
                     + bytes.fromhex(tmpl["previousblockhash"]) + root
                     + struct.pack("<I", tmpl["curtime"])
                     + struct.pack("<I", int(tmpl["bits"], 16)))
    nonce = 0
    while True:
        header = header_prefix + struct.pack("<I", nonce)
        if int.from_bytes(sha256d(header), "big") <= target:
            break
        nonce += 1
        if nonce > 50_000_000:
            raise RuntimeError("no nonce found (should be instant at test bits)")
    block_hex = (header.hex() + varint(1 + len(tx_hexes)).hex()
                 + cb.hex() + "".join(tx_hexes))
    res = rpc.call("submitblock", [block_hex])
    if res is not None:
        raise RuntimeError("submitblock rejected: %s" % (res,))
    return height, (time.time() - t0) * 1000.0, len(tx_hexes)


def rss_and_cpu(pid):
    rss_kb = None
    try:
        with open("/proc/%d/status" % pid) as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    rss_kb = int(line.split()[1])
                    break
    except OSError:
        pass
    cpu = None
    try:
        with open("/proc/%d/stat" % pid) as f:
            parts = f.read().split()
        cpu = (int(parts[13]) + int(parts[14])) / 100.0  # utime+stime, seconds
    except OSError:
        pass
    return rss_kb, cpu


def main():
    start = time.time()
    # Absolute deadline: SOAK_DEADLINE (epoch) wins when set, so a
    # driver restart mid-soak keeps the original end time instead of
    # extending the run. Otherwise 2h from (re)start.
    deadline = float(os.environ.get("SOAK_DEADLINE", "0")) or start + SOAK_SECONDS
    LOG.info("soak driver starting: deadline %s (%.0fs remaining)",
             time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(deadline)),
             deadline - start)
    rpc_a = Rpc("A", 64877, os.path.join(BASE, "data-a"))
    rpc_b = Rpc("B", 64878, os.path.join(BASE, "data-b"))
    pid_a = int(open(os.path.join(BASE, "node-a.pid")).read().strip())
    pid_b = int(open(os.path.join(BASE, "node-b.pid")).read().strip())

    # Wallet setup (test-only passphrase; mnemonic never logged).
    for rpc in (rpc_a, rpc_b):
        try:
            rpc.call("createwallet", [PASSPHRASE])
            LOG.info("%s: wallet created", rpc.name)
        except RuntimeError as e:
            LOG.info("%s: createwallet: %s", rpc.name, e)
        LOG.info("%s: unlock: %s", rpc.name, rpc.call("walletpassphrase", [PASSPHRASE, 7200]))
    addr_a = rpc_a.call("getnewaddress")
    addr_b = rpc_b.call("getnewaddress")
    LOG.info("miner addr A=%s B=%s", addr_a, addr_b)

    samples_path = os.path.join(LOGS, "soak18-samples.log")
    os.makedirs(LOGS, exist_ok=True)
    with open(samples_path, "a") as sf:
        sf.write("# soak18 samples: epoch_iso heightA heightB peersA peersB "
                 "mempoolA mempoolB rssA_kB rssB_kB cpuA_s cpuB_s\n")

    stats = {"blocks": 0, "txs": 0, "tx_failures": 0, "block_failures": 0,
             "max_lag_s": 0.0, "lag_over_30s": 0, "samples": 0,
             "b_height_mismatches": 0}
    next_block = time.time()
    next_tx = time.time() + 600  # first tx attempt after 10 min
    next_sample = time.time()
    send_to_b = True
    last_report = time.time()

    def sample():
        ha = rpc_a.call("getblockcount")
        hb = rpc_b.call("getblockcount")
        pa = len(rpc_a.call("getpeerinfo"))
        pb = len(rpc_b.call("getpeerinfo"))
        ma = rpc_a.call("getmempoolinfo")
        mb = rpc_b.call("getmempoolinfo")
        ra, ca = rss_and_cpu(pid_a)
        rb, cb = rss_and_cpu(pid_b)
        with open(samples_path, "a") as sf:
            sf.write("%s %d %d %d %d %s %s %s %s %s %s\n" % (
                time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime()),
                ha, hb, pa, pb,
                ma.get("size"), mb.get("size"), ra, rb, ca, cb))
        stats["samples"] += 1
        LOG.info("sample hA=%d hB=%d peers %d/%d mempool %s/%s RSS %s/%s kB",
                 ha, hb, pa, pb, ma.get("size"), mb.get("size"), ra, rb)
        return ha, hb

    sample()
    while time.time() < deadline:
        now = time.time()
        if now >= next_block:
            next_block = now + BLOCK_INTERVAL
            try:
                height, solve_ms, ntx = mine_one_block(rpc_a, addr_a)
                stats["blocks"] += 1
                # Wait for B to catch up over the P2P bridge.
                t_wait = time.time()
                lag = None
                while time.time() - t_wait < 30:
                    if rpc_b.call("getblockcount") >= height:
                        lag = time.time() - t_wait
                        break
                    time.sleep(0.5)
                if lag is None:
                    stats["b_height_mismatches"] += 1
                    LOG.warning("block %d: B did NOT catch up within 30s", height)
                else:
                    stats["max_lag_s"] = max(stats["max_lag_s"], lag)
                    if lag > 30:
                        stats["lag_over_30s"] += 1
                if stats["blocks"] % 20 == 0:
                    LOG.info("mined %d blocks (latest h=%d, %d txs, solve %.1fms, lag %.1fs)",
                             stats["blocks"], height, ntx, solve_ms, lag if lag is not None else -1)
            except Exception as e:
                stats["block_failures"] += 1
                LOG.warning("mine failed: %s", e)
        if now >= next_tx:
            next_tx = now + TX_INTERVAL
            # Send from whichever side can afford it; the alternation
            # must not deadlock (first version stalled: after A->B it
            # waited forever on B's balance while B could only be
            # funded by A). Try the intended sender first, fall back
            # to the other side, and only skip if neither can pay.
            try:
                order = ([(rpc_a, addr_b, "A->B"), (rpc_b, addr_a, "B->A")]
                         if send_to_b else
                         [(rpc_b, addr_a, "B->A"), (rpc_a, addr_b, "A->B")])
                sent = False
                for sender, dest, label in order:
                    bal = sender.call("getwalletinfo").get("balance", 0)
                    if bal >= 2:
                        txid = sender.call("sendtoaddress", [dest, 1.0])
                        stats["txs"] += 1
                        send_to_b = (label == "B->A")  # alternate next time
                        LOG.info("tx %s %s (bal %.1f)", label, txid, bal)
                        sent = True
                        break
                if not sent:
                    LOG.info("tx skipped: neither balance >= 2.0 "
                             "(A %.2f / B %.2f)",
                             rpc_a.call("getwalletinfo").get("balance", 0),
                             rpc_b.call("getwalletinfo").get("balance", 0))
            except Exception as e:
                stats["tx_failures"] += 1
                LOG.warning("send failed: %s", e)
        if now >= next_sample:
            next_sample = now + SAMPLE_INTERVAL
            try:
                sample()
            except Exception as e:
                LOG.warning("sample failed: %s", e)
        if now - last_report >= 900:
            last_report = now
            LOG.info("progress: %d blocks, %d txs, %d block failures, %d tx failures",
                     stats["blocks"], stats["txs"], stats["block_failures"],
                     stats["tx_failures"])
        time.sleep(0.5)

    # Final sample + summary, then clean shutdown via RPC stop.
    try:
        ha, hb = sample()
    except Exception as e:
        LOG.warning("final sample failed: %s", e)
        ha = hb = -1
    summary = {
        "start": time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(start)),
        "end": time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(time.time())),
        "duration_s": round(time.time() - start, 1),
        "binary": "build-asan/oeeoo-node HEAD b9977af (M17, no dialing merged — bridge reused)",
        "genesis": "soak18/genesis17.json hash "
                   "7819075d2e1f77757476a7bee410c3d1e92b38b0d292d8f70a6bb443b0af8c01 (fresh genesis18.json, throwaway test keys)",
        "heightA": ha, "heightB": hb,
        "blocks_mined": stats["blocks"], "txs_sent": stats["txs"],
        "block_failures": stats["block_failures"],
        "tx_failures": stats["tx_failures"],
        "max_B_catchup_lag_s": round(stats["max_lag_s"], 2),
        "B_missed_30s_window": stats["b_height_mismatches"],
        "samples": stats["samples"],
        "driver_restart_note": "driver restarted once 2026-11-06 ~12:09 UTC "
            "to fix a tx-alternation deadlock (see driver.log); nodes ran "
            "continuously since 11:21 UTC. blocks_mined/txs_sent below are "
            "SECOND-LEG counts only — add leg 1 (160 blocks, 1 tx) from "
            "driver.log for the full totals.",
    }
    with open(os.path.join(LOGS, "soak18-summary.json"), "w") as f:
        json.dump(summary, f, indent=2)
    with open(os.path.join(LOGS, "soak18-summary.txt"), "w") as f:
        for k, v in summary.items():
            f.write("%s: %s\n" % (k, v))
    LOG.info("SOAK DONE: %s", json.dumps(summary))
    for rpc in (rpc_a, rpc_b):
        try:
            rpc.call("stop")
            LOG.info("%s: stop sent", rpc.name)
        except Exception as e:
            LOG.warning("%s: stop failed: %s", rpc.name, e)
    try:
        bridge_pid = int(open(os.path.join(BASE, "bridge.pid")).read().strip())
        os.kill(bridge_pid, 15)
        LOG.info("bridge (pid %d) SIGTERM sent", bridge_pid)
    except Exception as e:
        LOG.warning("bridge stop failed: %s", e)
    LOG.info("driver exiting")
    return 0


if __name__ == "__main__":
    sys.exit(main())
