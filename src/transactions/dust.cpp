// SPDX-License-Identifier: MIT
// Oeeoo Coin — dynamic dust threshold implementation.
#include "transactions/dust.h"

#include "script/script.h"
#include "serialize/varint.h"

namespace oeeoo {

namespace {
// OP_DUP OP_HASH160 <20> OP_EQUALVERIFY OP_CHECKSIG.
bool IsP2PKH(const std::vector<unsigned char>& script) {
  return script.size() == 25 && script[0] == 0x76 && script[1] == 0xa9 &&
         script[2] == 0x14 && script[23] == 0x88 && script[24] == 0xac;
}

// OP_m <pk1> ... <pkn> OP_n OP_CHECKMULTISIG. On success sets
// `required` (m) and returns true.
bool ParseMultisig(const std::vector<unsigned char>& script,
                   int& required) {
  if (script.size() < 4 || script[0] < 0x51 || script[0] > 0x60) {
    return false;
  }
  required = script[0] - 0x50;
  std::size_t pos = 1;
  int keys = 0;
  while (pos < script.size()) {
    const unsigned char op = script[pos];
    if (op >= 0x51 && op <= 0x60) {
      // OP_n: must be followed by OP_CHECKMULTISIG as the last byte.
      if (pos + 1 != script.size() - 1 || script.back() != 0xae) {
        return false;
      }
      const int total = op - 0x50;
      return required >= 1 && required <= total && keys == total;
    }
    if (op != 0x21 && op != 0x41) return false;  // 33- or 65-byte key
    const std::size_t keyLen = op;
    if (pos + 1 + keyLen > script.size()) return false;
    pos += 1 + keyLen;
    ++keys;
  }
  return false;
}

// Typical serialized size of an input spending this script type:
// outpoint (36) + scriptSig length varint (1) + scriptSig +
// sequence (4). The scriptSig size is estimated from the script:
// P2PKH ~107 bytes (push 72-byte sig + push 33-byte pubkey);
// bare multisig m-of-n: OP_0 (1) + m signatures (~73 bytes each
// with their length bytes). Unknown scripts use the P2PKH size.
std::size_t EstimateInputSize(
    const std::vector<unsigned char>& scriptPubKey) {
  constexpr std::size_t kOutpointAndSequence = 36 + 1 + 4;
  if (IsP2PKH(scriptPubKey)) {
    return kOutpointAndSequence + 107;
  }
  int required = 0;
  if (ParseMultisig(scriptPubKey, required)) {
    const std::size_t sigSize = 1 + static_cast<std::size_t>(required) * 74;
    return kOutpointAndSequence + sigSize;
  }
  return kOutpointAndSequence + 107;  // conservative default
}
}  // namespace

Amount GetDustThreshold(const std::vector<unsigned char>& scriptPubKey,
                        Amount minRelayFeeSatPerVb) {
  // Serialized output size: value (8) + script-length varint +
  // script.
  const std::size_t outputSize =
      8 + VarIntSize(scriptPubKey.size()) + scriptPubKey.size();
  const std::size_t total = outputSize + EstimateInputSize(scriptPubKey);
  const __int128 threshold =
      static_cast<__int128>(total) * minRelayFeeSatPerVb;
  return threshold > INT64_MAX ? INT64_MAX
                               : static_cast<Amount>(threshold);
}

bool IsDust(Amount value, const std::vector<unsigned char>& scriptPubKey,
            Amount minRelayFeeSatPerVb) {
  return value < GetDustThreshold(scriptPubKey, minRelayFeeSatPerVb);
}

}  // namespace oeeoo
