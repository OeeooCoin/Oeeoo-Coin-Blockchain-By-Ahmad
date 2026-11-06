// SPDX-License-Identifier: MIT
// Oeeoo Coin — seed-corpus generator for the Milestone 18
// libFuzzer harnesses (NOT a test: run it once, check the
// outputs into tests/fuzz/corpus/). Usage:
//   oeeoo_gen_corpus [output-dir]   (default: tests/fuzz/corpus)
// Every file is a small VALID input for its target, built from
// the same builders the unit tests use, so the fuzzer starts
// from real protocol bytes instead of empty files. The wallet
// seed uses the public test passphrase "fuzz-corpus-passphrase"
// and a fixed BIP39 test mnemonic — test data only, no funds.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "blockchain/block.h"
#include "crypto/ecdsa.h"
#include "fuzz_common.h"
#include "mempool/mempool.h"
#include "mining/miner.h"
#include "network/compact.h"
#include "network/protocol.h"
#include "rpc/json.h"
#include "script/script.h"
#include "transactions/transaction.h"
#include "wallet/walletfile.h"

namespace fs = std::filesystem;
using namespace oeeoo;

namespace {
bool Write(const fs::path& path, const std::vector<unsigned char>& bytes) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  const bool ok =
      bytes.empty() ||
      std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  std::fclose(f);
  return ok;
}

std::vector<unsigned char> Bytes(const std::string& s) {
  return std::vector<unsigned char>(s.begin(), s.end());
}
}  // namespace

