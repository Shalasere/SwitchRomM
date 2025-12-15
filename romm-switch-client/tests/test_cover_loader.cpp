#include "catch.hpp"
#include "romm/cover_loader.hpp"
#include "romm/config.hpp"
#include <atomic>
#include <thread>
#include <chrono>

namespace {
// Minimal 1x1 PNG (red pixel) for decode tests.
// Valid tiny 1x1 RGBA PNG (transparent pixel).
const unsigned char kPng1x1[] = {
    0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,
    0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
    0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
    0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,0x89,
    0x00,0x00,0x00,0x0B,0x49,0x44,0x41,0x54,
    0x08,0xD7,0x63,0xF8,0xCF,0xC0,0x00,0x00,
    0x03,0x01,0x01,0x00,0x18,0xDD,0x8D,0x18,
    0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,
    0xAE,0x42,0x60,0x82
};

bool fetchOk(const std::string& url, const romm::Config&, std::vector<unsigned char>& outData, std::string& err) {
    (void)url;
    err.clear();
    outData.assign(kPng1x1, kPng1x1 + sizeof(kPng1x1));
    return true;
}

bool fetchFail(const std::string& url, const romm::Config&, std::vector<unsigned char>& outData, std::string& err) {
    (void)url;
    outData.clear();
    err = "fetch error";
    return false;
}

// Wait briefly for a result; returns true if a result was seen.
bool waitForResult(romm::CoverLoader& loader, std::optional<romm::CoverResult>& out) {
    for (int i = 0; i < 20; ++i) {
        out = loader.poll();
        if (out) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
} // namespace

TEST_CASE("CoverLoader fetches and decodes a 1x1 PNG") {
    romm::CoverLoader loader;
    loader.start(fetchOk);
    romm::Config cfg;
    romm::CoverJob job{ "http://example/cover.png", "Test", cfg };
    loader.request(job, /*currentTextureUrl*/"");

    std::optional<romm::CoverResult> res;
    REQUIRE(waitForResult(loader, res));
    REQUIRE(res->ok);
    REQUIRE(res->w == 1);
    REQUIRE(res->h == 1);
    REQUIRE(res->url == job.url);
    loader.stop();
}

TEST_CASE("CoverLoader reports fetch failure") {
    romm::CoverLoader loader;
    loader.start(fetchFail);
    romm::Config cfg;
    romm::CoverJob job{ "http://bad/cover.png", "Bad", cfg };
    loader.request(job, "");

    std::optional<romm::CoverResult> res;
    REQUIRE(waitForResult(loader, res));
    REQUIRE_FALSE(res->ok);
    REQUIRE_FALSE(res->error.empty());
    loader.stop();
}

TEST_CASE("CoverLoader dedupes current texture URL") {
    romm::CoverLoader loader;
    // No worker start needed; request should early-exit.
    romm::Config cfg;
    romm::CoverJob job{ "http://example/cover.png", "Test", cfg };
    loader.request(job, job.url); // should be ignored
    std::optional<romm::CoverResult> res = loader.poll();
    REQUIRE_FALSE(res.has_value());
    loader.stop();
}

TEST_CASE("CoverLoader keeps latest request and drops empty URL") {
    romm::CoverLoader loader;
    loader.start(fetchOk);
    romm::Config cfg;
    romm::CoverJob emptyJob{ "", "NoUrl", cfg };
    loader.request(emptyJob, "");
    // Immediately queue a valid job; the empty URL should be ignored.
    romm::CoverJob goodJob{ "http://example/2.png", "Test2", cfg };
    loader.request(goodJob, "");
    std::optional<romm::CoverResult> res;
    REQUIRE(waitForResult(loader, res));
    REQUIRE(res->ok);
    REQUIRE(res->url == goodJob.url);
    loader.stop();
}

TEST_CASE("CoverLoader replaces queued job with newer URL") {
    romm::CoverLoader loader;
    loader.start(fetchOk);
    romm::Config cfg;
    romm::CoverJob first{ "http://example/first.png", "First", cfg };
    romm::CoverJob second{ "http://example/second.png", "Second", cfg };
    loader.request(first, "");
    // Immediately queue another; only the latest should be processed.
    loader.request(second, "");
    std::optional<romm::CoverResult> res;
    REQUIRE(waitForResult(loader, res));
    REQUIRE(res->ok);
    REQUIRE(res->url == second.url);
    loader.stop();
}
