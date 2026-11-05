// Tests: transaction serialization against an independently computed
// vector (Python, struct-based), txid, round-trip, and structural
// validation (CheckBasic).
#include <string>

#include "consensus/consensus.h"
#include "test_harness.h"
#include "transactions/transaction.h"

using namespace oeeoo;

namespace {
std::vector<unsigned char> FromHex(const std::string& hex) {
  std::vector<unsigned char> out;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    const auto nib = [](char c) -> unsigned char {
      if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
      if (c >= 'a' && c <= 'f') return static_cast<unsigned char>(c - 'a' + 10);
      return static_cast<unsigned char>(c - 'A' + 10);
    };
    out.push_back(static_cast<unsigned char>((nib(hex[i]) << 4) | nib(hex[i + 1])));
  }
  return out;
}

std::string ToHex(const std::vector<unsigned char>& v) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string s;
  for (unsigned char b : v) {
    s.push_back(kHex[b >> 4]);
    s.push_back(kHex[b & 0x0f]);
  }
  return s;
}

// The reference transaction (built independently in Python):
// version 1, 1 input (prev hash all zero, index 0xffffffff,
// scriptSig 04ffff001d0101, sequence 0xffffffff), 1 output
// (50 OEEOO to P2PKH hash160 0x11*20), lockTime 0.
const char* kTxHex =
    "0100000001000000000000000000000000000000000000000000000000000000"
    "0000000000ffffffff0704ffff001d0101ffffffff0100f2052a010000001976a9"
    "14111111111111111111111111111111111111111188ac00000000";
const char* kTxId =
    "9ac2b1bac217553b606a716a3917d3a65b25b4ea2f08ccc1bc2534a5736be8c5";

Transaction MakeRefTx() {
  Transaction tx;
  tx.version = 1;
  TxIn in;
  in.prevTxHash.fill(0);
  in.prevOutIndex = 0xffffffff;
  in.scriptSig = FromHex("04ffff001d0101");
  in.sequence = 0xffffffff;
  tx.vin.push_back(in);
  TxOut out;
  out.value = 50 * COIN;
  out.scriptPubKey = FromHex("76a9141111111111111111111111111111111111"
                             "11111188ac");
  tx.vout.push_back(out);
  tx.lockTime = 0;
  return tx;
}
}  // namespace

void TestTransaction() {
  test::BeginSection("transaction serialization + txid");
  const Transaction tx = MakeRefTx();
  const std::vector<unsigned char> ser = tx.Serialize();
  CHECK(ser.size() == 92);
  CHECK(ToHex(ser) == kTxHex);
  CHECK(tx.GetTxIdHex() == kTxId);
  CHECK(tx.CheckBasic());

  test::BeginSection("transaction deserialize round-trip + rejections");
  Transaction back;
  CHECK(Transaction::Deserialize(ser, back));
  CHECK(back.version == 1);
  CHECK(back.vin.size() == 1);
  CHECK(back.vout.size() == 1);
  CHECK(back.vout[0].value == 50 * COIN);
  CHECK(back.GetTxIdHex() == kTxId);
  CHECK(back.Serialize() == ser);

  // Truncated input rejected.
  Transaction bad;
  CHECK(!Transaction::Deserialize(
      std::vector<unsigned char>(ser.begin(), ser.end() - 1), bad));
  CHECK(!Transaction::Deserialize(
      std::vector<unsigned char>(ser.begin(), ser.begin() + 10), bad));
  // Trailing bytes rejected.
  std::vector<unsigned char> trailing = ser;
  trailing.push_back(0x00);
  CHECK(!Transaction::Deserialize(trailing, bad));
  // Empty input rejected.
  CHECK(!Transaction::Deserialize({}, bad));

  test::BeginSection("transaction CheckBasic rules");
  Transaction t2 = MakeRefTx();
  t2.vout[0].value = -1;
  CHECK(!t2.CheckBasic());  // negative output
  t2 = MakeRefTx();
  t2.vout[0].value = MAX_MONEY + 1;
  CHECK(!t2.CheckBasic());  // output above max supply
  t2 = MakeRefTx();
  t2.vout.push_back(t2.vout[0]);
  t2.vout[1].value = MAX_MONEY;
  CHECK(!t2.CheckBasic());  // outputs sum above max supply
  t2 = MakeRefTx();
  t2.vin.push_back(t2.vin[0]);
  CHECK(!t2.CheckBasic());  // duplicate input
  t2 = MakeRefTx();
  t2.vin.clear();
  CHECK(!t2.CheckBasic());  // no inputs
  t2 = MakeRefTx();
  t2.vout.clear();
  CHECK(!t2.CheckBasic());  // no outputs
}
