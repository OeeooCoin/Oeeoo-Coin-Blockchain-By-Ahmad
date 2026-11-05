// SPDX-License-Identifier: MIT
// Oeeoo Coin — Script interpreter implementation.
#include "script/script.h"

#include "crypto/ecdsa.h"
#include "transactions/sighash.h"

namespace oeeoo {

namespace {

// One parsed instruction: either a data push or an opcode.
struct Op {
  unsigned char opcode = 0;
  std::vector<unsigned char> data;  // only for pushes
  bool isPush = false;
};

// Parse a script into instructions. False on truncation / bad pushdata.
bool ParseScript(const std::vector<unsigned char>& script,
                 std::vector<Op>& ops, std::string& error) {
  std::size_t i = 0;
  while (i < script.size()) {
    unsigned char opcode = script[i++];
    Op op;
    op.opcode = opcode;
    if (opcode <= 0x4b) {
      std::size_t n = opcode;
      if (i + n > script.size()) {
        error = "truncated pushdata";
        return false;
      }
      op.isPush = true;
      op.data.assign(script.begin() + i, script.begin() + i + n);
      i += n;
    } else if (opcode == OP_PUSHDATA1) {
      if (i + 1 > script.size()) {
        error = "truncated PUSHDATA1";
        return false;
      }
      std::size_t n = script[i++];
      if (i + n > script.size()) {
        error = "truncated pushdata";
        return false;
      }
      op.isPush = true;
      op.data.assign(script.begin() + i, script.begin() + i + n);
      i += n;
    } else if (opcode == OP_PUSHDATA2) {
      if (i + 2 > script.size()) {
        error = "truncated PUSHDATA2";
        return false;
      }
      std::size_t n = script[i] | (static_cast<std::size_t>(script[i + 1]) << 8);
      i += 2;
      if (i + n > script.size()) {
        error = "truncated pushdata";
        return false;
      }
      op.isPush = true;
      op.data.assign(script.begin() + i, script.begin() + i + n);
      i += n;
    } else if (opcode == OP_PUSHDATA4) {
      if (i + 4 > script.size()) {
        error = "truncated PUSHDATA4";
        return false;
      }
      std::size_t n = static_cast<std::size_t>(script[i]) |
                      (static_cast<std::size_t>(script[i + 1]) << 8) |
                      (static_cast<std::size_t>(script[i + 2]) << 16) |
                      (static_cast<std::size_t>(script[i + 3]) << 24);
      i += 4;
      if (i + n > script.size()) {
        error = "truncated pushdata";
        return false;
      }
      op.isPush = true;
      op.data.assign(script.begin() + i, script.begin() + i + n);
      i += n;
    }
    ops.push_back(std::move(op));
  }
  return true;
}

// Bitcoin CastToBool: false iff every byte is zero (negative zero,
// 0x80 alone, also counts as false).
bool CastToBool(const std::vector<unsigned char>& v) {
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (v[i] != 0) {
      if (i == v.size() - 1 && v[i] == 0x80) return false;
      return true;
    }
  }
  return false;
}

// Minimal-encoded little-endian signed-magnitude script number.
bool ParseScriptNum(const std::vector<unsigned char>& v, int64_t& out,
                    std::size_t maxSize, std::string& error) {
  if (!DecodeScriptNum(v, out, maxSize)) {
    error = "bad script number";
    return false;
  }
  return true;
}

using Stack = std::vector<std::vector<unsigned char>>;

bool NeedStack(Stack& stack, std::size_t n, std::string& error) {
  if (stack.size() < n) {
    error = "stack underflow";
    return false;
  }
  return true;
}

void PushStack(Stack& stack, std::vector<unsigned char> v,
               std::string& error, bool& ok) {
  if (stack.size() + 1 > kMaxStackSize || v.size() > kMaxScriptElementSize) {
    error = "stack or element size limit exceeded";
    ok = false;
    return;
  }
  stack.push_back(std::move(v));
  ok = true;
}

}  // namespace

