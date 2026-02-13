#define CATCH_CONFIG_MAIN
#include "catch.hpp"
#include <vector>
#include "api_test_hooks.hpp"
#include "romm/api.hpp"
#include "romm/util.hpp"

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

TEST_CASE("parseHttpUrl supports https") {
    std::string host, port, path, err;
    bool ok = romm::parseHttpUrl("https://good.com", host, port, path, err);
    REQUIRE(ok);
    REQUIRE(host == "good.com");
    REQUIRE(port == "443");
    REQUIRE(path == "/");
}

TEST_CASE("parseHttpUrl rejects unsupported scheme") {
    std::string host, port, path, err;
    bool ok = romm::parseHttpUrl("ftp://bad.com", host, port, path, err);
    REQUIRE_FALSE(ok);
    REQUIRE(err == "URL must start with http:// or https://");
}

TEST_CASE("parseHttpUrl missing host fails") {
    std::string host, port, path, err;
    bool ok = romm::parseHttpUrl("http:///path", host, port, path, err);
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

TEST_CASE("decodeChunkedBody incomplete data fails") {
    std::string decoded;
    // Declares 4 bytes, only 2 provided
    std::string body = "4\r\nWi\r\n0\r\n\r\n";
    bool ok = romm::decodeChunkedBody(body, decoded);
    REQUIRE_FALSE(ok);
}

TEST_CASE("decodeChunkedBody bad CRLF after chunk") {
    std::string decoded;
    // No CRLF after data
    std::string body = "1\r\naXX0\r\n\r\n";
    bool ok = romm::decodeChunkedBody(body, decoded);
    REQUIRE_FALSE(ok);
}

TEST_CASE("base64Encode matches expected") {
    REQUIRE(romm::util::base64Encode("user:pass") == "dXNlcjpwYXNz");
    REQUIRE(romm::util::base64Encode("") == "");
}

TEST_CASE("urlEncode handles safe and unsafe chars") {
    REQUIRE(romm::util::urlEncode("simple") == "simple");
    REQUIRE(romm::util::urlEncode("Hello World") == "Hello%20World");
    REQUIRE(romm::util::urlEncode("a+b/c") == "a%2Bb%2Fc");
}

TEST_CASE("httpRequestStreamMock streams without buffering") {
    // Simulate a response where headers and part of the body arrive together,
    // then additional body chunks follow.
    std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 15\r\n"
        "\r\n"
        "hello world 123";

    romm::HttpResponse resp;
    size_t total = 0;
    std::string err;
    bool ok = romm::httpRequestStreamMock(raw, resp,
        [&](const char* /*data*/, size_t len) {
            total += len;
            return true;
        },
        err);
    REQUIRE(ok);
    REQUIRE(err.empty());
    REQUIRE(resp.statusCode == 200);
    REQUIRE(total == 15);
    REQUIRE(resp.body.empty()); // streamed, not buffered into resp.body
}

TEST_CASE("parseGames builds cover URL from path_cover_small") {
    const std::string body = R"([{
        "id": "1",
        "name": "Test Game",
        "fs_size_bytes": 1234,
        "fs_name": "test.xci",
        "path_cover_small": "/assets/romm/resources/roms/19/4076/cover/small.png?ts=2025-12-15 09:33:40"
    }])";
    std::vector<romm::Game> games;
    std::string err;
    bool ok = romm::parseGamesTest(body, "19", "http://example.com", games, err);
    REQUIRE(ok);
    REQUIRE(err.empty());
    REQUIRE(games.size() == 1);
    REQUIRE(games[0].coverUrl == "http://example.com/assets/romm/resources/roms/19/4076/cover/small.png?ts=2025-12-15%2009:33:40");
}

