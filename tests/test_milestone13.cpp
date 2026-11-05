// Tests: Milestone 13 — audit gap closures.
// Dust thresholds, opt-in RBF, mempool eviction + expiry, fee
// estimator, long-range supply simulation, genesis file negatives,
// vesting at extreme times, low-S signatures, JSON depth limit,
// wallet truncation, crash recovery, genesis identity, peer cap,
// transport close, and a SIGHASH_ALL vector.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>

#include "app/genesis.h"
#include "blockchain/chain.h"
#include "consensus/consensus.h"
#include "consensus/difficulty.h"
#include "consensus/subsidy.h"
#include "consensus/vesting.h"
#include "crypto/ecdsa.h"
#include "db/chainstore.h"
#include "mempool/feeestimator.h"
#include "mempool/mempool.h"
#include "mining/miner.h"
#include "network/addrman.h"
#include "network/compact.h"
#include "network/node.h"
#include "network/protocol.h"
#include "network/transport.h"
#include "rpc/json.h"
#include "script/script.h"
#include "test_harness.h"
#include "transactions/dust.h"
#include "transactions/sighash.h"
#include "transactions/validation.h"
#include "wallet/walletfile.h"

using namespace oeeoo;

// The low-level OpenSSL EC API is deprecated in OpenSSL 3.0 but
// still correct and fully supported; silence the warning here the
// same way ecdsa.cpp does, so the build stays warning-free.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

