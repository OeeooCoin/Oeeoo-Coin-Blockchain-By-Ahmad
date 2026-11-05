// SPDX-License-Identifier: MIT
// Oeeoo Coin — genesis block generation (Milestone 12).
// The genesis block is NOT finalized in the source code: it is
// generated at launch, in the open, by the operator (see
// docs/MAINNET_LAUNCH.md). This module builds it, mines it, and
// writes/reads the genesis file the node consumes.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "blockchain/block.h"

namespace oeeoo {

class Node;

// Verify that a genesis file matches the chain already loaded in
// `node` (a different genesis is refused, never silently ignored),
// and that the loaded genesis matches `checkpointHex` (empty = no
// checkpoint). `haveGenesisFile` says whether a genesis file was
// supplied. On a fresh (empty) chain this checks the file's hash
// against the checkpoint only. Returns false + `error` on any
// mismatch.
bool CheckGenesisIdentity(Node& node, const Block& genesisFileBlock,
                          bool haveGenesisFile,
                          const std::string& checkpointHex,
                          std::string& error);

// Build a bare m-of-n multisig script:
// OP_m <pk1> ... <pkn> OP_n OP_CHECKMULTISIG.
// Returns an empty script when the parameters are invalid
// (required < 1, required > n, n > 20, or a key of the wrong size).
std::vector<unsigned char> BuildMultisigScript(
    int required, const std::vector<std::vector<unsigned char>>& pubkeys);
// Build the (unmined) genesis block: version 1, zero previous
// hash, `time`, `bits`, and a coinbase paying exactly
// FOUNDER_ALLOCATION to `founderScript` with the genesis message
// in its scriptSig. The nonce is 0; call MineGenesisParallel.
Block BuildGenesisBlock(const std::vector<unsigned char>& founderScript,
                        uint32_t time, uint32_t bits);
// Mine the block's nonce in parallel: thread t tries nonces
// t, t+threads, t+2*threads, ... The first valid nonce wins and
// stops the others. `hashesDone` (optional) counts hashes for
// progress reporting; `stop` (optional) aborts early. Returns
// false + `error` when the nonce space is exhausted.
bool MineGenesisParallel(Block& block, int threads,
                         std::atomic<uint64_t>* hashesDone,
                         std::atomic<bool>* stop, std::string& error);
// Write the genesis file (JSON): the serialized block, its hash,
// Merkle root, nonce, time, bits, founder script, and message.
bool WriteGenesisFile(const std::string& path, const Block& block,
                      const std::vector<unsigned char>& founderScript,
                      std::string& error);
// Read and fully verify a genesis file: the block deserializes,
// its hash matches, its proof of work is valid, and its coinbase
// pays exactly FOUNDER_ALLOCATION to `founderScript` (the
// vesting rule). False + `error` on any mismatch.
bool ReadGenesisFile(const std::string& path, Block& block,
                     std::vector<unsigned char>& founderScript,
                     std::string& error);

}  // namespace oeeoo
