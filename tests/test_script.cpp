// Tests: Script interpreter (Milestone 4) — P2PKH, multisig, CLTV,
// CSV, opcode limits and rejections. All signatures are real ECDSA
// (secp256k1) produced with the project's own signing code.
#include <string>
#include <vector>

#include "crypto/ecdsa.h"
#include "script/script.h"
#include "test_harness.h"
#include "transactions/sighash.h"

using namespace oeeoo;

namespace {

// Sign input nIn of tx against scriptPubKey with key kp; returns the
// signature with the SIGHASH_ALL byte appended.
std::vector<unsigned char> SignInput(const Transaction& tx, std::size_t nIn,
                                     const std::vector<unsigned char>& spk,
                                     const KeyPair& kp) {
  Hash256 h;
  CHECK(LegacySighash(tx, nIn, spk, kSigHashAll, h));
  std::vector<unsigned char> sig = SignHash(h, kp.privkey);
  CHECK(!sig.empty());
  sig.push_back(static_cast<unsigned char>(kSigHashAll));
  return sig;
}

// A spending transaction with one input (prev txid `prev`, output 0)
// and one P2PKH output. The caller fills scriptSig after signing.
Transaction SpendTx(const Hash256& prev, const Hash160& destHash,
                    Amount value, uint32_t lockTime = 0, int32_t version = 1,
                    uint32_t sequence = 0xffffffff) {
  Transaction tx;
  tx.version = version;
  TxIn in;
  in.prevTxHash = prev;
  in.prevOutIndex = 0;
  in.sequence = sequence;
  tx.vin.push_back(in);
  TxOut out;
  out.value = value;
  out.scriptPubKey = CreateP2PKHScriptPubKey(destHash);
  tx.vout.push_back(out);
  tx.lockTime = lockTime;
  return tx;
}

bool VerifyOk(const std::vector<unsigned char>& sig,
              const std::vector<unsigned char>& spk, const Transaction& tx,
              std::size_t nIn = 0) {
  std::string error;
  bool ok = VerifyScript(sig, spk, tx, nIn, error);
  if (!ok) std::printf("  unexpected script failure: %s\n", error.c_str());
  return ok;
}

bool VerifyFails(const std::vector<unsigned char>& sig,
                 const std::vector<unsigned char>& spk,
                 const Transaction& tx, std::size_t nIn = 0) {
  std::string error;
  bool ok = VerifyScript(sig, spk, tx, nIn, error);
  CHECK(!ok);
  CHECK(!error.empty());
  return !ok;
}

std::vector<unsigned char> ScriptNum(int64_t v) {
  // Minimal little-endian signed-magnitude encoding.
  if (v == 0) return {};
  std::vector<unsigned char> out;
  bool negative = v < 0;
  uint64_t mag = negative ? static_cast<uint64_t>(-v) : static_cast<uint64_t>(v);
  while (mag > 0) {
    out.push_back(static_cast<unsigned char>(mag & 0xff));
    mag >>= 8;
  }
  if (out.back() & 0x80) out.push_back(negative ? 0x80 : 0x00);
  else if (negative) out.back() |= 0x80;
  return out;
}
}  // namespace

