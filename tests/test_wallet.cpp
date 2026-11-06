// Tests: wallet (Milestone 8) — BIP39 vectors, BIP32 vectors,
// wallet addresses/balance/transactions, encrypted wallet file.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "consensus/consensus.h"
#include "crypto/address.h"
#include "crypto/ecdsa.h"
#include "script/script.h"
#include "test_harness.h"
#include "transactions/validation.h"
#include "utxo/coins.h"
#include "wallet/bip32.h"
#include "wallet/bip39.h"
#include "wallet/wallet.h"
#include "wallet/walletfile.h"

using namespace oeeoo;

namespace {
std::vector<unsigned char> HexToBytes(const std::string& hex) {
  std::vector<unsigned char> out;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    const auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return c - 'A' + 10;
    };
    out.push_back(static_cast<unsigned char>((nibble(hex[i]) << 4) |
                                             nibble(hex[i + 1])));
  }
  return out;
}

std::string BytesToHex(const std::vector<unsigned char>& bytes) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  for (unsigned char b : bytes) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0f]);
  }
  return out;
}

template <std::size_t N>
std::array<unsigned char, N> HexToArray(const std::string& hex) {
  std::vector<unsigned char> bytes = HexToBytes(hex);
  std::array<unsigned char, N> out{};
  if (bytes.size() == N) {
    std::memcpy(out.data(), bytes.data(), N);
  }
  return out;
}

std::string WalletFilePath(const std::string& name) {
  return (std::filesystem::temp_directory_path() /
          ("oeeoo-test-" + name + ".wallet"))
      .string();
}
}  // namespace

