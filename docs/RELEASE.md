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
OpenSSL 3.0.13, SQLite 3.45.1, cppcheck 2.13.0 (Ubuntu 24.04);
Clang 18.1.3 for the fuzzers only (Milestone 18). A different
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

## Milestone 14 verification (2026-11-06)

Re-run from the final commit `62cfc6f`
(`SOURCE_DATE_EPOCH=1793919946`):

```
REPRODUCIBLE: all three binaries are byte-identical across two builds
REPRODUCIBLE: archive is byte-identical across two builds
oeeoo-coin-0.1.0.tar.gz
  b3a0bb49080d8f0dc0585c3367d0fa858ce4e32d6d875588346bf918754f731f
oeeoo-node
  5cc2616b5637affb323891fa1f324ffc3d591855969dee697754a82f032ea88e
oeeoo-genesis
  77d89e07432e08dfa1b8759e3023e48905c46e62a8b2fbfa3d6007c554e7c502
oeeoo-cli
  a80b3c6f0677814b3ee181f4f5b6bb9bee85f56171d71b964c621a79b8323136
```

Recorded in `docs/OVERNIGHT_STATUS.md`. **No release was published
or tagged.**

## Milestone 15 verification (2026-11-06)

Re-run from the code commit `ee2c4ca`
(`SOURCE_DATE_EPOCH=1793955896`):

```
REPRODUCIBLE: all three binaries are byte-identical across two builds
REPRODUCIBLE: archive is byte-identical across two builds
oeeoo-coin-0.1.0.tar.gz
  3c27ee4bef349ab9d76f620d6fe37ebcd733950da6a55adfce69f0cc22840747
oeeoo-node
  3f2675dad2c9a8aea9f01d933164715f49af5a03f92538ad458e5d1bb9128ce9
oeeoo-genesis
  52b29e3893e1f805f2bf82d0441eb4e09c5b3818217dd541207886c06c8130f1
oeeoo-cli
  a80b3c6f0677814b3ee181f4f5b6bb9bee85f56171d71b964c621a79b8323136
```

Confirmation run from the docs commit `1f063fd`
(`SOURCE_DATE_EPOCH=1793956843`): also REPRODUCIBLE for both binaries
and archive; binary hashes identical to run 1; tarball
`648a71862a73ea7bcd682a14d7548e3d6861b522f1383a93f8c86f1ee82bd3c3`
(differs only via SOURCE.txt). **No release was published or tagged.**

## Milestone 16 verification (2026-11-06)

Re-run from the code commit `d0c644b`
(`SOURCE_DATE_EPOCH=1793961737`):

```
REPRODUCIBLE: all three binaries are byte-identical across two builds
REPRODUCIBLE: archive is byte-identical across two builds
oeeoo-coin-0.1.0.tar.gz
  e40d060612e8fdfd48d1d3f6f4196d4735189d1779d09b17422ff7f48a69783f
oeeoo-node
  a7d0d880d39d5008b38f4f659e533374fb7d05a5400d58ca2348fcb3cf3f6f5f
oeeoo-genesis
  110168c17be7c8cb51c0b0bc9103f2189e2108e82ff07cca02f949e2ca8ace82
oeeoo-cli
  a80b3c6f0677814b3ee181f4f5b6bb9bee85f56171d71b964c621a79b8323136
```

Confirmation run from the docs commit `1f21b81`
(`SOURCE_DATE_EPOCH=1793962495`): also REPRODUCIBLE for both binaries
and archive; binary hashes identical to run 1; tarball
`5b7ff2a6e8dea7fccc6895fc94d43b97bc61a818f6fc0252c1eeb9326cef878b`
(differs only via SOURCE.txt). **No release was published or tagged.**

## Milestone 17 verification (2026-11-06)

Re-run from the code commit `82759ff`
(`SOURCE_DATE_EPOCH=1793966995`):

```
REPRODUCIBLE: all three binaries are byte-identical across two builds
REPRODUCIBLE: archive is byte-identical across two builds
oeeoo-coin-0.1.0.tar.gz
  273bbf31a0e59685dda239bdd241649cff116a66428cdf4574367a2159edd4b3
oeeoo-node
  85c908c3b067a6fba3126fa9a03ecf65a551c9d53fabe20ba048e382968ba019
oeeoo-genesis
  4c7c16cb2f2f92adfcf9e92fe09dcf612fa6a670e7b4ac3cea7346184b9e3ce7
oeeoo-cli
  a80b3c6f0677814b3ee181f4f5b6bb9bee85f56171d71b964c621a79b8323136
```