void TestScript() {
  KeyPair alice, bob, carol, dave;
  CHECK(GenerateKeyPair(alice));
  CHECK(GenerateKeyPair(bob));
  CHECK(GenerateKeyPair(carol));
  CHECK(GenerateKeyPair(dave));
  const Hash160 aliceHash = PubKeyHash160(alice.pubkey);
  const std::vector<unsigned char> aliceSpk =
      CreateP2PKHScriptPubKey(aliceHash);
  Hash256 prevTx{};
  prevTx.fill(0x11);

  test::BeginSection("script: valid P2PKH spend");
  {
    Transaction tx = SpendTx(prevTx, aliceHash, 49 * COIN);
    std::vector<unsigned char> sig = SignInput(tx, 0, aliceSpk, alice);
    std::vector<unsigned char> scriptSig =
        CreateP2PKHScriptSig(sig, alice.pubkey);
    CHECK(VerifyOk(scriptSig, aliceSpk, tx));
  }

  test::BeginSection("script: P2PKH failures");
  {
    // Wrong key signs.
    Transaction tx = SpendTx(prevTx, aliceHash, 49 * COIN);
    std::vector<unsigned char> sig = SignInput(tx, 0, aliceSpk, bob);
    CHECK(VerifyFails(CreateP2PKHScriptSig(sig, bob.pubkey), aliceSpk, tx));
    // Alice signs, but Bob's pubkey is presented (hash mismatch).
    std::vector<unsigned char> sigA = SignInput(tx, 0, aliceSpk, alice);
    CHECK(VerifyFails(CreateP2PKHScriptSig(sigA, bob.pubkey), aliceSpk, tx));
    // Signature for a different transaction (tampered output value).
    Transaction tx2 = SpendTx(prevTx, aliceHash, 48 * COIN);
    std::vector<unsigned char> sig2 = SignInput(tx2, 0, aliceSpk, alice);
    Transaction txTampered = SpendTx(prevTx, aliceHash, 49 * COIN);
    CHECK(VerifyFails(CreateP2PKHScriptSig(sig2, alice.pubkey), aliceSpk,
                      txTampered));
    // Empty signature.
    std::vector<unsigned char> emptySig;
    CHECK(VerifyFails(CreateP2PKHScriptSig(emptySig, alice.pubkey), aliceSpk,
                      tx));
    // Unsupported sighash type (2 = SIGHASH_NONE, not implemented).
    std::vector<unsigned char> sigBadType = sigA;
    sigBadType.back() = 0x02;
    CHECK(VerifyFails(CreateP2PKHScriptSig(sigBadType, alice.pubkey),
                      aliceSpk, tx));
    // scriptSig with a non-push opcode.
    std::vector<unsigned char> badSig = {OP_DUP};
    CHECK(VerifyFails(badSig, aliceSpk, tx));
  }

  test::BeginSection("script: multisig 2-of-3");
  {
    std::vector<std::vector<unsigned char>> keys = {alice.pubkey, bob.pubkey,
                                                    carol.pubkey};
    std::vector<unsigned char> msSpk = CreateMultisigScriptPubKey(2, keys);
    Transaction tx = SpendTx(prevTx, aliceHash, 49 * COIN);
    std::vector<unsigned char> sigA = SignInput(tx, 0, msSpk, alice);
    std::vector<unsigned char> sigC = SignInput(tx, 0, msSpk, carol);
    // Sigs in pubkey order (Alice then Carol).
    CHECK(VerifyOk(CreateMultisigScriptSig({sigA, sigC}), msSpk, tx));
    // Only one signature.
    CHECK(VerifyFails(CreateMultisigScriptSig({sigA}), msSpk, tx));
    // Wrong order (Carol then Alice).
    CHECK(VerifyFails(CreateMultisigScriptSig({sigC, sigA}), msSpk, tx));
    // Dummy element must be empty.
    std::vector<unsigned char> badDummy = {0x01, 0x01};
    std::vector<unsigned char> s1 = PushData(sigA);
    badDummy.insert(badDummy.end(), s1.begin(), s1.end());
    std::vector<unsigned char> s2 = PushData(sigC);
    badDummy.insert(badDummy.end(), s2.begin(), s2.end());
    CHECK(VerifyFails(badDummy, msSpk, tx));
    // One bad signature among the two.
    std::vector<unsigned char> sigDave = SignInput(tx, 0, msSpk, dave);
    CHECK(VerifyFails(CreateMultisigScriptSig({sigA, sigDave}), msSpk, tx));
  }

  test::BeginSection("script: CLTV timelock");
  {
    auto cltvSpk = [&](int64_t lockValue) {
      std::vector<unsigned char> spk = PushData(ScriptNum(lockValue));
      spk.push_back(OP_CHECKLOCKTIMEVERIFY);
      spk.push_back(OP_DROP);
      std::vector<unsigned char> p2pkh = aliceSpk;
      spk.insert(spk.end(), p2pkh.begin(), p2pkh.end());
      return spk;
    };
    std::vector<unsigned char> spk = cltvSpk(1000);
    // lockTime reached, sequence not final: OK.
    Transaction tx = SpendTx(prevTx, aliceHash, 49 * COIN, 1000, 1, 0);
    std::vector<unsigned char> sig = SignInput(tx, 0, spk, alice);
    CHECK(VerifyOk(CreateP2PKHScriptSig(sig, alice.pubkey), spk, tx));
    // lockTime not reached.
    Transaction early = SpendTx(prevTx, aliceHash, 49 * COIN, 999, 1, 0);
    std::vector<unsigned char> sigEarly =
        SignInput(early, 0, spk, alice);
    CHECK(VerifyFails(CreateP2PKHScriptSig(sigEarly, alice.pubkey), spk,
                      early));
    // Sequence final: CLTV must fail even with lockTime reached.
    Transaction finalSeq =
        SpendTx(prevTx, aliceHash, 49 * COIN, 1000, 1, 0xffffffff);
    std::vector<unsigned char> sigF = SignInput(finalSeq, 0, spk, alice);
    CHECK(VerifyFails(CreateP2PKHScriptSig(sigF, alice.pubkey), spk,
                      finalSeq));
    // Type mismatch: script value is a height, tx lockTime is a time.
    Transaction timeTx =
        SpendTx(prevTx, aliceHash, 49 * COIN, 1700000000, 1, 0);
    std::vector<unsigned char> sigT = SignInput(timeTx, 0, spk, alice);
    CHECK(VerifyFails(CreateP2PKHScriptSig(sigT, alice.pubkey), spk, timeTx));
  }

  test::BeginSection("script: CSV relative lock");
  {
    auto csvSpk = [&](int64_t seqValue) {
      std::vector<unsigned char> spk = PushData(ScriptNum(seqValue));
      spk.push_back(OP_CHECKSEQUENCEVERIFY);
      spk.push_back(OP_DROP);
      std::vector<unsigned char> p2pkh = aliceSpk;
      spk.insert(spk.end(), p2pkh.begin(), p2pkh.end());
      return spk;
    };
    std::vector<unsigned char> spk = csvSpk(10);
    // Version 2, sequence 10: OK.
    Transaction tx = SpendTx(prevTx, aliceHash, 49 * COIN, 0, 2, 10);
    std::vector<unsigned char> sig = SignInput(tx, 0, spk, alice);
    CHECK(VerifyOk(CreateP2PKHScriptSig(sig, alice.pubkey), spk, tx));
    // Sequence 9: not reached.
    Transaction early = SpendTx(prevTx, aliceHash, 49 * COIN, 0, 2, 9);
    std::vector<unsigned char> sigE = SignInput(early, 0, spk, alice);
    CHECK(VerifyFails(CreateP2PKHScriptSig(sigE, alice.pubkey), spk, early));
    // Version 1: CSV requires version >= 2.
    Transaction v1 = SpendTx(prevTx, aliceHash, 49 * COIN, 0, 1, 10);
    std::vector<unsigned char> sigV1 = SignInput(v1, 0, spk, alice);
    CHECK(VerifyFails(CreateP2PKHScriptSig(sigV1, alice.pubkey), spk, v1));
    // Disable flag set: CSV is a no-op, passes.
    Transaction disabled =
        SpendTx(prevTx, aliceHash, 49 * COIN, 0, 2, 0x80000000u);
    std::vector<unsigned char> sigD = SignInput(disabled, 0, spk, alice);
    CHECK(VerifyOk(CreateP2PKHScriptSig(sigD, alice.pubkey), spk, disabled));
  }

  test::BeginSection("script: limits and rejections");
  {
    Transaction tx = SpendTx(prevTx, aliceHash, 49 * COIN);
    // OP_RETURN is disabled.
    std::vector<unsigned char> retSpk = {OP_RETURN};
    CHECK(VerifyFails({}, retSpk, tx));
    // Unknown opcode OP_ADD.
    std::vector<unsigned char> addSpk = {0x93};
    CHECK(VerifyFails({}, addSpk, tx));
    // 202 counted opcodes: too many.
    std::vector<unsigned char> manyOps(202, OP_NOP);
    manyOps.push_back(OP_TRUE);
    CHECK(VerifyFails({}, manyOps, tx));
    // 201 counted opcodes: fine.
    std::vector<unsigned char> okOps(201, OP_NOP);
    okOps.push_back(OP_TRUE);
    CHECK(VerifyOk({}, okOps, tx));
    // Oversize element (521 bytes).
    std::vector<unsigned char> bigPush = PushData(std::vector<unsigned char>(521, 0x01));
    std::vector<unsigned char> bigSpk = bigPush;
    bigSpk.push_back(OP_DROP);
    bigSpk.push_back(OP_TRUE);
    CHECK(VerifyFails({}, bigSpk, tx));
    // Stack overflow: 1001 pushes.
    std::vector<unsigned char> stackBomb;
    for (int i = 0; i < 1001; ++i) stackBomb.push_back(OP_0);
    stackBomb.push_back(OP_TRUE);
    CHECK(VerifyFails({}, stackBomb, tx));
    // OP_EQUALVERIFY mismatch: OP_1 vs OP_2 are not equal.
    std::vector<unsigned char> eqBad = {OP_1};
    eqBad.push_back(OP_1 + 1);  // OP_2
    eqBad.push_back(OP_EQUALVERIFY);
    eqBad.push_back(OP_TRUE);
    CHECK(VerifyFails({}, eqBad, tx));
  }
}