namespace {
constexpr uint32_t kTestBits = 0x207fffff;  // easy test-only difficulty
constexpr uint32_t kTime = 1700000000;
constexpr uint32_t kNow = 1800000000;

KeyPair g_key;
bool g_keysReady = false;

void EnsureKeys() {
  if (!g_keysReady) {
    CHECK(GenerateKeyPair(g_key));
    g_keysReady = true;
  }
}

std::vector<unsigned char> Spk() {
  return CreateP2PKHScriptPubKey(PubKeyHash160(g_key.pubkey));
}

Hash256 TxIdWith(unsigned char fill) {
  Hash256 h;
  h.fill(fill);
  return h;
}

void Fund(CoinsView& utxo, const Hash256& txid, Amount value) {
  Coin coin;
  coin.out.value = value;
  coin.out.scriptPubKey = Spk();
  coin.height = 0;
  coin.blockTime = kTime;
  coin.isCoinbase = false;
  CHECK(utxo.AddCoin({txid, 0}, coin));
}

std::vector<unsigned char> SignInput(const Transaction& tx, std::size_t nIn) {
  Hash256 h;
  CHECK(LegacySighash(tx, nIn, Spk(), kSigHashAll, h));
  std::vector<unsigned char> sig = SignHash(h, g_key.privkey);
  CHECK(!sig.empty());
  sig.push_back(static_cast<unsigned char>(kSigHashAll));
  return sig;
}

// A one-input spend of `prev` (output 0) paying `sendValue` (plus
// optional extra outputs) to our own P2PKH, with the input's
// nSequence set to `sequence`.
Transaction Spend(const Hash256& prev, Amount sendValue,
                  uint32_t sequence = 0xffffffff,
                  const std::vector<Amount>& extra = {}) {
  Transaction tx;
  tx.version = 1;
  TxIn in;
  in.prevTxHash = prev;
  in.prevOutIndex = 0;
  in.sequence = sequence;
  tx.vin.push_back(in);
  TxOut out;
  out.value = sendValue;
  out.scriptPubKey = Spk();
  tx.vout.push_back(out);
  for (Amount v : extra) {
    TxOut extraOut;
    extraOut.value = v;
    extraOut.scriptPubKey = Spk();
    tx.vout.push_back(extraOut);
  }
  tx.lockTime = 0;
  tx.vin[0].scriptSig =
      CreateP2PKHScriptSig(SignInput(tx, 0), g_key.pubkey);
  return tx;
}

bool AddOk(Mempool& pool, const Transaction& tx, const CoinsView& utxo,
           uint32_t now = kNow) {
  std::string error;
  bool ok = pool.AddTransaction(tx, utxo, 1, kTime, now, error);
  if (!ok) std::printf("  unexpected mempool rejection: %s\n", error.c_str());
  return ok;
}

bool AddFails(Mempool& pool, const Transaction& tx, const CoinsView& utxo,
              const std::string& needle, uint32_t now = kNow) {
  std::string error;
  if (pool.AddTransaction(tx, utxo, 1, kTime, now, error)) return false;
  return error.find(needle) != std::string::npos;
}

// The shared test genesis: coinbase value 0 (GetBlockSubsidy(0) ==
// 0) with the genesis message as extra data.
Block MineGenesis() {
  Block genesis;
  genesis.header.nVersion = 1;
  genesis.header.nTime = kTime;
  genesis.header.nBits = kTestBits;
  genesis.vtx = {CreateCoinbase(0, Spk(), 0, "THINK ABOUT GOD?")};
  genesis.header.hashMerkleRoot = genesis.ComputeMerkleRoot();
  CHECK(MineBlock(genesis, 0, "THINK ABOUT GOD?"));
  return genesis;
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

bool WriteFile(const std::string& path, const std::string& text) {
  std::ofstream out(path);
  if (!out) return false;
  out << text;
  return static_cast<bool>(out);
}
}  // namespace

void TestMilestone13() {
  EnsureKeys();

  test::BeginSection("m13: dust thresholds are dynamic and script-aware");
  {
    // P2PKH at 1 sat/vB: (34-byte output + 148-byte input) = 182.
    CHECK(GetDustThreshold(Spk(), 1) == 182);
    CHECK(IsDust(181, Spk(), 1));
    CHECK(!IsDust(182, Spk(), 1));
    CHECK(!IsDust(183, Spk(), 1));
    // The rate scales: 2 sat/vB doubles the threshold.
    CHECK(GetDustThreshold(Spk(), 2) == 364);
    // Bare 2-of-3 multisig: (114-byte output + 190-byte input) = 304.
    std::vector<std::vector<unsigned char>> keys = {g_key.pubkey,
                                                    g_key.pubkey,
                                                    g_key.pubkey};
    const std::vector<unsigned char> ms = BuildMultisigScript(2, keys);
    CHECK(ms.size() == 105);
    CHECK(GetDustThreshold(ms, 1) == 304);
    CHECK(IsDust(303, ms, 1));
    CHECK(!IsDust(304, ms, 1));
  }

  test::BeginSection("m13: mempool opt-in RBF");
  {
    CoinsView utxo;
    Fund(utxo, TxIdWith(0x11), 50 * COIN);
    Mempool pool;
    // tx1 signals replaceability and pays 1 OEEOO.
    Transaction tx1 = Spend(TxIdWith(0x11), 49 * COIN, 0xfffffffd);
    CHECK(AddOk(pool, tx1, utxo));
    // A cheaper replacement is rejected.
    Transaction cheap = Spend(TxIdWith(0x11), 49 * COIN + COIN / 2);
    CHECK(AddFails(pool, cheap, utxo, "not higher"));
    CHECK(pool.Contains(tx1.GetTxId()));
    // A bigger replacement with a higher fee but a lower fee rate
    // is rejected too: two extra outputs grow the tx by ~68 bytes
    // while the fee only rises from 1 to 1.1 OEEOO, so the fee rate
    // falls below tx1's.
    Transaction big = Spend(TxIdWith(0x11), 46 * COIN + COIN * 9 / 10,
                            0xffffffff, {1 * COIN, 1 * COIN});
    CHECK(AddFails(pool, big, utxo, "fee rate is not higher"));
    // A valid replacement: higher fee, higher rate, same input.
    Transaction tx2 = Spend(TxIdWith(0x11), 48 * COIN);
    CHECK(AddOk(pool, tx2, utxo));
    CHECK(!pool.Contains(tx1.GetTxId()));
    CHECK(pool.Contains(tx2.GetTxId()));
    // tx2 does not signal, so it cannot be replaced at any price.
    Transaction tx3 = Spend(TxIdWith(0x11), 47 * COIN);
    CHECK(AddFails(pool, tx3, utxo, "already spent"));
    CHECK(pool.Contains(tx2.GetTxId()));
    // A dust output is rejected outright.
    Transaction dusty = Spend(TxIdWith(0x11), 100);
    CHECK(AddFails(pool, dusty, utxo, "dust"));
  }

  test::BeginSection("m13: mempool eviction under pressure");
  {
    CoinsView utxo;
    Fund(utxo, TxIdWith(0x21), 50 * COIN);
    Fund(utxo, TxIdWith(0x22), 50 * COIN);
    Fund(utxo, TxIdWith(0x23), 50 * COIN);
    Fund(utxo, TxIdWith(0x24), 50 * COIN);
    Mempool pool(400);  // tiny cap, to force eviction
    Transaction a = Spend(TxIdWith(0x21), 49 * COIN);
    Transaction b = Spend(TxIdWith(0x22), 49 * COIN);
    CHECK(AddOk(pool, a, utxo));
    CHECK(AddOk(pool, b, utxo));
    CHECK(pool.Bytes() <= 400);
    // A higher-fee tx that does not fit evicts the lowest-rate one.
    Transaction c = Spend(TxIdWith(0x23), 48 * COIN);
    CHECK(AddOk(pool, c, utxo));
    CHECK(pool.Contains(c.GetTxId()));
    CHECK(pool.Bytes() <= 400);
    CHECK(pool.Size() == 2);
    // A worse tx never evicts a better one.
    Transaction d = Spend(TxIdWith(0x24), 49 * COIN + COIN / 2);
    std::string error;
    CHECK(!pool.AddTransaction(d, utxo, 1, kTime, kNow, error));
    CHECK(pool.Size() == 2);
  }

  test::BeginSection("m13: mempool expiry is enforced by the node");
  {
    Node node(kTestBits);
    node.SetTime(kNow);
    auto transport = std::make_shared<LoopbackTransport>("loopback:m13a");
    node.SetTransport(transport);
    Block genesis = MineGenesis();
    std::string error;
    CHECK(node.BroadcastBlock(genesis, error));
    Coin coin;
    coin.out.value = 50 * COIN;
    coin.out.scriptPubKey = Spk();
    coin.height = 0;
    coin.blockTime = kTime;
    coin.isCoinbase = false;
    CHECK(node.Utxo().AddCoin({TxIdWith(0x31), 0}, coin));
    Transaction tx = Spend(TxIdWith(0x31), 49 * COIN);
    CHECK(node.BroadcastTx(tx, error));
    CHECK(node.HasTx(tx.GetTxId()));
    // One minute later: still there (14 days have not passed).
    node.SetTime(kNow + 60);
    node.Poll(0);
    CHECK(node.HasTx(tx.GetTxId()));
    // 14 days + 1 second later: the expiry sweep drops it.
    node.SetTime(kNow + 14 * 24 * 60 * 60 + 1);
    node.Poll(0);
    CHECK(!node.HasTx(tx.GetTxId()));
  }

  test::BeginSection("m13: fee estimator over the last 24 blocks");
  {
    FeeEstimator est;
    CHECK(est.EstimateFeeRate() == 1);  // no data: minimum relay fee
    CHECK(est.BlocksRecorded() == 0);
    est.RecordBlock(1, {{1000, 100}, {2000, 100}, {3000, 100}});
    CHECK(est.EstimateFeeRate() == 20);  // median of 10, 20, 30 sat/vB
    CHECK(est.BlocksRecorded() == 1);
    // Out-of-order records are ignored.
    est.RecordBlock(1, {{999999, 1}});
    CHECK(est.EstimateFeeRate() == 20);
    // 24 more blocks push the first block out of the window.
    for (int h = 2; h <= 25; ++h) {
      est.RecordBlock(h, {{5000, 100}});
    }
    CHECK(est.BlocksRecorded() == 24);
    CHECK(est.EstimateFeeRate() == 50);  // only 50 sat/vB data left
  }

  test::BeginSection("m13: long-range supply simulation");
  {
    // Sum the subsidy over every height that pays one. A loop of
    // 940,000 heights is the actual issuance, not an era formula.
    __int128 total = 0;
    for (int h = 1; h <= 940000; ++h) {
      total += GetBlockSubsidy(h);
    }
    CHECK(total == static_cast<__int128>(MINING_ALLOCATION));
    CHECK(total + FOUNDER_ALLOCATION == MAX_MONEY);
    CHECK(GetBlockSubsidy(940001) == 0);
    CHECK(GetBlockSubsidy(1000000) == 0);
    // Every halving boundary, both sides.
    CHECK(GetBlockSubsidy(210000) == 50 * COIN);
    CHECK(GetBlockSubsidy(210001) == 25 * COIN);
    CHECK(GetBlockSubsidy(420000) == 25 * COIN);
    CHECK(GetBlockSubsidy(420001) == 1250000000);
    CHECK(GetBlockSubsidy(630000) == 1250000000);
    CHECK(GetBlockSubsidy(630001) == 625000000);
    CHECK(GetBlockSubsidy(840000) == 625000000);
    CHECK(GetBlockSubsidy(840001) == 312500000);
    CHECK(GetBlockSubsidy(940000) == 312500000);
  }

  test::BeginSection("m13: genesis file negatives");
  {
    std::vector<std::vector<unsigned char>> keys = {g_key.pubkey,
                                                    g_key.pubkey,
                                                    g_key.pubkey};
    const std::vector<unsigned char> founder = BuildMultisigScript(2, keys);
    Block genesis = BuildGenesisBlock(founder, 1793923200, kTestBits);
    std::string error;
    CHECK(MineGenesisParallel(genesis, 2, nullptr, nullptr, error));
    const std::string path = "/tmp/oeeoo-m13-genesis.json";
    CHECK(WriteGenesisFile(path, genesis, founder, error));
    const std::string good = ReadFile(path);
    // Wrong hash field.
    {
      std::string bad = good;
      const std::size_t pos = bad.find("\"hash\":\"");
      CHECK(pos != std::string::npos);
      bad[pos + 8] = (bad[pos + 8] == 'a' ? 'b' : 'a');
      CHECK(WriteFile(path, bad));
      Block out;
      std::vector<unsigned char> script;
      CHECK(!ReadGenesisFile(path, out, script, error));
    }
    // Wrong time field.
    {
      std::string bad = good;
      const std::size_t pos = bad.find("\"time\":");
      CHECK(pos != std::string::npos);
      bad.replace(pos + 7, 10, "1793923201");
      CHECK(WriteFile(path, bad));
      Block out;
      std::vector<unsigned char> script;
      CHECK(!ReadGenesisFile(path, out, script, error));
    }
    // Wrong bits field.
    {
      std::string bad = good;
      const std::size_t pos = bad.find("\"bits\":\"");
      CHECK(pos != std::string::npos);
      bad.replace(pos + 8, 8, "207ffffe");
      CHECK(WriteFile(path, bad));
      Block out;
      std::vector<unsigned char> script;
      CHECK(!ReadGenesisFile(path, out, script, error));
    }
    // Wrong Merkle root field.
    {
      std::string bad = good;
      const std::size_t pos = bad.find("\"merkleroot\":\"");
      CHECK(pos != std::string::npos);
      bad[pos + 14] = (bad[pos + 14] == 'a' ? 'b' : 'a');
      CHECK(WriteFile(path, bad));
      Block out;
      std::vector<unsigned char> script;
      CHECK(!ReadGenesisFile(path, out, script, error));
    }
    // Wrong nonce field (replace the whole value with 999999).
    {
      std::string bad = good;
      const std::size_t pos = bad.find("\"nonce\":");
      CHECK(pos != std::string::npos);
      const std::size_t start = pos + 8;
      std::size_t end = bad.find_first_of(",}", start);
      CHECK(end != std::string::npos);
      bad.replace(start, end - start, "999999");
      CHECK(WriteFile(path, bad));
      Block out;
      std::vector<unsigned char> script;
      CHECK(!ReadGenesisFile(path, out, script, error));
    }
    // Wrong founder script field.
    {
      std::string bad = good;
      const std::size_t pos = bad.find("\"founder_script\":\"");
      CHECK(pos != std::string::npos);
      bad[pos + 18] = (bad[pos + 18] == 'a' ? 'b' : 'a');
      CHECK(WriteFile(path, bad));
      Block out;
      std::vector<unsigned char> script;
      CHECK(!ReadGenesisFile(path, out, script, error));
    }
    // Wrong message field.
    {
      std::string bad = good;
      const std::size_t pos = bad.find("THINK ABOUT GOD");
      CHECK(pos != std::string::npos);
      bad.replace(pos, 5, "THANK");
      CHECK(WriteFile(path, bad));
      Block out;
      std::vector<unsigned char> script;
      CHECK(!ReadGenesisFile(path, out, script, error));
    }
    // A tampered block (one byte of the serialized block changed)
    // fails the hash check.
    {
      std::string bad = good;
      const std::size_t pos = bad.find("\"block\":\"");
      CHECK(pos != std::string::npos);
      bad[pos + 9] = (bad[pos + 9] == 'a' ? 'b' : 'a');
      CHECK(WriteFile(path, bad));
      Block out;
      std::vector<unsigned char> script;
      CHECK(!ReadGenesisFile(path, out, script, error));
    }
    // The untampered file reads back fine.
    CHECK(WriteFile(path, good));
    Block out;
    std::vector<unsigned char> script;
    CHECK(ReadGenesisFile(path, out, script, error));
    CHECK(out.GetHash() == genesis.GetHash());
    std::filesystem::remove(path);
  }

  test::BeginSection("m13: vesting at extreme times");
  {
    // A launch near the uint32_t limit: anniversaries must clamp
    // to UINT32_MAX, never wrap to an early (wrong) time.
    const uint32_t launch = UINT32_MAX - 1000;
    CHECK(AnniversaryTime(launch, 9) == UINT32_MAX);
    CHECK(AnniversaryTime(launch, 0) == launch);
    VestingParams params;
    params.enabled = true;
    params.launchTime = launch;
    params.founderScript = Spk();
    // Even at the maximum time, the vested amount never exceeds
    // the founder allocation.
    CHECK(VestedAmount(params, UINT32_MAX) <= FOUNDER_ALLOCATION);
    CHECK(VestedAmount(params, UINT32_MAX) == FOUNDER_ALLOCATION);
    // Before launch, nothing is vested.
    CHECK(VestedAmount(params, launch - 1) == 0);
  }

  test::BeginSection("m13: low-S signatures (BIP62)");
  {
    // SignHash always produces low-S signatures, and the verifier
    // rejects a high-S variant of the same signature.
    const Hash256 h = Sha256d("low-s test");
    std::vector<unsigned char> sig = SignHash(h, g_key.privkey);
    CHECK(!sig.empty());
    CHECK(VerifySignature(g_key.pubkey, sig, h));
    // Rebuild the signature with S' = n - S (high-S).
    const unsigned char* p = sig.data();
    ECDSA_SIG* parsed = d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(sig.size()));
    CHECK(parsed != nullptr);
    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    ECDSA_SIG_get0(parsed, &r, &s);
    EC_GROUP* group = EC_GROUP_new_by_curve_name(NID_secp256k1);
    CHECK(group != nullptr);
    BIGNUM* order = BN_new();
    BIGNUM* highS = BN_new();
    CHECK(EC_GROUP_get_order(group, order, nullptr) == 1);
    CHECK(BN_sub(highS, order, s) == 1);
    BIGNUM* rCopy = BN_dup(r);
    ECDSA_SIG* highSig = ECDSA_SIG_new();
    CHECK(ECDSA_SIG_set0(highSig, rCopy, highS) == 1);
    int derLen = i2d_ECDSA_SIG(highSig, nullptr);
    CHECK(derLen > 0);
    std::vector<unsigned char> highDer(static_cast<std::size_t>(derLen));
    unsigned char* q = highDer.data();
    CHECK(i2d_ECDSA_SIG(highSig, &q) == derLen);
    // The high-S signature must be rejected.
    CHECK(!VerifySignature(g_key.pubkey, highDer, h));
    ECDSA_SIG_free(highSig);
    ECDSA_SIG_free(parsed);
    BN_free(order);
    EC_GROUP_free(group);
    // Repeated signing always verifies (low-S by construction).
    for (int i = 0; i < 50; ++i) {
      Hash256 hi = Sha256d("round " + std::to_string(i));
      std::vector<unsigned char> si = SignHash(hi, g_key.privkey);
      CHECK(VerifySignature(g_key.pubkey, si, hi));
    }
  }

