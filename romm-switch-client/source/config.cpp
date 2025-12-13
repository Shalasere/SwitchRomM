#include "romm/config.hpp"
#include "romm/logger.hpp"
#include "mini/json.hpp"
#include <fstream>
#include <cctype>

namespace romm {

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

static bool parseEnv(const std::string& path, Config& outCfg) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (line[0] == '#' || line[0] == ';') continue;
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string key = toLower(line.substr(0, pos));
        std::string val = line.substr(pos + 1);
        trim(key); trim(val);
        if (!val.empty() && val.front() == '"' && val.back() == '"' && val.size() >= 2) {
            val = val.substr(1, val.size() - 2);
        }
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
    }
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
    return true;
}

bool loadConfig(Config& outCfg, std::string& outError) {
    const std::string envPath = "sdmc:/switch/romm_switch_client/.env";
    const std::string jsonPath = "sdmc:/switch/romm_switch_client/config.json";

    bool envTried = parseEnv(envPath, outCfg);
    bool jsonTried = parseJson(jsonPath, outCfg, outError);

    if (!envTried && !jsonTried) {
        outError = "Missing config: place .env at sdmc:/switch/romm_switch_client/.env";
        return false;
    }

    if (outCfg.serverUrl.empty() || outCfg.downloadDir.empty()) {
        if (outError.empty()) outError = "Config missing server_url or download_dir.";
        return false;
    }

    // Enforce http-only for now (TLS not implemented).
    if (outCfg.serverUrl.rfind("https://", 0) == 0) {
        outError = "https:// not supported; use http:// or a local TLS terminator.";
        return false;
    }

    return true;
}

} // namespace romm
