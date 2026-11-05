// SPDX-License-Identifier: MIT
// Oeeoo Coin — wallet implementation.
#include "wallet/wallet.h"

#include <algorithm>

#include "consensus/consensus.h"
#include "crypto/address.h"
#include "crypto/ecdsa.h"
#include "script/script.h"
#include "transactions/dust.h"
#include "transactions/sighash.h"
#include "transactions/validation.h"
#include "wallet/bip39.h"

namespace oeeoo {

namespace {
std::string PathFor(uint32_t account, bool isChange, uint32_t index) {
  return "m/44'/65535'/" + std::to_string(account) + "'/" +
         (isChange ? "1/" : "0/") + std::to_string(index);
}

std::string HexStr(const std::vector<unsigned char>& bytes) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char b : bytes) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0f]);
  }
  return out;
}
}  // namespace

bool Wallet::CreateNew(std::string& error) {
  const std::string mnemonic = GenerateMnemonic(256);
  if (mnemonic.empty()) {
    error = "failed to generate mnemonic";
    return false;
  }
  return RestoreFromMnemonic(mnemonic, "", error);
}

bool Wallet::RestoreFromMnemonic(const std::string& mnemonic,
                                 const std::string& passphrase,
                                 std::string& error) {
  if (!ValidateMnemonic(mnemonic)) {
    error = "invalid mnemonic (unknown word or bad checksum)";
    return false;
  }
  mnemonic_ = mnemonic;
  watchOnly_ = false;
  return RestoreFromSeed(MnemonicToSeed(mnemonic, passphrase), error);
}

bool Wallet::RestoreFromSeed(const std::vector<unsigned char>& seed,
                             std::string& error) {
  if (!MasterKeyFromSeed(seed, master_)) {
    error = "invalid seed";
    return false;
  }
  watchOnly_ = false;
  addresses_.clear();
  scriptToPath_.clear();
  unspent_.clear();
  balance_ = 0;
  nextExternal_ = 0;
  nextChange_ = 0;
  return SetAccount(0, error);
}

bool Wallet::RestoreFromXpub(const std::string& xpub, std::string& error) {
  ExtendedKey key;
  if (!ParseExtendedKey(xpub, key) || key.isPrivate) {
    error = "invalid account public key";
    return false;
  }
  master_ = ExtendedKey{};
  account_ = key;
  accountIndex_ = 0;  // unknown from the xpub alone
  mnemonic_.clear();
  watchOnly_ = true;
  addresses_.clear();
  scriptToPath_.clear();
  unspent_.clear();
  balance_ = 0;
  nextExternal_ = 0;
  nextChange_ = 0;
  EnsureLookahead();
  return true;
}

std::string Wallet::GetAccountXprv() const {
  if (!account_.isPrivate) return {};
  return SerializePrivate(account_);
}

std::string Wallet::GetAccountXpub() const {
  return SerializePublic(account_);
}

bool Wallet::SetAccount(uint32_t accountIndex, std::string& error) {
  ExtendedKey account;
  if (!DerivePath(master_,
                  "m/44'/65535'/" + std::to_string(accountIndex) + "'",
                  account)) {
    error = "failed to derive account key";
    return false;
  }
  account_ = account;
  accountIndex_ = accountIndex;
  addresses_.clear();
  scriptToPath_.clear();
  unspent_.clear();
  balance_ = 0;
  nextExternal_ = 0;
  nextChange_ = 0;
  EnsureLookahead();
  return true;
}

bool Wallet::DeriveAddress(uint32_t index, bool isChange, WalletAddress& out,
                           std::string& error) {
  ExtendedKey chain, key;
  if (!DeriveChild(account_, isChange ? 1 : 0, chain)) {
    error = "failed to derive chain key";
    return false;
  }
  if (!DeriveChild(chain, index, key)) {
    error = "failed to derive address key";
    return false;
  }
  out.pubkey.assign(key.pubkey.begin(), key.pubkey.end());
  out.scriptPubKey = CreateP2PKHScriptPubKey(PubKeyHash160(out.pubkey));
  out.address = PubKeyToAddress(out.pubkey);
  out.path = PathFor(accountIndex_, isChange, index);
  out.isChange = isChange;
  out.used = false;
  return true;
}

void Wallet::EnsureLookahead() {
  // Keep 20 fresh addresses of lookahead on each chain.
  for (uint32_t i = nextExternal_;
       i < nextExternal_ + kWalletGapLimit; ++i) {
    WalletAddress addr;
    std::string error;
    if (!DeriveAddress(i, false, addr, error)) break;
    const std::string key = addr.path;
    if (addresses_.try_emplace(key, addr).second) {
      scriptToPath_[HexStr(addr.scriptPubKey)] = key;
    }
  }
  for (uint32_t i = nextChange_; i < nextChange_ + kWalletGapLimit; ++i) {
    WalletAddress addr;
    std::string error;
    if (!DeriveAddress(i, true, addr, error)) break;
    const std::string key = addr.path;
    if (addresses_.try_emplace(key, addr).second) {
      scriptToPath_[HexStr(addr.scriptPubKey)] = key;
    }
  }
}

