#include "romm/speed_test.hpp"
#include "romm/config.hpp"
#include "romm/status.hpp"
#include "romm/util.hpp"
#include "romm/api.hpp"
#include "romm/logger.hpp"
#include "romm/raii.hpp"
#include "romm/http_common.hpp"
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/time.h>
#include <cstring>
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
    std::string host, portStr, path, perr;
    if (!romm::parseHttpUrl(url, host, portStr, path, perr)) { err = perr; return false; }
    romm::logLine("SpeedTest: target=" + url + " bytes=" + std::to_string(testBytes));

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    int ret = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
    if (ret != 0 || !res) { if (res) freeaddrinfo(res); err = "DNS failed"; return false; }
    romm::UniqueFd sock(socket(res->ai_family, res->ai_socktype, res->ai_protocol));
    if (!sock) { freeaddrinfo(res); err = "Socket failed"; return false; }
    if (timeoutSec > 0) {
        struct timeval tv{}; tv.tv_sec = timeoutSec;
        setsockopt(sock.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    if (connect(sock.fd, res->ai_addr, res->ai_addrlen) != 0) { freeaddrinfo(res); err = "Connect failed"; return false; }
    freeaddrinfo(res);

    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n";
    if (!authBasic.empty()) req += "Authorization: Basic " + authBasic + "\r\n";
    if (testBytes > 0) {
        req += "Range: bytes=0-" + std::to_string(testBytes - 1) + "\r\n";
    }
    req += "\r\n";
    if (!sendAll(sock.fd, req.c_str(), req.size())) { err = "Send failed"; return false; }

    std::string headerAccum;
    constexpr size_t kSpeedTestBuf = 64 * 1024;
    char buf[kSpeedTestBuf];
    bool headersDone = false;
    uint64_t contentLength = 0;
    size_t bodyStart = 0;
    int statusCode = 0;
    while (!headersDone) {
        ssize_t n = recv(sock.fd, buf, sizeof(buf), 0);
        if (n <= 0) { err = "No HTTP headers received"; return false; }
        headerAccum.append(buf, buf + n);
        auto hdrEnd = headerAccum.find("\r\n\r\n");
        if (hdrEnd == std::string::npos) continue;
        headersDone = true;
        bodyStart = hdrEnd + 4;
        std::string headers = headerAccum.substr(0, hdrEnd);
        std::istringstream hs(headers);
        std::string statusLine;
        std::getline(hs, statusLine);
        if (!statusLine.empty() && statusLine.back() == '\r') statusLine.pop_back();
        std::istringstream sl(statusLine);
        std::string httpVer; sl >> httpVer >> statusCode;
        if (!(statusCode == 200 || statusCode == 206)) { err = "HTTP status " + std::to_string(statusCode); return false; }
        std::string line;
        while (std::getline(hs, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto cpos = line.find(':');
            if (cpos == std::string::npos) continue;
            std::string key = line.substr(0, cpos);
            std::string val = line.substr(cpos + 1);
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
            std::string keyLower = key;
            for (auto& c : keyLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (keyLower == "content-length") contentLength = static_cast<uint64_t>(std::strtoull(val.c_str(), nullptr, 10));
        }
    }

    uint64_t target = (contentLength > 0) ? std::min<uint64_t>(contentLength, testBytes) : testBytes;
    uint64_t received = 0;
    auto start = std::chrono::steady_clock::now();
    if (bodyStart < headerAccum.size()) {
        size_t bodyBytes = headerAccum.size() - bodyStart;
        size_t use = static_cast<size_t>(std::min<uint64_t>(bodyBytes, target));
        received += use;
    }
    while (received < target) {
        ssize_t n = recv(sock.fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        uint64_t use = std::min<uint64_t>(static_cast<uint64_t>(n), target - received);
        received += use;
    }
    auto end = std::chrono::steady_clock::now();
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
        status.lastSpeedMbps = mbps;
    }
    return true;
}

} // namespace romm
