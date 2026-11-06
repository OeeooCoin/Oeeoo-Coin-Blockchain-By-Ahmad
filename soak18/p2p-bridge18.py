#!/usr/bin/env python3
"""M17 soak P2P bridge (test-only harness).

WHY THIS EXISTS (disclose in the M17 report): the shipped oeeoo-node
binary (M16, HEAD d964bca) has NO operator path to open an outbound
P2P connection -- no --addnode flag, no addnode RPC, and node_main /
Node::Poll never dials AddrMan entries; Node::ConnectTo is only ever
called from tests. Two stock nodes on localhost therefore NEVER peer
(getpeerinfo == [] on both). The bridge works around that WITHOUT
touching the node binary: it opens a TCP connection to each node's
P2P listen port, completes the real version/verack handshake as a
peer, and forwards wire messages verbatim between the two nodes, so
blocks and transactions genuinely travel through each node's real
P2P code (inv/getdata/block/tx, ping/pong, headers sync).

One deliberate exception: `sendcmpct` is DROPPED in both directions.
Compact-block short ids are SipHash-keyed per peer session, so a
verbatim-forwarded cmpctblock would carry short ids computed under
A<->bridge keys that B (keyed under B<->bridge) cannot match; both
nodes then fall back to classic inv/block relay, which is what the
soak exercises. The bridge never modifies any other message.
"""
import hashlib
import logging
import os
import random
import socket
import struct
import sys
import threading
import time

MAGIC = bytes([0xE2, 0xD8, 0x04, 0xD6])
HEADER = 24
MAX_MSG = 2 * 1024 * 1024

LOG = logging.getLogger("bridge")
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
    handlers=[logging.FileHandler("/home/hatch/workspace/oeeoo-coin/soak18/bridge.log"),
              logging.StreamHandler(sys.stdout)],
    datefmt="%Y-%m-%d %H:%M:%S")

A_ADDR = ("127.0.0.1", 64875)
B_ADDR = ("127.0.0.1", 64876)


def sha256d(b):
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()


def frame(cmd, payload):
    raw_cmd = cmd if isinstance(cmd, bytes) else cmd.encode()
    c = raw_cmd[:12].ljust(12, b"\x00")
    return MAGIC + c + struct.pack("<I", len(payload)) + sha256d(payload)[:4] + payload


def varint(n):
    if n < 0xFD:
        return bytes([n])
    if n <= 0xFFFF:
        return b"\xfd" + struct.pack("<H", n)
    if n <= 0xFFFFFFFF:
        return b"\xfe" + struct.pack("<I", n)
    return b"\xff" + struct.pack("<Q", n)


def version_payload(nonce, height=0):
    out = struct.pack("<i", 70015) + struct.pack("<Q", 0)
    out += struct.pack("<q", int(time.time())) + struct.pack("<Q", nonce)
    ua = b"/OeeooSoakBridge:0.1.0/"
    out += varint(len(ua)) + ua
    out += struct.pack("<i", height) + b"\x01"
    return out


class PeerConn:
    def __init__(self, name, addr):
        self.name = name
        self.addr = addr
        self.sock = None
        self.buf = bytearray()
        self.handshook = False
        self.got_version = False
        self.got_verack = False
        self.nonce = random.getrandbits(64)
        self.lock = threading.Lock()
        self.counts = {}
        self.stop = False

    def connect(self):
        self.sock = socket.create_connection(self.addr, timeout=10)
        self.sock.settimeout(5)
        self.buf = bytearray()
        self.handshook = False
        self.got_version = False
        self.got_verack = False
        self.send(b"version", version_payload(self.nonce))
        LOG.info("%s: connected, sent version (nonce %d)", self.name, self.nonce)

    def send(self, cmd, payload):
        with self.lock:
            self.sock.sendall(frame(cmd, payload))

    def send_raw(self, raw):
        with self.lock:
            self.sock.sendall(raw)

    def read_msg(self):
        """Return (cmd, payload, raw_bytes) or None on timeout/close."""
        while True:
            if len(self.buf) >= HEADER:
                length = struct.unpack("<I", self.buf[16:20])[0]
                if length > MAX_MSG:
                    raise ValueError("oversize message")
                if len(self.buf) >= HEADER + length:
                    raw = bytes(self.buf[:HEADER + length])
                    del self.buf[:HEADER + length]
                    if raw[:4] != MAGIC:
                        raise ValueError("bad magic")
                    cmd = raw[4:16].split(b"\x00")[0].decode()
                    payload = raw[HEADER:]
                    if sha256d(payload)[:4] != raw[20:24]:
                        raise ValueError("bad checksum")
                    return cmd, payload, raw
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                return None
            if not chunk:
                raise ConnectionError("closed by peer")
            self.buf += chunk


