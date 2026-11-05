// Tests: DNS seed resolution (launch support, post-Milestone 12).
#include <string>
#include <vector>

#include "network/dnsseed.h"
#include "network/node.h"
#include "test_harness.h"

using namespace oeeoo;

void TestDnsSeed() {
  test::BeginSection("dnsseed: localhost resolves to 127.0.0.1");
  {
    std::vector<AddrEntry> addrs;
    std::string error;
    CHECK(ResolveSeed("localhost", 64775, addrs, error));
    CHECK(!addrs.empty());
    bool found = false;
    for (const AddrEntry& a : addrs) {
      CHECK(a.port == 64775);
      CHECK(!a.ip.empty());
      if (a.ip == "127.0.0.1") found = true;
    }
    CHECK(found);
  }

  test::BeginSection("dnsseed: unresolvable names fail cleanly");
  {
    std::vector<AddrEntry> addrs;
    std::string error;
    // A 300-character hostname exceeds the DNS limit and never
    // resolves (a plain ".invalid" name is NOT used: some sandbox
    // resolvers answer every name, so it is not a reliable failure
    // case in this environment).
    const std::string tooLong(300, 'a');
    CHECK(!ResolveSeed(tooLong, 64775, addrs, error));
    CHECK(!error.empty());
    CHECK(!ResolveSeed("", 64775, addrs, error));
  }

  test::BeginSection("dnsseed: node queries seeds into the AddrMan");
  {
    Node node(0x207fffff);
    node.AddDnsSeed("localhost");
    node.AddDnsSeed(std::string(300, 'b'));  // never resolves
    std::string error;
    const std::size_t added = node.QueryDnsSeeds(error);
    CHECK(added >= 1);
    CHECK(node.AddrMan().Contains("127.0.0.1", 64775));
    // The bad seed is reported but does not stop the good one.
    CHECK(!error.empty());
    // Querying again adds nothing new (already known).
    std::string error2;
    CHECK(node.QueryDnsSeeds(error2) == 0);
  }

  test::BeginSection("dnsseed: a node with no seeds adds nothing");
  {
    Node node(0x207fffff);
    std::string error;
    CHECK(node.QueryDnsSeeds(error) == 0);
    CHECK(error.empty());
    CHECK(node.AddrMan().Size() == 0);
  }
}
