#include "catch.hpp"
#include "api_test_hooks.hpp"

TEST_CASE("parseHttpUrl variants (legacy runner parity)") {
    std::string host, port, path, err;
    bool ok = romm::parseHttpUrl("http://example.com:8080/path?x=1", host, port, path, err);
    REQUIRE(ok);
    REQUIRE(host == "example.com");
    REQUIRE(port == "8080");
    REQUIRE(path == "/path?x=1");

    host.clear(); port.clear(); path.clear(); err.clear();
    ok = romm::parseHttpUrl("https://bad.com", host, port, path, err);
    REQUIRE_FALSE(ok);
    REQUIRE_FALSE(err.empty());
}

TEST_CASE("decodeChunkedBody mirrors legacy assertions") {
    std::string decoded;
    std::string body = "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
    REQUIRE(romm::decodeChunkedBody(body, decoded));
    REQUIRE(decoded == "Wikipedia");

    decoded.clear();
    std::string bad = "4\r\nWiki\r\nZ\r\nbad\r\n0\r\n\r\n"; // malformed chunk size
    REQUIRE_FALSE(romm::decodeChunkedBody(bad, decoded));
}
