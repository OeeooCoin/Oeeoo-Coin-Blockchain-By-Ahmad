// SPDX-License-Identifier: MIT
// Oeeoo Coin — mining implementation.
#include "mining/miner.h"

#include "consensus/consensus.h"
#include "consensus/difficulty.h"
#include "consensus/subsidy.h"
#include "script/script.h"
#include "serialize/endian.h"
#include "transactions/validation.h"

namespace oeeoo {

namespace {
// Coinbase scriptSig: 2..100 bytes (Bitcoin limits).
constexpr std::size_t kCoinbaseScriptSigMin = 2;
constexpr std::size_t kCoinbaseScriptSigMax = 100;

std::vector<unsigned char> BuildCoinbaseScriptSig(
    int height, const std::string& extraData, uint32_t extraNonce) {
  std::vector<unsigned char> scriptSig =
      PushData(EncodeScriptNum(height));
  scriptSig.insert(scriptSig.end(), extraData.begin(), extraData.end());
  if (extraNonce != 0) {
    AppendLE32(scriptSig, extraNonce);
  }
  return scriptSig;
}
}  // namespace

Transaction CreateCoinbase(
    int height, const std::vector<unsigned char>& minerScriptPubKey,
    Amount value, const std::string& extraData, uint32_t extraNonce) {
  Transaction coinbase;
  coinbase.version = 1;
  TxIn in;
  in.prevTxHash.fill(0);
  in.prevOutIndex = 0xffffffff;
  in.scriptSig = BuildCoinbaseScriptSig(height, extraData, extraNonce);
  in.sequence = 0xffffffff;
  coinbase.vin.push_back(in);
  TxOut out;
  out.value = value;
  out.scriptPubKey = minerScriptPubKey;
  coinbase.vout.push_back(out);
  coinbase.lockTime = 0;
  return coinbase;
}

bool ValidateCoinbase(const Transaction& coinbase, int height,
                      Amount maxValue, std::string& error) {
  if (coinbase.vin.size() != 1) {
    error = "coinbase must have exactly one input";
    return false;
  }
  const TxIn& in = coinbase.vin[0];
  if (in.prevTxHash != Hash256{}) {
    error = "coinbase prev hash must be zero";
    return false;
  }
  if (in.prevOutIndex != 0xffffffff) {
    error = "coinbase prev index must be 0xffffffff";
    return false;
  }
  if (in.scriptSig.size() < kCoinbaseScriptSigMin ||
      in.scriptSig.size() > kCoinbaseScriptSigMax) {
    error = "coinbase scriptSig size out of range (2..100 bytes)";
    return false;
  }
  int64_t scriptHeight = -1;
  if (!GetCoinbaseHeight(in.scriptSig, scriptHeight) ||
      scriptHeight != height) {
    error = "coinbase scriptSig does not start with the block height";
    return false;
  }
  if (!coinbase.CheckBasic()) {
    error = "coinbase fails structural checks";
    return false;
  }
  __int128 total = 0;
  for (const TxOut& out : coinbase.vout) total += out.value;
  if (total > maxValue) {
    error = "coinbase claims more than subsidy + fees";
    return false;
  }
  return true;
}

bool CheckBlockReward(const Block& block, int height, Amount fees,
                      std::string& error, const VestingParams& vesting) {
  if (block.vtx.empty()) {
    error = "block has no transactions";
    return false;
  }
  // Checked addition: subsidy + fees can never exceed the maximum
  // money supply (fees are bounded by the UTXO value spent in the
  // block), and the check makes that explicit instead of relying on
  // it implicitly.
  __int128 maxValue128 = static_cast<__int128>(GetBlockSubsidy(height)) + fees;
  if (height == 0 && vesting.enabled) {
    // Genesis with vesting: the coinbase pays the founder vault.
    maxValue128 = static_cast<__int128>(FOUNDER_ALLOCATION) + fees;
  }
  if (maxValue128 > MAX_MONEY) {
    error = "coinbase claims more than the maximum money supply";
    return false;
  }
  return ValidateCoinbase(block.vtx[0], height,
                          static_cast<Amount>(maxValue128), error);
}

bool ComputeBlockFees(const Block& block, const CoinsView& utxo, int height,
                      uint32_t medianTimePast, Amount& totalFees,
                      std::string& error) {
  totalFees = 0;
  CoinsView working = utxo;
  for (const Transaction& tx : block.vtx) {
    if (IsCoinbase(tx)) continue;  // coinbase: no fee
    TxValidationResult r =
        ValidateTransaction(tx, working, height, medianTimePast);
    if (!r.ok) {
      error = r.error;
      return false;
    }
    totalFees += r.fee;
    for (const TxIn& in : tx.vin) {
      working.SpendCoin({in.prevTxHash, in.prevOutIndex});
    }
    for (std::size_t i = 0; i < tx.vout.size(); ++i) {
      Coin coin;
      coin.out = tx.vout[i];
      coin.height = height;
      coin.isCoinbase = false;
      working.AddCoin({tx.GetTxId(), static_cast<uint32_t>(i)}, coin);
    }
  }
  return true;
}

BlockTemplate AssembleBlock(const Hash256& prevHash, int height,
                            uint32_t blockTime, uint32_t bits,
                            const Mempool& mempool, CoinsView& utxo,
                            uint32_t medianTimePast,
                            const std::vector<unsigned char>& minerScriptPubKey,
                            const std::string& extraData) {
  BlockTemplate templ;
  CoinsView working = utxo;
  std::vector<Transaction> included;
  std::size_t blockBytes = 0;
  // Reserve room for the coinbase: its size is fully determined by
  // the height, the miner script, and the extra data, so measure a
  // dummy coinbase instead of estimating (the old 250-byte estimate
  // was 15 bytes short for a 100-byte scriptSig + 105-byte multisig
  // miner output, which could have made the node reject its own
  // mined block).
  const std::size_t coinbaseReserveBytes =
      CreateCoinbase(height, minerScriptPubKey, 0, extraData)
          .Serialize()
          .size();
  for (const Mempool::Entry& entry : mempool.GetByFeeRate()) {
    if (blockBytes + entry.size + coinbaseReserveBytes >
        static_cast<std::size_t>(MAX_BLOCK_SIZE)) {
      continue;  // too big for the remaining space; try the next tx
    }
    TxValidationResult r = ValidateTransaction(entry.tx, working, height,
                                               medianTimePast);
    if (!r.ok) continue;  // invalid against current state: skip it
    for (const TxIn& in : entry.tx.vin) {
      working.SpendCoin({in.prevTxHash, in.prevOutIndex});
    }
    for (std::size_t i = 0; i < entry.tx.vout.size(); ++i) {
      Coin coin;
      coin.out = entry.tx.vout[i];
      coin.height = height;
      coin.isCoinbase = false;
      working.AddCoin({entry.tx.GetTxId(), static_cast<uint32_t>(i)}, coin);
    }
    included.push_back(entry.tx);
    templ.totalFees += r.fee;
    blockBytes += entry.size;
  }
  templ.txCount = included.size();
  Transaction coinbase =
      CreateCoinbase(height, minerScriptPubKey,
                     GetBlockSubsidy(height) + templ.totalFees, extraData);
  Block block;
  block.header.nVersion = 1;
  block.header.hashPrevBlock = prevHash;
  block.header.nTime = blockTime;
  block.header.nBits = bits;
  block.header.nNonce = 0;
  block.vtx.push_back(coinbase);
  for (const Transaction& tx : included) block.vtx.push_back(tx);
  block.header.hashMerkleRoot = block.ComputeMerkleRoot();
  templ.block = std::move(block);
  return templ;
}

void BumpExtraNonce(Block& block, int height, const std::string& extraData,
                    uint32_t extraNonce) {
  Transaction coinbase = block.vtx[0];
  coinbase.vin[0].scriptSig =
      BuildCoinbaseScriptSig(height, extraData, extraNonce);
  block.vtx[0] = coinbase;
  block.header.hashMerkleRoot = block.ComputeMerkleRoot();
}

bool MineBlock(Block& block, int height, const std::string& extraData,
               uint64_t maxIterations) {
  uint32_t extraNonce = 0;
  uint64_t tried = 0;
  uint32_t nonce = block.header.nNonce;
  while (tried < maxIterations) {
    block.header.nNonce = nonce;
    if (CheckProofOfWork(block.GetHash(), block.header.nBits)) {
      return true;
    }
    ++tried;
    if (nonce == 0xffffffff) {
      // Nonce space exhausted: bump the coinbase extraNonce, rebuild
      // the Merkle root, and start the nonce range again.
      ++extraNonce;
      BumpExtraNonce(block, height, extraData, extraNonce);
      nonce = 0;
    } else {
      ++nonce;
    }
  }
  block.header.nNonce = nonce;
  return false;
}

}  // namespace oeeoo