bool VerifyScript(const std::vector<unsigned char>& scriptSig,
                  const std::vector<unsigned char>& scriptPubKey,
                  const Transaction& tx, std::size_t nIn,
                  std::string& error) {
  if (nIn >= tx.vin.size()) {
    error = "input index out of range";
    return false;
  }
  if (scriptSig.size() > kMaxScriptSize ||
      scriptPubKey.size() > kMaxScriptSize) {
    error = "script too large";
    return false;
  }
  std::vector<Op> sigOps;
  if (!ParseScript(scriptSig, sigOps, error)) return false;
  for (const Op& op : sigOps) {
    if (!op.isPush) {
      error = "scriptSig must be push-only";
      return false;
    }
  }
  std::vector<Op> pubOps;
  if (!ParseScript(scriptPubKey, pubOps, error)) return false;

  // Count opcodes > OP_16 (Bitcoin rule) over both scripts.
  int opCount = 0;
  for (const Op& op : sigOps) {
    if (!op.isPush && op.opcode > OP_16) ++opCount;
  }
  for (const Op& op : pubOps) {
    if (!op.isPush && op.opcode > OP_16) ++opCount;
  }
  if (opCount > kMaxOpsPerScript) {
    error = "too many opcodes";
    return false;
  }

  Stack stack;
  auto execute = [&](const std::vector<Op>& ops) -> bool {
    for (const Op& op : ops) {
      if (op.isPush) {
        bool ok = false;
        PushStack(stack, op.data, error, ok);
        if (!ok) return false;
        continue;
      }
      switch (op.opcode) {
        case OP_0: {
          bool ok = false;
          PushStack(stack, {}, error, ok);
          if (!ok) return false;
          break;
        }
        case OP_1NEGATE: {
          bool ok = false;
          PushStack(stack, {0x81}, error, ok);
          if (!ok) return false;
          break;
        }
        case OP_1: case OP_1 + 1: case OP_1 + 2: case OP_1 + 3:
        case OP_1 + 4: case OP_1 + 5: case OP_1 + 6: case OP_1 + 7:
        case OP_1 + 8: case OP_1 + 9: case OP_1 + 10: case OP_1 + 11:
        case OP_1 + 12: case OP_1 + 13: case OP_1 + 14: case OP_16: {
          bool ok = false;
          PushStack(stack, {static_cast<unsigned char>(op.opcode - OP_1 + 1)},
                    error, ok);
          if (!ok) return false;
          break;
        }
        case OP_NOP:
          break;
        case OP_VERIFY: {
          if (!NeedStack(stack, 1, error)) return false;
          if (!CastToBool(stack.back())) {
            error = "OP_VERIFY failed";
            return false;
          }
          stack.pop_back();
          break;
        }
        case OP_RETURN:
          error = "OP_RETURN is disabled (no arbitrary-data outputs)";
          return false;
        case OP_DROP: {
          if (!NeedStack(stack, 1, error)) return false;
          stack.pop_back();
          break;
        }
        case OP_DUP: {
          if (!NeedStack(stack, 1, error)) return false;
          bool ok = false;
          PushStack(stack, stack.back(), error, ok);
          if (!ok) return false;
          break;
        }
        case OP_HASH160: {
          if (!NeedStack(stack, 1, error)) return false;
          Hash160 h = ComputeHash160(stack.back());
          stack.pop_back();
          bool ok = false;
          PushStack(stack, std::vector<unsigned char>(h.begin(), h.end()),
                    error, ok);
          if (!ok) return false;
          break;
        }
        case OP_EQUAL:
        case OP_EQUALVERIFY: {
          if (!NeedStack(stack, 2, error)) return false;
          bool equal = stack[stack.size() - 1] == stack[stack.size() - 2];
          stack.pop_back();
          stack.pop_back();
          if (op.opcode == OP_EQUALVERIFY) {
            if (!equal) {
              error = "OP_EQUALVERIFY failed";
              return false;
            }
          } else {
            bool ok = false;
            PushStack(stack, {static_cast<unsigned char>(equal ? 1 : 0)},
                      error, ok);
            if (!ok) return false;
          }
          break;
        }
        case OP_CHECKSIG:
        case OP_CHECKSIGVERIFY: {
          if (!NeedStack(stack, 2, error)) return false;
          std::vector<unsigned char> pubkey = stack.back();
          stack.pop_back();
          std::vector<unsigned char> sig = stack.back();
          stack.pop_back();
          bool valid = false;
          if (!sig.empty()) {
            uint32_t hashType = sig.back();
            if (hashType != kSigHashAll) {
              error = "unsupported sighash type (only SIGHASH_ALL)";
              return false;
            }
            std::vector<unsigned char> derSig(sig.begin(), sig.end() - 1);
            Hash256 sighash;
            if (!LegacySighash(tx, nIn, scriptPubKey, hashType, sighash)) {
              error = "sighash failed";
              return false;
            }
            valid = VerifySignature(pubkey, derSig, sighash);
          }
          if (op.opcode == OP_CHECKSIGVERIFY) {
            if (!valid) {
              error = "OP_CHECKSIGVERIFY failed";
              return false;
            }
          } else {
            bool ok = false;
            PushStack(stack, {static_cast<unsigned char>(valid ? 1 : 0)},
                      error, ok);
            if (!ok) return false;
          }
          break;
        }
        case OP_CHECKMULTISIG:
        case OP_CHECKMULTISIGVERIFY: {
          // Stack (top first): n, pubkey_n..pubkey_1, m, sig_m..sig_1,
          // dummy. Pop in that order.
          if (!NeedStack(stack, 1, error)) return false;
          int64_t nKeys = 0;
          if (!ParseScriptNum(stack.back(), nKeys, 4, error)) return false;
          stack.pop_back();
          if (nKeys < 0 || nKeys > kMaxPubKeysPerMultisig) {
            error = "bad pubkey count in CHECKMULTISIG";
            return false;
          }
          if (!NeedStack(stack, static_cast<std::size_t>(nKeys), error)) {
            return false;
          }
          std::vector<std::vector<unsigned char>> pubkeys;
          for (int64_t i = 0; i < nKeys; ++i) {
            pubkeys.push_back(stack.back());
            stack.pop_back();
          }
          if (!NeedStack(stack, 1, error)) return false;
          int64_t nSigs = 0;
          if (!ParseScriptNum(stack.back(), nSigs, 4, error)) return false;
          stack.pop_back();
          if (nSigs < 0 || nSigs > nKeys) {
            error = "bad signature count in CHECKMULTISIG";
            return false;
          }
          if (!NeedStack(stack, static_cast<std::size_t>(nSigs), error)) {
            return false;
          }
          std::vector<std::vector<unsigned char>> sigs;
          for (int64_t i = 0; i < nSigs; ++i) {
            sigs.push_back(stack.back());
            stack.pop_back();
          }
          // Dummy element (Bitcoin quirk): must be present and empty.
          if (!NeedStack(stack, 1, error)) return false;
          if (!stack.back().empty()) {
            error = "CHECKMULTISIG dummy element must be empty";
            return false;
          }
          stack.pop_back();
          // Match signatures to pubkeys in order.
          bool valid = true;
          std::size_t keyIdx = 0;
          for (const auto& sigWithType : sigs) {
            if (sigWithType.empty()) {
              valid = false;
              break;
            }
            uint32_t hashType = sigWithType.back();
            if (hashType != kSigHashAll) {
              error = "unsupported sighash type (only SIGHASH_ALL)";
              return false;
            }
            std::vector<unsigned char> derSig(sigWithType.begin(),
                                              sigWithType.end() - 1);
            Hash256 sighash;
            if (!LegacySighash(tx, nIn, scriptPubKey, hashType, sighash)) {
              error = "sighash failed";
              return false;
            }
            bool matched = false;
            while (keyIdx < pubkeys.size()) {
              if (VerifySignature(pubkeys[keyIdx], derSig, sighash)) {
                matched = true;
                ++keyIdx;
                break;
              }
              ++keyIdx;
            }
            if (!matched) {
              valid = false;
              break;
            }
          }
          if (op.opcode == OP_CHECKMULTISIGVERIFY) {
            if (!valid) {
              error = "OP_CHECKMULTISIGVERIFY failed";
              return false;
            }
          } else {
            bool ok = false;
            PushStack(stack, {static_cast<unsigned char>(valid ? 1 : 0)},
                      error, ok);
            if (!ok) return false;
          }
          break;
        }
        case OP_CHECKLOCKTIMEVERIFY: {
          if (!NeedStack(stack, 1, error)) return false;
          int64_t lockValue = 0;
          if (!ParseScriptNum(stack.back(), lockValue, 5, error)) {
            return false;
          }
          if (lockValue < 0) {
            error = "CLTV: negative locktime";
            return false;
          }
          const uint32_t txLock = tx.lockTime;
          const bool valueIsHeight = lockValue < 500000000;
          const bool txIsHeight = txLock < 500000000;
          if (valueIsHeight != txIsHeight) {
            error = "CLTV: locktime type mismatch (height vs time)";
            return false;
          }
          if (txLock < static_cast<uint32_t>(lockValue)) {
            error = "CLTV: locktime not reached";
            return false;
          }
          if (tx.vin[nIn].sequence == 0xffffffff) {
            error = "CLTV: input sequence is final";
            return false;
          }
          // CLTV does not pop the value.
          break;
        }
        case OP_CHECKSEQUENCEVERIFY: {
          if (!NeedStack(stack, 1, error)) return false;
          int64_t seqValue = 0;
          if (!ParseScriptNum(stack.back(), seqValue, 5, error)) {
            return false;
          }
          const uint32_t sequence = tx.vin[nIn].sequence;
          if (tx.version < 2) {
            error = "CSV: transaction version must be >= 2";
            return false;
          }
          // Disabled flag set on the input sequence: CSV is a no-op.
          if (sequence & 0x80000000u) break;
          if (seqValue < 0) {
            error = "CSV: negative sequence value";
            return false;
          }
          const uint32_t kTypeFlag = 0x00400000;
          if ((static_cast<uint32_t>(seqValue) & kTypeFlag) !=
              (sequence & kTypeFlag)) {
            error = "CSV: sequence type mismatch (blocks vs time)";
            return false;
          }
          const uint32_t kMask = 0x0000ffff;
          if ((static_cast<uint32_t>(seqValue) & kMask) >
              (sequence & kMask)) {
            error = "CSV: relative locktime not reached";
            return false;
          }
          // CSV does not pop the value.
          break;
        }
        default:
          error = "unsupported opcode";
          return false;
      }
    }
    return true;
  };

  if (!execute(sigOps)) return false;
  if (!execute(pubOps)) return false;
  if (stack.empty() || !CastToBool(stack.back())) {
    error = "script evaluated to false";
    return false;
  }
  return true;
}

