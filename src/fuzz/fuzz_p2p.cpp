// SPDX-License-Identifier: MIT
// Oeeoo Coin — libFuzzer target: P2P message framing + payload
// parsers (Milestone 18). The input is tried as a whole framed
// message, and (split by a leading selector byte) as a raw
// payload for each parser, so framing and payload logic are both
// covered. Seed corpus: tests/fuzz/corpus/p2p/.
//
// NOTE (Core's caveat, documented): this target stops at
// parsing. It does not run Node::OnMessage, so no validation is
// mocked out here — the deeper net-processing paths (tx/block
// acceptance) are covered by the live harness in
// tests/test_milestone18b.cpp, where a real Node validates real
// (mined/signed) messages and mutated ones are expected to be
// rejected, not accepted: a fuzzer cannot produce valid PoW or
// valid signatures, so acceptance paths are unreachable from
// mutated bytes by construction.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "blockchain/block.h"
#include "network/compact.h"
#include "network/protocol.h"
#include "transactions/transaction.h"

using namespace oeeoo;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::vector<unsigned char> bytes(data, data + size);
  // Whole framed message.
  {
    Message msg;
    std::size_t consumed = 0;
    if (ParseMessage(bytes, 0, msg, consumed)) {
      // Dispatch the parsed payload to its parser, like the
      // node does after framing.
      if (msg.command == "version") {
        VersionPayload v;
        (void)ParseVersion(msg.payload, v);
      } else if (msg.command == "inv" || msg.command == "getdata" ||
                 msg.command == "notfound") {
        std::vector<InventoryItem> items;
        (void)ParseInventory(msg.payload, items);
      } else if (msg.command == "ping" || msg.command == "pong") {
        uint64_t nonce = 0;
        (void)ParsePing(msg.payload, nonce);
      } else if (msg.command == "getheaders") {
        int32_t version = 0;
        std::vector<Hash256> locator;
        Hash256 stop;
        (void)ParseGetHeaders(msg.payload, version, locator, stop);
      } else if (msg.command == "headers") {
        std::vector<BlockHeader> headers;
        (void)ParseHeaders(msg.payload, headers);
      } else if (msg.command == "addr") {
        std::vector<AddrEntry> addrs;
        (void)ParseAddr(msg.payload, addrs);
      } else if (msg.command == "sendcmpct") {
        bool announce = false;
        uint64_t version = 0;
        (void)ParseSendCmpct(msg.payload, announce, version);
      } else if (msg.command == "reject") {
        std::string command;
        unsigned char code = 0;
        std::string reason;
        (void)ParseReject(msg.payload, command, code, reason);
      } else if (msg.command == "tx") {
        Transaction tx;
        (void)Transaction::Deserialize(msg.payload, tx);
      } else if (msg.command == "block") {
        Block block;
        (void)Block::Deserialize(msg.payload, block);
      } else if (msg.command == "cmpctblock") {
        CompactBlock cb;
        (void)ParseCompactBlock(msg.payload, cb);
      }
    }
  }
  // Raw payload: first byte selects the parser, the rest is the
  // payload. This reaches each parser without valid framing.
  if (size >= 2) {
    const std::vector<unsigned char> payload(data + 1, data + size);
    switch (data[0] % 10) {
      case 0: {
        VersionPayload v;
        (void)ParseVersion(payload, v);
        break;
      }
      case 1: {
        std::vector<InventoryItem> items;
        (void)ParseInventory(payload, items);
        break;
      }
      case 2: {
        uint64_t nonce = 0;
        (void)ParsePing(payload, nonce);
        break;
      }
      case 3: {
        int32_t version = 0;
        std::vector<Hash256> locator;
        Hash256 stop;
        (void)ParseGetHeaders(payload, version, locator, stop);
        break;
      }
      case 4: {
        std::vector<BlockHeader> headers;
        (void)ParseHeaders(payload, headers);
        break;
      }
      case 5: {
        std::vector<AddrEntry> addrs;
        (void)ParseAddr(payload, addrs);
        break;
      }
      case 6: {
        bool announce = false;
        uint64_t version = 0;
        (void)ParseSendCmpct(payload, announce, version);
        break;
      }
      case 7: {
        std::string command;
        unsigned char code = 0;
        std::string reason;
        (void)ParseReject(payload, command, code, reason);
        break;
      }
      case 8: {
        CompactBlock cb;
        (void)ParseCompactBlock(payload, cb);
        break;
      }
      default: {
        Block block;
        (void)Block::Deserialize(payload, block);
        break;
      }
    }
  }
  return 0;
}
