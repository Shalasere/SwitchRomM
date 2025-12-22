#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace romm {

// Send entire buffer, handling short writes and EINTR.
bool sendAll(int fd, const char* data, size_t len);

struct ParsedHttpResponse {
    int statusCode{0};
    std::string statusText;
    uint64_t contentLength{0};
    bool chunked{false};
    bool acceptRanges{false};
    std::string headersRaw;
};

// Parse HTTP status line + headers (headerBlock excludes the trailing CRLFCRLF).
// Returns false and sets err on malformed input.
bool parseHttpResponseHeaders(const std::string& headerBlock, ParsedHttpResponse& out, std::string& err);

} // namespace romm
