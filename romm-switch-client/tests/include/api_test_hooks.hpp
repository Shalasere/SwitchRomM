#pragma once

#include <functional>
#include <string>
#include <vector>
#include "romm/models.hpp"

namespace romm {
// Helpers are defined in source/api.cpp (not exposed via a header in production).
bool parseHttpUrl(const std::string& url,
                  std::string& host,
                  std::string& port,
                  std::string& path,
                  std::string& err);

bool decodeChunkedBody(const std::string& body, std::string& decoded);

struct HttpResponse;
bool httpRequestStreamMock(const std::string& rawResponse,
                           HttpResponse& resp,
                           const std::function<bool(const char*, size_t)>& onData,
                           std::string& err);

bool parseGamesTest(const std::string& body,
                    const std::string& platformId,
                    const std::string& serverUrl,
                    std::vector<Game>& outGames,
                    std::string& err);
} // namespace romm