int main(int argc, char** argv) {
  const fs::path root =
      argc > 1 ? fs::path(argv[1]) : fs::path("tests/fuzz/corpus");
  for (const char* dir :
       {"tx", "block", "script", "json", "p2p", "compact", "wallet",
        "stratum"}) {
    fs::create_directories(root / dir);
  }
  int written = 0;
  const auto put = [&](const char* dir, const char* name,
                       const std::vector<unsigned char>& bytes) {
    if (!Write(root / dir / name, bytes)) {
      std::fprintf(stderr, "gen_corpus: cannot write %s/%s\n", dir, name);
      return;
    }
    ++written;
  };
  const auto putText = [&](const char* dir, const char* name,
                           const std::string& text) {
    put(dir, name, Bytes(text));
  };

  const Transaction tx = fuzz::ValidTx();
  const std::vector<unsigned char> txBytes = tx.Serialize();
  put("tx", "spend.bin", txBytes);
  put("tx", "coinbase.bin",
      CreateCoinbase(0, fuzz::FuzzSpk(), 0, "x").Serialize());

  const Block block = fuzz::ValidBlock();
  put("block", "block.bin", block.Serialize());
  put("block", "header.bin", block.header.Serialize());

  put("script", "p2pkh-sig.bin", tx.vin[0].scriptSig);
  put("script", "p2pkh-pubkey.bin", fuzz::FuzzSpk());
  put("script", "op-return.bin",
      std::vector<unsigned char>{0x6a, 0x04, 0xde, 0xad, 0xbe, 0xef});
  put("script", "multisig.bin",
      std::vector<unsigned char>{0x52, 0x21, 0x02, 0x11, 0x22, 0x33, 0x21,
                                 0x02, 0x44, 0x55, 0x66, 0x52, 0xae});

  putText("json", "rpc-getblockcount.json",
          R"({"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]})");
  putText("json", "rpc-nested.json",
          R"({"method":"getblock","params":["abc",true],"id":2,"nested":{"a":[1,2.5,"x",true,null,{"b":[]}]}})");
  putText("json", "rpc-numbers.json",
          R"({"a":0,"b":-1,"c":9223372036854775807,"d":1e400,"e":0.001,"f":1.5e-10})");
  putText("json", "rpc-batch.json",
          R"([{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]},{"jsonrpc":"2.0","id":2,"method":"getbestblockhash","params":[]}])");
  putText("json", "rpc-unicode.json",
          R"({"method":"getnewaddress","params":["caf\u00e9 \ud83d\ude00"],"id":"x"})");

  VersionPayload version;
  version.version = kProtocolVersion;
  version.services = 1;
  version.timestamp = 1700000000;
  version.nonce = 0x123456789abcdef0ULL;
  version.userAgent = "/OeeooCoin:0.1.0/";
  version.startHeight = 42;
  version.relay = true;
  put("p2p", "version.bin",
      SerializeMessage({"version", SerializeVersion(version)}));
  put("p2p", "tx.bin", SerializeMessage({"tx", txBytes}));
  put("p2p", "block.bin", SerializeMessage({"block", block.Serialize()}));
  put("p2p", "verack.bin", SerializeMessage({"verack", {}}));
  put("p2p", "ping.bin", SerializeMessage({"ping", SerializePing(99)}));
  put("p2p", "pong.bin", SerializeMessage({"pong", SerializePing(99)}));
  std::vector<InventoryItem> inv = {{kInvTx, tx.GetTxId()},
                                     {kInvBlock, block.GetHash()}};
  put("p2p", "inv.bin", SerializeMessage({"inv", SerializeInventory(inv)}));
  put("p2p", "getdata.bin",
      SerializeMessage({"getdata", SerializeInventory(inv)}));
  std::vector<AddrEntry> addrs = {{1700000000, 1, "1.2.3.4", kMainnetPort, 0},
                                  {1700000100, 1, "8.8.8.8", kMainnetPort, 0}};
  put("p2p", "addr.bin", SerializeMessage({"addr", SerializeAddr(addrs)}));
  std::vector<Hash256> locator = {block.GetHash()};
  put("p2p", "getheaders.bin",
      SerializeMessage(
          {"getheaders",
           SerializeGetHeaders(70015, locator, Hash256{})}));
  put("p2p", "headers.bin",
      SerializeMessage({"headers", SerializeHeaders({block.header})}));
  put("p2p", "sendcmpct.bin",
      SerializeMessage({"sendcmpct", SerializeSendCmpct(true, 1)}));
  put("p2p", "reject.bin",
      SerializeMessage(
          {"reject", SerializeReject("block", 0x10, "bad-blk")}));

  const CompactBlock cb = MakeCompactBlock(block, 0xabcdefULL);
  put("compact", "cmpctblock.bin",
      SerializeMessage({"cmpctblock", SerializeCompactBlock(cb)}));
  put("compact", "cmpctblock-raw.bin", SerializeCompactBlock(cb));
  put("compact", "getblocktxn.bin",
      SerializeMessage(
          {"getblocktxn",
           SerializeGetBlockTxn(block.GetHash(), {1, 2})}));
  put("compact", "blocktxn.bin",
      SerializeMessage(
          {"blocktxn",
           SerializeBlockTxn(block.GetHash(), {{1, tx}})}));

  WalletFileData walletData;
  walletData.mnemonic =
      "abandon abandon abandon abandon abandon abandon abandon abandon "
      "abandon abandon abandon about";
  walletData.nextExternal = 3;
  walletData.nextChange = 1;
  std::string error;
  if (!SaveWalletFile((root / "wallet" / "valid.oeeoo").string(), walletData,
                      "fuzz-corpus-passphrase", error)) {
    std::fprintf(stderr, "gen_corpus: wallet seed failed: %s\n",
                 error.c_str());
    return 1;
  }
  ++written;

  putText("stratum", "subscribe.txt",
          R"({"id":1,"method":"mining.subscribe","params":["oeeoo-test/1.0"]})");
  putText("stratum", "authorize.txt",
          R"({"id":2,"method":"mining.authorize","params":["worker1","x"]})");
  putText("stratum", "submit.txt",
          R"({"id":3,"method":"mining.submit","params":["worker1","1","00000002","5db8d0a9","5f4c3b2a"]})");

  std::printf("gen_corpus: wrote %d files under %s\n", written,
              root.c_str());
  return 0;
}
