#!/usr/bin/env python3
"""M17 soak: generate 3 THROWAWAY test secp256k1 keypairs (pure Python)
and print ONLY the compressed public keys (66 hex each).

The private keys are generated in-memory and NEVER printed or saved:
the founder vault on this throwaway test chain is therefore
unspendable by design. These keys are test-only and unrelated to the
real Oeeoo founder keys.
"""
import secrets

P = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
Gx = 0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798
Gy = 0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8


def add(p, q):
    if p is None:
        return q
    if q is None:
        return p
    x1, y1 = p
    x2, y2 = q
    if x1 == x2 and (y1 + y2) % P == 0:
        return None
    if p == q:
        lam = (3 * x1 * x1) * pow(2 * y1, P - 2, P) % P
    else:
        lam = (y2 - y1) * pow(x2 - x1, P - 2, P) % P
    x3 = (lam * lam - x1 - x2) % P
    y3 = (lam * (x1 - x3) - y1) % P
    return (x3, y3)


def mul(k, point=(Gx, Gy)):
    r = None
    while k:
        if k & 1:
            r = add(r, point)
        point = add(point, point)
        k >>= 1
    return r


for _ in range(3):
    priv = secrets.randbelow(N - 1) + 1
    x, y = mul(priv)
    print(("03" if (y & 1) else "02") + format(x, "064x"))