std::vector<unsigned char> EncodeScriptNum(int64_t value) {
  if (value == 0) return {};
  std::vector<unsigned char> out;
  const bool negative = value < 0;
  uint64_t mag = negative ? static_cast<uint64_t>(-value)
                          : static_cast<uint64_t>(value);
  while (mag > 0) {
    out.push_back(static_cast<unsigned char>(mag & 0xff));
    mag >>= 8;
  }
  if (out.back() & 0x80) {
    out.push_back(negative ? 0x80 : 0x00);
  } else if (negative) {
    out.back() |= 0x80;
  }
  return out;
}

bool DecodeScriptNum(const std::vector<unsigned char>& v, int64_t& out,
                     std::size_t maxSize) {
  if (v.size() > maxSize) return false;
  if (v.empty()) {
    out = 0;
    return true;
  }
  // Minimal encoding (Bitcoin rule): if the top byte's magnitude bits
  // are all zero (top byte 0x00 or 0x80), the encoding is non-minimal
  // unless the byte below carries the sign bit.
  if ((v.back() & 0x7f) == 0) {
    if (v.size() <= 1 || (v[v.size() - 2] & 0x80) == 0) return false;
  }
  // Little-endian signed magnitude; sign is the 0x80 bit of the last
  // byte.
  int64_t value = 0;
  for (std::size_t i = 0; i < v.size(); ++i) {
    value |= static_cast<int64_t>(v[i]) << (8 * i);
  }
  if (v.back() & 0x80) {
    value &= ~(static_cast<int64_t>(0x80) << (8 * (v.size() - 1)));
    value = -value;
  }
  out = value;
  return true;
}

