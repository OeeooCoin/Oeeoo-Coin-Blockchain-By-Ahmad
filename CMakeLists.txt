cmake_minimum_required(VERSION 3.16)
project(OeeooCoin VERSION 0.1.0 LANGUAGES CXX)

# Oeeoo Coin — C++ foundation (Phase 1) + Milestone 2 (serialization).
# C++17, warnings on, no extensions. External dependencies: OpenSSL
# (SHA-256 via EVP, never hand-rolled crypto) and SQLite3 (Milestone 7
# chain store). On a fresh machine: sudo apt-get install libssl-dev
# libsqlite3-dev
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra)

find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)

# The core source list, shared by oeeoo_core and (Milestone 18)
# the coverage-instrumented oeeoo_core_fuzz library below.
set(OEEOO_CORE_SOURCES
  src/consensus/subsidy.cpp
  src/consensus/uint256.cpp
  src/consensus/difficulty.cpp
  src/consensus/vesting.cpp
  src/crypto/sha256.cpp
  src/crypto/base58.cpp
  src/crypto/hash160.cpp
  src/crypto/address.cpp
  src/crypto/ecdsa.cpp
  src/serialize/varint.cpp
  src/transactions/transaction.cpp
  src/transactions/sighash.cpp
  src/transactions/validation.cpp
  src/transactions/dust.cpp
  src/script/script.cpp
  src/utxo/coins.cpp
  src/mempool/mempool.cpp
  src/mempool/mempooldump.cpp
  src/mempool/feeestimator.cpp
  src/mempool/orphan.cpp
  src/mining/miner.cpp
  src/network/protocol.cpp
  src/network/dnsseed.cpp
  src/network/siphash.cpp
  src/network/compact.cpp
  src/network/peer.cpp
  src/network/addrman.cpp
  src/network/transport.cpp
  src/network/node.cpp
  src/blockchain/block.cpp
  src/blockchain/chain.cpp
  src/blockchain/merkle.cpp
  src/blockchain/verify.cpp
  src/db/database.cpp
  src/db/chainstore.cpp
  src/wallet/bip39.cpp
  src/wallet/bip32.cpp
  src/wallet/wallet.cpp
  src/wallet/walletfile.cpp
  src/wallet/txjournal.cpp
  src/wallet/nodewallet.cpp
  src/rpc/json.cpp
  src/rpc/rpcserver.cpp
  src/rpc/stratum.cpp
  src/app/config.cpp
  src/app/genesis.cpp
  src/app/cli.cpp
)

add_library(oeeoo_core ${OEEOO_CORE_SOURCES})
target_include_directories(oeeoo_core PUBLIC src)
target_link_libraries(oeeoo_core PUBLIC OpenSSL::Crypto SQLite::SQLite3)

enable_testing()

add_executable(oeeoo_tests
  tests/test_main.cpp
  tests/test_amount.cpp
  tests/test_subsidy.cpp
  tests/test_crypto.cpp
  tests/test_block.cpp
  tests/test_varint.cpp
  tests/test_transaction.cpp
  tests/test_merkle.cpp
  tests/test_address.cpp
  tests/test_difficulty.cpp
  tests/test_chain.cpp
  tests/test_script.cpp
  tests/test_validation.cpp
  tests/test_mining.cpp
  tests/test_network.cpp
  tests/test_node.cpp
  tests/test_db.cpp
  tests/test_wallet.cpp
  tests/test_vesting.cpp
  tests/test_rpc.cpp
  tests/test_stratum.cpp
  tests/test_reorg.cpp
  tests/test_verify.cpp
  tests/test_fuzz.cpp
  tests/test_config.cpp
  tests/test_genesis.cpp
  tests/test_cli.cpp
  tests/test_dnsseed.cpp
  tests/test_milestone13.cpp
  tests/test_milestone14.cpp
  tests/test_milestone15.cpp
  tests/test_milestone16a.cpp
  tests/test_milestone16b.cpp
  tests/test_milestone16c.cpp
  tests/test_milestone17a.cpp
  tests/test_milestone17b.cpp
  tests/test_milestone18a.cpp
  tests/test_milestone18b.cpp
  tests/test_milestone18c.cpp
)
target_link_libraries(oeeoo_tests PRIVATE oeeoo_core)
add_test(NAME oeeoo_tests COMMAND oeeoo_tests)

# Benchmark harness (Milestone 17).
add_executable(oeeoo_bench src/bench/bench.cpp)
target_link_libraries(oeeoo_bench PRIVATE oeeoo_core)

# Runnable binaries (Milestone 12).
add_executable(oeeoo-node src/app/node_main.cpp)
target_link_libraries(oeeoo-node PRIVATE oeeoo_core)
add_executable(oeeoo-genesis src/app/genesis_main.cpp)
target_link_libraries(oeeoo-genesis PRIVATE oeeoo_core)
add_executable(oeeoo-cli src/app/cli_main.cpp)
target_link_libraries(oeeoo-cli PRIVATE oeeoo_core)