WalletAddress Wallet::NewAddress(std::string& error) {
  WalletAddress out;
  const std::string path = PathFor(accountIndex_, false, nextExternal_);
  auto it = addresses_.find(path);
  if (it != addresses_.end()) {
    out = it->second;
  } else if (!DeriveAddress(nextExternal_, false, out, error)) {
    return out;
  }
  ++nextExternal_;
  EnsureLookahead();
  return out;
}

WalletAddress Wallet::NewChangeAddress(std::string& error) {
  WalletAddress out;
  const std::string path = PathFor(accountIndex_, true, nextChange_);
  auto it = addresses_.find(path);
  if (it != addresses_.end()) {
    out = it->second;
  } else if (!DeriveAddress(nextChange_, true, out, error)) {
    return out;
  }
  ++nextChange_;
  EnsureLookahead();
  return out;
}

bool Wallet::GetAddressAt(uint32_t index, bool isChange, WalletAddress& out,
                          std::string& error) {
  const std::string path = PathFor(accountIndex_, isChange, index);
  auto it = addresses_.find(path);
  if (it != addresses_.end()) {
    out = it->second;
    return true;
  }
  if (!DeriveAddress(index, isChange, out, error)) return false;
  addresses_[path] = out;
  scriptToPath_[HexStr(out.scriptPubKey)] = path;
  return true;
}

std::vector<WalletAddress> Wallet::ListAddresses() const {
  std::vector<WalletAddress> out;
  for (const auto& kv : addresses_) out.push_back(kv.second);
  return out;
}

bool Wallet::IsMine(const std::string& address) const {
  for (const auto& kv : addresses_) {
    if (kv.second.address == address) return true;
  }
  return false;
}

bool Wallet::IsMineScript(
    const std::vector<unsigned char>& scriptPubKey) const {
  return scriptToPath_.find(HexStr(scriptPubKey)) != scriptToPath_.end();
}

void Wallet::ScanUtxo(const CoinsView& utxo) {
  // Reset usage, then mark every address that owns a UTXO.
  for (auto& kv : addresses_) kv.second.used = false;
  unspent_.clear();
  balance_ = 0;
  std::vector<std::pair<OutPoint, Coin>> coins;
  utxo.GetAllCoins(coins);
  uint32_t maxUsedExternal = 0;
  uint32_t maxUsedChange = 0;
  bool anyExternalUsed = false;
  bool anyChangeUsed = false;
  for (const auto& entry : coins) {
    const std::string scriptHex = HexStr(entry.second.out.scriptPubKey);
    auto it = scriptToPath_.find(scriptHex);
    if (it == scriptToPath_.end()) continue;
    const std::string& path = it->second;
    auto ait = addresses_.find(path);
    if (ait == addresses_.end()) continue;
    ait->second.used = true;
    // Parse the index from the path's last component.
    const std::size_t slash = path.rfind('/');
    const uint32_t index =
        static_cast<uint32_t>(std::stoul(path.substr(slash + 1)));
    WalletUtxo wu;
    wu.outpoint = entry.first;
    wu.value = entry.second.out.value;
    wu.address = ait->second.address;
    wu.path = path;
    wu.index = index;
    wu.isChange = ait->second.isChange;
    unspent_.push_back(wu);
    balance_ += entry.second.out.value;
    if (ait->second.isChange) {
      anyChangeUsed = true;
      maxUsedChange = std::max(maxUsedChange, index);
    } else {
      anyExternalUsed = true;
      maxUsedExternal = std::max(maxUsedExternal, index);
    }
  }
  // Advance past the last used address (+1) so the gap window moves.
  if (anyExternalUsed) nextExternal_ = std::max(nextExternal_, maxUsedExternal + 1);
  if (anyChangeUsed) nextChange_ = std::max(nextChange_, maxUsedChange + 1);
  EnsureLookahead();
}

std::vector<WalletUtxo> Wallet::ListUnspent() const { return unspent_; }

std::size_t Wallet::EstimateTxSize(std::size_t inputCount,
                                   std::size_t outputCount) {
  // 4 (version) + 1 (vin count) + 1 (vout count) + 4 (locktime) +
  // ~148 per input (P2PKH) + ~34 per output (P2PKH).
  return 10 + inputCount * 148 + outputCount * 34;
}

