# Oeeoo Coin — Reproducible Builds & Releases

## Reproducible build
Run from the repository root:

```bash
./scripts/reproducible-build.sh 0.1.0
```

The script builds all three binaries TWICE (in two different
directories) and compares their SHA-256 hashes. If they match,
the build is reproducible: anyone with the same toolchain gets
the same binaries from the same source. It then creates the
release package `oeeoo-coin-0.1.0.tar.gz` (binaries + README +
LICENSE + launch/build guides) and `release-0.1.0/checksums.txt`.

Determinism comes from:
- `SOURCE_DATE_EPOCH` (the git commit date, or today outside a
  git checkout — printed as a warning),
- `-ffile-prefix-map` (no absolute build paths in the binaries),
- `-Wdate-time` (no `__DATE__`/`__TIME__` in the code),
- a fixed build type (Release).

Toolchain this was verified with: g++ 13.3, CMake 3.28,
OpenSSL 3.0.13, SQLite 3.45.1 (Ubuntu 24.04). A different
compiler version can produce different binaries — record the
toolchain in the release notes.

## Milestone 13 verification (2026-11-06)

Re-run from a clean Git checkout (commit
`f866618bb3f1f0dbebe7ecebd7e48d917e5a9ac6`,
`SOURCE_DATE_EPOCH=1793915486`):

```
REPRODUCIBLE: all three binaries are byte-identical across two builds
REPRODUCIBLE: archive is byte-identical across two builds
oeeoo-coin-0.1.0.tar.gz
  95047a0642a0242de32608dceddef21963eeccb49d6ece74507fb119a7f3726d
oeeoo-node
  bb88f13f8d178e1e236d34ff8ab9f8c0dc392b5f972106a72c598708efe672cf
oeeoo-genesis
  4468f4653254015b48bb647c66e7ba99cd0d113402facbc66fcc8e57b91fc7c6
oeeoo-cli
  59cd1105741ab025722067a66e2140cfdad42ad57e431e8c2b3aa3e492f7b246
```

The script now also verifies the **archive** itself (built twice
with sorted entries, fixed mtime/owner/group, `gzip -n`) — the
earlier run only compared the binaries. The `CMAKE_C_FLAGS` unused
warning and the `GZIP` deprecation warning from the earlier run are
both fixed. Same-machine, two-directory reproducibility only;
cross-environment reproducibility is NOT claimed. **No release was
published or tagged** (the brief forbids it without approval).

## DNS seeds (launch support)
- The node has no hardcoded seeds (none are invented). The
  operator adds real ones:
  - config file: `dnsseed = seed.example.com` (repeatable)
  - CLI: `--dns-seed seed.example.com` (repeatable)
- At startup the node resolves its seeds (only when its address
  manager is empty) and adds the returned peer addresses.
- `src/network/dnsseed.*` resolves a hostname with getaddrinfo
  (IPv4 only, at most 100 addresses). A failing seed is reported
  but never stops the others.
- Caveat: a hostile or wildcard DNS resolver can return bogus
  addresses. The node only tries to CONNECT to them; a dead or
  misbehaving address is dropped by the peer scoring/ban rules.
  Seeds are a bootstrap, not a trust anchor.
- The 4 community seed domains from the spec (item 15) still
  need real operators and domains — that is a launch task, not
  a code task. See `docs/MAINNET_LAUNCH.md` step 5.
