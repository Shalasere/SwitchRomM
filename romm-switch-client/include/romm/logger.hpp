#pragma once

#include <string>

namespace romm {

enum class LogLevel { Debug = 0, Info, Warn, Error };

void initLogFile();
void setLogLevel(LogLevel level);
void setLogLevelFromString(const std::string& level);

// Tagged logging helpers. logLine remains for backward compatibility (Info level, "APP" tag).
void logLine(const std::string& msg);
void logDebug(const std::string& msg, const std::string& tag = "DBG");
void logInfo(const std::string& msg, const std::string& tag = "APP");
void logWarn(const std::string& msg, const std::string& tag = "APP");
void logError(const std::string& msg, const std::string& tag = "APP");

} // namespace romm
