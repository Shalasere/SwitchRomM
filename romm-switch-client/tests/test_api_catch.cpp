#define CATCH_CONFIG_MAIN
#include "catch.hpp"
#include "api_test_hooks.hpp"

TEST_CASE("parseHttpUrl basic http") {
    std::string host, port, path, err;
    bool ok = romm::parseHttpUrl("http://example.com:8080/path?x=1", host, port, path, err);
    REQUIRE(ok);
    REQUIRE(host == "example.com");
    REQUIRE(port == "8080");
    REQUIRE(path == "/path?x=1");
}

TEST_CASE("parseHttpUrl defaults and USER_REDACTED path") {
    std::string host, port, path, err;
    bool ok = romm::parseHttpUrl("http://romm.local", host, port, path, err);
    REQUIRE(ok);
    REQUIRE(host == "romm.local");
    REQUIRE(port == "80"); // defaulted
    REQUIRE(path == "/");  // USER_REDACTED when no path provided
}

TEST_CASE("parseHttpUrl rejects https") {
    std::string host, port, path, err;
    bool ok = romm::parseHttpUrl("https://bad.com", host, port, path, err);
    REQUIRE_FALSE(ok);
    REQUIRE_FALSE(err.empty());
}

TEST_CASE("decodeChunkedBody valid") {
    std::string decoded;
    std::string body = "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
    bool ok = romm::decodeChunkedBody(body, decoded);
    REQUIRE(ok);
    REQUIRE(decoded == "Wikipedia");
}

TEST_CASE("decodeChunkedBody valid uppercase hex and extensions") {
    std::string decoded;
    // Chunk size 0xA with an extension; then zero chunk
    std::string body = "A;ext=1\r\n0123456789\r\n0\r\n\r\n";
    bool ok = romm::decodeChunkedBody(body, decoded);
    REQUIRE(ok);
    REQUIRE(decoded == "0123456789");
}

TEST_CASE("decodeChunkedBody malformed chunk size") {
    std::string decoded;
    std::string body = "4\r\nWiki\r\nZ\r\nbad\r\n0\r\n\r\n";
    bool ok = romm::decodeChunkedBody(body, decoded);
    REQUIRE_FALSE(ok);
}

TEST_CASE("decodeChunkedBody missing final CRLF") {
    std::string decoded;
    // Missing trailing CRLF after zero chunk
    std::string body = "1\r\na\r\n0\r\n";
    bool ok = romm::decodeChunkedBody(body, decoded);
    REQUIRE_FALSE(ok);
}
