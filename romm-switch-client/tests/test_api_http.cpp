#include "catch.hpp"
#include "api_test_hooks.hpp"
#include "romm/api.hpp"
#include "romm/http_common.hpp"

TEST_CASE("httpRequestStreamMock parses status and headers") {
    const std::string raw =
        "HTTP/1.1 206 Partial Content\r\n"
        "Content-Length: 5\r\n"
        "X-Test: ok\r\n"
        "\r\n"
        "hello";

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
    REQUIRE(resp.statusCode == 206);
    REQUIRE(resp.statusText == "Partial Content");
    REQUIRE(total == 5);
}

TEST_CASE("httpRequestStreamMock rejects chunked transfer") {
    const std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "4\r\nTest\r\n0\r\n\r\n";

    romm::HttpResponse resp;
    std::string err;
    bool ok = romm::httpRequestStreamMock(raw, resp,
        [&](const char* /*data*/, size_t len) {
            (void)len;
            return true;
        },
        err);

    REQUIRE_FALSE(ok);
    REQUIRE_FALSE(err.empty());
}

TEST_CASE("httpRequestStreamMock detects short read against Content-Length") {
    const std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 10\r\n"
        "\r\n"
        "short";

    romm::HttpResponse resp;
    std::string err;
    bool ok = romm::httpRequestStreamMock(raw, resp,
        [&](const char* /*data*/, size_t /*len*/) {
            return true;
        },
        err);

    REQUIRE_FALSE(ok);
    REQUIRE(err == "Short read");
}

TEST_CASE("httpRequestStreamMock propagates sink abort") {
    const std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";

    romm::HttpResponse resp;
    std::string err;
    bool ok = romm::httpRequestStreamMock(raw, resp,
        [&](const char* /*data*/, size_t /*len*/) {
            return false; // simulate sink abort (e.g., disk error)
        },
        err);

    REQUIRE_FALSE(ok);
    REQUIRE(err == "Sink aborted");
}

TEST_CASE("parseHttpResponseHeaders preserves explicit zero Content-Length") {
    const std::string headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 0\r\n"
        "Connection: keep-alive";
    romm::ParsedHttpResponse parsed;
    std::string err;
    bool ok = romm::parseHttpResponseHeaders(headers, parsed, err);
    REQUIRE(ok);
    REQUIRE(err.empty());
    REQUIRE(parsed.hasContentLength);
    REQUIRE(parsed.contentLength == 0);
}

TEST_CASE("parseHttpResponseHeaders rejects conflicting Content-Length headers") {
    const std::string headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 10\r\n"
        "Content-Length: 11";
    romm::ParsedHttpResponse parsed;
    std::string err;
    bool ok = romm::parseHttpResponseHeaders(headers, parsed, err);
    REQUIRE_FALSE(ok);
    REQUIRE(err == "Conflicting Content-Length headers");
}

TEST_CASE("parseHttpResponseHeaders parses Content-Range span and total") {
    const std::string headers =
        "HTTP/1.1 206 Partial Content\r\n"
        "Content-Range: bytes 5-9/20\r\n"
        "Content-Length: 5";
    romm::ParsedHttpResponse parsed;
    std::string err;
    bool ok = romm::parseHttpResponseHeaders(headers, parsed, err);
    REQUIRE(ok);
    REQUIRE(err.empty());
    REQUIRE(parsed.hasContentRange);
    REQUIRE(parsed.contentRangeStart == 5);
    REQUIRE(parsed.contentRangeEnd == 9);
    REQUIRE(parsed.hasContentRangeTotal);
    REQUIRE(parsed.contentRangeTotal == 20);
}

TEST_CASE("parseHttpResponseHeaders parses Content-Range with wildcard total") {
    const std::string headers =
        "HTTP/1.1 206 Partial Content\r\n"
        "Content-Range: bytes 10-19/*\r\n"
        "Content-Length: 10";
    romm::ParsedHttpResponse parsed;
    std::string err;
    bool ok = romm::parseHttpResponseHeaders(headers, parsed, err);
    REQUIRE(ok);
    REQUIRE(err.empty());
    REQUIRE(parsed.hasContentRange);
    REQUIRE(parsed.contentRangeStart == 10);
    REQUIRE(parsed.contentRangeEnd == 19);
    REQUIRE_FALSE(parsed.hasContentRangeTotal);
}

TEST_CASE("parseHttpResponseHeaders parses Connection close and Location") {
    const std::string headers =
        "HTTP/1.1 302 Found\r\n"
        "Connection: close\r\n"
        "Location: https://example.com/new";
    romm::ParsedHttpResponse parsed;
    std::string err;
    bool ok = romm::parseHttpResponseHeaders(headers, parsed, err);
    REQUIRE(ok);
    REQUIRE(err.empty());
    REQUIRE(parsed.connectionClose);
    REQUIRE(parsed.location == "https://example.com/new");
}
