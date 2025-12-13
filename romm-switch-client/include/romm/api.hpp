#pragma once

#include "romm/config.hpp"
#include "romm/status.hpp"
#include <string>

namespace romm {

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

} // namespace romm
