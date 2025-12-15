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
    // Platform slug (defaults to switch)
    std::string platform{"switch"};
    // Destination directory on SD for downloads
    std::string downloadDir{"sdmc:/romm_cache/switch"};
    // HTTP timeout (seconds) for network calls
    int httpTimeoutSeconds{30};
    // FAT32-safe split flag (currently not used in downloader logic)
    bool fat32Safe{false};
    // Logging verbosity (debug, info, warn, error)
    std::string logLevel{"info"};
};

bool loadConfig(Config& outCfg, std::string& outError);

#ifdef UNIT_TEST
// Test helper: parse .env-style content from an in-memory string.
bool parseEnvString(const std::string& contents, Config& outCfg, std::string& outError);
#endif

} // namespace romm
