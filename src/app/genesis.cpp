// SPDX-License-Identifier: MIT
// Oeeoo Coin — genesis block generation implementation.
#include "app/genesis.h"

#include <cstring>
#include <fstream>
#include <limits>
#include <thread>

#include <openssl/sha.h>

#include "blockchain/genesis.h"
#include "consensus/consensus.h"
#include "consensus/difficulty.h"
#include "consensus/vesting.h"
#include "crypto/sha256.h"
#include "mining/miner.h"
#include "network/node.h"
#include "rpc/json.h"
#include "rpc/rpcutil.h"
#include "script/script.h"
#include "transactions/validation.h"

namespace oeeoo {

namespace {
std::string HashHex(const Hash256& h) {
  return BytesToHexStr(std::vector<unsigned char>(h.begin(), h.end()));
}

// SHA-256 midstate hashing for mining: the header's first 64 bytes
// never change across nonces, so the compression of block 1 is done
// once and copied per nonce. Each nonce then costs 2 compressions
// (block 2 of the first hash + the 32-byte second hash) instead of
// 3, and no per-hash EVP setup. The low-level SHA-256 API is
// deprecated in OpenSSL 3.0 but still correct and fully supported;
// the pragma silences the warning (same pattern as ecdsa.cpp).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
bool HashWithMidstate(const SHA256_CTX& base, const unsigned char* tail,
                      Hash256& out) {
  SHA256_CTX ctx = base;
  unsigned char first[32];
  SHA256_Update(&ctx, tail, 16);
  SHA256_Final(first, &ctx);
  SHA256(first, 32, out.data());
  return true;
}
#pragma GCC diagnostic pop
}  // namespace

std::vector<unsigned char> BuildMultisigScript(
    int required, const std::vector<std::vector<unsigned char>>& pubkeys) {
  if (required < 1 || required > static_cast<int>(pubkeys.size()) ||
      pubkeys.empty() || pubkeys.size() > 20) {
    return {};
  }
  for (const auto& pk : pubkeys) {
    if (pk.size() != 33) return {};  // compressed secp256k1 only
  }
  std::vector<unsigned char> script;
  script.push_back(static_cast<unsigned char>(0x50 + required));
  for (const auto& pk : pubkeys) {
    const std::vector<unsigned char> push = PushData(pk);
    script.insert(script.end(), push.begin(), push.end());
  }
  script.push_back(
      static_cast<unsigned char>(0x50 + pubkeys.size()));
  script.push_back(OP_CHECKMULTISIG);
  return script;
}

Block BuildGenesisBlock(const std::vector<unsigned char>& founderScript,
                        uint32_t time, uint32_t bits) {
  Block block;
  block.header.nVersion = 1;
  block.header.hashPrevBlock = Hash256{};
  block.header.nTime = time;
  block.header.nBits = bits;
  block.header.nNonce = 0;
  block.vtx = {CreateCoinbase(0, founderScript, FOUNDER_ALLOCATION,
                              kGenesisMessage)};
  block.header.hashMerkleRoot = block.ComputeMerkleRoot();
  return block;
}

bool MineGenesisParallel(Block& block, int threads,
                         std::atomic<uint64_t>* hashesDone,
                         std::atomic<bool>* stop, std::string& error) {
  if (threads < 1) threads = 1;
  const std::vector<unsigned char> headerBytes = block.header.Serialize();
  // Precompute the midstate: the SHA-256 context after the header's
  // first 64 bytes (version + prev hash + the first 28 bytes of the
  // Merkle root). Only the last 16 bytes (tail) change per nonce.
  SHA256_CTX base;
  {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    SHA256_Init(&base);
    SHA256_Update(&base, headerBytes.data(), 64);
#pragma GCC diagnostic pop
  }
  std::atomic<uint64_t> winner{
      std::numeric_limits<uint64_t>::max()};
  std::atomic<bool> localStop{false};
  std::atomic<bool>& stopFlag = stop != nullptr ? *stop : localStop;
  std::atomic<uint64_t> localHashes{0};
  std::atomic<uint64_t>& counter =
      hashesDone != nullptr ? *hashesDone : localHashes;
  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t]() {
      unsigned char tail[16];
      std::memcpy(tail, headerBytes.data() + 64, 16);
      uint64_t nonce = static_cast<uint64_t>(t);
      while (!stopFlag.load(std::memory_order_relaxed)) {
        if (nonce > 0xffffffffULL) break;
        const uint32_t n = static_cast<uint32_t>(nonce);
        tail[12] = static_cast<unsigned char>(n & 0xff);
        tail[13] = static_cast<unsigned char>((n >> 8) & 0xff);
        tail[14] = static_cast<unsigned char>((n >> 16) & 0xff);
        tail[15] = static_cast<unsigned char>((n >> 24) & 0xff);
        Hash256 hash;
        HashWithMidstate(base, tail, hash);
        counter.fetch_add(1, std::memory_order_relaxed);
        if (CheckProofOfWork(hash, block.header.nBits)) {
          uint64_t expected = std::numeric_limits<uint64_t>::max();
          if (winner.compare_exchange_strong(expected, nonce)) {
            stopFlag.store(true, std::memory_order_relaxed);
          }
          return;
        }
        nonce += static_cast<uint64_t>(threads);
      }
    });
  }
  for (auto& w : workers) w.join();
  const uint64_t won = winner.load();
  if (won == std::numeric_limits<uint64_t>::max()) {
    error = "nonce space exhausted without a valid block";
    return false;
  }
  block.header.nNonce = static_cast<uint32_t>(won);
  return true;
}

