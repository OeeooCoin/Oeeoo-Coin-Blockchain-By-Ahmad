// SPDX-License-Identifier: MIT
// Oeeoo Coin — mining (Milestone 5): coinbase construction, block
// template assembly, nonce search, reward validation.
// A coinbase pays exactly the block subsidy plus collected fees (it
// may claim less; never more). Its scriptSig starts with the block
// height (BIP34). Block templates are assembled from a mempool's
// transactions, highest fee rate first, within the 1 MB block limit.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "blockchain/block.h"
#include "consensus/amount.h"
#include "consensus/vesting.h"
#include "mempool/mempool.h"
#include "utxo/coins.h"

namespace oeeoo {

// Build a coinbase transaction for `height` paying `value` (subsidy +
// fees) to `minerScriptPubKey`. scriptSig = push(height) + extraData
// (+ 4-byte LE extraNonce when non-zero). scriptSig is at most 100
// bytes (Bitcoin limit).
Transaction CreateCoinbase(int height,
                           const std::vector<unsigned char>& minerScriptPubKey,
                           Amount value, const std::string& extraData = "",
                           uint32_t extraNonce = 0);

// Validate a coinbase transaction for `height`: single input, zero
// prev hash, 0xffffffff prev index, scriptSig size 2..100 bytes
// starting with the height, outputs in MoneyRange, and total output
// value <= `maxValue` (subsidy + fees). False + `error` on failure.
bool ValidateCoinbase(const Transaction& coinbase, int height,
                      Amount maxValue, std::string& error);

// Check that the first transaction of `block` is a valid coinbase for
// `height` claiming at most subsidy(height) + `fees`. At height 0
// with vesting enabled, the allowed value is the founder allocation
// instead (the genesis coinbase pays the founder vault).
bool CheckBlockReward(const Block& block, int height, Amount fees,
                      std::string& error,
                      const VestingParams& vesting = VestingParams{});

// Sum the fees of a block's non-coinbase transactions against `utxo`
// (each must validate). False + `error` on the first invalid tx.
bool ComputeBlockFees(const Block& block, const CoinsView& utxo, int height,
                      uint32_t medianTimePast, Amount& totalFees,
                      std::string& error);

struct BlockTemplate {
  Block block;
  Amount totalFees = 0;
  std::size_t txCount = 0;  // non-coinbase transactions included
};

// Assemble a block template on `prevHash` at `height`: take the
// mempool's transactions in fee-rate order, validate each against a
// working copy of `utxo` (invalid or over-budget ones are skipped),
// then build the coinbase paying subsidy + collected fees to the
// miner. The block is unmined (nonce 0); call MineBlock to search.
BlockTemplate AssembleBlock(const Hash256& prevHash, int height,
                            uint32_t blockTime, uint32_t bits,
                            const Mempool& mempool, CoinsView& utxo,
                            uint32_t medianTimePast,
                            const std::vector<unsigned char>& minerScriptPubKey,
                            const std::string& extraData = "");

// Rebuild a block's coinbase with a new extraNonce (only the
// scriptSig changes) and recompute the Merkle root. Used by MineBlock
// when the nonce range is exhausted; exposed for testing.
void BumpExtraNonce(Block& block, int height, const std::string& extraData,
                    uint32_t extraNonce);

// Search for a nonce making the block satisfy proof of work. On nonce
// exhaustion (0xffffffff reached) the coinbase extraNonce is bumped,
// the Merkle root recomputed, and the search continues. Returns true
// when a valid nonce is found; false when `maxIterations` hashes
// have been tried without success. `block.header.nNonce` is left at
// the winning nonce (or at the last tried nonce on failure).
bool MineBlock(Block& block, int height, const std::string& extraData,
               uint64_t maxIterations = UINT64_MAX);

}  // namespace oeeoo
