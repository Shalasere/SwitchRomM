#include "catch.hpp"
#include "api_test_hooks.hpp"
#include "romm/api.hpp"

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