bool WriteGenesisFile(const std::string& path, const Block& block,
                      const std::vector<unsigned char>& founderScript,
                      std::string& error) {
  Json file = Json::Object();
  file.Set("block", BytesToHexStr(block.Serialize()));
  file.Set("hash", HashHex(block.GetHash()));
  file.Set("merkleroot", HashHex(block.header.hashMerkleRoot));
  file.Set("nonce", static_cast<int64_t>(block.header.nNonce));
  file.Set("time", static_cast<int64_t>(block.header.nTime));
  char bitsBuf[16];
  std::snprintf(bitsBuf, sizeof(bitsBuf), "%08x", block.header.nBits);
  file.Set("bits", std::string(bitsBuf));
  file.Set("founder_script", BytesToHexStr(founderScript));
  file.Set("message", std::string(kGenesisMessage));
  std::ofstream out(path);
  if (!out) {
    error = "cannot write genesis file: " + path;
    return false;
  }
  out << file.Dump() << "\n";
  if (!out) {
    error = "failed writing genesis file: " + path;
    return false;
  }
  return true;
}

bool ReadGenesisFile(const std::string& path, Block& block,
                     std::vector<unsigned char>& founderScript,
                     std::string& error) {
  std::ifstream in(path);
  if (!in) {
    error = "cannot open genesis file: " + path;
    return false;
  }
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  Json file;
  std::string parseError;
  if (!Json::Parse(text, file, parseError)) {
    error = "genesis file is not valid JSON: " + parseError;
    return false;
  }
  const Json* blockHex = file.Get("block");
  const Json* hashHex = file.Get("hash");
  const Json* scriptHex = file.Get("founder_script");
  const Json* timeJson = file.Get("time");
  const Json* merkleHex = file.Get("merkleroot");
  const Json* nonceJson = file.Get("nonce");
  const Json* bitsJson = file.Get("bits");
  const Json* messageJson = file.Get("message");
  if (blockHex == nullptr || hashHex == nullptr || scriptHex == nullptr ||
      timeJson == nullptr || merkleHex == nullptr || nonceJson == nullptr ||
      bitsJson == nullptr || messageJson == nullptr ||
      !blockHex->IsString() || !hashHex->IsString() ||
      !scriptHex->IsString() || !timeJson->IsInt() ||
      !merkleHex->IsString() || !nonceJson->IsInt() ||
      !bitsJson->IsString() || !messageJson->IsString()) {
    error = "genesis file is missing required fields";
    return false;
  }
  std::vector<unsigned char> blockBytes;
  if (!HexToBytesStr(blockHex->AsString(), blockBytes) ||
      !Block::Deserialize(blockBytes, block)) {
    error = "genesis block does not deserialize";
    return false;
  }
  if (HashHex(block.GetHash()) != hashHex->AsString()) {
    error = "genesis block hash does not match the file";
    return false;
  }
  if (block.header.nTime !=
      static_cast<uint32_t>(timeJson->AsInt())) {
    error = "genesis block time does not match the file";
    return false;
  }
  // Every recorded field is verified, not just the hash: a file
  // whose metadata disagrees with its block is refused.
  if (HashHex(block.header.hashMerkleRoot) != merkleHex->AsString()) {
    error = "genesis Merkle root does not match the file";
    return false;
  }
  if (block.header.nNonce != static_cast<uint32_t>(nonceJson->AsInt())) {
    error = "genesis nonce does not match the file";
    return false;
  }
  char bitsBuf[16];
  std::snprintf(bitsBuf, sizeof(bitsBuf), "%08x", block.header.nBits);
  if (bitsJson->AsString() != std::string(bitsBuf)) {
    error = "genesis difficulty bits do not match the file";
    return false;
  }
  if (messageJson->AsString() != std::string(kGenesisMessage)) {
    error = "genesis message does not match the specification";
    return false;
  }
  if (!CheckProofOfWork(block.GetHash(), block.header.nBits)) {
    error = "genesis block does not meet its difficulty";
    return false;
  }
  if (!HexToBytesStr(scriptHex->AsString(), founderScript)) {
    error = "founder script is not valid hex";
    return false;
  }
  // The block's own Merkle root must match its transactions (the
  // hash check above covers the serialized block, but this catches
  // a header whose root was never recomputed after a change).
  if (block.header.hashMerkleRoot != block.ComputeMerkleRoot()) {
    error = "genesis Merkle root does not match its transactions";
    return false;
  }
  VestingParams vesting;
  vesting.enabled = true;
  vesting.launchTime = block.header.nTime;
  vesting.founderScript = founderScript;
  if (!ValidateGenesisCoinbase(block, vesting, error)) {
    error = "genesis coinbase fails the founder rule: " + error;
    return false;
  }
  return true;
}

