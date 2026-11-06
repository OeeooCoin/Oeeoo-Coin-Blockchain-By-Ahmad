#!/usr/bin/env python3
"""M17 RPC latency probe: time JSON-RPC round-trips (getblockcount and
getblock) against a running node. Usage:
  rpc-latency-probe.py <rpcport> <datadir> [samples]
Reads the node's .cookie for Basic auth. Prints avg/p50/p95/p99/max.
Light load (a few hundred small RPCs); safe to run during the soak.
"""
import base64
import json
import statistics
import sys
import time
import urllib.request

port = int(sys.argv[1])
datadir = sys.argv[2]
n = int(sys.argv[3]) if len(sys.argv) > 3 else 100

with open(datadir + "/.cookie") as f:
    cookie = f.read().strip().split(":", 1)[1]
auth = "Basic " + base64.b64encode(("__cookie__:" + cookie).encode()).decode()
url = "http://127.0.0.1:%d/" % port
_id = [0]


def call(method, params=None):
    _id[0] += 1
    body = json.dumps({"jsonrpc": "2.0", "id": _id[0],
                       "method": method, "params": params or []}).encode()
    req = urllib.request.Request(url, data=body, method="POST",
                                 headers={"Content-Type": "application/json",
                                          "Authorization": auth})
    with urllib.request.urlopen(req, timeout=30) as r:
        resp = json.loads(r.read().decode())
    if resp.get("error") is not None:
        raise RuntimeError("%s -> %s" % (method, resp["error"]))
    return resp.get("result")


def bench(method, params=None):
    ts = []
    for _ in range(n):
        t0 = time.perf_counter()
        call(method, params)
        ts.append((time.perf_counter() - t0) * 1000.0)
    ts.sort()
    def pct(p):
        return ts[min(len(ts) - 1, int(p * len(ts)))]
    print("%-14s n=%d avg=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3f ms" % (
        method, n, statistics.mean(ts), pct(0.50), pct(0.95), pct(0.99), ts[-1]))
    return ts


print("RPC latency vs 127.0.0.1:%d (%s), %d samples each, %s" % (
    port, datadir, n, time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())))
bench("getblockcount")
best = call("getbestblockhash")
bench("getblock", [best])
print("bestblockhash used for getblock:", best)