  test::BeginSection("m13: JSON depth limit");
  {
    Json out;
    std::string error;
    std::string deep;
    for (int i = 0; i < 200; ++i) deep += '[';
    for (int i = 0; i < 200; ++i) deep += ']';
    CHECK(!Json::Parse(deep, out, error));
    CHECK(error.find("deep") != std::string::npos);
    std::string shallow = "[[[[1]]]]";
    CHECK(Json::Parse(shallow, out, error));
    std::string nestedObj;
    for (int i = 0; i < 200; ++i) nestedObj += "{\"a\":";
    nestedObj += "1";
    for (int i = 0; i < 200; ++i) nestedObj += '}';
    CHECK(!Json::Parse(nestedObj, out, error));
  }

  test::BeginSection("m13: wallet file truncation is rejected");
  {
    const std::string path = "/tmp/oeeoo-m13-wallet.dat";
    WalletFileData data;
    data.mnemonic =
        "abandon abandon abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon abandon abandon abandon abandon art";
    data.nextExternal = 7;
    data.nextChange = 3;
    std::string error;
    CHECK(SaveWalletFile(path, data, "correct horse battery staple", error));
    const std::size_t fullSize = std::filesystem::file_size(path);
    // Truncate to half the file: the size check must refuse it.
    std::error_code ec;
    std::filesystem::resize_file(path, fullSize / 2, ec);
    CHECK(!ec);
    WalletFileData loaded;
    CHECK(!LoadWalletFile(path, "correct horse battery staple", loaded,
                         error));
    // Truncate to one byte: refused as well.
    std::filesystem::resize_file(path, 1, ec);
    CHECK(!ec);
    CHECK(!LoadWalletFile(path, "correct horse battery staple", loaded,
                         error));
    std::filesystem::remove(path);
  }

