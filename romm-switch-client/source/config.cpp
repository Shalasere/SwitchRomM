#include "romm/config.hpp"
#include "romm/logger.hpp"
#include "mini/json.hpp"
#include <fstream>
#include <cctype>
#include <sstream>

namespace romm {

namespace {
void setConfigError(std::string& outError,
                    ErrorInfo* outInfo,
                    const std::string& detail,
                    ErrorCode code,
                    const char* userMessage) {
    outError = detail;
    if (!outInfo) return;
    outInfo->category = ErrorCategory::Config;
    outInfo->code = code;
    outInfo->httpStatus = 0;
    outInfo->retryable = false;
    outInfo->userMessage = userMessage;
    outInfo->detail = detail;
}
}

static std::string toLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static void trim(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    s = s.substr(i);
}

// Strip trailing inline comments for dotenv-style parsing.
// Rules (pragmatic):
// - Full-line comments are handled elsewhere (# or ; after trimming).
// - For unquoted values: treat " #..." or " ;..." (comment delimiter preceded by whitespace) as a comment.
//   This preserves values like "abc#123" (no whitespace).
// - For quoted values: capture the quoted string and ignore any trailing " #..." / " ;...".
static void stripInlineComment(std::string& val) {
    trim(val);
    if (val.empty()) return;

    if (val.front() == '"') {
        // Parse a simple quoted value with basic backslash escaping.
        std::string out;
        out.reserve(val.size());
        bool escaped = false;
        size_t i = 1;
        for (; i < val.size(); ++i) {
            char c = val[i];
            if (escaped) {
                out.push_back(c);
                escaped = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                continue;
            }
            if (c == '"') {
                // End quote.
                ++i;
                break;
            }
            out.push_back(c);
        }
        val = out;
        (void)i;
        return;
    }

    // Unquoted: strip a comment marker if preceded by whitespace.
    for (size_t i = 0; i < val.size(); ++i) {
        char c = val[i];
        if ((c == '#' || c == ';') && i > 0 && std::isspace(static_cast<unsigned char>(val[i - 1]))) {
            size_t cut = i;
            while (cut > 0 && std::isspace(static_cast<unsigned char>(val[cut - 1]))) cut--;
            val = val.substr(0, cut);
            trim(val);
            return;
        }
    }
}

static bool parseEnvStream(std::istream& in, Config& outCfg) {
    std::string line;
    while (std::getline(in, line)) {
        trim(line);
        if (line.empty()) continue;

        // Allow common "export KEY=VALUE" style lines.
        if (line.rfind("export ", 0) == 0) {
            line = line.substr(7);
            trim(line);
            if (line.empty()) continue;
        }

        if (line[0] == '#' || line[0] == ';') continue;
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string key = toLower(line.substr(0, pos));
        std::string val = line.substr(pos + 1);
        trim(key); trim(val);
        stripInlineComment(val);
        if (key == "server_url") outCfg.serverUrl = val;
        else if (key == "api_token") outCfg.apiToken = val;
        else if (key == "username") outCfg.username = val;
        else if (key == "password") outCfg.password = val;
        else if (key == "platform") outCfg.platform = val;
        else if (key == "download_dir") outCfg.downloadDir = val;
        else if (key == "http_timeout_seconds") outCfg.httpTimeoutSeconds = std::atoi(val.c_str());
        else if (key == "fat32_safe") {
            std::string v = toLower(val);
            outCfg.fat32Safe = (v == "1" || v == "true" || v == "yes");
        } else if (key == "log_level") outCfg.logLevel = toLower(val);
        else if (key == "speed_test_url") outCfg.speedTestUrl = val;
        else if (key == "platform_prefs_mode") outCfg.platformPrefsMode = val;
        else if (key == "platform_prefs_sd") outCfg.platformPrefsPathSd = val;
        else if (key == "platform_prefs_romfs") outCfg.platformPrefsPathRomfs = val;
    }
    return true;
}

static bool parseEnv(const std::string& path, Config& outCfg) {
    std::ifstream f(path);
    if (!f) return false;
    return parseEnvStream(f, outCfg);
}

constexpr int kConfigSchemaLegacy = 0;
constexpr int kConfigSchemaCurrent = 1;

static void aliasKeyIfMissing(mini::Object& obj, const char* from, const char* to) {
    auto fromIt = obj.find(from);
    if (fromIt == obj.end()) return;
    if (obj.find(to) != obj.end()) return;
    obj[to] = fromIt->second;
}

static void migrateSchema0To1(mini::Object& obj) {
    // Support older/alternate key styles while converging on snake_case JSON keys.
    aliasKeyIfMissing(obj, "SERVER_URL", "server_url");
    aliasKeyIfMissing(obj, "API_TOKEN", "api_token");
    aliasKeyIfMissing(obj, "USERNAME", "username");
    aliasKeyIfMissing(obj, "PASSWORD", "password");
    aliasKeyIfMissing(obj, "PLATFORM", "platform");
    aliasKeyIfMissing(obj, "DOWNLOAD_DIR", "download_dir");
    aliasKeyIfMissing(obj, "HTTP_TIMEOUT_SECONDS", "http_timeout_seconds");
    aliasKeyIfMissing(obj, "FAT32_SAFE", "fat32_safe");
    aliasKeyIfMissing(obj, "LOG_LEVEL", "log_level");
    aliasKeyIfMissing(obj, "SPEED_TEST_URL", "speed_test_url");
    aliasKeyIfMissing(obj, "PLATFORM_PREFS_MODE", "platform_prefs_mode");
    aliasKeyIfMissing(obj, "PLATFORM_PREFS_SD", "platform_prefs_sd");
    aliasKeyIfMissing(obj, "PLATFORM_PREFS_ROMFS", "platform_prefs_romfs");

    aliasKeyIfMissing(obj, "serverUrl", "server_url");
    aliasKeyIfMissing(obj, "apiToken", "api_token");
    aliasKeyIfMissing(obj, "downloadDir", "download_dir");
    aliasKeyIfMissing(obj, "httpTimeoutSeconds", "http_timeout_seconds");
    aliasKeyIfMissing(obj, "fat32Safe", "fat32_safe");
    aliasKeyIfMissing(obj, "logLevel", "log_level");
    aliasKeyIfMissing(obj, "speedTestUrl", "speed_test_url");
    aliasKeyIfMissing(obj, "platformPrefsMode", "platform_prefs_mode");
    aliasKeyIfMissing(obj, "platformPrefsSd", "platform_prefs_sd");
    aliasKeyIfMissing(obj, "platformPrefsRomfs", "platform_prefs_romfs");

    aliasKeyIfMissing(obj, "platform_id", "platform");
    aliasKeyIfMissing(obj, "download_path", "download_dir");
    aliasKeyIfMissing(obj, "timeout_seconds", "http_timeout_seconds");
    aliasKeyIfMissing(obj, "fat32_split", "fat32_safe");
}

static bool readSchemaVersion(const mini::Object& obj, int& outVersion, std::string& outError) {
    outVersion = kConfigSchemaLegacy; // missing schema_version implies legacy schema.
    auto it = obj.find("schema_version");
    if (it == obj.end()) return true;
    if (it->second.type != mini::Value::Type::Number) {
        outError = "Invalid config JSON: schema_version must be a number.";
        return false;
    }
    if (it->second.number < 0) {
        outError = "Invalid config JSON: schema_version must be non-negative.";
        return false;
    }
    outVersion = static_cast<int>(it->second.number);
    return true;
}

static bool migrateSchema(mini::Object& obj, int& schemaVersion, std::string& outError) {
    if (schemaVersion > kConfigSchemaCurrent) {
        outError = "Unsupported config schema_version " + std::to_string(schemaVersion) +
                   "; max supported is " + std::to_string(kConfigSchemaCurrent) + ".";
        return false;
    }

    while (schemaVersion < kConfigSchemaCurrent) {
        if (schemaVersion == 0) {
            migrateSchema0To1(obj);
            schemaVersion = 1;
            continue;
        }
        outError = "No migration available for config schema_version " + std::to_string(schemaVersion) + ".";
        return false;
    }
    return true;
}

static bool parseJsonObject(mini::Object& obj, Config& outCfg, std::string& outError) {
    int schemaVersion = kConfigSchemaLegacy;
    if (!readSchemaVersion(obj, schemaVersion, outError)) {
        return false;
    }
    if (!migrateSchema(obj, schemaVersion, outError)) {
        return false;
    }

    outCfg.schemaVersion = schemaVersion;

    auto getStr = [&](const char* key, std::string& out) {
        auto it = obj.find(key);
        if (it != obj.end() && it->second.type == mini::Value::Type::String) {
            out = it->second.str;
        }
    };
    auto getInt = [&](const char* key, int& out) {
        auto it = obj.find(key);
        if (it != obj.end() && it->second.type == mini::Value::Type::Number) {
            out = it->second.number;
        }
    };
    auto getBool = [&](const char* key, bool& out) {
        auto it = obj.find(key);
        if (it != obj.end() && it->second.type == mini::Value::Type::Bool) {
            out = it->second.boolean;
        }
    };
    getStr("server_url", outCfg.serverUrl);
    getStr("api_token", outCfg.apiToken);
    getStr("username", outCfg.username);
    getStr("password", outCfg.password);
    getStr("platform", outCfg.platform);
    getStr("download_dir", outCfg.downloadDir);
    getInt("http_timeout_seconds", outCfg.httpTimeoutSeconds);
    getBool("fat32_safe", outCfg.fat32Safe);
    {
        std::string lvl;
        getStr("log_level", lvl);
        if (!lvl.empty()) outCfg.logLevel = toLower(lvl);
    }
    getStr("speed_test_url", outCfg.speedTestUrl);
    getStr("platform_prefs_mode", outCfg.platformPrefsMode);
    getStr("platform_prefs_sd", outCfg.platformPrefsPathSd);
    getStr("platform_prefs_romfs", outCfg.platformPrefsPathRomfs);
    return true;
}

static bool parseJson(const std::string& path, Config& outCfg, std::string& outError) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file) return false;
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    mini::Object obj;
    if (!mini::parse(content, obj)) {
        outError = "Invalid config JSON.";
        return true; // file found but bad
    }
    if (!parseJsonObject(obj, outCfg, outError)) {
        return true; // file found but invalid/incompatible
    }
    return true;
}