def handle_handshake(pc):
    """Drive the handshake; returns True once handshook."""
    while not pc.handshook:
        m = pc.read_msg()
        if m is None:
            continue
        cmd, payload, raw = m
        if cmd == "version":
            pc.got_version = True
            pc.send(b"verack", b"")
        elif cmd == "verack":
            pc.got_verack = True
        else:
            LOG.warning("%s: unexpected pre-handshake %s", pc.name, cmd)
        if pc.got_version and pc.got_verack:
            pc.handshook = True
            LOG.info("%s: handshake complete", pc.name)
            return True
    return True


def pump(src, dst, stats):
    """Forward messages src -> dst (post-handshake)."""
    while not src.stop:
        try:
            m = src.read_msg()
        except (ConnectionError, ValueError, OSError) as e:
            LOG.warning("%s: read failed (%s); pump exiting", src.name, e)
            return False
        if m is None:
            continue
        cmd, payload, raw = m
        stats[cmd] = stats.get(cmd, 0) + 1
        if cmd == "sendcmpct":
            stats["sendcmpct_dropped"] = stats.get("sendcmpct_dropped", 0) + 1
            continue  # see module docstring
        try:
            dst.send_raw(raw)
        except OSError as e:
            LOG.warning("%s: forward to %s failed (%s)", src.name, dst.name, e)
            return False
    return True


def reconnect(pc):
    for attempt in range(60):
        try:
            pc.connect()
            handle_handshake(pc)
            return True
        except Exception as e:
            LOG.warning("%s: reconnect attempt %d failed (%s)", pc.name, attempt, e)
            time.sleep(2)
    return False


def main():
    with open("/home/hatch/workspace/oeeoo-coin/soak18/bridge.pid", "w") as f:
        f.write(str(os.getpid()))
    a = PeerConn("A", A_ADDR)
    b = PeerConn("B", B_ADDR)
    stats_ab = {}
    stats_ba = {}
    if not reconnect(a) or not reconnect(b):
        LOG.error("could not connect both sides; exiting")
        return 1
    LOG.info("both sides handshook; forwarding")
    threads = {}

    def start_pump(src, dst, stats):
        t = threading.Thread(target=pump, args=(src, dst, stats), daemon=True)
        t.start()
        return t

    threads["ab"] = start_pump(a, b, stats_ab)
    threads["ba"] = start_pump(b, a, stats_ba)
    try:
        while True:
            time.sleep(5)
            LOG.info("forwarded A->B %s | B->A %s", stats_ab, stats_ba)
            for key, (src, dst, stats) in {
                    "ab": (a, b, stats_ab), "ba": (b, a, stats_ba)}.items():
                if not threads[key].is_alive():
                    LOG.warning("%s pump died; reconnecting %s", key, src.name)
                    if reconnect(src):
                        threads[key] = start_pump(src, dst, stats)
                    else:
                        LOG.error("%s: reconnect failed; stopping", src.name)
                        return 1
    except KeyboardInterrupt:
        pass
    LOG.info("bridge stopping. Final A->B %s | B->A %s", stats_ab, stats_ba)
    return 0


if __name__ == "__main__":
    sys.exit(main())
