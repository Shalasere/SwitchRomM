#include "romm/filesystem.hpp"
#include "romm/logger.hpp"
#include "romm/util.hpp"
#include <filesystem>
#include <sys/statvfs.h>

namespace romm {

namespace {
static std::string safeName(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if (c <= 31 || c == '/' || c == '\\' || c == ':') continue;
        out.push_back(static_cast<char>(c));
    }
    if (out.empty()) out = "rom";
    return out;
}
}

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

bool isGameCompletedOnDisk(const Game& g, const Config& cfg) {
    std::string idSafe = safeName(!g.id.empty() ? g.id : g.fileId);
    std::string romSafe = idSafe.empty() ? safeName(g.title) : idSafe;
    if (romSafe.empty()) romSafe = "rom";
    std::string titleSafe = safeName(g.title);
    std::string folder = romSafe;
    if (!titleSafe.empty()) folder = titleSafe + "_" + romSafe;
    std::string plat = g.platformSlug.empty() ? "unknown" : g.platformSlug;
    std::error_code ec;

    // Primary new layout: <downloadDir>/<platform>/<title_id>/...
    std::filesystem::path baseDir = std::filesystem::path(cfg.downloadDir) / plat / folder;
    if (std::filesystem::exists(baseDir, ec)) {
        if (std::filesystem::is_regular_file(baseDir, ec)) return true;
        if (std::filesystem::is_directory(baseDir, ec)) {
            auto it = std::filesystem::directory_iterator(baseDir, ec);
            if (!ec && it != std::filesystem::end(it)) return true;
        }
    }

    // Backward compatibility: flat layout under <downloadDir>/<platform>/ and <downloadDir>/.
    std::vector<std::filesystem::path> candidates;
    candidates.push_back(std::filesystem::path(cfg.downloadDir) / plat / (romSafe + ".xci"));
    candidates.push_back(std::filesystem::path(cfg.downloadDir) / plat / (romSafe + ".nsp"));
    candidates.push_back(std::filesystem::path(cfg.downloadDir) / (romSafe + ".xci"));
    candidates.push_back(std::filesystem::path(cfg.downloadDir) / (romSafe + ".nsp"));
    for (const auto& p : candidates) {
        if (!std::filesystem::exists(p, ec)) continue;
        if (std::filesystem::is_regular_file(p, ec)) return true;
    }

    return false;
}

} // namespace romm