bool loadConfig(Config& outCfg, std::string& outError, ErrorInfo* outInfo) {
    if (outInfo) *outInfo = ErrorInfo{};
    outError.clear();
    const std::string envPath = "sdmc:/switch/romm_switch_client/.env";
    const std::string jsonPath = "sdmc:/switch/romm_switch_client/config.json";

    bool envTried = parseEnv(envPath, outCfg);
    bool jsonTried = parseJson(jsonPath, outCfg, outError);

    if (!envTried && !jsonTried) {
        setConfigError(outError, outInfo,
                       "Missing config: place .env at sdmc:/switch/romm_switch_client/.env",
                       ErrorCode::ConfigMissing,
                       "Configuration file is missing.");
        return false;
    }

    if (outCfg.serverUrl.empty() || outCfg.downloadDir.empty()) {
        if (outError.empty()) {
            setConfigError(outError, outInfo,
                           "Config missing server_url or download_dir.",
                           ErrorCode::MissingRequiredField,
                           "Required config field is missing.");
        } else if (outInfo) {
            *outInfo = classifyError(outError, ErrorCategory::Config);
        }
        return false;
    }

    // Enforce http-only for now (TLS not implemented).
    if (outCfg.serverUrl.rfind("https://", 0) == 0) {
        setConfigError(outError, outInfo,
                       "https:// not supported; use http:// or a local TLS terminator.",
                       ErrorCode::ConfigUnsupported,
                       "HTTPS is not supported in this build.");
        return false;
    }

    outError.clear();
    return true;
}

