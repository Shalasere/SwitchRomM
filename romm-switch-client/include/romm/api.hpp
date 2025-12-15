#pragma once

#include "romm/config.hpp"
#include "romm/status.hpp"
#include <string>
#include <functional>

namespace romm {

struct HttpResponse {
    int         statusCode   = 0;
    std::string statusText;
    std::string headersRaw;
    std::string body;
};

// Stubbed API client; replace with real HTTP soon.
bool fetchPlatforms(const Config& cfg, Status& status, std::string& outError);
bool fetchGamesForPlatform(const Config& cfg, const std::string& platformId, Status& status, std::string& outError);
bool fetchBinary(const Config& cfg, const std::string& url, std::string& outData, std::string& outError);
bool enrichGameWithFiles(const Config& cfg, Game& g, std::string& outError);
// Shared URL parser (http:// only).
bool parseHttpUrl(const std::string& url,
                  std::string& host,
                  std::string& portStr,
                  std::string& path,
                  std::string& err);
// Exposed for tests and downloader reuse.
bool decodeChunkedBody(const std::string& body, std::string& decoded);

// Stream an HTTP request body to a sink without buffering the whole payload in memory.
// Note: In UNIT_TEST builds this is stubbed and not executed over the network.
bool httpRequestStream(const std::string& method,
                       const std::string& url,
                       const std::vector<std::pair<std::string, std::string>>& extraHeaders,
                       int timeoutSec,
                       struct HttpResponse& resp,
                       const std::function<bool(const char*, size_t)>& onData,
                       std::string& err);

#ifdef UNIT_TEST
// Test helper: parse a raw HTTP response string and stream its body to a sink.
bool httpRequestStreamMock(const std::string& rawResponse,
                           HttpResponse& resp,
                           const std::function<bool(const char*, size_t)>& onData,
                           std::string& err);
#endif

} // namespace romm
