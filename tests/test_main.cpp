// Oeeoo Coin Phase 1 test runner.
#include <cstdio>
#include <cstdlib>

#include "test_harness.h"

void TestAmount();
void TestSubsidy();
void TestCrypto();
void TestBlock();
void TestVarInt();
void TestTransaction();
void TestMerkle();
void TestAddress();
void TestDifficulty();
void TestChain();
void TestScript();
void TestValidation();
void TestMining();
void TestNetwork();
void TestNode();
void TestDb();
void TestWallet();
void TestVesting();
void TestRpc();
void TestStratum();
void TestReorg();
void TestVerify();
void TestFuzz();
void TestConfig();
void TestGenesis();
void TestCli();
void TestDnsSeed();
void TestMilestone13();

int main() {
  TestAmount();
  TestSubsidy();
  TestCrypto();
  TestBlock();
  TestVarInt();
  TestTransaction();
  TestMerkle();
  TestAddress();
  TestDifficulty();
  TestChain();
  TestScript();
  TestValidation();
  TestMining();
  TestNetwork();
  TestNode();
  TestDb();
  TestWallet();
  TestVesting();
  TestRpc();
  TestStratum();
  TestReorg();
  TestVerify();
  TestFuzz();
  TestConfig();
  TestGenesis();
  TestCli();
  TestDnsSeed();
  TestMilestone13();
  test::Report();
  return test::Failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
