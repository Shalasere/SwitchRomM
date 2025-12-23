#pragma once

#include <string>

namespace romm {

struct Config {
    // Base RomM server URL (http only)
    std::string serverUrl;
    // Optional token (currently unused)
    std::string apiToken;
    // Optional Basic auth credentials
    std::string username;
    std::string password;
    // Platform slug (optional; UI drives selection when empty)
    std::string platform{""};
    // Destination directory on SD for downloads (platform/rom subfolders created automatically)
    std::string downloadDir{"sdmc:/romm_cache"};
    // HTTP timeout (seconds) for network calls
    int httpTimeoutSeconds{30};
    // FAT32-safe split flag
    bool fat32Safe{false};
    // Logging verbosity (debug, info, warn, error)
    std::string logLevel{"info"};
    // Optional URL to fetch ~10MB for a quick throughput estimate; blank to skip.
    std::string speedTestUrl;
    // Platform prefs source selection
    std::string platformPrefsMode{"auto"};      // auto | sd | romfs
    std::string platformPrefsPathSd{"sdmc:/switch/SwitchRomM/platform_prefs.json"};
    std::string platformPrefsPathRomfs{"romfs:/platform_prefs.json"};
};

bool loadConfig(Config& outCfg, std::string& outError);

#ifdef UNIT_TEST
// Test helper: parse .env-style content from an in-memory string.
bool parseEnvString(const std::string& contents, Config& outCfg, std::string& outError);
#endif

} // namespace romm
