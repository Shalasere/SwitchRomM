#include "romm/logger.hpp"
#include "romm/filesystem.hpp"
#include <fstream>
#include <iostream>
#include <cctype>
#include <switch.h>

namespace romm {

static const char* kLogPath = "sdmc:/switch/romm_switch_client/log.txt";
static bool gLogReady = false;
static LogLevel gMinLevel = LogLevel::Info;

void initLogFile() {
    // Ensure the log directory exists before creating the file.
    std::string logDir = kLogPath;
    auto slash = logDir.find_last_of("/\\");
    if (slash != std::string::npos) {
        logDir = logDir.substr(0, slash);
        romm::ensureDirectory(logDir);
    }
    // Start a fresh log file on launch
    std::ofstream lf(kLogPath, std::ios::trunc);
    if (lf) {
        lf << "RomM Switch Client log start\n";
        lf.flush();
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
    std::cout << line << std::endl;
    printf("%s\n", line.c_str());
    // Mirror to debug monitor so nxlink sees output even without nxlinkStdio
    svcOutputDebugString(line.c_str(), line.size());
    if (gLogReady) {
        std::ofstream lf(kLogPath, std::ios::app);
        if (lf) { lf << line << "\n"; lf.flush(); }
    }
}

void logLine(const std::string& msg) { logInternal(LogLevel::Info, "APP", msg); }
void logDebug(const std::string& msg, const std::string& tag) { logInternal(LogLevel::Debug, tag, msg); }
void logInfo(const std::string& msg, const std::string& tag) { logInternal(LogLevel::Info, tag, msg); }
void logWarn(const std::string& msg, const std::string& tag) { logInternal(LogLevel::Warn, tag, msg); }
void logError(const std::string& msg, const std::string& tag) { logInternal(LogLevel::Error, tag, msg); }

} // namespace romm
