// Tests: node configuration (Milestone 12).
#include <cstdio>
#include <fstream>
#include <string>

#include "app/config.h"
#include "test_harness.h"

using namespace oeeoo;

namespace {
bool WriteFile(const std::string& path, const std::string& content) {
  std::ofstream out(path);
  out << content;
  return static_cast<bool>(out);
}
}  // namespace

void TestConfig() {
  test::BeginSection("config: file parsing + defaults");
  {
    AppConfig config;
    CHECK(config.dataDir == "oeeoo-data");
    CHECK(config.port == 64775);
    CHECK(config.rpcPort == 64776);
    CHECK(config.stratumPort == 64777);
    CHECK(config.minerAddress.empty());
    // Deep verification is ON by default since Milestone 13 (a
    // light load trusts the stored UTXO table without re-validating
    // the chain).
    CHECK(config.deepVerify);
    CHECK(config.genesisCheckpoint.empty());
    CHECK(config.maxMempoolBytes == 300ULL * 1024 * 1024);
    const std::string path = "/tmp/oeeoo-test.conf";
    CHECK(WriteFile(path,
                    "# comment line\n"
                    "\n"
                    "datadir = /tmp/oeeoo-data\n"
                    "port = 64780\n"
                    "rpcport = 64781\n"
                    "mineraddress = 7L88G1Not1Gv8nmci9mchhoN9XgbXTXqX5\n"
                    "deepverify = true\n"
                    "maxpeers = 50\n"
                    "genesisfile = \"/tmp/genesis.json\"\n"));
    std::string error;
    CHECK(LoadConfigFile(path, config, error));
    CHECK(config.dataDir == "/tmp/oeeoo-data");
    CHECK(config.port == 64780);
    CHECK(config.rpcPort == 64781);
    CHECK(config.minerAddress ==
          "7L88G1Not1Gv8nmci9mchhoN9XgbXTXqX5");
    CHECK(config.deepVerify);
    CHECK(config.maxPeers == 50);
    CHECK(config.genesisFile == "/tmp/genesis.json");
    ::remove(path.c_str());
  }

  test::BeginSection("config: file errors");
  {
    AppConfig config;
    std::string error;
    const std::string badKey = "/tmp/oeeoo-badkey.conf";
    CHECK(WriteFile(badKey, "nosuchkey = 1\n"));
    CHECK(!LoadConfigFile(badKey, config, error));
    CHECK(error.find("unknown config key") != std::string::npos);
    ::remove(badKey.c_str());
    const std::string badPort = "/tmp/oeeoo-badport.conf";
    CHECK(WriteFile(badPort, "port = 70000\n"));
    CHECK(!LoadConfigFile(badPort, config, error));
    ::remove(badPort.c_str());
    const std::string badLine = "/tmp/oeeoo-badline.conf";
    CHECK(WriteFile(badLine, "this is not a key value line\n"));
    CHECK(!LoadConfigFile(badLine, config, error));
    ::remove(badLine.c_str());
    const std::string badBool = "/tmp/oeeoo-badbool.conf";
    CHECK(WriteFile(badBool, "deepverify = maybe\n"));
    CHECK(!LoadConfigFile(badBool, config, error));
    ::remove(badBool.c_str());
    CHECK(!LoadConfigFile("/tmp/oeeoo-does-not-exist.conf", config, error));
  }

  test::BeginSection("config: CLI overrides the file");
  {
    const std::string path = "/tmp/oeeoo-override.conf";
    CHECK(WriteFile(path, "port = 64780\ndatadir = /tmp/from-file\n"));
    AppConfig config;
    std::string error;
    char* argv[] = {(char*)"oeeoo-node",
                    (char*)"--config",
                    (char*)"/tmp/oeeoo-override.conf",
                    (char*)"--port",
                    (char*)"64799",
                    (char*)"--deep-verify",
                    (char*)"--miner-address",
                    (char*)"7abc",
                    nullptr};
    CHECK(ParseArgs(8, argv, config, error));
    CHECK(config.port == 64799);       // CLI wins
    CHECK(config.dataDir == "/tmp/from-file");  // file value kept
    CHECK(config.deepVerify);
    CHECK(config.minerAddress == "7abc");
    ::remove(path.c_str());
  }

  test::BeginSection("config: new M13 options");
  {
    // --no-deep-verify turns the (now default-on) deep verification
    // off; --genesis-checkpoint and --maxmempool parse, and bad
    // values are rejected.
    AppConfig config;
    std::string error;
    char* argv[] = {(char*)"oeeoo-node",
                    (char*)"--no-deep-verify",
                    (char*)"--genesis-checkpoint",
                    (char*)"2ee2bc4bee136c8bc30e22b298c9b5f9ed883b96c0afdfc"
                           "b0db5d537c8323faa",
                    (char*)"--maxmempool",
                    (char*)"100",
                    nullptr};
    CHECK(ParseArgs(6, argv, config, error));
    CHECK(!config.deepVerify);
    CHECK(config.genesisCheckpoint ==
          "2ee2bc4bee136c8bc30e22b298c9b5f9ed883b96c0afdfcb0db5d537c8323faa");
    CHECK(config.maxMempoolBytes == 100ULL * 1024 * 1024);
    char* badCheckpoint[] = {(char*)"oeeoo-node",
                             (char*)"--genesis-checkpoint",
                             (char*)"xyz",
                             nullptr};
    AppConfig config2;
    CHECK(!ParseArgs(3, badCheckpoint, config2, error));
    char* badMempool[] = {(char*)"oeeoo-node", (char*)"--maxmempool",
                          (char*)"0", nullptr};
    AppConfig config3;
    CHECK(!ParseArgs(3, badMempool, config3, error));
    // File keys for the same options.
    const std::string path = "/tmp/oeeoo-m13.conf";
    CHECK(WriteFile(path,
                    "genesischeckpoint = "
                    "2ee2bc4bee136c8bc30e22b298c9b5f9ed883b96c0afdfcb0db5d"
                    "537c8323faa\n"
                    "maxmempool = 50\n"
                    "deepverify = false\n"));
    AppConfig config4;
    CHECK(LoadConfigFile(path, config4, error));
    CHECK(config4.genesisCheckpoint ==
          "2ee2bc4bee136c8bc30e22b298c9b5f9ed883b96c0afdfcb0db5d537c8323faa");
    CHECK(config4.maxMempoolBytes == 50ULL * 1024 * 1024);
    CHECK(!config4.deepVerify);
    ::remove(path.c_str());
  }

  test::BeginSection("config: CLI errors + help/version");
  {
    AppConfig config;
    std::string error;
    char* badArg[] = {(char*)"oeeoo-node", (char*)"--nope", nullptr};
    CHECK(!ParseArgs(2, badArg, config, error));
    char* missing[] = {(char*)"oeeoo-node", (char*)"--port", nullptr};
    CHECK(!ParseArgs(2, missing, config, error));
    char* badPort[] = {(char*)"oeeoo-node", (char*)"--port", (char*)"abc",
                       nullptr};
    CHECK(!ParseArgs(3, badPort, config, error));
    char* help[] = {(char*)"oeeoo-node", (char*)"--help", nullptr};
    CHECK(ParseArgs(2, help, config, error));
    CHECK(config.help);
    CHECK(UsageText("oeeoo-node").find("--datadir") != std::string::npos);
    char* version[] = {(char*)"oeeoo-node", (char*)"--version", nullptr};
    AppConfig config2;
    CHECK(ParseArgs(2, version, config2, error));
    CHECK(config2.version);
  }
}