  test::BeginSection("m13: crash recovery (store never closed)");
  {
    // Simulate a crash: write blocks to a store, then never close
    // it (the object is leaked on purpose) and reopen the same
    // file. WAL + synchronous=FULL must make the committed data
    // durable, and a garbage file must fail to open cleanly.
    const std::string path = "/tmp/oeeoo-m13-crash.db";
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
    Block genesis = MineGenesis();
    {
      ChainStore* store = new ChainStore();
      std::string error;
      CHECK(store->Open(path, error));
      // ApplyBlock (not PutBlockAtHeight) is the path that saves
      // the tip; the undo record for a genesis is just its coinbase
      // output.
      BlockUndo undo;
      Coin genesisCoin;
      genesisCoin.out = genesis.vtx[0].vout[0];
      genesisCoin.height = 0;
      genesisCoin.blockTime = genesis.header.nTime;
      genesisCoin.isCoinbase = true;
      undo.added.push_back(
          {{genesis.GetHash(), 0}, genesisCoin});
      CHECK(store->ApplyBlock(genesis, Target{}, undo, error));
      // No Close(), no delete: the "crash".
      (void)store;
    }
    {
      ChainStore reopened;
      std::string error;
      CHECK(reopened.Open(path, error));
      Hash256 tip;
      CHECK(reopened.LoadTip(tip, error));
      CHECK(tip == genesis.GetHash());
      std::vector<std::tuple<int, BlockHeader, Target>> headers;
      CHECK(reopened.GetAllHeaders(headers, error));
      CHECK(headers.size() == 1);
    }
    // A garbage file fails to open (not a crash).
    const std::string badPath = "/tmp/oeeoo-m13-garbage.db";
    CHECK(WriteFile(badPath, "this is not a sqlite database at all"));
    ChainStore bad;
    std::string error;
    CHECK(!bad.Open(badPath, error));
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
    std::filesystem::remove(badPath);
  }

