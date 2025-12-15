#include "catch.hpp"
#include "api_test_hooks.hpp"
#include "romm/api.hpp"
#include "romm/downloader.hpp"
#include "romm/models.hpp"

namespace {

// Minimal helper to run the mock stream and count bytes.
size_t streamBytes(const std::string& raw, int& statusCode, std::string& err) {
    romm::HttpResponse resp;
    size_t total = 0;
    bool ok = romm::httpRequestStreamMock(raw, resp,
        [&](const char* /*data*/, size_t len) {
            total += len;
            return true;
        },
        err);
    statusCode = resp.statusCode;
    return ok ? total : 0;
}

} // namespace

TEST_CASE("preflight-style header parsing: content-length and accept-ranges") {
    const std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 12345\r\n"
        "Accept-Ranges: bytes\r\n"
        "\r\n";

    int code = 0;
    std::string err;
    size_t total = streamBytes(raw, code, err);

    REQUIRE(total == 0); // no body in preflight mock
    REQUIRE(err.empty());
    REQUIRE(code == 200);
}

TEST_CASE("preflight fallback 206 with content-range") {
    const std::string raw =
        "HTTP/1.1 206 Partial Content\r\n"
        "Content-Range: bytes 0-0/9999\r\n"
        "\r\n";

    int code = 0;
    std::string err;
    size_t total = streamBytes(raw, code, err);

    REQUIRE(total == 0);
    REQUIRE(err.empty());
    REQUIRE(code == 206);
}

TEST_CASE("httpRequestStreamMock short-read vs content-length") {
    // Body is shorter than declared Content-Length => should error.
    const std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 10\r\n"
        "\r\n"
        "short";

    romm::HttpResponse resp;
    size_t total = 0;
    std::string err;
    bool ok = romm::httpRequestStreamMock(raw, resp,
        [&](const char* /*data*/, size_t len) {
            total += len;
            return true;
        },
        err);

    REQUIRE_FALSE(ok);
    REQUIRE(err == "Short read");
    REQUIRE(total == 5);
}

TEST_CASE("httpRequestStreamMock exact content-length passes") {
    const std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
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
    REQUIRE(resp.statusCode == 200);
    REQUIRE(total == 5);
}

TEST_CASE("part planning sanity: number of parts vs size") {
    constexpr uint64_t kPartSize = 0xFFFF0000ULL; // 4 GiB-ish

    auto partsFor = [&](uint64_t size) {
        return (size + kPartSize - 1) / kPartSize;
    };

    REQUIRE(partsFor(0) == 0);
    REQUIRE(partsFor(1) == 1);
    REQUIRE(partsFor(kPartSize) == 1);
    REQUIRE(partsFor(kPartSize + 1) == 2);
    REQUIRE(partsFor(kPartSize * 2) == 2);
    REQUIRE(partsFor(kPartSize * 2 + 1234) == 3);
}

TEST_CASE("parseLengthAndRangesForTest extracts length and ranges") {
    std::string hdrs =
        "Content-Length: 12345\r\n"
        "Accept-Ranges: bytes\r\n";
    bool ranges = false;
    uint64_t len = 0;
    REQUIRE(romm::parseLengthAndRangesForTest(hdrs, ranges, len));
    REQUIRE(ranges);
    REQUIRE(len == 12345);

    hdrs = "Content-Range: bytes 0-0/999\r\n";
    ranges = false; len = 0;
    REQUIRE(romm::parseLengthAndRangesForTest(hdrs, ranges, len));
    REQUIRE(len == 999);
}

TEST_CASE("parseLengthAndRangesForTest fails without length info") {
    std::string hdrs = "Accept-Ranges: bytes\r\n";
    bool ranges = false;
    uint64_t len = 0;
    REQUIRE_FALSE(romm::parseLengthAndRangesForTest(hdrs, ranges, len));
    REQUIRE(len == 0);
}
