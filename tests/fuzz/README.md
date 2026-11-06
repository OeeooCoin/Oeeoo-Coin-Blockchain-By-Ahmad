# Oeeoo Coin — fuzzing (Milestone 18, Stream B)

Two layers, both deterministic and CI-time bounded:

## 1. Coverage-guided fuzzing (libFuzzer)

Requires Clang (the VM builds with g++ by default). Toolchain
check, recorded in `docs/MILESTONE18.md`: Clang was NOT
installed at M18 start (`which clang clang++ afl-fuzz` empty);
Clang 18.1.3 + compiler-rt (libFuzzer) + cppcheck 2.13.0 +
libsqlite3-dev were installed from the Ubuntu 24.04 archive
(manual `.deb` download — `apt-get update` was held by a stuck
lock from another process — then `dpkg -i`). AFL++ was not
needed: libFuzzer is the chosen engine. System packages do NOT
survive a VM replacement: after the VM was replaced mid-M18,
all of them were gone and had to be reinstalled from the cached
`.deb`s with `~/workspace/m18b-toolchain-reinstall.sh`.

Build (separate dir; never the shared `build/`):

```sh
cmake -B build-m18b -DCMAKE_CXX_COMPILER=clang++-18 \
  -DCMAKE_C_COMPILER=clang-18 \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DOEEOO_BUILD_FUZZERS=ON ..
cmake --build build-m18b -j4
```

Targets (`src/fuzz/`, one binary per parser, all linked against
the coverage-instrumented `oeeoo_core_fuzz` library):

| binary | target | corpus |
|---|---|---|
| `fuzz_tx` | transaction deserialize + validate + serialize round-trip | `corpus/tx/` |
| `fuzz_block` | block + header deserialize + CheckBasic | `corpus/block/` |
| `fuzz_script` | script interpreter, both directions | `corpus/script/` |
| `fuzz_json` | JSON parse + Dump round-trip + IntFromString | `corpus/json/` |
| `fuzz_p2p` | P2P framing + every payload parser | `corpus/p2p/` |
| `fuzz_compact` | compact blocks, getblocktxn, blocktxn, reconstruct | `corpus/compact/` |
| `fuzz_wallet` | encrypted wallet file loader | `corpus/wallet/` |

Run (what `ctest` does in a fuzz build — fixed seeds, bounded
iterations, no hangs). NEVER run a fuzzer directly against
`tests/fuzz/corpus/`: libFuzzer writes newly-interesting inputs
back into the corpus directory, which would pollute the checked-in
seeds (this happened once during M18 development: 600+ stray
files). Always run against a copy:

```sh
cd build-m18b
ctest -R '^fuzz_' --output-on-failure
# or one target, longer, against a scratch copy:
cp -r ../tests/fuzz/corpus /tmp/fuzz-corpus
./fuzz_tx -runs=100000 -seed=1 /tmp/fuzz-corpus/tx/
```

Each `ctest` entry passes `-runs=N -seed=S -timeout=10
-rss_limit_mb=1024`. The script target runs 2,000 iterations
(~30 s: each input pays up to three ECDSA verifications) and
the wallet target 20 (~40 s): `LoadWalletFile` costs a
200,000-iteration PBKDF2 per magic-matching input (~1.7 s on
this VM, measured directly against OpenSSL).

Regenerate the seed corpus (checked in; the generator is the
source of truth):

```sh
./oeeoo_gen_corpus tests/fuzz/corpus
```

The wallet seed is a real encrypted file with the PUBLIC test
passphrase `fuzz-corpus-passphrase` and the BIP39 test mnemonic
(`abandon` ×11 + `about`). Test data only — no funds, and the
passphrase is documented here on purpose.

Coverage (after a run with the coverage build):

```sh
./fuzz_tx -runs=20000 -seed=1 corpus/tx/ -print_coverage=1
# or llvm-cov over a .profraw run:
LLVM_PROFILE_FILE=fuzz.profraw ./fuzz_tx -runs=20000 corpus/tx/
llvm-profdata-18 merge -sparse fuzz.profraw -o fuzz.profdata
llvm-cov-18 report ./fuzz_tx -instr-profile=fuzz.profdata
```

## 2. Live fuzzing harnesses (`tests/test_milestone18b.cpp`)

Deterministic, fixed-seed mutation fuzzing against REAL servers
in the unit-test binary (`oeeoo_tests`, any compiler):

- **P2P**: mutated `version`/`tx`/`block`/`addr`/`cmpctblock`/
  `inv`/`ping`/`headers` payloads into a real `Node` — both via
  a second node (`SendRawToAddress`) and via a raw
  `LoopbackTransport` peer (split chunks, concatenated messages,
  bad magic, oversize length, bad checksum). The victim must stay
  alive with its tip and mempool intact; bans/disconnects of the
  attacker are expected, not failures.
- **RPC**: mutated JSON-RPC over HTTP — deep nesting, huge
  numbers, wrong types, unknown methods, batches, empty/huge
  bodies, bad `Content-Length` — the server must keep answering
  a valid `getblockcount` afterwards.
- **Stratum**: mutated `mining.submit`/`subscribe`/`authorize`
  lines over TCP, including a 20 MB no-newline flood — the
  server must drop the flooding client and keep serving new
  ones.

Core's caveat applies and is documented in `src/fuzz/fuzz_p2p.cpp`:
a fuzzer cannot produce valid PoW or valid signatures, so
acceptance paths are unreachable from mutated bytes; the live
harness covers the rejection paths, and valid baseline messages
cover the acceptance paths. Validation is NOT mocked out
anywhere: the live harness runs the real Node.

## Findings fixed by this fuzzing (all non-consensus)

- **M18B-R1** — RPC `Content-Length: abc` (or out-of-range)
  threw out of `std::stoul` with no catch anywhere up the
  worker thread → `std::terminate` → whole node aborted,
  unauthenticated. Fixed: strict digits-only parse in
  `RpcServer::HandleConnection`, 400 on bad input.
- **M18B-R2** — wallet payload `mnemonicLen` overflow: `16 +
  mnemonicLen` in uint32 wrapped to 0, so
  `std::string::assign` read ~4 GB out of a 16-byte plaintext
  buffer (ASan heap-buffer-overflow). Reachable only with a
  valid GCM tag (i.e. with the passphrase), but the loader must
  never trust the length field. Fixed: size_t comparison in
  `ParsePayload`.
- **M18B-R3** — Stratum session buffer grew without bound when
  a client sent data with no newline (20 MB held for a 20 MB
  stream). Fixed: drop the connection when the unterminated
  tail exceeds 1 MB. The cap is deliberately ABOVE the longest
  accepted line (65536 bytes): a long but terminated line is
  skipped on arrival, so capping at 65536 would have killed
  legitimate long lines mid-stream (caught by the 70 KB-line
  test in `test_milestone18b.cpp`).
- **M18B-R4** (latent, no callers yet) — `Json::IntFromString`
  called `std::stod` unguarded inside its own catch block, so
  garbage input threw out. Fixed: nested guard, returns null.
