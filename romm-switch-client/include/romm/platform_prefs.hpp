#pragma once

#include <string>
#include <vector>
#include <unordered_map>

namespace romm {

// Simple, data-driven per-platform preferences for file selection/downloading.
struct PlatformPref {
    std::string mode;                  // "single_best", "bundle_best", or "all_files"
    std::vector<std::string> preferExt; // ordered list of preferred extensions (lowercase, with dot)
    std::vector<std::string> ignoreExt; // extensions to skip outright
};

struct PlatformPrefs {
    int version{1};
    std::string defaultMode{"bundle_best"};
    std::vector<std::string> defaultIgnoreExt;
    std::unordered_map<std::string, PlatformPref> bySlug; // key: platform_fs_slug (preferred)
};

// Load platform preferences from SD/romfs according to config; falls back to defaults if unavailable.
bool loadPlatformPrefs(const std::string& mode, const std::string& sdPath, const std::string& romfsPath,
                       PlatformPrefs& outPrefs, std::string& outError);

// Minimal built-in defaults so the app always has something sane.
PlatformPrefs defaultPlatformPrefs();

} // namespace romm
