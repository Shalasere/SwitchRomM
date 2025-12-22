#include "romm/logger.hpp"
#include "romm/filesystem.hpp"
#include <fstream>
#include <iostream>
#include <cctype>
#include <mutex>
#include <filesystem>
#include <switch.h>

namespace romm {

static const char* kLogPath = "sdmc:/switch/romm_switch_client/log.txt";
static constexpr size_t kMaxLogBytes = 512 * 1024; // simple cap to limit SD wear
static bool gLogReady = false;
static LogLevel gMinLevel = LogLevel::Info;
static std::mutex gLogMutex;
static std::ofstream gLogFile;
static size_t gLogBytes = 0;

void initLogFile() {
    // Ensure the log directory exists before creating the file.
    std::string logDir = kLogPath;
    auto slash = logDir.find_last_of("/\\");
    if (slash != std::string::npos) {
        logDir = logDir.substr(0, slash);
        romm::ensureDirectory(logDir);
    }
    // Start a fresh log file on launch
    gLogFile.open(kLogPath, std::ios::trunc);
    if (gLogFile) {
        gLogFile << "RomM Switch Client log start\n";
        gLogFile.flush();
        gLogBytes = static_cast<size_t>(gLogFile.tellp());
        gLogReady = true;
    }
}

void setLogLevel(LogLevel level) { gMinLevel = level; }

void setLogLevelFromString(const std::string& level) {
    std::string l;
    l.reserve(level.size());
    for (char c : level) l.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (l == "debug") gMinLevel = LogLevel::Debug;
    else if (l == "warn") gMinLevel = LogLevel::Warn;
    else if (l == "error") gMinLevel = LogLevel::Error;
    else gMinLevel = LogLevel::Info;
}

static void logInternal(LogLevel level, const std::string& tag, const std::string& msg) {
    if (level < gMinLevel) return;
    std::string line = "[" + tag + "] " + msg;
    // Write to stdio (nxlink), debug SVC, and append to sdmc log file
    // stdout/nxlink + debug monitor
    std::cout << line << std::endl;
    printf("%s\n", line.c_str());
    svcOutputDebugString(line.c_str(), line.size());
    if (!gLogReady) return;

    std::lock_guard<std::mutex> lock(gLogMutex);
    if (!gLogReady) return;

    auto rotate = []() {
        if (gLogFile.is_open()) gLogFile.close();
        std::error_code ec;
        std::filesystem::path p(kLogPath);
        std::filesystem::path rotated = p;
        rotated += ".1";
        std::filesystem::remove(rotated, ec);
        ec.clear();
        std::filesystem::rename(p, rotated, ec); // best-effort
        gLogFile.open(kLogPath, std::ios::trunc);
        gLogBytes = 0;
        if (gLogFile) {
            gLogFile << "RomM Switch Client log start (rotated)\n";
            gLogFile.flush();
            gLogBytes = static_cast<size_t>(gLogFile.tellp());
        }
    };

    size_t writeBytes = line.size() + 1; // newline
    if (gLogBytes + writeBytes > kMaxLogBytes) {
        rotate();
    }
    if (gLogFile) {
        gLogFile << line << "\n";
        gLogFile.flush();
        gLogBytes += writeBytes;
    }
}

void logLine(const std::string& msg) { logInternal(LogLevel::Info, "APP", msg); }
void logDebug(const std::string& msg, const std::string& tag) { logInternal(LogLevel::Debug, tag, msg); }
void logInfo(const std::string& msg, const std::string& tag) { logInternal(LogLevel::Info, tag, msg); }
void logWarn(const std::string& msg, const std::string& tag) { logInternal(LogLevel::Warn, tag, msg); }
void logError(const std::string& msg, const std::string& tag) { logInternal(LogLevel::Error, tag, msg); }

} // namespace romm
