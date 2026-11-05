// Tests: P2P protocol (Milestone 6) — message framing, payload
// codecs, SipHash-2-4, compact blocks, peer scoring and bans,
// address manager.
#include <string>
#include <vector>

#include "consensus/consensus.h"
#include "crypto/ecdsa.h"
#include "mempool/mempool.h"
#include "mining/miner.h"
#include "network/addrman.h"
#include "network/compact.h"
#include "network/peer.h"
#include "network/protocol.h"
#include "network/siphash.h"
#include "script/script.h"
#include "test_harness.h"
#include "transactions/sighash.h"

using namespace oeeoo;

namespace {
constexpr uint32_t kTestBits = 0x207fffff;
constexpr uint32_t kTime = 1700000000;

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

// A funded, signed spend used by compact-block tests.
Transaction MakeSpend(CoinsView& utxo, const Hash256& prev, Amount value) {
  Coin coin;
  coin.out.value = value;
  coin.out.scriptPubKey = Spk();
  coin.height = 0;
  coin.blockTime = kTime;
  coin.isCoinbase = false;
  CHECK(utxo.AddCoin({prev, 0}, coin));
  Transaction tx;
  tx.version = 1;
  TxIn in;
  in.prevTxHash = prev;
  in.prevOutIndex = 0;
  in.sequence = 0xffffffff;
  tx.vin.push_back(in);
  TxOut out;
  out.value = value - 1000;
  out.scriptPubKey = Spk();
  tx.vout.push_back(out);
  tx.lockTime = 0;
  Hash256 h;
  CHECK(LegacySighash(tx, 0, Spk(), kSigHashAll, h));
  std::vector<unsigned char> sig = SignHash(h, g_key.privkey);
  sig.push_back(static_cast<unsigned char>(kSigHashAll));
  tx.vin[0].scriptSig = CreateP2PKHScriptSig(sig, g_key.pubkey);
  return tx;
}

Block MakeBlock(const std::vector<Transaction>& txs, uint32_t bits = kTestBits) {
  Block block;
  block.header.nVersion = 1;
  block.header.nTime = kTime;
  block.header.nBits = bits;
  block.vtx = txs;
  block.header.hashMerkleRoot = block.ComputeMerkleRoot();
  return block;
}
}  // namespace

