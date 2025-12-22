#pragma once

#include <string>
#include <cstdint>
#include "romm/models.hpp"
#include "romm/config.hpp"

namespace romm {

// Ensure a directory exists, creating it if necessary.
bool ensureDirectory(const std::string& path);
// Check if a file exists.
bool fileExists(const std::string& path);
// Best-effort free-space query for a path (bytes).
uint64_t getFreeSpace(const std::string& path);

// Determine if a game's final output appears to be on disk (ID-suffixed, with/without extension).
bool isGameCompletedOnDisk(const Game& g, const Config& cfg);

} // namespace romm