bool GetCoinbaseHeight(const std::vector<unsigned char>& scriptSig,
                       int64_t& height) {
  // Read ONLY the first push: the rest of a coinbase scriptSig is
  // arbitrary miner data (e.g. the extraNonce's raw bytes) and must
  // not be parsed as script.
  if (scriptSig.empty()) return false;
  const unsigned char op = scriptSig[0];
  std::size_t n = 0;
  std::size_t dataOff = 0;
  if (op <= 0x4b) {
    n = op;
    dataOff = 1;
  } else if (op == OP_PUSHDATA1) {
    if (scriptSig.size() < 2) return false;
    n = scriptSig[1];
    dataOff = 2;
  } else if (op == OP_PUSHDATA2) {
    if (scriptSig.size() < 3) return false;
    n = scriptSig[1] | (static_cast<std::size_t>(scriptSig[2]) << 8);
    dataOff = 3;
  } else if (op == OP_PUSHDATA4) {
    if (scriptSig.size() < 5) return false;
    n = static_cast<std::size_t>(scriptSig[1]) |
        (static_cast<std::size_t>(scriptSig[2]) << 8) |
        (static_cast<std::size_t>(scriptSig[3]) << 16) |
        (static_cast<std::size_t>(scriptSig[4]) << 24);
    dataOff = 5;
  } else {
    return false;
  }
  if (dataOff + n > scriptSig.size()) return false;
  std::vector<unsigned char> data(scriptSig.begin() + dataOff,
                                  scriptSig.begin() + dataOff + n);
  return DecodeScriptNum(data, height, 5);
}