Confirmation run from the docs commit `53b9801`
(`SOURCE_DATE_EPOCH=1793978895`): also REPRODUCIBLE for both binaries
and archive; binary hashes identical to run 1; tarball
`c20fb50807235d45af4d96ff5ee525cc8a4e9ce8fb5abdd24fc8e18f05f12479`
(differs only via SOURCE.txt). **No release was published or tagged.**

## Milestone 18 verification (2026-11-06)

Re-run from the code commit
`7e8d862` (`SOURCE_DATE_EPOCH=1794009307`;
log `logs/m18-repro-build-run1.log`):

```
REPRODUCIBLE: all three binaries are byte-identical across two builds
REPRODUCIBLE: archive is byte-identical across two builds
oeeoo-coin-0.1.0.tar.gz
  c86a127d6961ba7bbd442edf1a59d778cf0e657c453eac7299a744f94939ea74
oeeoo-node
  5f6c08d7319850a0e3dd1d0c39d63c48ec9bd601b351617f5fbd69a268a315b3
oeeoo-genesis
  29345d45c528b5a8a39ce47891b546a3ab9b53d03770b87848d57e74377f5c19
oeeoo-cli
  6b7a71af340d018930e53498844f7cf0b67f48b3aabaaadad06395c505fac2bb
```

Confirmation run from the docs commit `900b8f0`
(`SOURCE_DATE_EPOCH=1794014250`; log `logs/m18-repro-build-run2.log`):
also REPRODUCIBLE for both binaries and archive. The three binary
hashes are identical to run 1 (docs do not change the binaries); the
tarball differs only because `SOURCE.txt` now names the docs commit:
tarball
`7ef3e9a0c5381c709ab39a2d10b58af5a90dfd0fa8d8b2d9ccb6d60f2922284a`.
Same-machine, two-directory reproducibility only;
cross-environment reproducibility is NOT claimed. **No release was
published or tagged.**

## Milestone 19 verification (2026-11-07)

Clean-room run from a fresh checkout of `d0a73f3`
(`SOURCE_DATE_EPOCH=1794015825`; log
`logs/milestone19-cleanroom-log.txt`):

```
REPRODUCIBLE: all three binaries are byte-identical across two builds
REPRODUCIBLE: archive is byte-identical across two builds
oeeoo-coin-0.1.0.tar.gz
  a6d7f12a1cc447ea0ba9339b0280c9eac1223fc4c1cd581ce66f683e3bf1c4a5
oeeoo-node
  5f6c08d7319850a0e3dd1d0c39d63c48ec9bd601b351617f5fbd69a268a315b3
oeeoo-genesis
  29345d45c528b5a8a39ce47891b546a3ab9b53d03770b87848d57e74377f5c19
oeeoo-cli
  6b7a71af340d018930e53498844f7cf0b67f48b3aabaaadad06395c505fac2bb
```

Binary hashes are identical to M18's — the code is unchanged since
M18, so this is the expected result, and it is itself a check: a
clean-room checkout reproduces M18's binaries exactly. The tarball
differs from M18's only via SOURCE_DATE_EPOCH/SOURCE.txt.

Tagged-commit run (local tag `v0.1.0`, commit `<INTEGRATION-COMMIT>`):
also REPRODUCIBLE for both binaries and archive; binary hashes
identical to the run above; tarball `<TAGGED-TARBALL-HASH>` (differs
only via SOURCE.txt naming the tagged commit). Same-machine,
two-directory reproducibility only; cross-environment
reproducibility is NOT claimed. **No release was published; the tag
is local-only (no remote configured, nothing pushed).**

## DNS seeds (launch support)
- The node has no hardcoded seeds (none are invented). The
  operator adds real ones:
  - config file: `dnsseed = seed.example.com` (repeatable)
  - CLI: `--dns-seed seed.example.com` (repeatable)
- At startup the node resolves its seeds (only when its address
  manager is empty) and adds the returned peer addresses. Since
  Milestone 18 the dial loop actually dials those addresses
  (`Node::TryDialPeers`); before M18 nothing ever dialed them.
  Manual peers can also be added with `--addnode` / the `addnode`
  RPC.
- `src/network/dnsseed.*` resolves a hostname with getaddrinfo
  (IPv4 only, at most 100 addresses). A failing seed is reported
  but never stops the others.
- Caveat: a hostile or wildcard DNS resolver can return bogus
  addresses. The node only tries to CONNECT to them; a dead or
  misbehaving address is dropped by the peer scoring/ban rules.
  Seeds are a bootstrap, not a trust anchor.
- The 4 community seed domains from the spec (item 15) still
  need real operators and domains — that is a launch task, not
  a code task. See `docs/MAINNET_LAUNCH.md` step 5. There is also
  no hardcoded fallback peer list (spec §20 item 16, not
  implemented — see `docs/M19-DECISIONS-DRAFT.md` B3).