void TestNetwork() {
  EnsureKeys();

  test::BeginSection("network: message framing + checksum");
  {
    Message msg{"version", SerializeVersion(VersionPayload{})};
    std::vector<unsigned char> bytes = SerializeMessage(msg);
    CHECK(bytes.size() == 24 + msg.payload.size());
    CHECK(bytes[0] == 0xe2 && bytes[1] == 0xd8 && bytes[2] == 0x04 &&
          bytes[3] == 0xd6);  // Oeeoo magic
    Message parsed;
    std::size_t consumed = 0;
    CHECK(ParseMessage(bytes, 0, parsed, consumed));
    CHECK(parsed.command == "version");
    CHECK(parsed.payload == msg.payload);
    CHECK(consumed == bytes.size());
    // Two messages back to back parse in sequence.
    std::vector<unsigned char> two = bytes;
    two.insert(two.end(), bytes.begin(), bytes.end());
    Message m1, m2;
    std::size_t c1 = 0, c2 = 0;
    CHECK(ParseMessage(two, 0, m1, c1));
    CHECK(ParseMessage(two, c1, m2, c2));
    CHECK(c1 + c2 == two.size());
    // Truncated payload: not parseable yet.
    std::vector<unsigned char> cut(bytes.begin(), bytes.end() - 3);
    CHECK(!ParseMessage(cut, 0, parsed, consumed));
    // Bad checksum.
    std::vector<unsigned char> bad = bytes;
    bad.back() ^= 0x01;
    CHECK(!ParseMessage(bad, 0, parsed, consumed));
    // Bad magic.
    bad = bytes;
    bad[0] = 0x00;
    CHECK(!ParseMessage(bad, 0, parsed, consumed));
    // Oversize length.
    bad = bytes;
    bad[16] = 0xff;
    bad[17] = 0xff;
    bad[18] = 0xff;
    bad[19] = 0x7f;
    CHECK(!ParseMessage(bad, 0, parsed, consumed));
  }

  test::BeginSection("network: payload codecs");
  {
    VersionPayload v;
    v.version = 70015;
    v.services = 1;
    v.timestamp = 1700000000;
    v.nonce = 123456789;
    v.userAgent = "/OeeooCoin:0.1.0/";
    v.startHeight = 42;
    v.relay = true;
    VersionPayload v2;
    CHECK(ParseVersion(SerializeVersion(v), v2));
    CHECK(v2.version == 70015 && v2.services == 1 &&
          v2.timestamp == 1700000000 && v2.nonce == 123456789 &&
          v2.userAgent == "/OeeooCoin:0.1.0/" && v2.startHeight == 42 &&
          v2.relay);

    std::vector<InventoryItem> inv = {{kInvTx, Hash256{}}};
    inv[0].hash.fill(0xab);
    inv.push_back({kInvBlock, Hash256{}});
    inv[1].hash.fill(0xcd);
    std::vector<InventoryItem> inv2;
    CHECK(ParseInventory(SerializeInventory(inv), inv2));
    CHECK(inv2.size() == 2 && inv2[0].type == kInvTx &&
          inv2[0].hash == inv[0].hash && inv2[1].type == kInvBlock);

    uint64_t nonce = 0;
    CHECK(ParsePing(SerializePing(0xdeadbeefcafe1234ULL), nonce));
    CHECK(nonce == 0xdeadbeefcafe1234ULL);
    CHECK(!ParsePing(std::vector<unsigned char>{1, 2, 3}, nonce));

    std::vector<Hash256> locator = {Hash256{}};
    locator[0].fill(0x11);
    Hash256 stop;
    stop.fill(0x22);
    int32_t ver = 0;
    std::vector<Hash256> loc2;
    Hash256 stop2;
    CHECK(ParseGetHeaders(SerializeGetHeaders(70015, locator, stop), ver,
                          loc2, stop2));
    CHECK(ver == 70015 && loc2.size() == 1 && loc2[0] == locator[0] &&
          stop2 == stop);

    BlockHeader h1, h2;
    h1.nVersion = 1;
    h1.nTime = kTime;
    h1.nBits = kTestBits;
    h2.nVersion = 1;
    h2.nTime = kTime + 600;
    h2.nBits = kTestBits;
    h2.hashPrevBlock = h1.GetHash();
    std::vector<BlockHeader> hdrs;
    CHECK(ParseHeaders(SerializeHeaders({h1, h2}), hdrs));
    CHECK(hdrs.size() == 2 && hdrs[0].GetHash() == h1.GetHash() &&
          hdrs[1].GetHash() == h2.GetHash());

    std::vector<AddrEntry> addrs = {{1700000000, 1, "1.2.3.4", 64775},
                                    {1700000001, 1, "5.6.7.8", 64775}};
    std::vector<AddrEntry> addrs2;
    CHECK(ParseAddr(SerializeAddr(addrs), addrs2));
    CHECK(addrs2.size() == 2 && addrs2[0].ip == "1.2.3.4" &&
          addrs2[0].port == 64775 && addrs2[1].ip == "5.6.7.8");

    bool announce = false;
    uint64_t cmpctVer = 0;
    CHECK(ParseSendCmpct(SerializeSendCmpct(true, 1), announce, cmpctVer));
    CHECK(announce && cmpctVer == 1);

    std::string cmd;
    unsigned char code = 0;
    std::string reason;
    CHECK(ParseReject(SerializeReject("block", 0x10, "bad pow"), cmd, code,
                      reason));
    CHECK(cmd == "block" && code == 0x10 && reason == "bad pow");
  }

  test::BeginSection("network: SipHash-2-4 reference vector");
  {
    // Reference vector: key 00 01 02 ... 0f, empty input.
    const uint64_t k0 = 0x0706050403020100ULL;
    const uint64_t k1 = 0x0f0e0d0c0b0a0908ULL;
    CHECK(SipHash24(k0, k1, {}) == 0x726fdb47dd0e0e31ULL);
    // Determinism + key sensitivity.
    CHECK(SipHash24(k0, k1, {1, 2, 3}) ==
          SipHash24(k0, k1, std::vector<unsigned char>{1, 2, 3}));
    CHECK(SipHash24(k0, k1, {1, 2, 3}) != SipHash24(k0, k1, {1, 2, 4}));
    CHECK(SipHash24(k0, k1, {1, 2, 3}) != SipHash24(k0 + 1, k1, {1, 2, 3}));
  }

  test::BeginSection("network: compact blocks");
  {
    CoinsView utxo;
    Hash256 p1, p2;
    p1.fill(0x31);
    p2.fill(0x32);
    Transaction tx1 = MakeSpend(utxo, p1, 50 * COIN);
    Transaction tx2 = MakeSpend(utxo, p2, 10 * COIN);
    Transaction coinbase = CreateCoinbase(1, Spk(), 50 * COIN + 2000);
    Block block = MakeBlock({coinbase, tx1, tx2});
    CompactBlock cb = MakeCompactBlock(block, 0x123456789abcdef0ULL);
    CHECK(cb.shortIds.size() == 2);
    CHECK(cb.prefilled.size() == 1 && cb.prefilled[0].first == 0);
    // Short IDs are stable and distinct per tx.
    CHECK(ShortTxId(block.header, cb.nonce, tx1.GetTxId()) ==
          ShortTxId(block.header, cb.nonce, tx1.GetTxId()));
    CHECK(ShortTxId(block.header, cb.nonce, tx1.GetTxId()) !=
          ShortTxId(block.header, cb.nonce, tx2.GetTxId()));
    // Round-trip through the wire format.
    CompactBlock cb2;
    CHECK(ParseCompactBlock(SerializeCompactBlock(cb), cb2));
    CHECK(cb2.header.GetHash() == block.header.GetHash());
    CHECK(cb2.nonce == cb.nonce);
    CHECK(cb2.shortIds == cb.shortIds);
    CHECK(cb2.prefilled.size() == 1);
    CHECK(cb2.prefilled[0].second.GetTxId() == coinbase.GetTxId());
    // Reconstruct with a mempool that has both txs.
    Mempool pool;
    std::string error;
    CHECK(pool.AddTransaction(tx1, utxo, 1, kTime, kTime, error));
    CHECK(pool.AddTransaction(tx2, utxo, 1, kTime, kTime, error));
    Block rebuilt;
    std::vector<uint32_t> missing;
    CHECK(ReconstructBlock(cb2, pool, rebuilt, missing));
    CHECK(missing.empty());
    CHECK(rebuilt.GetHash() == block.GetHash());
    // Reconstruct with an empty mempool: both txs missing.
    Mempool empty;
    CHECK(!ReconstructBlock(cb2, empty, rebuilt, missing));
    CHECK(missing.size() == 2 && missing[0] == 1 && missing[1] == 2);
    // Merge the missing txs as if they arrived in a blocktxn answer:
    // reconstruction must now succeed with the same block hash.
    CompactBlock merged = cb2;
    merged.prefilled.push_back({1, tx1});
    merged.prefilled.push_back({2, tx2});
    CHECK(ReconstructBlock(merged, empty, rebuilt, missing));
    CHECK(missing.empty());
    CHECK(rebuilt.GetHash() == block.GetHash());
    // getblocktxn / blocktxn round-trip.
    Hash256 bh;
    std::vector<uint32_t> idx;
    CHECK(ParseGetBlockTxn(SerializeGetBlockTxn(block.GetHash(), {1, 2}), bh,
                          idx));
    CHECK(bh == block.GetHash() && idx.size() == 2 && idx[0] == 1);
    Hash256 bh2;
    std::vector<std::pair<uint32_t, Transaction>> txs;
    CHECK(ParseBlockTxn(SerializeBlockTxn(block.GetHash(), {{1, tx1}, {2, tx2}}),
                        bh2, txs));
    CHECK(bh2 == block.GetHash() && txs.size() == 2 &&
          txs[0].second.GetTxId() == tx1.GetTxId());
  }

  test::BeginSection("network: peer scoring + bans");
  {
    Peer peer;
    CHECK(!peer.AddMisbehavior(50));
    CHECK(peer.misbehavior == 50);
    CHECK(peer.AddMisbehavior(50));  // 100: banned
    BanManager bans;
    bans.Ban("1.2.3.4:64775", 1000);
    CHECK(bans.IsBanned("1.2.3.4:64775", 1000));
    CHECK(bans.IsBanned("1.2.3.4:64775", 1000 + 24 * 60 * 60 - 1));
    CHECK(!bans.IsBanned("1.2.3.4:64775", 1000 + 24 * 60 * 60));
    CHECK(!bans.IsBanned("5.6.7.8:64775", 1000));
    // Second offence doubles the ban (48h).
    bans.Ban("1.2.3.4:64775", 2000);
    CHECK(bans.IsBanned("1.2.3.4:64775", 2000 + 48 * 60 * 60 - 1));
    CHECK(!bans.IsBanned("1.2.3.4:64775", 2000 + 48 * 60 * 60));
    bans.Unban("1.2.3.4:64775");
    CHECK(!bans.IsBanned("1.2.3.4:64775", 2000));
  }

  test::BeginSection("network: address manager");
  {
    AddressManager am;
    CHECK(am.Add({1700000000, 1, "1.2.3.4", 64775}));
    CHECK(!am.Add({1700000000, 1, "1.2.3.4", 64775}));  // duplicate
    CHECK(!am.Add({1700000000, 1, "", 64775}));          // invalid
    CHECK(am.Add({1700000001, 1, "5.6.7.8", 64775}));
    CHECK(am.Size() == 2);
    CHECK(am.Contains("1.2.3.4", 64775));
    CHECK(!am.Contains("9.9.9.9", 64775));
    std::vector<AddrEntry> cands = am.GetCandidates(10);
    CHECK(cands.size() == 2 && cands[0].ip == "5.6.7.8");  // newest first
    CHECK(am.Remove("1.2.3.4", 64775));
    CHECK(am.Size() == 1);
    CHECK(!am.Remove("1.2.3.4", 64775));
  }
}
