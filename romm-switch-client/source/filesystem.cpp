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
    std::string fileIdSafe = g.fileId.empty() ? "" : safeName(g.fileId);
    std::string baseStem;
    std::string ext;
    if (!g.fsName.empty()) {
        auto dot = g.fsName.rfind('.');
        if (dot != std::string::npos) {
            baseStem = g.fsName.substr(0, dot);
            ext = g.fsName.substr(dot);
        } else {
            baseStem = g.fsName;
        }
    } else {
        baseStem = safeName(g.title);
    }
    if (baseStem.empty() && !g.id.empty()) baseStem = idSafe;
    if (baseStem.empty()) baseStem = "rom";

    auto addVariants = [&](const std::string& stem, const std::string& extension, std::vector<std::filesystem::path>& out) {
        std::filesystem::path dir(cfg.downloadDir);
        std::string plat = g.platformSlug.empty() ? "unknown" : g.platformSlug;
        auto pushPath = [&](const std::string& name) {
            out.push_back(dir / plat / name);
            // Backward compatibility: also check flat layout.
            out.push_back(dir / name);
        };
        std::string base = stem + extension;
        pushPath(base);
        if (!idSafe.empty()) {
            pushPath(stem + "_" + idSafe + extension);
            pushPath(stem + "." + idSafe + extension);
        }
        if (!fileIdSafe.empty() && fileIdSafe != idSafe) {
            pushPath(stem + "_" + fileIdSafe + extension);
            pushPath(stem + "." + fileIdSafe + extension);
        }
    };

    std::vector<std::filesystem::path> candidates;
    if (!ext.empty()) {
        addVariants(baseStem, ext, candidates);
    } else {
        addVariants(baseStem, "", candidates);
        addVariants(baseStem, ".xci", candidates);
        addVariants(baseStem, ".nsp", candidates);
    }
    std::error_code ec;
    for (const auto& p : candidates) {
        if (!std::filesystem::exists(p, ec)) continue;
        if (std::filesystem::is_regular_file(p, ec)) return true;
        if (std::filesystem::is_directory(p, ec)) {
            auto it = std::filesystem::directory_iterator(p, ec);
            if (!ec && it != std::filesystem::end(it)) return true;
        }
    }
    return false;
}

} // namespace romm
