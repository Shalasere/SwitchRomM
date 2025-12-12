#include "romm/filesystem.hpp"
#include "romm/logger.hpp"
#include <filesystem>
#include <sys/statvfs.h>

namespace romm {

bool ensureDirectory(const std::string& path) {
    std::filesystem::path p(path);
    std::error_code ec;
    bool ok = std::filesystem::create_directories(p, ec) || std::filesystem::exists(p);
    if (!ok) logLine("Failed to ensure directory: " + path);
    return ok;
}

bool fileExists(const std::string& path) {
    std::filesystem::path p(path);
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

uint64_t getFreeSpace(const std::string& path) {
    struct statvfs s{};
    if (statvfs(path.c_str(), &s) != 0) return 0;
    return static_cast<uint64_t>(s.f_bavail) * static_cast<uint64_t>(s.f_frsize);
}

} // namespace romm
