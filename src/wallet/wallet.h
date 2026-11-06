// SPDX-License-Identifier: MIT
// Oeeoo Coin — wallet (Milestone 8).
// BIP44 layout for Oeeoo: m/44'/65535'/account'/change/index
// (coin type 65535 is provisional and unregistered). Legacy P2PKH
// only (no SegWit/Taproot at launch). Gap limit 20: the wallet
// keeps 20 unused addresses of lookahead on each chain; an address
// counts as used once a UTXO pays to it. Address reuse is allowed
// by the spec, so the wallet never forbids it — it only prefers
// fresh addresses.
// The wallet holds private keys in memory. It NEVER logs them.
// Persistence (encrypted at rest) is WalletFile (walletfile.h).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "consensus/consensus.h"
#include "crypto/sha256.h"
#include "transactions/transaction.h"
#include "utxo/coins.h"
#include "wallet/bip32.h"

namespace oeeoo {

constexpr int kWalletGapLimit = 20;
// The wallet no longer has its own dust constant: the dust limit is
// the dynamic, script-aware threshold from transactions/dust.h
// (P2PKH at 1 sat/vB = 182 sats). The old provisional 546-sat rule
// was removed in Milestone 13.
constexpr uint64_t kWalletMaxFeeRateSatVb = 100;  // policy, not consensus

struct WalletAddress {
  std::string address;  // P2PKH, "75..." prefix
  std::vector<unsigned char> pubkey;  // 33 bytes compressed
  std::vector<unsigned char> scriptPubKey;  // P2PKH script
  std::string path;     // e.g. m/44'/65535'/0'/0/7
  bool isChange = false;
  bool used = false;
};

struct WalletUtxo {
  OutPoint outpoint;
  Amount value = 0;
  std::string address;
  std::string path;  // derivation path of the spending key
  uint32_t index = 0;  // address index on its chain
  bool isChange = false;
  uint32_t height = 0;  // block height of the funding transaction
  bool isCoinbase = false;
};

// One wallet transaction-history record (Milestone 16). Persisted in
// the sidecar journal (wallet/txjournal.h), NOT in the encrypted
// wallet file. Amount follows the Bitcoin convention: negative for
// sends (excluding the fee, reported separately in `fee`), positive
// for receives. `confirmations` is NOT stored — it is computed at
// query time from the tip height. `label` is always "" in v0.1.0
// (no labels). The record holds no secrets.
struct WalletTxRecord {
  std::string txid;      // 64 hex chars
  std::string category;  // "send" or "receive"
  int64_t amount = 0;    // sats, signed
  int64_t fee = 0;       // sats, sends only (0 for receives)
  int64_t time = 0;      // unix: block time when confirmed, else first seen
  int blockheight = -1;  // -1 when unconfirmed
  std::string address;   // counterparty (sends) / first mine address (receives)
  std::string label;     // "" in v0.1.0
  bool operator==(const WalletTxRecord& o) const {
    return txid == o.txid && category == o.category &&
           amount == o.amount && fee == o.fee && time == o.time &&
           blockheight == o.blockheight && address == o.address &&
           label == o.label;
  }
};

class Wallet {
 public:
  Wallet() = default;

  // Create a new wallet from a fresh 24-word mnemonic.
  bool CreateNew(std::string& error);
  // Restore from a mnemonic (+ optional passphrase).
  bool RestoreFromMnemonic(const std::string& mnemonic,
                           const std::string& passphrase, std::string& error);
  // Restore from a 64-byte seed.
  bool RestoreFromSeed(const std::vector<unsigned char>& seed,
                       std::string& error);
  // Watch-only restore from an account public key (no spending).
  bool RestoreFromXpub(const std::string& xpub, std::string& error);
  bool IsWatchOnly() const { return watchOnly_; }
  bool HasKeys() const { return account_.isPrivate; }
  std::string GetMnemonic() const { return mnemonic_; }
  std::string GetAccountXprv() const;
  std::string GetAccountXpub() const;
  uint32_t GetAccountIndex() const { return accountIndex_; }
  // Derivation indices (for the node wallet: persisted in the wallet
  // file so a restart never re-issues an address).
  uint32_t GetNextExternal() const { return nextExternal_; }
  uint32_t GetNextChange() const { return nextChange_; }
  // Set the derivation indices, e.g. from the wallet file at load
  // time. Call ONLY with the persisted values right after a restore:
  // moving the indices backwards would re-issue addresses that may
  // already have been handed out. Refreshes the lookahead window.
  void SetNextIndices(uint32_t nextExternal, uint32_t nextChange);
  // Best-effort wipe of the secrets held in memory (mnemonic and
  // private keys), used by the node wallet's Lock(): after this the
  // object holds no spendable material. The public derivation state
  // (addresses, scripts) is left intact.
  void WipeSecrets();
  // Switch to another BIP44 account (derives a new account key).
  bool SetAccount(uint32_t accountIndex, std::string& error);

