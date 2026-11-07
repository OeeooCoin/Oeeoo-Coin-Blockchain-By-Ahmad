# Oeeoo Coin — Audit Package (v0.1.0 — Milestone 19)

What an independent auditor receives, and how to use it.

## Contents

- **Source code** — `~/workspace/oeeoo-coin/` (this repository),
  or the matching `oeeoo-coin-milestone19.zip` (v0.1.0, local
  tag `v0.1.0`; code unchanged since Milestone 18).
- **Specification** — `docs/MASTER_SPECIFICATION.md` (26 sections,
  the final spec; the rename from EarthCoin to Oeeoo Coin is
  recorded in Appendix A).
- **Decision log** — `docs/DECISION_LOG.md`: every decision the
  spec left open (ports, schema choice, coin type 65535
  provisional, Stratum byte order, RPC method list, ...),
  each marked as an engineering decision, NOT a spec fact.
- **Milestone reports** — `docs/MILESTONE2.md` … `MILESTONE20.md`:
  what was built, the decisions, and a "Bugs found and fixed
  (disclosed)" section in each. Read these before the code —
  they record where the authors already found their own
  mistakes. Milestone 18 re-runs the full M13 audit
  (`soak18/M18C-AUDIT-DRAFT.md`); Milestone 19 re-verifies
  everything clean-room and gives the launch-readiness verdict:
  NOT READY (genesis unapproved + no independent audit).
  Milestone 20 fixes the first independent audit's findings
  (report OOC-SA-001, 2026-11-07: OOC-001 multiple coinbase,
  OOC-002 persistence semantics, OOC-003 wallet permissions)
  with regression tests and a live two-node demonstration.
- **Security review** — `docs/SECURITY_REVIEW.md`: threat
  model, invariants, attack surfaces, known gaps, and the
  auditor checklist.
- **Build guide** — `docs/BUILD_GUIDE.md`: toolchain, clean
  build, test commands. Note: `libsqlite3-dev` is a VM-level
  install and does not survive a VM replacement.
- **Test logs** — `logs/milestone*.txt`: the real output of
  every milestone's clean build + full test run.

## Reproducing the results

```sh
cd oeeoo-coin
rm -rf build && cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure
./build/oeeoo_tests   # expect: ALL CHECKS PASSED (205 sections)
```

Static analysis (as run in Milestones 13–18):

```sh
sudo apt-get install -y cppcheck
cppcheck --enable=warning,performance,portability --std=c++17 \
  --suppress=missingIncludeSystem -I src src/ tests/
# expect: 0 findings
```

Coverage-guided fuzzing (Milestone 18, needs Clang 18 with
libFuzzer — see `tests/fuzz/README.md`):

```sh
cmake -S . -B build-fuzz -DOEEOO_BUILD_FUZZERS=ON \
  -DCMAKE_CXX_COMPILER=clang++-18 \
  -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer,address,undefined"
cmake --build build-fuzz -j4
ctest --test-dir build-fuzz -R '^fuzz_'   # expect: 7/7 passed
```

## What the auditor should NOT trust

- **This package is self-review.** The authors wrote the code,
  the tests, and this document. The test suite proves the code
  does what the authors intended; it cannot prove the intent
  is right.
- **The verify page** (`oeeoo-coin-verify.html`) checks key
  calculations only. It is a teaching aid, not evidence.
- **No genesis block exists yet.** The genesis parameters
  (timestamp, nonce, Merkle root, hash) are not finalized and
  must be generated at launch, in the open, by the user.
- **No mainnet exists.** There is nothing to attack yet except
  the code in this package.

## After the audit

- The auditor's findings go into a new milestone report
  (an audit-fix round after M19 — Milestones 1–19 are already
  complete, including mainnet prep (M12) and launch readiness
  (M19)).
- Every finding is fixed, re-tested, and re-logged with the
  same disclosure discipline as Milestones 2–19.
- Only after the audit's findings are closed does the project
  move to launch (genesis approval + mainnet).