bool Wallet::CreateTransaction(const std::string& toAddress, Amount amount,
                               const CoinsView& utxo, int height,
                               uint32_t medianTimePast, Transaction& tx,
                               Amount& fee, std::string& error,
                               uint64_t feeRateSatVb) {
  if (watchOnly_) {
    error = "watch-only wallet cannot spend";
    return false;
  }
  if (feeRateSatVb == 0) feeRateSatVb = 1;
  if (feeRateSatVb > maxFeeRateSatVb_) {
    error = "fee rate exceeds the wallet maximum (" +
            std::to_string(maxFeeRateSatVb_) + " sat/vB)";
    return false;
  }
  if (amount <= 0) {
    error = "amount must be positive";
    return false;
  }
  Hash160 toHash;
  if (!DecodeP2PKHAddress(toAddress, toHash)) {
    error = "invalid destination address";
    return false;
  }
  // Dynamic, script-aware dust threshold (P2PKH at the 1 sat/vB
  // minimum relay fee = 182 sats). The same threshold applies to
  // the change output, which is also P2PKH.
  const Amount dustLimit =
      GetDustThreshold(CreateP2PKHScriptPubKey(toHash), MIN_RELAY_FEE_SAT_VB);
  if (amount < dustLimit) {
    error = "amount below the dust limit";
    return false;
  }
  ScanUtxo(utxo);
  if (balance_ < amount) {
    error = "insufficient funds";
    return false;
  }
  // Coin selection: smallest UTXOs first (fewest coins spent is NOT
  // the goal here — spending small coins first keeps large coins
  // intact for later; documented policy).
  std::vector<WalletUtxo> candidates = unspent_;
  std::sort(candidates.begin(), candidates.end(),
            [](const WalletUtxo& a, const WalletUtxo& b) {
              return a.value < b.value;
            });
  std::vector<WalletUtxo> selected;
  Amount selectedTotal = 0;
  fee = 0;
  for (const WalletUtxo& candidate : candidates) {
    selected.push_back(candidate);
    selectedTotal += candidate.value;
    // Fee for the current shape (assume 2 outputs: payment + change).
    const std::size_t size = EstimateTxSize(selected.size(), 2);
    fee = static_cast<Amount>(size * feeRateSatVb);
    if (selectedTotal >= amount + fee) break;
  }
  if (selectedTotal < amount + fee) {
    error = "insufficient funds (including fee)";
    return false;
  }
  Amount change = selectedTotal - amount - fee;
  std::size_t outputCount = 1;
  if (change >= dustLimit) {
    outputCount = 2;
  } else {
    // Dust change is added to the fee (no change output).
    fee += change;
    change = 0;
  }
  // Recompute the fee for the final output count.
  fee = static_cast<Amount>(EstimateTxSize(selected.size(), outputCount) *
                            feeRateSatVb);
  if (change > 0) {
    change = selectedTotal - amount - fee;
    if (change < 0 || (change > 0 && change < dustLimit)) {
      // Rounding pushed change below dust: fold it into the fee.
      fee += change;
      change = 0;
      outputCount = 1;
      fee = static_cast<Amount>(
          EstimateTxSize(selected.size(), outputCount) * feeRateSatVb);
    }
  }
  if (selectedTotal < amount + fee) {
    error = "insufficient funds (including fee)";
    return false;
  }

  Transaction built;
  built.version = 1;
  for (const WalletUtxo& input : selected) {
    TxIn in;
    in.prevTxHash = input.outpoint.txid;
    in.prevOutIndex = input.outpoint.index;
    in.sequence = 0xffffffff;
    built.vin.push_back(in);
  }
  TxOut payment;
  payment.value = amount;
  payment.scriptPubKey = CreateP2PKHScriptPubKey(toHash);
  built.vout.push_back(payment);
  WalletAddress changeAddr;
  if (change > 0) {
    std::string changeError;
    changeAddr = NewChangeAddress(changeError);
    if (changeAddr.address.empty()) {
      error = "failed to derive change address: " + changeError;
      return false;
    }
    TxOut changeOut;
    changeOut.value = change;
    changeOut.scriptPubKey = changeAddr.scriptPubKey;
    built.vout.push_back(changeOut);
  }
  built.lockTime = 0;

  // Sign every input with its own key (account -> change -> index).
  for (std::size_t i = 0; i < built.vin.size(); ++i) {
    const WalletUtxo& input = selected[i];
    ExtendedKey chain, key;
    if (!DeriveChild(account_, input.isChange ? 1 : 0, chain) ||
        !DeriveChild(chain, input.index, key)) {
      error = "failed to derive signing key";
      return false;
    }
    std::vector<unsigned char> privkey(key.privkey.begin(), key.privkey.end());
    // The scriptPubKey of the spent output.
    Coin coin;
    if (!utxo.GetCoin(input.outpoint, coin)) {
      error = "spent coin vanished from the UTXO set";
      return false;
    }
    Hash256 sighash;
    if (!LegacySighash(built, i, coin.out.scriptPubKey, kSigHashAll,
                       sighash)) {
      error = "sighash failed";
      return false;
    }
    std::vector<unsigned char> sig = SignHash(sighash, privkey);
    if (sig.empty()) {
      error = "signing failed";
      return false;
    }
    sig.push_back(static_cast<unsigned char>(kSigHashAll));
    std::vector<unsigned char> pubkey(key.pubkey.begin(), key.pubkey.end());
    built.vin[i].scriptSig = CreateP2PKHScriptSig(sig, pubkey);
  }
  // The built transaction must validate against the UTXO set.
  TxValidationResult r =
      ValidateTransaction(built, utxo, height, medianTimePast);
  if (!r.ok) {
    error = "built transaction does not validate: " + r.error;
    return false;
  }
  if (r.fee != fee) {
    // Keep the caller honest: report the actual fee.
    fee = r.fee;
  }
  tx = built;
  return true;
}

}  // namespace oeeoo