  // Fresh external / change addresses (advances the index).
  WalletAddress NewAddress(std::string& error);
  WalletAddress NewChangeAddress(std::string& error);
  // Address at a fixed index (does not advance anything).
  bool GetAddressAt(uint32_t index, bool isChange, WalletAddress& out,
                    std::string& error);
  // All addresses derived so far (external + change).
  std::vector<WalletAddress> ListAddresses() const;
  bool IsMine(const std::string& address) const;
  bool IsMineScript(const std::vector<unsigned char>& scriptPubKey) const;

  // Scan a UTXO set: mark used addresses, refresh balances.
  // `nextHeight` is the height a transaction built now would be
  // mined at (tip height + 1). Coinbase outputs are spendable only
  // once mature (COINBASE_MATURITY = 100 blocks; the genesis
  // coinbase at height 0 is exempt, exactly as in consensus
  // validation): immature coinbases are NOT in unspent_/balance_ —
  // they are tracked separately in immatureBalance_ — so neither
  // getbalance nor coin selection can ever see them as spendable.
  void ScanUtxo(const CoinsView& utxo, int nextHeight);
  Amount GetBalance() const { return balance_; }
  // Immature coinbase funds (not spendable yet).
  Amount GetImmatureBalance() const { return immatureBalance_; }
  std::vector<WalletUtxo> ListUnspent() const;

  // --- Transaction history (Milestone 16) ---
  // Minimal in-memory tx-record store, persisted in the sidecar
  // journal (txjournal.h) by the node wallet. RecordTx upserts by
  // txid (latest wins, never duplicated). Populated (a) by ScanUtxo:
  // every UTXO paying to a mine address creates/updates a "receive"
  // record (time = the funding block's time, from Coin.blockTime;
  // blockheight = the funding height) — including immature
  // coinbases, which are receives even though they are not yet
  // spendable; and (b) by the RPC layer on sendtoaddress, which
  // records a "send" immediately after broadcast. There is NO
  // mempool-acceptance hook in the Node: receives appear once
  // confirmed via rescan, sends appear immediately. A receive whose
  // UTXO has since been spent stays in the journal (the record
  // outlives the UTXO); a re-scan only refreshes records for UTXOs
  // still present. Sends made before the journal existed are not
  // recoverable (see txjournal.h).
  void RecordTx(const WalletTxRecord& record);
  std::vector<WalletTxRecord> ListTxRecords() const { return txRecords_; }
  // Replace the whole record store (used when loading the journal
  // from disk). Clears the dirty flag.
  void SetTxRecords(const std::vector<WalletTxRecord>& records);
  bool TxRecordsDirty() const { return txDirty_; }
  void ClearTxRecordsDirty() { txDirty_ = false; }

  // Build + sign a payment. `feeRateSatVb` defaults to 1; values
  // above the wallet max (100 sat/vB) are refused. Change below the
  // dust limit is added to the fee instead of creating an output.
  // On success returns true and sets `tx` + `fee`.
  bool CreateTransaction(const std::string& toAddress, Amount amount,
                         const CoinsView& utxo, int height,
                         uint32_t medianTimePast, Transaction& tx,
                         Amount& fee, std::string& error,
                         uint64_t feeRateSatVb = 1);

  // Fee estimate for a transaction shape (sat/vB * bytes).
  static std::size_t EstimateTxSize(std::size_t inputCount,
                                    std::size_t outputCount);

  void SetMaxFeeRate(uint64_t satVb) { maxFeeRateSatVb_ = satVb; }
  uint64_t GetMaxFeeRate() const { return maxFeeRateSatVb_; }

 private:
  bool DeriveAddress(uint32_t index, bool isChange, WalletAddress& out,
                     std::string& error);
  void EnsureLookahead();

  ExtendedKey master_;
  ExtendedKey account_;  // m/44'/65535'/account'
  uint32_t accountIndex_ = 0;
  std::string mnemonic_;
  bool watchOnly_ = false;
  uint64_t maxFeeRateSatVb_ = kWalletMaxFeeRateSatVb;
  // Derivation state.
  uint32_t nextExternal_ = 0;
  uint32_t nextChange_ = 0;
  // path -> address record (both chains).
  std::map<std::string, WalletAddress> addresses_;
  // scriptPubKey hex -> path (for IsMine / scanning).
  std::map<std::string, std::string> scriptToPath_;
  std::vector<WalletUtxo> unspent_;
  Amount balance_ = 0;
  Amount immatureBalance_ = 0;
  // Transaction history (Milestone 16), keyed by txid via RecordTx.
  std::vector<WalletTxRecord> txRecords_;
  bool txDirty_ = false;
};

}  // namespace oeeoo
