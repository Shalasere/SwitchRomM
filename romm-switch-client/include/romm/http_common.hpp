#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace romm {

// Send entire buffer, handling short writes and EINTR.
bool sendAll(int fd, const char* data, size_t len);

struct ParsedHttpResponse {
    int statusCode{0};
    std::string statusText;
    uint64_t contentLength{0};
    bool hasContentLength{false};
    uint64_t contentRangeStart{0};
    uint64_t contentRangeEnd{0};
    bool hasContentRange{false};
    uint64_t contentRangeTotal{0};
    bool hasContentRangeTotal{false};
    bool chunked{false};
    bool acceptRanges{false};
    bool connectionClose{false};
    std::string headersRaw;
    std::string location;
};

// Parse HTTP status line + headers (headerBlock excludes the trailing CRLFCRLF).
// Returns false and sets err on malformed input.
bool parseHttpResponseHeaders(const std::string& headerBlock, ParsedHttpResponse& out, std::string& err);

struct HttpRequestOptions {
    int timeoutSec{0};
    bool keepAlive{false};
    bool decodeChunked{true};
    size_t maxBodyBytes{0}; // 0 = unlimited
    std::atomic<bool>* cancelRequested{nullptr};
    std::atomic<int>* activeSocketFd{nullptr};
};

struct HttpTransaction {
    ParsedHttpResponse parsed;
    std::string body;
};

bool httpRequestBuffered(const std::string& method,
                         const std::string& url,
                         const std::vector<std::pair<std::string, std::string>>& headers,
                         const HttpRequestOptions& options,
                         HttpTransaction& out,
                         std::string& err);

bool httpRequestStreamed(const std::string& method,
                         const std::string& url,
                         const std::vector<std::pair<std::string, std::string>>& headers,
                         const HttpRequestOptions& options,
                         ParsedHttpResponse& outHeaders,
                         const std::function<bool(const char*, size_t)>& onData,
                         std::string& err);

} // namespace romm