#ifdef UNIT_TEST
bool parseEnvString(const std::string& contents, Config& outCfg, std::string& outError, ErrorInfo* outInfo) {
    if (outInfo) *outInfo = ErrorInfo{};
    outError.clear();
    outCfg = Config{};
    // Force required fields to be explicitly provided in tests.
    outCfg.downloadDir.clear();
    std::istringstream iss(contents);
    if (!parseEnvStream(iss, outCfg)) {
        setConfigError(outError, outInfo, "Failed to parse env string",
                       ErrorCode::ConfigInvalid,
                       "Configuration format is invalid.");
        return false;
    }
    // mimic normal flow: basic validation
    if (outCfg.serverUrl.empty() || outCfg.downloadDir.empty()) {
        setConfigError(outError, outInfo, "Config missing server_url or download_dir.",
                       ErrorCode::MissingRequiredField,
                       "Required config field is missing.");
        return false;
    }
    if (outCfg.serverUrl.rfind("https://", 0) == 0) {
        setConfigError(outError, outInfo,
                       "https:// not supported; use http:// or a local TLS terminator.",
                       ErrorCode::ConfigUnsupported,
                       "HTTPS is not supported in this build.");
        return false;
    }
    return true;
}

bool parseJsonString(const std::string& contents, Config& outCfg, std::string& outError, ErrorInfo* outInfo) {
    if (outInfo) *outInfo = ErrorInfo{};
    outError.clear();
    outCfg = Config{};
    // Force required fields to be explicitly provided in tests.
    outCfg.downloadDir.clear();

    mini::Object obj;
    if (!mini::parse(contents, obj)) {
        setConfigError(outError, outInfo, "Invalid config JSON.",
                       ErrorCode::ConfigInvalid,
                       "Configuration format is invalid.");
        return false;
    }
    if (!parseJsonObject(obj, outCfg, outError)) {
        if (outInfo) *outInfo = classifyError(outError, ErrorCategory::Config);
        return false;
    }
    if (outCfg.serverUrl.empty() || outCfg.downloadDir.empty()) {
        setConfigError(outError, outInfo, "Config missing server_url or download_dir.",
                       ErrorCode::MissingRequiredField,
                       "Required config field is missing.");
        return false;
    }
    if (outCfg.serverUrl.rfind("https://", 0) == 0) {
        setConfigError(outError, outInfo,
                       "https:// not supported; use http:// or a local TLS terminator.",
                       ErrorCode::ConfigUnsupported,
                       "HTTPS is not supported in this build.");
        return false;
    }
    return true;
}
#endif

} // namespace romm