bool CheckGenesisIdentity(Node& node, const Block& genesisFileBlock,
                          bool haveGenesisFile,
                          const std::string& checkpointHex,
                          std::string& error) {
  Hash256 checkpoint;
  bool haveCheckpoint = false;
  if (!checkpointHex.empty()) {
    std::vector<unsigned char> bytes;
    if (!HexToBytesStr(checkpointHex, bytes) || bytes.size() != 32) {
      error = "genesis checkpoint is not a 32-byte hash";
      return false;
    }
    std::copy(bytes.begin(), bytes.end(), checkpoint.begin());
    haveCheckpoint = true;
  }
  if (node.TipHeight() >= 0) {
    Hash256 storedGenesis;
    if (!node.GenesisHash(storedGenesis)) {
      error = "cannot read the stored genesis block";
      return false;
    }
    if (haveGenesisFile && genesisFileBlock.GetHash() != storedGenesis) {
      error = "--genesis-file does not match the genesis block in this "
              "data directory (" +
              HashHex(genesisFileBlock.GetHash()) + " vs " +
              HashHex(storedGenesis) + ")";
      return false;
    }
    if (haveCheckpoint && storedGenesis != checkpoint) {
      error = "stored genesis " + HashHex(storedGenesis) +
              " does not match the genesis checkpoint " + checkpointHex;
      return false;
    }
    return true;
  }
  // Fresh chain: only the file's hash can be checked.
  if (haveGenesisFile && haveCheckpoint &&
      genesisFileBlock.GetHash() != checkpoint) {
    error = "genesis file hash " + HashHex(genesisFileBlock.GetHash()) +
            " does not match the genesis checkpoint " + checkpointHex;
    return false;
  }
  return true;
}

}  // namespace oeeoo
