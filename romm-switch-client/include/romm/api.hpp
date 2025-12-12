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

} // namespace romm