void TestWallet() {
  test::BeginSection("wallet: BIP39 mnemonic vectors");
  {
    // Canonical BIP39 vectors, verified against the official
    // wordlist with an independent Python implementation first.
    struct Case {
      const char* entropyHex;
      const char* mnemonic;
    };
    const Case cases[] = {
        {"00000000000000000000000000000000",
         "abandon abandon abandon abandon abandon abandon abandon abandon "
         "abandon abandon abandon about"},
        {"7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f",
         "legal winner thank year wave sausage worth useful legal winner "
         "thank yellow"},
        {"80808080808080808080808080808080",
         "letter advice cage absurd amount doctor acoustic avoid letter "
         "advice cage above"},
        {"ffffffffffffffffffffffffffffffff",
         "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong"},
        {"9e885d952ad362caeb4efe34a8e91bd2",
         "ozone drill grab fiber curtain grace pudding thank cruise elder "
         "eight picnic"},
        {"0000000000000000000000000000000000000000000000000000000000000000",
         "abandon abandon abandon abandon abandon abandon abandon abandon "
         "abandon abandon abandon abandon abandon abandon abandon abandon "
         "abandon abandon abandon abandon abandon abandon abandon art"},
        {"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
         "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo "
         "zoo zoo zoo zoo zoo zoo zoo vote"},
    };
    for (const Case& c : cases) {
      std::string got = MnemonicFromEntropy(HexToBytes(c.entropyHex));
      CHECK(got == c.mnemonic);
      std::vector<unsigned char> entropy;
      CHECK(EntropyFromMnemonic(c.mnemonic, entropy));
      CHECK(entropy == HexToBytes(c.entropyHex));
      CHECK(ValidateMnemonic(c.mnemonic));
    }
    // Bad checksum / unknown word / wrong count.
    CHECK(!ValidateMnemonic(
        "abandon abandon abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon abandon"));
    CHECK(!ValidateMnemonic(
        "abandon abandon abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon zzzzz"));
    CHECK(!ValidateMnemonic("abandon abandon abandon"));
    // Seed vectors (PBKDF2, computed independently in Python).
    const std::string zero24 =
        "abandon abandon abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon abandon abandon abandon abandon art";
    CHECK(BytesToHex(MnemonicToSeed(zero24, "TREZOR")) ==
          "bda85446c68413707090a52022edd26a1c9462295029f2e60cd7c4f2bbd30971"
          "70af7a4d73245cafa9c3cca8d561a7c3de6f5d4a10be8ed2a5e608d68f92fcc8");
    CHECK(BytesToHex(MnemonicToSeed(zero24, "")) ==
          "408b285c123836004f4b8842c89324c1f01382450c0d439af345ba7fc49acf70"
          "5489c6fc77dbd4e3dc1dd8cc6bc9f043db8ada1e243c4a0eafb290d399480840");
    // Fresh generation: 24 words, valid checksum.
    const std::string fresh = GenerateMnemonic(256);
    CHECK(ValidateMnemonic(fresh));
    CHECK(fresh.size() > 0);
  }

  test::BeginSection("wallet: BIP32 test vector 1");
  {
    // Official BIP32 test vector 1 (seed 000102030405060708090a0b0c0d0e0f);
    // key material decoded from the published xprv/xpub strings.
    ExtendedKey master;
    CHECK(MasterKeyFromSeed(HexToBytes("000102030405060708090a0b0c0d0e0f"),
                            master));
    CHECK(BytesToHex(std::vector<unsigned char>(master.privkey.begin(),
                                                master.privkey.end())) ==
          "e8f32e723decf4051aefac8e2c93c9c5b214313817cdb01a1494b917c8436b35");
    CHECK(BytesToHex(std::vector<unsigned char>(master.chainCode.begin(),
                                                master.chainCode.end())) ==
          "873dff81c02f525623fd1fe5167eac3a55a049de3d314bb42ee227ffed37d508");
    CHECK(BytesToHex(std::vector<unsigned char>(master.pubkey.begin(),
                                                master.pubkey.end())) ==
          "0339a36013301597daef41fbe593a02cc513d0b55527ec2df1050e2e8ff49c85c2");
    struct PathCase {
      const char* path;
      const char* priv;
      const char* chain;
      const char* pub;
    };
    const PathCase paths[] = {
        {"m/0'",
         "edb2e14f9ee77d26dd93b4ecede8d16ed408ce149b6cd80b0715a2d911a0afea",
         "47fdacbd0f1097043b78c63c20c34ef4ed9a111d980047ad16282c7ae6236141",
         "035a784662a4a20a65bf6aab9ae98a6c068a81c52e4b032c0fb5400c706cfccc56"},
        {"m/0'/1",
         "3c6cb8d0f6a264c91ea8b5030fadaa8e538b020f0a387421a12de9319dc93368",
         "2a7857631386ba23dacac34180dd1983734e444fdbf774041578e9b6adb37c19",
         "03501e454bf00751f24b1b489aa925215d66af2234e3891c3b21a52bedb3cd711c"},
        {"m/0'/1/2'",
         "cbce0d719ecf7431d88e6a89fa1483e02e35092af60c042b1df2ff59fa424dca",
         "04466b9cc8e161e966409ca52986c584f07e9dc81f735db683c3ff6ec7b1503f",
         "0357bfe1e341d01c69fe5654309956cbea516822fba8a601743a012a7896ee8dc2"},
        {"m/0'/1/2'/2",
         "0f479245fb19a38a1954c5c7c0ebab2f9bdfd96a17563ef28a6a4b1a2a764ef4",
         "cfb71883f01676f587d023cc53a35bc7f88f724b1f8c2892ac1275ac822a3edd",
         "02e8445082a72f29b75ca48748a914df60622a609cacfce8ed0e35804560741d29"},
        {"m/0'/1/2'/2/1000000000",
         "471b76e389e528d6de6d816857e012c5455051cad6660850e58372a6c3e6e7c8",
         "c783e67b921d2beb8f6b389cc646d7263b4145701dadd2161548a8b078e65e9e",
         "022a471424da5e657499d1ff51cb43c47481a03b1e77f951fe64cec9f5a48f7011"},
    };
    for (const PathCase& p : paths) {
      ExtendedKey key;
      CHECK(DerivePath(master, p.path, key));
      CHECK(BytesToHex(std::vector<unsigned char>(key.privkey.begin(),
                                                  key.privkey.end())) ==
            p.priv);
      CHECK(BytesToHex(std::vector<unsigned char>(key.chainCode.begin(),
                                                  key.chainCode.end())) ==
            p.chain);
      CHECK(BytesToHex(std::vector<unsigned char>(key.pubkey.begin(),
                                                  key.pubkey.end())) ==
            p.pub);
    }
    // Neutered derivation gives the same public keys: derive the
    // hardened level privately, neuter it, then derive a normal
    // child from both the private and the neutered key.
    ExtendedKey prv0, pub0, prv01;
    CHECK(DerivePath(master, "m/0'", prv0));
    CHECK(DerivePath(master, "m/0'/1", prv01));
    CHECK(DerivePath(Neuter(prv0), "m/1", pub0));
    CHECK(pub0.pubkey == prv01.pubkey);
    CHECK(pub0.chainCode == prv01.chainCode);
    // Hardened derivation from a public key fails.
    ExtendedKey bad;
    CHECK(!DeriveChild(Neuter(master), 0x80000000, bad));
    // Serialize / parse round-trip (Oeeoo version bytes).
    const std::string xprv = SerializePrivate(master);
    const std::string xpub = SerializePublic(master);
    CHECK(!xprv.empty() && !xpub.empty());
    ExtendedKey parsed;
    CHECK(ParseExtendedKey(xprv, parsed));
    CHECK(parsed.isPrivate);
    CHECK(parsed.privkey == master.privkey);
    CHECK(parsed.chainCode == master.chainCode);
    CHECK(ParseExtendedKey(xpub, parsed));
    CHECK(!parsed.isPrivate);
    CHECK(parsed.pubkey == master.pubkey);
    // Bad checksum / wrong version rejected.
    std::string tampered = xprv;
    tampered[tampered.size() - 2] =
        tampered[tampered.size() - 2] == '1' ? '2' : '1';
    CHECK(!ParseExtendedKey(tampered, parsed));
    // Path syntax errors.
    CHECK(!DerivePath(master, "44'/0'", prv0));
    CHECK(!DerivePath(master, "m/44''/0'", prv0));
    CHECK(!DerivePath(master, "m/", prv0));
  }

  test::BeginSection("wallet: addresses, balance, gap limit");
  {
    Wallet wallet;
    std::string error;
    const std::string mnemonic =
        "abandon abandon abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon abandon abandon abandon abandon art";
    CHECK(wallet.RestoreFromMnemonic(mnemonic, "", error));
    CHECK(!wallet.IsWatchOnly());
    CHECK(wallet.HasKeys());
    CHECK(wallet.GetMnemonic() == mnemonic);
    // Same mnemonic restores the same addresses (deterministic).
    Wallet wallet2;
    CHECK(wallet2.RestoreFromMnemonic(mnemonic, "", error));
    WalletAddress a0, b0;
    CHECK(wallet.GetAddressAt(0, false, a0, error));
    CHECK(wallet2.GetAddressAt(0, false, b0, error));
    CHECK(a0.address == b0.address);
    CHECK(a0.path == "m/44'/65535'/0'/0/0");
    // Independent Python cross-check (pure-Python secp256k1 + HMAC):
    // the first address of this mnemonic, and the account keys.
    CHECK(a0.address == "7L88G1Not1Gv8nmci9mchhoN9XgbXTXqX5");
    CHECK(wallet.GetAccountXprv() ==
          "xq6pcqnZytVx5DfBCDRZWczYaDqzbNkvdvyb5giEYiG1wZBBAs28edpFRE"
          "MaS6nbcTQLpCqz6QVqG4ioz4goLv1Gab4CrEM3CTjnJqBBvXEuPZz");
    CHECK(wallet.GetAccountXpub() ==
          "xpyEjGsjUsxCaV7JVdAX4w9eHx9qvKaj2wyfE3UtYT6m5Lp7cEtwaXtPrj"
          "Z7UM4vW67fzv5y5QhFHPLY2Xt8QQ7NwC3fxftj3HEVKhLs18epxKA");
    // Addresses are P2PKH, 34 chars, starting with "7" (version
    // byte 0x0f). NOTE: the requested "75Pk" prefix is NOT universal
    // — the second character depends on the key's hash, so only
    // some addresses start with "75" (verified with 10 random
    // hash160s; see docs/MILESTONE8.md). "75Pk" is achievable per
    // address by vanity search, not for every address.
    CHECK(a0.address.rfind("7", 0) == 0);
    CHECK(a0.address.size() == 34);
    Hash160 hash;
    CHECK(DecodeP2PKHAddress(a0.address, hash));
    CHECK(hash == PubKeyHash160(a0.pubkey));
    // Fresh addresses differ; change addresses use chain 1.
    WalletAddress a1;
    CHECK(wallet.GetAddressAt(1, false, a1, error));
    CHECK(a1.address != a0.address);
    WalletAddress ch0;
    CHECK(wallet.GetAddressAt(0, true, ch0, error));
    CHECK(ch0.path == "m/44'/65535'/0'/1/0");
    CHECK(ch0.address != a0.address);
    CHECK(wallet.IsMine(a0.address));
    CHECK(!wallet.IsMine("75PkvD7AKStxvaDSyQH9gtC9kTStqPnPxV"));
    // Fund the first two external addresses; scan finds them.
    CoinsView utxo;
    auto fund = [&](const WalletAddress& addr, Amount value, int height) {
      Coin coin;
      coin.out.value = value;
      coin.out.scriptPubKey = addr.scriptPubKey;
      coin.height = height;
      coin.blockTime = 1700000000;
      coin.isCoinbase = false;
      Hash256 txid;
      txid.fill(static_cast<unsigned char>(height + 1));
      CHECK(utxo.AddCoin({txid, 0}, coin));
      return txid;
    };
    fund(a0, 10 * COIN, 1);
    fund(a1, 5 * COIN, 2);
    wallet.ScanUtxo(utxo, 3);
    CHECK(wallet.GetBalance() == 15 * COIN);
    CHECK(wallet.ListUnspent().size() == 2);
    // Used addresses are marked; the next fresh address moves past
    // the last used index (gap limit window).
    WalletAddress fresh;
    CHECK(wallet.GetAddressAt(0, false, fresh, error));
    CHECK(fresh.used);
    std::string newError;
    WalletAddress next = wallet.NewAddress(newError);
    CHECK(next.path == "m/44'/65535'/0'/0/2");
    // Account xpub restores watch-only with the same addresses.
    Wallet watch;
    CHECK(watch.RestoreFromXpub(wallet.GetAccountXpub(), error));
    CHECK(watch.IsWatchOnly());
    CHECK(!watch.HasKeys());
    WalletAddress w0;
    CHECK(watch.GetAddressAt(0, false, w0, error));
    CHECK(w0.address == a0.address);
  }

  test::BeginSection("wallet: build + sign a payment");
  {
    Wallet wallet;
    std::string error;
    const std::string mnemonic =
        "abandon abandon abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon abandon abandon abandon abandon art";
    CHECK(wallet.RestoreFromMnemonic(mnemonic, "", error));
    // Destination: a fresh wallet's first address.
    Wallet dest;
    CHECK(dest.RestoreFromMnemonic(
        "legal winner thank year wave sausage worth useful legal winner "
        "thank yellow",
        "", error));
    WalletAddress destAddr;
    CHECK(dest.GetAddressAt(0, false, destAddr, error));
    // Fund the sender with 10 OEEOO on its first address.
    CoinsView utxo;
    WalletAddress sender;
    CHECK(wallet.GetAddressAt(0, false, sender, error));
    Coin coin;
    coin.out.value = 10 * COIN;
    coin.out.scriptPubKey = sender.scriptPubKey;
    coin.height = 1;
    coin.blockTime = 1700000000;
    coin.isCoinbase = false;
    Hash256 fundTxid;
    fundTxid.fill(0x61);
    CHECK(utxo.AddCoin({fundTxid, 0}, coin));
    wallet.ScanUtxo(utxo, 2);
    CHECK(wallet.GetBalance() == 10 * COIN);
    Transaction tx;
    Amount fee = 0;
    CHECK(wallet.CreateTransaction(destAddr.address, 3 * COIN, utxo, 2,
                                   1700000000, tx, fee, error, 1));
    CHECK(tx.vin.size() == 1);
    CHECK(tx.vout.size() == 2);  // payment + change
    CHECK(tx.vout[0].value == 3 * COIN);
    // Fee = size * 1 sat/vB; change is the rest.
    const std::size_t size = Wallet::EstimateTxSize(1, 2);
    CHECK(fee == static_cast<Amount>(size));
    CHECK(tx.vout[1].value == 10 * COIN - 3 * COIN - fee);
    // The built transaction validates against the UTXO set.
    TxValidationResult r = ValidateTransaction(tx, utxo, 2, 1700000000);
    CHECK(r.ok);
    CHECK(r.fee == fee);
    // Insufficient funds / dust / bad address / fee cap.
    Transaction tx2;
    Amount fee2 = 0;
    CHECK(!wallet.CreateTransaction(destAddr.address, 100 * COIN, utxo, 2,
                                    1700000000, tx2, fee2, error, 1));
    CHECK(!wallet.CreateTransaction(destAddr.address, 100, utxo, 2,
                                    1700000000, tx2, fee2, error, 1));
    CHECK(!wallet.CreateTransaction("75invalidaddress", 1 * COIN, utxo, 2,
                                    1700000000, tx2, fee2, error, 1));
    CHECK(!wallet.CreateTransaction(destAddr.address, 1 * COIN, utxo, 2,
                                    1700000000, tx2, fee2, error, 101));
    // Watch-only wallet cannot spend.
    Wallet watch;
    CHECK(watch.RestoreFromXpub(wallet.GetAccountXpub(), error));
    CHECK(!watch.CreateTransaction(destAddr.address, 1 * COIN, utxo, 2,
                                   1700000000, tx2, fee2, error, 1));
  }

  test::BeginSection("wallet: encrypted wallet file");
  {
    const std::string path = WalletFilePath("main");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    WalletFileData data;
    data.mnemonic =
        "abandon abandon abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon abandon abandon abandon abandon art";
    data.nextExternal = 7;
    data.nextChange = 3;
    std::string error;
    CHECK(SaveWalletFile(path, data, "correct horse battery staple", error));
    WalletFileData loaded;
    CHECK(LoadWalletFile(path, "correct horse battery staple", loaded,
                         error));
    CHECK(loaded.mnemonic == data.mnemonic);
    CHECK(loaded.nextExternal == 7);
    CHECK(loaded.nextChange == 3);
    // Wrong passphrase fails.
    WalletFileData bad;
    CHECK(!LoadWalletFile(path, "wrong passphrase", bad, error));
    // Tampered file fails.
    {
      FILE* f = std::fopen(path.c_str(), "r+b");
      CHECK(f != nullptr);
      // M17: CHECK does not abort, so a failed fopen would segfault
      // on the fseek below (found when parallel suite runs collided
      // on this shared /tmp path). Bail out; the CHECK already failed.
      if (f == nullptr) return;
      std::fseek(f, 30, SEEK_SET);
      unsigned char byte = 0;
      CHECK(std::fread(&byte, 1, 1, f) == 1);
      // XOR, don't write a fixed value: offset 30 is a random nonce
      // byte, so writing a fixed 0xFF was a no-op (and the test
      // flaky, p = 1/256) whenever the byte was already 0xFF.
      byte ^= 0xFF;
      std::fseek(f, 30, SEEK_SET);
      CHECK(std::fwrite(&byte, 1, 1, f) == 1);
      std::fclose(f);
    }
    CHECK(!LoadWalletFile(path, "correct horse battery staple", bad, error));
    std::filesystem::remove(path, ec);
    // Missing file fails.
    CHECK(!LoadWalletFile(path, "correct horse battery staple", bad, error));
  }
}