  test::BeginSection("m13: genesis identity (file vs store vs checkpoint)");
  {
    Node node(kTestBits);
    node.SetTime(kNow);
    auto transport = std::make_shared<LoopbackTransport>("loopback:m13b");
    node.SetTransport(transport);
    Block genesis = MineGenesis();
    std::string error;
    CHECK(node.BroadcastBlock(genesis, error));
    // The same genesis file matches the stored chain.
    CHECK(CheckGenesisIdentity(node, genesis, true, "", error));
    // A different genesis file is refused.
    Block other = MineGenesis();
    other.header.nTime = kTime + 600;
    other.header.hashMerkleRoot = other.ComputeMerkleRoot();
    CHECK(MineBlock(other, 0, "THINK ABOUT GOD?"));
    CHECK(other.GetHash() != genesis.GetHash());
    CHECK(!CheckGenesisIdentity(node, other, true, "", error));
    // A checkpoint matching the stored genesis passes.
    const std::string hashHex = [&] {
      std::string s;
      for (unsigned char b : genesis.GetHash()) {
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x", b);
        s += buf;
      }
      return s;
    }();
    CHECK(CheckGenesisIdentity(node, genesis, true, hashHex, error));
    // A checkpoint for a different hash fails.
    CHECK(!CheckGenesisIdentity(node, genesis, true,
                                std::string(64, '0'), error));
    // A malformed checkpoint fails.
    CHECK(!CheckGenesisIdentity(node, genesis, true, "xyz", error));
    // A fresh node with no chain: the file's hash is checked
    // against the checkpoint only.
    Node fresh(kTestBits);
    CHECK(CheckGenesisIdentity(fresh, genesis, true, hashHex, error));
    CHECK(!CheckGenesisIdentity(fresh, genesis, true,
                                std::string(64, '0'), error));
  }