TEST_CASE("parseGames preserves absolute cover_url and encodes spaces") {
    const std::string body = R"([{
        "id": "2",
        "name": "Absolute",
        "fs_size_bytes": 1,
        "fs_name": "a.xci",
        "cover_url": "http://remote/img path.png?x=1 2"
    }])";
    std::vector<romm::Game> games;
    std::string err;
    bool ok = romm::parseGamesTest(body, "19", "http://example.com", games, err);
    REQUIRE(ok);
    REQUIRE(err.empty());
    REQUIRE(games.size() == 1);
    REQUIRE(games[0].coverUrl == "http://remote/img%20path.png?x=1%202");
}

TEST_CASE("parsePlatforms preserves numeric ids without decimal suffix") {
    const std::string body = R"([{
        "id": 2,
        "display_name": "Nintendo Switch",
        "slug": "switch",
        "rom_count": 123
    }])";
    std::vector<romm::Platform> plats;
    std::string err;
    bool ok = romm::parsePlatformsTest(body, plats, err);
    REQUIRE(ok);
    REQUIRE(err.empty());
    REQUIRE(plats.size() == 1);
    REQUIRE(plats[0].id == "2");
    REQUIRE(plats[0].slug == "switch");
    REQUIRE(plats[0].romCount == 123);
}

TEST_CASE("parseGames preserves numeric ids without decimal suffix") {
    const std::string body = R"([{
        "id": 19,
        "name": "Numeric Id Game",
        "platform_id": 2,
        "platform_slug": "switch",
        "fs_size_bytes": 10,
        "fs_name": "a.xci"
    }])";
    std::vector<romm::Game> games;
    std::string err;
    bool ok = romm::parseGamesTest(body, "2", "http://example.com", games, err);
    REQUIRE(ok);
    REQUIRE(err.empty());
    REQUIRE(games.size() == 1);
    REQUIRE(games[0].id == "19");
    REQUIRE(games[0].platformId == "2");
    REQUIRE(games[0].platformSlug == "switch");
}

TEST_CASE("parseGames preserves UTF-8 titles in model data") {
    const std::string body = u8R"([{
        "id": "501",
        "name": "Pokémon — Ōkami édition",
        "platform_id": "2",
        "platform_slug": "switch",
        "fs_size_bytes": 10,
        "fs_name": "utf8.xci"
    }])";
    std::vector<romm::Game> games;
    std::string err;
    bool ok = romm::parseGamesTest(body, "2", "http://example.com", games, err);
    REQUIRE(ok);
    REQUIRE(err.empty());
    REQUIRE(games.size() == 1);
    REQUIRE(games[0].title == u8"Pokémon — Ōkami édition");
}

TEST_CASE("parseGames accepts object payload with results array") {
    const std::string body = R"({
        "total": 2,
        "results": [
            {"id":"10","name":"Alpha","platform_id":"2","platform_slug":"switch","fs_size_bytes":1,"fs_name":"a.xci"},
            {"id":"11","name":"Beta","platform_id":"2","platform_slug":"switch","fs_size_bytes":2,"fs_name":"b.xci"}
        ]
    })";
    std::vector<romm::Game> games;
    std::string err;
    bool ok = romm::parseGamesTest(body, "2", "http://example.com", games, err);
    REQUIRE(ok);
    REQUIRE(err.empty());
    REQUIRE(games.size() == 2);
    REQUIRE(games[0].title == "Alpha");
    REQUIRE(games[1].title == "Beta");
}

TEST_CASE("identifiers digest is stable across item order") {
    const std::string a = R"([{"id":"1","updated_at":"2026-01-01"},{"id":"2","updated_at":"2026-01-02"}])";
    const std::string b = R"([{"id":"2","updated_at":"2026-01-02"},{"id":"1","updated_at":"2026-01-01"}])";
    std::string da, db, err;
    bool oka = romm::parseIdentifiersDigestTest(a, da, err);
    REQUIRE(oka);
    REQUIRE(err.empty());
    bool okb = romm::parseIdentifiersDigestTest(b, db, err);
    REQUIRE(okb);
    REQUIRE(err.empty());
    REQUIRE_FALSE(da.empty());
    REQUIRE(da == db);
}
