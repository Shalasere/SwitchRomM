#include "catch.hpp"
#include "romm/config.hpp"
#include "api_test_hooks.hpp"

TEST_CASE("parseHttpUrl accepts https URLs") {
    romm::Config cfg;
    std::string err;
    // Simulate a config object with an https URL; parse should succeed.
    cfg.serverUrl = "https://example.com";
    // loadConfig is not directly testable here (depends on SD paths), so assert parser behavior.
    std::string host, port, path, perr;
    bool ok = romm::parseHttpUrl(cfg.serverUrl, host, port, path, perr);
    REQUIRE(ok);
    REQUIRE(host == "example.com");
    REQUIRE(port == "443");
    REQUIRE(path == "/");
}

TEST_CASE("parseHttpUrl accepts http URLs") {
    std::string host, port, path, err;
    bool ok = romm::parseHttpUrl("http://example.com", host, port, path, err);
    REQUIRE(ok);
    REQUIRE(host == "example.com");
    REQUIRE(port == "80");
    REQUIRE(path == "/");
}

TEST_CASE("parseHttpUrl fails on missing host or malformed") {
    std::string host, port, path, err;
    bool ok = romm::parseHttpUrl("http:///path", host, port, path, err);
    REQUIRE_FALSE(ok);
    REQUIRE_FALSE(err.empty());

    ok = romm::parseHttpUrl("http://", host, port, path, err);
    REQUIRE_FALSE(ok);
    REQUIRE_FALSE(err.empty());
}
