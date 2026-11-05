// Tests: full block (Milestone 3) and chain index — block structure,
// Merkle/size/duplicate checks, chain validation (parent, height,
// Median-Time-Past, future-time, difficulty, proof of work), chain
// selection by greatest cumulative work (reorg), and the 2,016-block
// difficulty retarget at height 2016.
#include <string>
#include <vector>

#include "blockchain/chain.h"
#include "blockchain/genesis.h"
#include "consensus/consensus.h"
#include "test_harness.h"
#include "transactions/transaction.h"

using namespace oeeoo;

namespace {
constexpr uint32_t kTestBits = 0x207fffff;  // easy test-only difficulty
constexpr uint32_t kNow = 1800000000;      // fixed "current time" for tests
constexpr uint32_t kGenesisTime = 1700000000;

// Distinct transactions: the Milestone 2 reference tx with a tag byte
// in scriptSig, so each has a different txid.
Transaction MakeTx(uint8_t tag) {
  Transaction tx;
  tx.version = 1;
  TxIn in;
  in.prevTxHash.fill(0);
  in.prevOutIndex = 0xffffffff;
  in.scriptSig = {0x04, 0xff, 0xff, 0x00, 0x1d, 0x01, tag};
  in.sequence = 0xffffffff;
  tx.vin.push_back(in);
  TxOut out;
  out.value = 50 * COIN;
  out.scriptPubKey = {0x76, 0xa9, 0x14, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                      0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                      0x11, 0x11, 0x11, 0x11, 0x88, 0xac};
  tx.vout.push_back(out);
  tx.lockTime = 0;
  return tx;
}

// Build a block on `prev` at `time` with `bits`, mine a nonce that
// satisfies proof of work (easy at kTestBits: a few tries at most).
Block MineBlock(const Hash256& prev, uint32_t time, uint32_t bits,
                std::vector<Transaction> txs) {
  Block block;
  block.header.nVersion = 1;
  block.header.hashPrevBlock = prev;
  block.header.nTime = time;
  block.header.nBits = bits;
  block.vtx = std::move(txs);
  block.header.hashMerkleRoot = block.ComputeMerkleRoot();
  for (uint32_t nonce = 0; nonce < 100000; ++nonce) {
    block.header.nNonce = nonce;
    if (CheckProofOfWork(block.GetHash(), bits)) return block;
  }
  CHECK(false);  // mining failed — should never happen at kTestBits
  return block;
}

bool AddOk(ChainState& chain, const Block& block) {
  std::string error;
  bool ok = chain.AddBlock(block, kNow, error);
  if (!ok) {
    std::printf("  unexpected AddBlock failure: %s\n", error.c_str());
  }
  return ok;
}

bool AddFails(ChainState& chain, const Block& block) {
  std::string error;
  bool ok = chain.AddBlock(block, kNow, error);
  CHECK(!ok);
  CHECK(!error.empty());
  return !ok;
}
}  // namespace

