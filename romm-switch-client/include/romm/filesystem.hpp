#pragma once

#include <string>
#include <cstdint>

namespace romm {

// Ensure a directory exists, creating it if necessary.
bool ensureDirectory(const std::string& path);
// Check if a file exists.
bool fileExists(const std::string& path);
// Best-effort free-space query for a path (bytes).
uint64_t getFreeSpace(const std::string& path);

} // namespace romm
