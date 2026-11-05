// SPDX-License-Identifier: MIT
// Oeeoo Coin — full chain re-validation implementation.
#include "blockchain/verify.h"

#include <tuple>

#include "blockchain/chain.h"
#include "mining/miner.h"
#include "transactions/validation.h"
#include "utxo/coins.h"

namespace oeeoo {

bool VerifyChainFromStore(ChainStore& store, const VestingParams& vesting,
                          uint32_t powLimitBits, Hash256& tipOut,
                          std::string& error) {
  Hash256 storedTip;
  if (!store.LoadTip(storedTip, error)) return false;
  std::vector<std::tuple<int, BlockHeader, Target>> headers;
  if (!store.GetAllHeaders(headers, error)) return false;
  if (headers.empty()) {
    error = "stored chain has no headers";
    return false;
  }
  ChainState chain(powLimitBits);
  chain.SetVestingParams(vesting);
  for (const auto& entry : headers) {
    std::string indexError;
    if (!chain.AddIndexEntry(std::get<1>(entry), std::get<2>(entry),
                             indexError)) {
      error = "stored header failed validation at height " +
              std::to_string(std::get<0>(entry)) + ": " + indexError;
      return false;
    }
  }
  if (chain.GetTipHash() != storedTip) {
    error = "stored tip does not match the rebuilt chain tip";
    return false;
  }
  const int tipHeight = chain.GetTipHeight();
  CoinsView utxo;
  for (int height = 0; height <= tipHeight; ++height) {
    Hash256 hash;
    if (!chain.GetBlockHashAtHeight(height, hash)) {
      error = "cannot find the block at height " + std::to_string(height);
      return false;
    }
    Block block;
    std::string blockError;
    if (!store.GetBlock(hash, block, blockError)) {
      error = "block at height " + std::to_string(height) +
              " cannot be re-validated: " + blockError;
      return false;
    }
    uint32_t mtp = 0;
    if (!chain.GetMedianTimePastAtHeight(height, mtp)) {
      error = "cannot compute MTP at height " + std::to_string(height);
      return false;
    }
    Amount fees = 0;
    if (!ComputeBlockFees(block, utxo, height, mtp, fees, error)) {
      error = "block at height " + std::to_string(height) +
              " fails fee validation: " + error;
      return false;
    }
    if (!CheckBlockReward(block, height, fees, error, vesting)) {
      error = "block at height " + std::to_string(height) +
              " fails reward validation: " + error;
      return false;
    }
    std::string connError;
    if (!ConnectBlockTransactions(block, utxo, height, mtp,
                                  block.header.nTime, fees, connError,
                                  vesting)) {
      error = "block at height " + std::to_string(height) +
              " fails transaction validation: " + connError;
      return false;
    }
  }
  // The rebuilt UTXO set must exactly match the store's table.
  std::vector<std::pair<OutPoint, Coin>> storedCoins;
  if (!store.GetAllCoins(storedCoins, error)) return false;
  std::vector<std::pair<OutPoint, Coin>> rebuiltCoins;
  utxo.GetAllCoins(rebuiltCoins);
  if (storedCoins.size() != rebuiltCoins.size()) {
    error = "stored UTXO set size (" +
            std::to_string(storedCoins.size()) +
            ") does not match the replayed chain (" +
            std::to_string(rebuiltCoins.size()) + ")";
    return false;
  }
  for (const auto& entry : rebuiltCoins) {
    Coin stored;
    bool found = false;
    for (const auto& storedEntry : storedCoins) {
      if (storedEntry.first == entry.first) {
        stored = storedEntry.second;
        found = true;
        break;
      }
    }
    if (!found) {
      error = "replayed UTXO is missing from the store";
      return false;
    }
    const Coin& rebuilt = entry.second;
    if (stored.out.value != rebuilt.out.value ||
        stored.out.scriptPubKey != rebuilt.out.scriptPubKey ||
        stored.height != rebuilt.height ||
        stored.blockTime != rebuilt.blockTime ||
        stored.isCoinbase != rebuilt.isCoinbase) {
      error = "stored UTXO does not match the replayed chain";
      return false;
    }
  }
  tipOut = storedTip;
  return true;
}

}  // namespace oeeoo