  test::BeginSection("m13: peer cap is enforced");
  {
    Node node(kTestBits);
    node.SetMaxPeers(1);
    CHECK(node.MaxPeers() == 1);
    auto t1 = std::make_shared<LoopbackTransport>("loopback:cap1");
    auto t2 = std::make_shared<LoopbackTransport>("loopback:cap2");
    node.SetTransport(t1);
    CHECK(node.ConnectTo("loopback:cap2"));
    CHECK(node.PeerCount() == 1);
    // A second outbound connection past the cap is refused.
    CHECK(!node.ConnectTo("loopback:cap2"));
    CHECK(node.PeerCount() == 1);
    // Inbound: a node at its cap refuses a new peer's bytes.
    Node capped(kTestBits);
    capped.SetMaxPeers(0);
    auto tc = std::make_shared<LoopbackTransport>("loopback:cap3");
    capped.SetTransport(tc);
    capped.Poll(0);  // nothing to do, but must not crash
    CHECK(capped.PeerCount() == 0);
  }

  test::BeginSection("m13: transport close is idempotent and safe");
  {
    // Repeated Close() on both transports, then destruction.
    {
      TcpTransport tcp;
      CHECK(tcp.Listen(0));
      tcp.Close();
      tcp.Close();
    }
    {
      LoopbackTransport loop("loopback:close1");
      loop.Close();
      loop.Close();
    }
    {
      auto t1 = std::make_shared<LoopbackTransport>("loopback:close2");
      auto t2 = std::make_shared<LoopbackTransport>("loopback:close3");
      std::string peerId;
      CHECK(t1->Connect("loopback:close3", peerId));
      t1->Close();
      t2->Close();
    }
  }

  test::BeginSection("m13: SIGHASH_ALL vector");
  {
    // A SIGHASH_ALL signature verifies, and changing the sighash
    // flag (or any signed field) changes the hash, so the same
    // signature does not verify against it.
    Transaction tx;
    tx.version = 1;
    TxIn in;
    in.prevTxHash = TxIdWith(0x41);
    in.prevOutIndex = 0;
    in.sequence = 0xffffffff;
    tx.vin.push_back(in);
    TxOut out;
    out.value = 49 * COIN;
    out.scriptPubKey = Spk();
    tx.vout.push_back(out);
    tx.lockTime = 0;
    Hash256 h;
    CHECK(LegacySighash(tx, 0, Spk(), kSigHashAll, h));
    std::vector<unsigned char> sig = SignHash(h, g_key.privkey);
    CHECK(VerifySignature(g_key.pubkey, sig, h));
    // A different locktime changes the sighash, so the signature
    // must not verify against the new hash.
    Transaction tx2 = tx;
    tx2.lockTime = 1;
    Hash256 h2;
    CHECK(LegacySighash(tx2, 0, Spk(), kSigHashAll, h2));
    CHECK(h2 != h);
    CHECK(!VerifySignature(g_key.pubkey, sig, h2));
    // A different output value also changes the sighash.
    Transaction tx3 = tx;
    tx3.vout[0].value = 48 * COIN;
    Hash256 h3;
    CHECK(LegacySighash(tx3, 0, Spk(), kSigHashAll, h3));
    CHECK(h3 != h);
    CHECK(!VerifySignature(g_key.pubkey, sig, h3));
  }

