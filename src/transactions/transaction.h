// Oeeoo Coin transaction format (Milestone 2 — serialization only).
// UTXO model, Bitcoin-style layout, deterministic serialization:
//   version      int32 LE
//   input count  VarInt
//   inputs       prevTxHash (32 B) | prevOutIndex (uint32 LE)
//                | scriptSig length (VarInt) + scriptSig
//                | sequence (uint32 LE)
//   output count VarInt
//   outputs      value (int64 LE, smallest units)
//                | scriptPubKey length (VarInt) + scriptPubKey
//   lockTime     uint32 LE
// Transaction id = SHA256d(serialization).
// Script execution and signature validation are Milestone 4; this
// milestone only guarantees that every node serializes, deserializes
// and identifies the same transaction identically.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "consensus/amount.h"
#include "crypto/sha256.h"

namespace oeeoo {

struct TxIn {
  Hash256 prevTxHash{};
  uint32_t prevOutIndex = 0;
  std::vector<unsigned char> scriptSig;
  uint32_t sequence = 0xffffffff;
};

struct TxOut {
  Amount value = 0;
  std::vector<unsigned char> scriptPubKey;
};

struct Transaction {
  int32_t version = 1;
  std::vector<TxIn> vin;
  std::vector<TxOut> vout;
  uint32_t lockTime = 0;

  // Deterministic serialization (see header comment).
  std::vector<unsigned char> Serialize() const;

  // Deserialize exactly the bytes of one transaction. Returns false
  // on truncation, trailing bytes, oversize input (> MAX_BLOCK_SIZE)
  // or a malformed VarInt. Does NOT check scripts or signatures.
  static bool Deserialize(const std::vector<unsigned char>& bytes,
                          Transaction& out);

  // Deserialize one transaction starting at `offset` inside a larger
  // buffer (e.g. a block). On success sets `bytesConsumed` to the
  // transaction's length. Used by Block::Deserialize.
  static bool Deserialize(const std::vector<unsigned char>& bytes,
                          std::size_t offset, Transaction& out,
                          std::size_t& bytesConsumed);

  // SHA256d(Serialize()).
  Hash256 GetTxId() const;
  std::string GetTxIdHex() const;

  // Structural checks only (Milestone 2 scope):
  // non-empty vin/vout, every output in MoneyRange, output total
  // within MAX_MONEY, no duplicate inputs, size within MAX_BLOCK_SIZE.
  bool CheckBasic() const;
};

// A coinbase transaction: exactly one input spending the null
// outpoint (zero prev hash, index 0xffffffff). This is the rule
// used everywhere a coinbase must be recognized (block connection,
// fee computation) — an empty vin is NOT a coinbase.
bool IsCoinbase(const Transaction& tx);

}  // namespace oeeoo