void TestChain() {
  test::BeginSection("block structure: serialize round-trip + CheckBasic");
  {
    Block block = MineBlock(Hash256{}, kGenesisTime, kTestBits,
                            {MakeTx(1), MakeTx(2)});
    std::vector<unsigned char> bytes = block.Serialize();
    Block decoded;
    CHECK(Block::Deserialize(bytes, decoded));
    CHECK(decoded.GetHash() == block.GetHash());
    CHECK(decoded.vtx.size() == 2);
    std::string error;
    CHECK(block.CheckBasic(error));

    // Merkle tamper: change a tx after the root was set.
    Block tampered = block;
    tampered.vtx[0].lockTime = 1;
    CHECK(!tampered.CheckBasic(error));
    // Duplicate txid.
    Block dup = block;
    dup.vtx.push_back(dup.vtx[0]);
    dup.header.hashMerkleRoot = dup.ComputeMerkleRoot();
    CHECK(!dup.CheckBasic(error));
    // Empty block.
    Block empty;
    empty.header.nVersion = 1;
    CHECK(!empty.CheckBasic(error));
    // Oversize block (1 MB+ scriptSig).
    Block big = block;
    big.vtx[0].vin[0].scriptSig.assign(MAX_BLOCK_SIZE + 1, 0x00);
    CHECK(!big.CheckBasic(error));
  }

  test::BeginSection("chain: genesis + 5 blocks, tip + work");
  ChainState chain(kTestBits);
  Block genesis = MineBlock(Hash256{}, kGenesisTime, kTestBits, {MakeTx(0)});
  CHECK(AddOk(chain, genesis));
  CHECK(chain.GetTipHeight() == 0);
  // Genesis work = proof(0x207fffff) = 2.
  CHECK(chain.GetTipWorkHex() ==
        std::string("00000000000000000000000000000000000000000000000000000000"
                    "00000002"));
  Hash256 tip = genesis.GetHash();
  for (int i = 1; i <= 5; ++i) {
    Block b = MineBlock(tip, kGenesisTime + 600 * i, kTestBits,
                        {MakeTx(static_cast<uint8_t>(i))});
    CHECK(AddOk(chain, b));
    tip = b.GetHash();
  }
  CHECK(chain.GetTipHeight() == 5);
  CHECK(chain.GetTipHash() == tip);
  // 6 blocks x proof 2 = 12 work.
  CHECK(chain.GetTipWorkHex() ==
        std::string("00000000000000000000000000000000000000000000000000000000"
                    "0000000c"));
  CHECK(chain.GetHeight(tip) == 5);
  CHECK(chain.GetMedianTimePast() == kGenesisTime + 600 * 3);

  test::BeginSection("chain: reorg — heavier fork becomes tip");
  {
    // Fork from height 2: rebuild the branch with 4 blocks (heights
    // 3,4,5,6) so its work (7 blocks x 2 = 14) beats the main tip (12).
    // First recover the height-2 hash: rebuild the same chain prefix.
    ChainState forkChain(kTestBits);
    Block g = MineBlock(Hash256{}, kGenesisTime, kTestBits, {MakeTx(0)});
    CHECK(AddOk(forkChain, g));
    Hash256 h = g.GetHash();
    for (int i = 1; i <= 2; ++i) {
      Block b = MineBlock(h, kGenesisTime + 600 * i, kTestBits,
                          {MakeTx(static_cast<uint8_t>(i))});
      CHECK(AddOk(forkChain, b));
      h = b.GetHash();
    }
    const Hash256 forkPoint = h;
    // Fork branch: distinct txs, same times as main chain + 1s offset
    // (still after MTP, before now+2h).
    for (int i = 3; i <= 6; ++i) {
      Block b = MineBlock(h, kGenesisTime + 600 * i + 1, kTestBits,
                          {MakeTx(static_cast<uint8_t>(100 + i))});
      CHECK(AddOk(forkChain, b));
      h = b.GetHash();
    }
    CHECK(forkChain.GetTipHeight() == 6);
    // Apply the same fork blocks to the original chain: tip must move.
    ChainState mainChain(kTestBits);
    CHECK(AddOk(mainChain, genesis));
    Hash256 mh = genesis.GetHash();
    for (int i = 1; i <= 5; ++i) {
      Block b = MineBlock(mh, kGenesisTime + 600 * i, kTestBits,
                          {MakeTx(static_cast<uint8_t>(i))});
      CHECK(AddOk(mainChain, b));
      mh = b.GetHash();
    }
    CHECK(mainChain.GetTipHeight() == 5);
    // Replay fork blocks (heights 3..6) onto the main chain.
    Hash256 fh = forkPoint;
    for (int i = 3; i <= 6; ++i) {
      Block b = MineBlock(fh, kGenesisTime + 600 * i + 1, kTestBits,
                          {MakeTx(static_cast<uint8_t>(100 + i))});
      CHECK(AddOk(mainChain, b));
      fh = b.GetHash();
    }
    CHECK(mainChain.GetTipHeight() == 6);
    CHECK(mainChain.GetTipHash() == fh);
    CHECK(mainChain.GetTipWorkHex() ==
          std::string("0000000000000000000000000000000000000000000000000000"
                      "00000000000e"));  // 7 blocks x 2 = 14
  }

  test::BeginSection("chain: invalid blocks rejected");
  {
    ChainState c(kTestBits);
    Block g = MineBlock(Hash256{}, kGenesisTime, kTestBits, {MakeTx(0)});
    std::string error;
    // Genesis with zero timestamp.
    Block badGenesis = g;
    badGenesis.header.nTime = 0;
    badGenesis.header.nNonce = 0;
    CHECK(!c.AddBlock(badGenesis, kNow, error));
    // Genesis with non-zero prev hash.
    Block badPrev = g;
    badPrev.header.hashPrevBlock.fill(0x11);
    CHECK(!c.AddBlock(badPrev, kNow, error));
    CHECK(AddOk(c, g));
    // Duplicate block.
    AddFails(c, g);
    // Unknown parent.
    Block orphan = MineBlock(Hash256{0x99}, kGenesisTime + 600, kTestBits,
                             {MakeTx(9)});
    AddFails(c, orphan);
    // Wrong difficulty bits.
    Block wrongBits = MineBlock(g.GetHash(), kGenesisTime + 600,
                                0x207ffffe, {MakeTx(9)});
    AddFails(c, wrongBits);
    // Proof of work: correct bits, but a nonce whose hash fails.
    Block badPow;
    badPow.header.nVersion = 1;
    badPow.header.hashPrevBlock = g.GetHash();
    badPow.header.nTime = kGenesisTime + 600;
    badPow.header.nBits = kTestBits;
    badPow.vtx = {MakeTx(9)};
    badPow.header.hashMerkleRoot = badPow.ComputeMerkleRoot();
    bool foundFailing = false;
    for (uint32_t nonce = 0; nonce < 1000; ++nonce) {
      badPow.header.nNonce = nonce;
      if (!CheckProofOfWork(badPow.GetHash(), kTestBits)) {
        foundFailing = true;
        break;
      }
    }
    CHECK(foundFailing);
    AddFails(c, badPow);
    // Timestamp too far in the future.
    Block future = MineBlock(g.GetHash(), kNow + 2 * 60 * 60 + 1, kTestBits,
                             {MakeTx(9)});
    AddFails(c, future);
  }

  test::BeginSection("chain: median-time-past rule");
  {
    ChainState c(kTestBits);
    Block g = MineBlock(Hash256{}, kGenesisTime, kTestBits, {MakeTx(0)});
    CHECK(AddOk(c, g));
    Hash256 h = g.GetHash();
    for (int i = 1; i <= 10; ++i) {
      Block b = MineBlock(h, kGenesisTime + 600 * i, kTestBits,
                          {MakeTx(static_cast<uint8_t>(i))});
      CHECK(AddOk(c, b));
      h = b.GetHash();
    }
    // Median of last 11 times (T..T+6000) is T+3000. A block at T+3000
    // must be rejected; T+3001 accepted.
    Block atMtp = MineBlock(h, kGenesisTime + 3000, kTestBits, {MakeTx(50)});
    AddFails(c, atMtp);
    Block afterMtp =
        MineBlock(h, kGenesisTime + 3001, kTestBits, {MakeTx(50)});
    CHECK(AddOk(c, afterMtp));
  }

  test::BeginSection("chain: retarget at height 2016");
  {
    // Exact timespan: 2016 blocks spanning exactly 1,209,600s, so the
    // bits at height 2016 must stay 0x207fffff.
    ChainState c(kTestBits);
    Block g = MineBlock(Hash256{}, kGenesisTime, kTestBits, {MakeTx(0)});
    CHECK(AddOk(c, g));
    Hash256 h = g.GetHash();
    for (int i = 1; i <= 2015; ++i) {
      uint32_t t = (i == 2015) ? kGenesisTime + 1209600
                               : kGenesisTime + 600 * i;
      Block b = MineBlock(h, t, kTestBits,
                          {MakeTx(static_cast<uint8_t>(i & 0xff))});
      if (!AddOk(c, b)) break;
      h = b.GetHash();
    }
    CHECK(c.GetTipHeight() == 2015);
    Block b2016 = MineBlock(h, kGenesisTime + 1209600 + 600, kTestBits,
                            {MakeTx(201)});
    CHECK(AddOk(c, b2016));
    CHECK(c.GetTipHeight() == 2016);
  }
  {
    // Fast chain: +300s per block -> timespan 604,500s -> bits at
    // height 2016 must become 0x203ff7de (computed independently).
    ChainState c(kTestBits);
    Block g = MineBlock(Hash256{}, kGenesisTime, kTestBits, {MakeTx(0)});
    CHECK(AddOk(c, g));
    Hash256 h = g.GetHash();
    for (int i = 1; i <= 2015; ++i) {
      Block b = MineBlock(h, kGenesisTime + 300 * i, kTestBits,
                          {MakeTx(static_cast<uint8_t>(i & 0xff))});
      if (!AddOk(c, b)) break;
      h = b.GetHash();
    }
    CHECK(c.GetTipHeight() == 2015);
    Block wrong = MineBlock(h, kGenesisTime + 300 * 2016, kTestBits,
                            {MakeTx(202)});
    AddFails(c, wrong);  // old bits no longer valid at the boundary
    Block right = MineBlock(h, kGenesisTime + 300 * 2016, 0x203ff7de,
                            {MakeTx(202)});
    CHECK(AddOk(c, right));
    CHECK(c.GetTipHeight() == 2016);
  }

  test::BeginSection("genesis params: not finalized yet");
  {
    GenesisParams params;
    CHECK(!params.IsUsable());
    CHECK(params.message == std::string(kGenesisMessage));
    CHECK(params.version == 1);
    CHECK(params.bits == 0x1d00ffff);
  }
}
