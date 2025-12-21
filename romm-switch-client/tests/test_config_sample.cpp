#include "catch.hpp"
#include <fstream>
#include <sstream>
#include "romm/config.hpp"

TEST_CASE("config.sample.env parses with required keys") {
    std::ifstream f("../config.sample.env");
    REQUIRE(f.good());
    std::stringstream buffer;
    buffer << f.rdbuf();
    romm::Config cfg;
    std::string err;
    bool ok = romm::parseEnvString(buffer.str(), cfg, err);
    REQUIRE(ok);
    REQUIRE(err.empty());
    REQUIRE_FALSE(cfg.serverUrl.empty());
    REQUIRE_FALSE(cfg.downloadDir.empty());
    // speed_test_url is optional; sample may leave it blank
}
