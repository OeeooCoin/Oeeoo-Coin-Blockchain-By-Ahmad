// SPDX-License-Identifier: MIT
// Oeeoo Coin — Script interpreter, Bitcoin-style subset (Milestone 4).
//
// Supported opcodes: pushdata, OP_0..OP_16, OP_1NEGATE, OP_NOP,
// OP_VERIFY, OP_DROP, OP_DUP, OP_HASH160, OP_EQUAL, OP_EQUALVERIFY,
// OP_CHECKSIG, OP_CHECKSIGVERIFY, OP_CHECKMULTISIG,
// OP_CHECKMULTISIGVERIFY, OP_CHECKLOCKTIMEVERIFY (CLTV),
// OP_CHECKSEQUENCEVERIFY (CSV).
// NOT supported (rejected): OP_RETURN (arbitrary-data outputs are
// disabled, spec section 8.3), OP_IF/conditionals, arithmetic,
// OP_CODESEPARATOR, and any other opcode.
// Only SIGHASH_ALL signatures are accepted. scriptSig must be
// push-only. Resource limits: script <= 10,000 bytes, stack <= 1,000
// elements, element <= 520 bytes, at most 201 counted opcodes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "crypto/hash160.h"
#include "transactions/transaction.h"

namespace oeeoo {

// Opcodes (Bitcoin values).
constexpr unsigned char OP_0 = 0x00;
constexpr unsigned char OP_FALSE = OP_0;
constexpr unsigned char OP_PUSHDATA1 = 0x4c;
constexpr unsigned char OP_PUSHDATA2 = 0x4d;
constexpr unsigned char OP_PUSHDATA4 = 0x4e;
constexpr unsigned char OP_1NEGATE = 0x4f;
constexpr unsigned char OP_1 = 0x51;
constexpr unsigned char OP_TRUE = OP_1;
constexpr unsigned char OP_16 = 0x60;
constexpr unsigned char OP_NOP = 0x61;
constexpr unsigned char OP_VERIFY = 0x69;
constexpr unsigned char OP_RETURN = 0x6a;
constexpr unsigned char OP_DROP = 0x75;
constexpr unsigned char OP_DUP = 0x76;
constexpr unsigned char OP_EQUAL = 0x87;
constexpr unsigned char OP_EQUALVERIFY = 0x88;
constexpr unsigned char OP_HASH160 = 0xa9;
constexpr unsigned char OP_CHECKSIG = 0xac;
constexpr unsigned char OP_CHECKSIGVERIFY = 0xad;
constexpr unsigned char OP_CHECKMULTISIG = 0xae;
constexpr unsigned char OP_CHECKMULTISIGVERIFY = 0xaf;
constexpr unsigned char OP_CHECKLOCKTIMEVERIFY = 0xb1;
constexpr unsigned char OP_CHECKSEQUENCEVERIFY = 0xb2;

constexpr std::size_t kMaxScriptSize = 10000;
constexpr std::size_t kMaxStackSize = 1000;
constexpr std::size_t kMaxScriptElementSize = 520;
constexpr int kMaxOpsPerScript = 201;
constexpr int kMaxPubKeysPerMultisig = 20;

// Execute scriptSig followed by scriptPubKey for input `nIn` of `tx`.
// On failure returns false and sets `error`. On success the top stack
// item must be true. `scriptPubKey` is also the sighash subscript for
// CHECKSIG / CHECKMULTISIG.
bool VerifyScript(const std::vector<unsigned char>& scriptSig,
                  const std::vector<unsigned char>& scriptPubKey,
                  const Transaction& tx, std::size_t nIn,
                  std::string& error);

// Minimal little-endian signed-magnitude encoding of an integer
// (used for BIP34 coinbase heights and locktime values).
std::vector<unsigned char> EncodeScriptNum(int64_t value);
// Decode a script number. False on oversize / non-minimal encoding.
bool DecodeScriptNum(const std::vector<unsigned char>& v, int64_t& out,
                     std::size_t maxSize = 5);

// Read the block height from a coinbase scriptSig (BIP34: the first
// push must be the height). False if the first push is missing or
// does not decode as a height.
bool GetCoinbaseHeight(const std::vector<unsigned char>& scriptSig,
                       int64_t& height);

// Standard script builders.
std::vector<unsigned char> PushData(
    const std::vector<unsigned char>& data);
std::vector<unsigned char> CreateP2PKHScriptPubKey(const Hash160& hash);
std::vector<unsigned char> CreateP2PKHScriptSig(
    const std::vector<unsigned char>& sigWithHashType,
    const std::vector<unsigned char>& pubkey);
std::vector<unsigned char> CreateMultisigScriptPubKey(
    int m, const std::vector<std::vector<unsigned char>>& pubkeys);
std::vector<unsigned char> CreateMultisigScriptSig(
    const std::vector<std::vector<unsigned char>>& sigsWithHashType);

}  // namespace oeeoo
