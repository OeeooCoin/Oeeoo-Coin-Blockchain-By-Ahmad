// Oeeoo Coin consensus constants — Phase 1 source of truth.
// These values are FINALIZED policy from the master specification.
// Do not change them without the community governance process.
// Unresolved values are NOT in this file; see docs/DECISION_LOG.md.
#pragma once

#include <cstdint>

namespace oeeoo {

// ---- Money (integer smallest units only; NEVER floating point) ----
static constexpr int64_t COIN = 100000000LL;  // 1 OEEOO = 1e8 units
static constexpr int64_t MAX_SUPPLY_OEEOO = 21000000LL;
static constexpr int64_t FOUNDER_ALLOCATION_OEEOO = 1000000LL;
static constexpr int64_t MINING_ALLOCATION_OEEOO = 20000000LL;
// 21,000,000 * 100,000,000 = 2,100,000,000,000,000 < INT64_MAX.
static constexpr int64_t MAX_MONEY = MAX_SUPPLY_OEEOO * COIN;
static constexpr int64_t MINING_ALLOCATION = MINING_ALLOCATION_OEEOO * COIN;
static constexpr int64_t FOUNDER_ALLOCATION = FOUNDER_ALLOCATION_OEEOO * COIN;
static constexpr int64_t INITIAL_BLOCK_REWARD = 50 * COIN;

// ---- Consensus timing / sizing ----
static constexpr int TARGET_BLOCK_TIME_SECONDS = 600;  // 10 minutes
static constexpr int DIFFICULTY_INTERVAL = 2016;      // blocks per retarget
static constexpr int HALVING_INTERVAL = 210000;        // blocks per era
// Last subsidy era is shorter: blocks 840,001-940,000 (100,000 blocks).
static constexpr int LAST_SUBSIDY_HEIGHT = 940000;
static constexpr int MAX_BLOCK_SIZE = 1000000;  // 1 MB
static constexpr int COINBASE_MATURITY = 100;    // blocks

// ---- Network ----
static constexpr uint16_t MAINNET_PORT = 64775;
static constexpr int MAX_PEER_CONNECTIONS = 125;
static constexpr int MAX_OUTBOUND_CONNECTIONS = 10;

// Network magic — CANDIDATE, calculated (not invented):
// SHA-256("Oeeoo Coin Mainnet Network Magic") = e2d804d6465fe089...
// First four bytes, in hash order: e2 d8 04 d6.
// Recorded here so every implementation can test against it.
// Final confirmation happens in Milestone 12 with the genesis parameters.
static constexpr unsigned char NETWORK_MAGIC[4] = {0xe2, 0xd8, 0x04, 0xd6};

// ---- Mempool / fees (policy, not unconditional consensus maxima) ----
static constexpr int MEMPOOL_DEFAULT_MB = 300;
static constexpr int MEMPOOL_EXPIRY_DAYS = 14;
static constexpr int MIN_RELAY_FEE_SAT_VB = 1;
static constexpr int WALLET_DEFAULT_MAX_FEE_SAT_VB = 100;

// ---- Wallet / addresses (Phase 1: verified parameters only) ----
// P2PKH version byte 0x0f was VERIFIED by brute-force search over all
// 256 version bytes (3,000 random payloads each): its Base58Check
// addresses are 34 chars and start with "7". (An earlier comment
// claimed a "75" prefix; that is NOT guaranteed — the second
// character depends on the key's hash. The "75Pk..." address in
// docs/DECISION_LOG.md is a brute-forced example, not a prefix
// guarantee. The user decided 2026-11-06 to drop "75Pk" and keep
// 0x0f for its distinct "7" prefix.)
static constexpr unsigned char P2PKH_VERSION_BYTE = 0x0f;

// SLIP-0044 coin type: NOT REGISTERED. Provisional development
// identifier 65535 (0xffff) — clearly marked, never to be described
// as officially registered. Must be replaced after registration.
static constexpr uint32_t SLIP44_COIN_TYPE_PROVISIONAL = 65535;

}  // namespace oeeoo
