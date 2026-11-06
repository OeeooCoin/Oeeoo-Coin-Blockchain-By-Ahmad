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

add_library(oeeoo_core
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
  src/wallet/nodewallet.cpp
  src/rpc/json.cpp
  src/rpc/rpcserver.cpp
  src/rpc/stratum.cpp
  src/app/config.cpp
  src/app/genesis.cpp
  src/app/cli.cpp
)
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
)
target_link_libraries(oeeoo_tests PRIVATE oeeoo_core)
add_test(NAME oeeoo_tests COMMAND oeeoo_tests)

# Runnable binaries (Milestone 12).
add_executable(oeeoo-node src/app/node_main.cpp)
target_link_libraries(oeeoo-node PRIVATE oeeoo_core)
add_executable(oeeoo-genesis src/app/genesis_main.cpp)
target_link_libraries(oeeoo-genesis PRIVATE oeeoo_core)
add_executable(oeeoo-cli src/app/cli_main.cpp)
target_link_libraries(oeeoo-cli PRIVATE oeeoo_core)