  test::BeginSection("m13: handshake rejects old versions and self-connections");
  {
    // A bare transport plays the attacker against a real node, so
    // the exact version bytes can be crafted.
    auto victim = std::make_unique<Node>(kTestBits);
    victim->SetTime(kNow);
    victim->SetTransport(
        std::make_shared<LoopbackTransport>("loopback:hs-victim"));
    LoopbackTransport attacker("loopback:hs-attacker");
    std::string attackerPeerId;
    VersionPayload v;
    v.userAgent = "/OeeooCoin:0.1.0/";
    // 1. An old protocol version is refused (peer disconnected).
    CHECK(attacker.Connect("loopback:hs-victim", attackerPeerId));
    v.version = kProtocolVersion - 1;
    v.nonce = 12345;
    CHECK(attacker.Send(attackerPeerId, SerializeMessage(
                                             {"version", SerializeVersion(v)})));
    victim->Poll(0);
    CHECK(victim->PeerCount() == 0);
    // 2. Our own nonce means a self-connection: refused.
    CHECK(attacker.Connect("loopback:hs-victim", attackerPeerId));
    v.version = kProtocolVersion;
    v.nonce = victim->NodeNonce();
    CHECK(attacker.Send(attackerPeerId, SerializeMessage(
                                             {"version", SerializeVersion(v)})));
    victim->Poll(0);
    CHECK(victim->PeerCount() == 0);
    // 3. A current version with a foreign nonce handshakes fine.
    CHECK(attacker.Connect("loopback:hs-victim", attackerPeerId));
    v.nonce = 987654321ULL;
    CHECK(attacker.Send(attackerPeerId, SerializeMessage(
                                             {"version", SerializeVersion(v)})));
    victim->Poll(0);
    CHECK(victim->PeerCount() == 1);
    CHECK(victim->HandshookPeerCount() == 0);  // verack not yet sent
    TransportMessage m;
    CHECK(attacker.Poll(m, 0));  // victim's version
    CHECK(attacker.Poll(m, 0));  // victim's verack
    CHECK(attacker.Send(attackerPeerId,
                        SerializeMessage({"verack", {}})));
    victim->Poll(0);
    CHECK(victim->HandshookPeerCount() == 1);
  }

  test::BeginSection(
      "m13: compact header without PoW is ignored, pending entries expire");
  {
    auto victim = std::make_unique<Node>(kTestBits);
    victim->SetTime(kNow);
    victim->SetTransport(
        std::make_shared<LoopbackTransport>("loopback:cb-victim"));
    LoopbackTransport attacker("loopback:cb-attacker");
    std::string attackerPeerId;
    CHECK(attacker.Connect("loopback:cb-victim", attackerPeerId));
    // Handshake first.
    VersionPayload v;
    v.version = kProtocolVersion;
    v.nonce = 424242ULL;
    CHECK(attacker.Send(attackerPeerId, SerializeMessage(
                                             {"version", SerializeVersion(v)})));
    victim->Poll(0);
    TransportMessage m;
    CHECK(attacker.Poll(m, 0));  // victim's version
    CHECK(attacker.Poll(m, 0));  // victim's verack
    CHECK(attacker.Send(attackerPeerId,
                        SerializeMessage({"verack", {}})));
    victim->Poll(0);
    CHECK(victim->HandshookPeerCount() == 1);
    // Drain everything the victim sent and report whether any of
    // it was a message with the given command. (The victim also
    // sends periodic pings, so the queue is never assumed empty.)
    auto gotCommand = [&](const std::string& command) {
      TransportMessage m;
      bool found = false;
      while (attacker.Poll(m, 0)) {
        Message msg;
        std::size_t consumed = 0;
        if (ParseMessage(m.bytes, 0, msg, consumed) &&
            msg.command == command) {
          found = true;
        }
      }
      return found;
    };
    // 1. A compact block whose header fails proof of work gets no
    // getblocktxn: the peer cannot make this node fetch
    // transactions for a fake header.
    Block fake;
    fake.header.nVersion = 1;
    fake.header.nTime = kNow;
    fake.header.nBits = kTestBits;
    fake.header.nNonce = 0;
    fake.vtx = {CreateCoinbase(1, Spk(), 50 * COIN)};
    fake.header.hashMerkleRoot = fake.ComputeMerkleRoot();
    // At test difficulty about half of all hashes pass, so hunt
    // for a nonce that really fails (a couple of tries at most).
    while (CheckProofOfWork(fake.GetHash(), fake.header.nBits)) {
      ++fake.header.nNonce;
    }
    CompactBlock cb = MakeCompactBlock(fake, 0);
    CHECK(attacker.Send(attackerPeerId,
                        SerializeMessage(
                            {"cmpctblock", SerializeCompactBlock(cb)})));
    victim->Poll(0);
    CHECK(!gotCommand("getblocktxn"));
    // 2. A real (mined) compact block whose non-coinbase
    // transaction the victim does not have triggers getblocktxn,
    // then the pending entry expires after 120s without an answer.
    // (A block with only a coinbase would reconstruct from the
    // prefilled transaction alone, so the block needs a second tx.)
    Block real = MineGenesis();  // mined at test bits; valid PoW
    Transaction spend = Spend(real.GetHash(), 49 * COIN);
    Block block1;
    block1.header.nVersion = 1;
    block1.header.hashPrevBlock = real.GetHash();
    block1.header.nTime = kTime + 600;
    block1.header.nBits = kTestBits;
    block1.vtx = {CreateCoinbase(1, Spk(), 50 * COIN), spend};
    block1.header.hashMerkleRoot = block1.ComputeMerkleRoot();
    CHECK(MineBlock(block1, 1, "THINK ABOUT GOD?"));
    CompactBlock cb2 = MakeCompactBlock(block1, 0);
    CHECK(attacker.Send(attackerPeerId,
                        SerializeMessage(
                            {"cmpctblock", SerializeCompactBlock(cb2)})));
    victim->Poll(0);
    CHECK(gotCommand("getblocktxn"));  // getblocktxn arrived
    // No answer: advance the clock past the 120s timeout and poll.
    victim->SetTime(kNow + 121);
    victim->Poll(0);
    // A late blocktxn for that hash is now ignored: no getdata
    // fallback is sent (the pending entry is gone).
    CHECK(attacker.Send(attackerPeerId,
                        SerializeMessage({"blocktxn",
                                          SerializeBlockTxn(block1.GetHash(),
                                                            {})})));
    victim->Poll(0);
    CHECK(!gotCommand("getdata"));
  }

