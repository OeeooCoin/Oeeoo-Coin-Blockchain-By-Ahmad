// SPDX-License-Identifier: MIT
// Oeeoo Coin — oeeoo-genesis: generate the genesis block (Milestone 12).
// Usage:
//   oeeoo-genesis --pubkey1 <hex> --pubkey2 <hex> --pubkey3 <hex>
//                 [--time <unix>] [--bits <hex>] [--threads <n>]
//                 [--message <text>] --out <path>
// The three compressed secp256k1 public keys become the founder
// 2-of-3 multisig vault. The genesis block pays exactly 1,000,000
// OEEOO to that vault. Keep the keys offline; this tool never sees
// a private key.
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

#include "app/genesis.h"
#include "blockchain/genesis.h"
#include "consensus/consensus.h"
#include "consensus/difficulty.h"
#include "rpc/rpcutil.h"

namespace {
bool ParseHex32(const std::string& hex,
                std::vector<unsigned char>& out) {
  return oeeoo::HexToBytesStr(hex, out) && out.size() == 33;
}

void PrintUsage(const char* program) {
  std::cout << "Usage: " << program
            << " --pubkey1 <hex> --pubkey2 <hex> --pubkey3 <hex>\n"
               "                 [--time <unix>] [--bits <hex>] "
               "[--threads <n>] --out <path>\n"
               "\n"
               "  --pubkeyN  compressed secp256k1 public key (66 hex chars)\n"
               "  --time     genesis timestamp (default: now)\n"
               "  --bits     difficulty bits in hex (default: 1d00ffff)\n"
               "  --threads  mining threads (default: hardware concurrency)\n"
               "  --out      genesis file to write (JSON)\n"
               "\n"
               "The genesis message is fixed by the spec: \""
            << oeeoo::kGenesisMessage << "\"\n";
}
}  // namespace

int main(int argc, char** argv) {
  using namespace oeeoo;
  std::vector<std::vector<unsigned char>> pubkeys;
  uint32_t time = static_cast<uint32_t>(
      std::chrono::system_clock::to_time_t(
          std::chrono::system_clock::now()));
  uint32_t bits = 0x1d00ffff;
  int threads = static_cast<int>(std::thread::hardware_concurrency());
  if (threads < 1) threads = 1;
  std::string outPath;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto needValue = [&](const char* name, std::string& out) {
      if (i + 1 >= argc) {
        std::cerr << "oeeoo-genesis: " << name << " needs a value\n";
        return false;
      }
      out = argv[++i];
      return true;
    };
    if (arg == "--pubkey1" || arg == "--pubkey2" || arg == "--pubkey3") {
      std::string value;
      if (!needValue(arg.c_str(), value)) return 1;
      std::vector<unsigned char> pk;
      if (!ParseHex32(value, pk)) {
        std::cerr << "oeeoo-genesis: " << arg
                  << " must be a 33-byte compressed public key in hex\n";
        return 1;
      }
      pubkeys.push_back(pk);
    } else if (arg == "--time") {
      std::string value;
      if (!needValue("--time", value)) return 1;
      try {
        time = static_cast<uint32_t>(std::stoull(value));
      } catch (...) {
        std::cerr << "oeeoo-genesis: invalid --time\n";
        return 1;
      }
    } else if (arg == "--bits") {
      std::string value;
      if (!needValue("--bits", value)) return 1;
      try {
        bits = static_cast<uint32_t>(std::stoul(value, nullptr, 16));
      } catch (...) {
        std::cerr << "oeeoo-genesis: invalid --bits\n";
        return 1;
      }
    } else if (arg == "--threads") {
      std::string value;
      if (!needValue("--threads", value)) return 1;
      try {
        threads = std::stoi(value);
      } catch (...) {
        std::cerr << "oeeoo-genesis: invalid --threads\n";
        return 1;
      }
      if (threads < 1) threads = 1;
    } else if (arg == "--out") {
      if (!needValue("--out", outPath)) return 1;
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return 0;
    } else {
      std::cerr << "oeeoo-genesis: unknown argument: " << arg << "\n";
      PrintUsage(argv[0]);
      return 1;
    }
  }
  if (pubkeys.size() != 3) {
    std::cerr << "oeeoo-genesis: three public keys are required "
                 "(--pubkey1/2/3)\n";
    PrintUsage(argv[0]);
    return 1;
  }
  if (outPath.empty()) {
    std::cerr << "oeeoo-genesis: --out is required\n";
    PrintUsage(argv[0]);
    return 1;
  }
  const std::vector<unsigned char> founderScript =
      BuildMultisigScript(2, pubkeys);
  if (founderScript.empty()) {
    std::cerr << "oeeoo-genesis: invalid public keys\n";
    return 1;
  }
  Block genesis = BuildGenesisBlock(founderScript, time, bits);
  std::cout << "Mining the genesis block (" << threads << " threads, bits "
            << std::hex << bits << std::dec << ")...\n";
  std::atomic<uint64_t> hashesDone{0};
  std::atomic<bool> stop{false};
  std::atomic<bool> done{false};
  std::thread progress([&]() {
    const auto start = std::chrono::steady_clock::now();
    while (!done.load()) {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      if (done.load()) break;
      const double seconds =
          std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                        start)
              .count();
      const double rate = hashesDone.load() / seconds / 1e6;
      std::cout << "  ... " << hashesDone.load() << " hashes (" << rate
                << " MH/s)\n";
    }
  });
  std::string error;
  const bool mined =
      MineGenesisParallel(genesis, threads, &hashesDone, &stop, error);
  done.store(true);
  progress.join();
  if (!mined) {
    std::cerr << "oeeoo-genesis: " << error << "\n";
    return 1;
  }
  if (!WriteGenesisFile(outPath, genesis, founderScript, error)) {
    std::cerr << "oeeoo-genesis: " << error << "\n";
    return 1;
  }
  std::cout << "Genesis block mined and written to " << outPath << "\n"
            << "  hash: " << genesis.header.GetHashHex() << "\n"
            << "  merkle root: "
            << BytesToHexStr(std::vector<unsigned char>(
                   genesis.header.hashMerkleRoot.begin(),
                   genesis.header.hashMerkleRoot.end()))
            << "\n"
            << "  nonce: " << genesis.header.nNonce << "\n"
            << "  time: " << genesis.header.nTime << "\n"
            << "  founder script: " << BytesToHexStr(founderScript) << "\n"
            << "  founder vault: 1,000,000 OEEOO (2-of-3 multisig)\n";
  return 0;
}
