#!/usr/bin/env python3
"""M18 Stream C — independent Python reference for differential tests.

Pure-stdlib re-implementation of five consensus/encoding rules, written
from the spec (docs/MASTER_SPECIFICATION.md) and Bitcoin conventions,
NOT by translating the C++ code: subsidy schedule, difficulty
retarget, founder vesting calendar, Merkle root, Base58Check.

tests/test_milestone18c.cpp runs this script, parses its stdout, and
compares every value against the C++ node code. Any disagreement is a
differential-test failure to investigate, not something to paper over.

Output lines (space-separated, one vector per line):
  subsidy <height> <satoshis>
  subsidy_total <satoshis>
  retarget <last_bits_8hex> <timespan_seconds> <next_bits_8hex>
  launch <unix_seconds>
  anniversary <years> <unix_seconds>
  vested <unix_seconds> <satoshis>
  merkle <n_leaves> <root_hex>
  b58check <payload_20byte_hex> <address>
"""
import hashlib
import sys
from datetime import datetime, timezone

COIN = 100_000_000


def sha256d(b):
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()


# --- subsidy: spec section 4 — 50/25/12.5/6.25/3.125, halving every
# 210,000 blocks, last subsidy block 940,000; 20,000,000 mined total.
def subsidy(height):
    if height <= 0 or height > 940000:
        return 0
    # Era 5 is SHORT (840001..940000, 100,000 blocks); the uniform
    # (h-1)//210000 division would wrongly extend it to 1,050,000.
    era = min((height - 1) // 210000, 4)
    return [50 * COIN, 25 * COIN, 125 * COIN // 10,
            625 * COIN // 100, 3125 * COIN // 1000][era]


# --- difficulty: Bitcoin-style compact bits + retarget, spec section 4.
# Target timespan 2016 blocks * 600 s = 1,209,600 s; clamp [302400, 4838400].
def bits_to_target(bits):
    size = bits >> 24
    mant = bits & 0xFFFFFF
    if size <= 3:
        return mant >> (8 * (3 - size))
    return mant << (8 * (size - 3))


def target_to_bits(t):
    if t == 0:
        return 0
    size = (t.bit_length() + 7) // 8
    if size <= 3:
        mant = t << (8 * (3 - size))
    else:
        mant = t >> (8 * (size - 3))
    if mant & 0x800000:
        mant >>= 8
        size += 1
    return (size << 24) | (mant & 0xFFFFFF)


def retarget(last_bits, timespan, limit_bits):
    clamped = min(max(timespan, 302400), 4838400)
    new = bits_to_target(last_bits) * clamped // 1209600
    limit = bits_to_target(limit_bits)
    if new > limit:
        new = limit
    return target_to_bits(new)


# --- vesting: spec section 6 — 10 x 100,000, launch + 9 anniversaries,
# Feb 29 -> Feb 28 in non-leap years. One tranche vested AT launch.
def is_leap(y):
    return y % 4 == 0 and (y % 100 != 0 or y % 400 == 0)


def anniversary(launch_dt, years):
    y = launch_dt.year + years
    m, d = launch_dt.month, launch_dt.day
    if m == 2 and d == 29 and not is_leap(y):
        d = 28
    return launch_dt.replace(year=y, month=m, day=d)


def vested(launch_dt, when_dt):
    n = 0
    for k in range(10):
        if anniversary(launch_dt, k) <= when_dt:
            n += 1
    return n * 100000 * COIN


# --- Merkle: Bitcoin-style, odd hash duplicated, 1 leaf = itself,
# 0 leaves = all-zero root.
def merkle_root(txids):
    if not txids:
        return b"\x00" * 32
    cur = list(txids)
    while len(cur) > 1:
        nxt = []
        for i in range(0, len(cur), 2):
            left = cur[i]
            right = cur[i + 1] if i + 1 < len(cur) else cur[i]
            nxt.append(sha256d(left + right))
        cur = nxt
    return cur[0]


# --- Base58Check, version byte 0x0f.
B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def b58check_encode(version, payload):
    raw = bytes([version]) + payload
    raw += sha256d(raw)[:4]
    n = int.from_bytes(raw, "big")
    s = ""
    while n > 0:
        n, r = divmod(n, 58)
        s = B58[r] + s
    pad = 0
    for b in raw:
        if b == 0:
            pad += 1
        else:
            break
    return "1" * pad + s


def main():
    out = []
    for h in [0, 1, 210000, 210001, 420000, 420001, 630000, 630001,
              840000, 840001, 940000, 940001, 1000000, 2000000]:
        out.append("subsidy %d %d" % (h, subsidy(h)))
    total = sum(subsidy(h) for h in range(1, 940001))
    out.append("subsidy_total %d" % total)

    for last in (0x207FFFFF, 0x1D00FFFF):
        for span in (1209600, 302400, 4838400, 604800, 2419200,
                     100000, 10000000):
            out.append("retarget %08x %d %08x" %
                       (last, span, retarget(last, span, 0x207FFFFF)))

    # Leap-day launch: the Feb 29 -> Feb 28 rule is the interesting case.
    launch = datetime(2028, 2, 29, 12, 0, 0, tzinfo=timezone.utc)
    launch_ts = int(launch.timestamp())
    out.append("launch %d" % launch_ts)
    for k in range(0, 11):
        out.append("anniversary %d %d" %
                   (k, int(anniversary(launch, k).timestamp())))
    probes = [
        launch_ts - 1,
        launch_ts,
        launch_ts + 86399,
        int(anniversary(launch, 1).timestamp()) - 1,
        int(anniversary(launch, 1).timestamp()),
        int(anniversary(launch, 5).timestamp()),
        int(anniversary(launch, 9).timestamp()),
        int(anniversary(launch, 9).timestamp()) + 31536000,
        int(anniversary(launch, 10).timestamp()),
    ]
    for t in probes:
        when = datetime.fromtimestamp(t, tz=timezone.utc)
        out.append("vested %d %d" % (t, vested(launch, when)))

    for n in (0, 1, 2, 3, 5, 7):
        txids = [sha256d(("diff-tx-%d" % i).encode()) for i in range(n)]
        out.append("merkle %d %s" % (n, merkle_root(txids).hex()))

    for payload_hex in ("00" * 20, "ff" * 20,
                        "3a7d4e8a1f2b6c905d4e8a1f2b6c905d4e8a1f2b"):
        payload = bytes.fromhex(payload_hex)
        out.append("b58check %s %s" %
                   (payload_hex, b58check_encode(0x0F, payload)))

    sys.stdout.write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