# ---- Milestone 18 (Stream B): coverage-guided fuzzing ----
# libFuzzer needs Clang; the g++ build leaves this OFF so the
# default build never breaks. Enable with a Clang configure:
#   cmake -B build-m18b -DCMAKE_CXX_COMPILER=clang++-18 \
#     -DOEEOO_BUILD_FUZZERS=ON ..
# The fuzz targets link a second copy of the core, built with
# -fsanitize=fuzzer (coverage instrumentation) on top of
# ASan/UBSan, so libFuzzer's coverage guidance sees the real
# parser code. All runs are bounded and fixed-seed (CI time);
# the script target runs 2,000 iterations (~30 s: each input
# pays up to three ECDSA verifications) and the wallet target
# 20 (~40 s: each magic-matching input pays a 200k-iteration
# PBKDF2, ~1.7 s on this VM); see tests/fuzz/README.md.
option(OEEOO_BUILD_FUZZERS "Build libFuzzer harnesses (requires Clang)" OFF)
if(OEEOO_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
      "OEEOO_BUILD_FUZZERS requires Clang (libFuzzer); configure with "
      "-DCMAKE_CXX_COMPILER=clang++-18")
  endif()
  add_library(oeeoo_core_fuzz ${OEEOO_CORE_SOURCES})
  target_include_directories(oeeoo_core_fuzz PUBLIC src)
  target_link_libraries(oeeoo_core_fuzz PUBLIC OpenSSL::Crypto
                                                SQLite::SQLite3)
  target_compile_options(oeeoo_core_fuzz PRIVATE
    -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
  target_link_options(oeeoo_core_fuzz PRIVATE
    -fsanitize=fuzzer,address,undefined)

  # Seed-corpus generator (run once; outputs are checked in).
  add_executable(oeeoo_gen_corpus tests/fuzz/gen_corpus.cpp)
  target_include_directories(oeeoo_gen_corpus PRIVATE src/fuzz)
  target_link_libraries(oeeoo_gen_corpus PRIVATE oeeoo_core)

  foreach(fuzz_target tx block script json p2p compact wallet)
    add_executable(fuzz_${fuzz_target} src/fuzz/fuzz_${fuzz_target}.cpp)
    target_include_directories(fuzz_${fuzz_target} PRIVATE src/fuzz)
    target_link_libraries(fuzz_${fuzz_target} PRIVATE oeeoo_core_fuzz)
    target_compile_options(fuzz_${fuzz_target} PRIVATE
      -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
    target_link_options(fuzz_${fuzz_target} PRIVATE
      -fsanitize=fuzzer,address,undefined)
  endforeach()

  # Bounded, fixed-seed CI runs. -timeout=10 fails a hanging
  # input instead of stalling CI; -rss_limit_mb caps memory.
  # libFuzzer writes newly-interesting inputs back into the
  # corpus directory it runs against, so the tests run against a
  # COPY of the checked-in seeds in the build dir (copied at
  # configure time), never against tests/fuzz/corpus itself.
  file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus
       DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
  set(_fuzz_corpus ${CMAKE_CURRENT_BINARY_DIR}/corpus)
  set(_fuzz_flags -timeout=10 -rss_limit_mb=1024)
  add_test(NAME fuzz_tx
    COMMAND fuzz_tx -runs=20000 -seed=1 ${_fuzz_flags}
            ${_fuzz_corpus}/tx)
  add_test(NAME fuzz_block
    COMMAND fuzz_block -runs=20000 -seed=2 ${_fuzz_flags}
            ${_fuzz_corpus}/block)
  add_test(NAME fuzz_script
    COMMAND fuzz_script -runs=2000 -seed=3 ${_fuzz_flags}
            ${_fuzz_corpus}/script)
  add_test(NAME fuzz_json
    COMMAND fuzz_json -runs=20000 -seed=4 ${_fuzz_flags}
            ${_fuzz_corpus}/json)
  add_test(NAME fuzz_p2p
    COMMAND fuzz_p2p -runs=20000 -seed=5 ${_fuzz_flags}
            ${_fuzz_corpus}/p2p)
  add_test(NAME fuzz_compact
    COMMAND fuzz_compact -runs=20000 -seed=6 ${_fuzz_flags}
            ${_fuzz_corpus}/compact)
  add_test(NAME fuzz_wallet
    COMMAND fuzz_wallet -runs=20 -seed=7 ${_fuzz_flags}
            ${_fuzz_corpus}/wallet)
  # Milestone 18 (coordinator fix): LeakSanitizer must be off for the
  # fuzz tests, exactly as for the main suite (detect_leaks=0 is the
  # repo's documented practice since M13). The fuzzers link OpenSSL
  # and any run whose inputs initialize libcrypto's legacy provider
  # (RIPEMD-160 via EVP) ends with the known benign one-time leak
  # (6,513 bytes in 117 allocations, all indirect inside libcrypto's
  # one-time globals; zero direct leaks from project code — verified
  # in the M18 audit re-run). Without this, fuzz_script fails
  # nondeterministically at process exit AFTER all 2,000 runs pass:
  # LSan fires, libFuzzer writes a 0-byte "crash" artifact (the SHA1
  # of the empty input), and ctest reports the test failed. The runs
  # themselves were always clean; only the exit-time leak check
  # failed. Real ASan/UBSan errors (heap-buffer-overflow,
  # use-after-free, etc.) are unaffected by detect_leaks=0 and
  # still fail the test.
  set_tests_properties(fuzz_tx fuzz_block fuzz_script fuzz_json
    fuzz_p2p fuzz_compact fuzz_wallet
    PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
endif()
