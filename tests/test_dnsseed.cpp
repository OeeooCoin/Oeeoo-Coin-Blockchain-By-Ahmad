// Tests: DNS seed resolution (launch support, post-Milestone 12).
#include <chrono>
#include <string>
#include <thread>
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

  test::BeginSection("dnsseed: slow resolver times out");
  {
    // Injected resolver that sleeps 1 second and then succeeds: the
    // 200 ms deadline must cut it off long before it answers. No
    // real slow DNS server is needed — the deadline logic is tested
    // through the injected callable.
    SeedResolver slow = [](const std::string& /*hostname*/) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      SeedLookupResult r;
      r.ok = true;
      r.ips = {"127.0.0.1"};
      return r;
    };
    std::vector<AddrEntry> addrs;
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    CHECK(!ResolveSeedWithResolver("slow.example", 64775, addrs, error,
                                   200, slow));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    // Returns promptly (generous bounds, not an exact value: the
    // call must take ~the deadline, not ~the resolver's 1 second).
    CHECK(elapsed.count() >= 100);
    CHECK(elapsed.count() < 5000);
    CHECK(addrs.empty());
    CHECK(error.find("timed out") != std::string::npos);
  }

  test::BeginSection("dnsseed: injected fast resolver still works");
  {
    // The same seam, healthy path: an instant resolver's addresses
    // come through with the right port and timestamps.
    SeedResolver fast = [](const std::string& /*hostname*/) {
      SeedLookupResult r;
      r.ok = true;
      r.ips = {"127.0.0.1", "127.0.0.2"};
      return r;
    };
    std::vector<AddrEntry> addrs;
    std::string error;
    CHECK(ResolveSeedWithResolver("fast.example", 64775, addrs, error,
                                  10000, fast));
    CHECK(addrs.size() == 2);
    CHECK(addrs[0].ip == "127.0.0.1" && addrs[0].port == 64775);
    CHECK(addrs[1].ip == "127.0.0.2" && addrs[1].port == 64775);
    CHECK(addrs[0].time != 0);
    // A resolver failure is propagated (not turned into a timeout).
    SeedResolver failing = [](const std::string& hostname) {
      SeedLookupResult r;
      r.ok = false;
      r.error = "cannot resolve seed " + hostname + ": boom";
      return r;
    };
    CHECK(!ResolveSeedWithResolver("bad.example", 64775, addrs, error,
                                   10000, failing));
    CHECK(error.find("boom") != std::string::npos);
    CHECK(addrs.empty());
    // A resolver that returns no addresses fails cleanly too.
    SeedResolver empty = [](const std::string& /*hostname*/) {
      SeedLookupResult r;
      r.ok = true;
      return r;
    };
    CHECK(!ResolveSeedWithResolver("empty.example", 64775, addrs, error,
                                   10000, empty));
    CHECK(error.find("no IPv4 address") != std::string::npos);
    // The 100-address cap is enforced on the resolver's output.
    SeedResolver many = [](const std::string& /*hostname*/) {
      SeedLookupResult r;
      r.ok = true;
      for (int i = 0; i < 150; ++i) {
        r.ips.push_back("10.0." + std::to_string(i / 256) + "." +
                        std::to_string(i % 256 + 1));
      }
      return r;
    };
    CHECK(ResolveSeedWithResolver("many.example", 64775, addrs, error,
                                  10000, many));
    CHECK(addrs.size() == 100);
  }

  test::BeginSection("dnsseed: normal resolve still works (localhost)");
  {
    // The defaulted timeout parameter: the plain ResolveSeed call
    // below uses kDnsSeedTimeoutMs (10 s) with zero changes.
    std::vector<AddrEntry> addrs;
    std::string error;
    CHECK(ResolveSeed("localhost", 64775, addrs, error));
    CHECK(!addrs.empty());
    bool found = false;
    for (const AddrEntry& a : addrs) {
      CHECK(a.port == 64775);
      if (a.ip == "127.0.0.1") found = true;
    }
    CHECK(found);
    CHECK(kDnsSeedTimeoutMs == 10000);
  }
}