  test::BeginSection("m13: addrman cap, IP validation, addr serialization");
  {
    AddressManager am;
    CHECK(!am.Add({1700000000, 1, "not-an-ip", 64775}));
    CHECK(!am.Add({1700000000, 1, "example.com", 64775}));
    CHECK(!am.Add({1700000000, 1, "1.2.3", 64775}));
    CHECK(!am.Add({1700000000, 1, "1.2.3.256", 64775}));
    CHECK(am.Add({1700000000, 1, "1.2.3.4", 64775}));
    // Fill to the cap: further adds are refused.
    for (int i = 0; i < 2000; ++i) {
      std::string ip = "10.0." + std::to_string(i / 256) + "." +
                       std::to_string(i % 256);
      am.Add({1700000000, 1, ip, 64775});
    }
    CHECK(am.Size() == AddressManager::kMaxAddresses);
    CHECK(!am.Add({1700000000, 1, "9.9.9.9", 64775}));
    // SerializeAddr counts only the entries actually written: an
    // unparseable entry must not inflate the count.
    std::vector<AddrEntry> entries = {{1700000000, 1, "1.2.3.4", 64775},
                                      {1700000000, 1, "bogus", 64775},
                                      {1700000001, 1, "5.6.7.8", 64775}};
    std::vector<AddrEntry> parsed;
    CHECK(ParseAddr(SerializeAddr(entries), parsed));
    CHECK(parsed.size() == 2);
    CHECK(parsed[0].ip == "1.2.3.4" && parsed[1].ip == "5.6.7.8");
    // ParseAddr rejects a non-IPv4-mapped address field: corrupt
    // the first entry's address prefix (varint count is 1 byte,
    // then time(4) + services(8) precede the 16 address bytes).
    std::vector<unsigned char> bad = SerializeAddr(entries);
    bad[1 + 4 + 8 + 10] = 0x00;
    CHECK(!ParseAddr(bad, parsed));
  }

  test::BeginSection("m13: loopback close purges peer links");
  {
    auto t1 = std::make_shared<LoopbackTransport>("loopback:purge1");
    auto t2 = std::make_shared<LoopbackTransport>("loopback:purge2");
    std::string peerId;
    CHECK(t1->Connect("loopback:purge2", peerId));
    // t1 sends one message so t2 learns its own peer id for the link.
    CHECK(t1->Send(peerId, {9}));
    TransportMessage m;
    CHECK(t2->Poll(m, 0));
    const std::string t2PeerId = m.peerId;
    CHECK(t2->Send(t2PeerId, {1, 2, 3}));  // works before close
    // t1 closes: t2 must no longer be able to reach it, and the
    // link must be gone (no dangling pointer if t1 is destroyed).
    t1->Close();
    CHECK(!t2->Send(t2PeerId, {1, 2, 3}));
    // A closed transport is unreachable for new connections too.
    std::string peerId2;
    CHECK(!t2->Connect("loopback:purge1", peerId2));
    // Destroying t1 after Close() is safe, and t2 still works.
    t1.reset();
    CHECK(!t2->Send(t2PeerId, {1, 2, 3}));
    // t2 can still connect elsewhere after the purge.
    auto t3 = std::make_shared<LoopbackTransport>("loopback:purge3");
    CHECK(t2->Connect("loopback:purge3", peerId2));
    // Destroying a transport WITHOUT Close() must also purge the
    // peer's link: the old code left a dangling pointer here.
    {
      auto a = std::make_shared<LoopbackTransport>("loopback:uaf1");
      auto b = std::make_shared<LoopbackTransport>("loopback:uaf2");
      std::string pid;
      CHECK(a->Connect("loopback:uaf2", pid));
      CHECK(a->Send(pid, {9}));
      TransportMessage mm;
      CHECK(b->Poll(mm, 0));
      const std::string bPid = mm.peerId;
      CHECK(b->Send(bPid, {1}));
      a.reset();  // destroyed without Close()
      CHECK(!b->Send(bPid, {1}));  // would have been use-after-free
    }
  }

  test::BeginSection("m13: tcp connect rejects malformed addresses");
  {
    TcpTransport tcp;
    std::string peerId;
    CHECK(!tcp.Connect("no-colon-here", peerId));
    CHECK(!tcp.Connect("127.0.0.1:notaport", peerId));
    CHECK(!tcp.Connect("127.0.0.1:99999", peerId));
    CHECK(!tcp.Connect("127.0.0.1:0", peerId));
    CHECK(!tcp.Connect("127.0.0.1:-1", peerId));
    CHECK(!tcp.Connect("256.256.256.256:64775", peerId));
  }
}

#pragma GCC diagnostic pop
