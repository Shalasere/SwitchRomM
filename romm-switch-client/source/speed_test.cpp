#include "romm/speed_test.hpp"
#include "romm/config.hpp"
#include "romm/status.hpp"
#include "romm/util.hpp"
#include "romm/logger.hpp"
#include "romm/http_common.hpp"
#include <chrono>
#include <mutex>

namespace romm {

// Measure throughput by downloading up to testBytes (discarded) and return MB/s.
static bool measureSpeed(const std::string& url,
                         const std::string& authBasic,
                         int timeoutSec,
                         uint64_t testBytes,
                         double& mbpsOut,
                         std::string& err) {
    mbpsOut = 0.0;
    err.clear();
    romm::logLine("SpeedTest: target=" + url + " bytes=" + std::to_string(testBytes));
    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("Accept", "*/*");
    if (!authBasic.empty()) headers.emplace_back("Authorization", "Basic " + authBasic);
    if (testBytes > 0) {
        headers.emplace_back("Range", "bytes=0-" + std::to_string(testBytes - 1));
    }

    HttpRequestOptions options;
    options.timeoutSec = timeoutSec;
    options.keepAlive = false;
    options.decodeChunked = false;

    ParsedHttpResponse parsed;
    uint64_t received = 0;
    auto start = std::chrono::steady_clock::now();
    bool ok = romm::httpRequestStreamed(
        "GET", url, headers, options, parsed,
        [&](const char* /*data*/, size_t len) {
            received += static_cast<uint64_t>(len);
            return true;
        },
        err);
    auto end = std::chrono::steady_clock::now();
    if (!ok) return false;

    if (!(parsed.statusCode == 200 || parsed.statusCode == 206)) {
        err = "HTTP status " + std::to_string(parsed.statusCode);
        return false;
    }
    if (testBytes > 0 && received > testBytes) {
        received = testBytes;
    }

    double secs = std::chrono::duration<double>(end - start).count();
    if (secs <= 0.0) secs = 1e-6;
    mbpsOut = (received / (1024.0 * 1024.0)) / secs; // MB/s
    if (received == 0) { err = "No data received"; return false; }
    return true;
}

bool runSpeedTest(const Config& cfg, Status& status, uint64_t testBytes, std::string& outError) {
    outError.clear();
    if (cfg.speedTestUrl.empty()) {
        outError = "No speed test URL set";
        return false;
    }
    std::string auth;
    if (!cfg.username.empty() || !cfg.password.empty()) {
        auth = romm::util::base64Encode(cfg.username + ":" + cfg.password);
    }
    double mbps = 0.0;
    if (!measureSpeed(cfg.speedTestUrl, auth, cfg.httpTimeoutSeconds, testBytes, mbps, outError)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        status.lastSpeedMBps = mbps;
    }
    return true;
}

} // namespace romm