std::vector<unsigned char> PushData(const std::vector<unsigned char>& data) {
  std::vector<unsigned char> out;
  if (data.size() <= 0x4b) {
    out.push_back(static_cast<unsigned char>(data.size()));
  } else if (data.size() <= 0xff) {
    out.push_back(OP_PUSHDATA1);
    out.push_back(static_cast<unsigned char>(data.size()));
  } else if (data.size() <= 0xffff) {
    out.push_back(OP_PUSHDATA2);
    out.push_back(static_cast<unsigned char>(data.size() & 0xff));
    out.push_back(static_cast<unsigned char>((data.size() >> 8) & 0xff));
  } else {
    out.push_back(OP_PUSHDATA4);
    out.push_back(static_cast<unsigned char>(data.size() & 0xff));
    out.push_back(static_cast<unsigned char>((data.size() >> 8) & 0xff));
    out.push_back(static_cast<unsigned char>((data.size() >> 16) & 0xff));
    out.push_back(static_cast<unsigned char>((data.size() >> 24) & 0xff));
  }
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

std::vector<unsigned char> CreateP2PKHScriptPubKey(const Hash160& hash) {
  std::vector<unsigned char> script;
  script.push_back(OP_DUP);
  script.push_back(OP_HASH160);
  std::vector<unsigned char> h(hash.begin(), hash.end());
  std::vector<unsigned char> push = PushData(h);
  script.insert(script.end(), push.begin(), push.end());
  script.push_back(OP_EQUALVERIFY);
  script.push_back(OP_CHECKSIG);
  return script;
}

std::vector<unsigned char> CreateP2PKHScriptSig(
    const std::vector<unsigned char>& sigWithHashType,
    const std::vector<unsigned char>& pubkey) {
  std::vector<unsigned char> script = PushData(sigWithHashType);
  std::vector<unsigned char> pk = PushData(pubkey);
  script.insert(script.end(), pk.begin(), pk.end());
  return script;
}

std::vector<unsigned char> CreateMultisigScriptPubKey(
    int m, const std::vector<std::vector<unsigned char>>& pubkeys) {
  std::vector<unsigned char> script;
  script.push_back(static_cast<unsigned char>(OP_1 + m - 1));
  for (const auto& pk : pubkeys) {
    std::vector<unsigned char> push = PushData(pk);
    script.insert(script.end(), push.begin(), push.end());
  }
  script.push_back(
      static_cast<unsigned char>(OP_1 + static_cast<int>(pubkeys.size()) - 1));
  script.push_back(OP_CHECKMULTISIG);
  return script;
}

std::vector<unsigned char> CreateMultisigScriptSig(
    const std::vector<std::vector<unsigned char>>& sigsWithHashType) {
  std::vector<unsigned char> script;
  script.push_back(OP_0);  // dummy element required by CHECKMULTISIG
  for (const auto& sig : sigsWithHashType) {
    std::vector<unsigned char> push = PushData(sig);
    script.insert(script.end(), push.begin(), push.end());
  }
  return script;
}

}  // namespace oeeoo
