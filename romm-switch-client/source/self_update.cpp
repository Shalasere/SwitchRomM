#include "romm/self_update.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace romm {

static void trimAsciiWhitespace(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
    if (i) s.erase(0, i);
}

std::string canonicalSelfNroPath(const std::string& argv0, const std::string& fallback) {
    if (argv0.find("sdmc:/switch/") == 0 && argv0.find(".nro") != std::string::npos) {
        return argv0;
    }
    return fallback;
}

bool readTextFileTrim(const std::string& path, std::string& out) {
    out.clear();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = 0;
    out = buf;
    trimAsciiWhitespace(out);
    return true;
}

bool writeTextFileEnsureParent(const std::string& path, const std::string& text) {
    auto openFile = [&]() -> std::FILE* { return std::fopen(path.c_str(), "wb"); };
    std::FILE* f = openFile();
    if (!f) {
        std::error_code ec;
        std::filesystem::path pp(path);
        std::filesystem::create_directories(pp.parent_path(), ec);
        f = openFile();
    }
    if (!f) return false;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fwrite("\n", 1, 1, f);
    std::fclose(f);
    return true;
}

void removeFileBestEffort(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

bool fileLooksLikeNro(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[4]{};
    size_t n = std::fread(magic, 1, sizeof(magic), f);
    std::fclose(f);
    return n == sizeof(magic) && magic[0] == 'N' && magic[1] == 'R' && magic[2] == 'O' && magic[3] == '0';
}

std::string computeUpdateDirFromDownloadDir(const std::string& downloadDir) {
    if (downloadDir.empty()) return "sdmc:/switch/romm_switch_client/app_update";
    // Keep behavior simple and predictable, even for "sdmc:/..." scheme paths.
    if (!downloadDir.empty() && (downloadDir.back() == '/' || downloadDir.back() == '\\')) {
        return downloadDir.substr(0, downloadDir.size() - 1) + "/app_update";
    }
    return downloadDir + "/app_update";
}

std::string defaultStagedUpdatePath(const std::string& updateDir) {
    return (std::filesystem::path(updateDir) / "romm-switch-client.nro.new").string();
}

std::string defaultBackupPath(const std::string& updateDir) {
    return (std::filesystem::path(updateDir) / "romm-switch-client.nro.bak").string();
}

ApplySelfUpdateResult applyPendingSelfUpdate(const std::string& selfNroPath,
                                            const std::string& pendingPath,
                                            const std::function<void(const std::string&)>& logFn) {
    ApplySelfUpdateResult out;
    std::string pending;
    if (!readTextFileTrim(pendingPath, pending) || pending.empty()) return out;
    out.hadPending = true;
    out.stagedPath = pending;

    // Delete any leftover partial next to the staged file.
    removeFileBestEffort(pending + ".part");

    std::error_code ec;
    const bool stagedExists = std::filesystem::exists(pending, ec);
    if (ec || !stagedExists || !fileLooksLikeNro(pending)) {
        if (logFn) logFn("Self-update pending path invalid or missing: " + pending);
        removeFileBestEffort(pendingPath);
        out.pendingCleared = true;
        out.error = "Pending update missing/invalid.";
        return out;
    }

    std::filesystem::path sp(pending);
    std::string updateDir = sp.parent_path().string();
    std::string bak = defaultBackupPath(updateDir);

    // Keep only last backup.
    removeFileBestEffort(bak);

    ec.clear();
    std::filesystem::rename(selfNroPath, bak, ec);
    if (ec) {
        if (logFn) logFn("Self-update apply: could not move current NRO to backup: " + ec.message());
    }

    ec.clear();
    std::filesystem::rename(pending, selfNroPath, ec);
    if (ec) {
        if (logFn) logFn("Self-update apply failed: " + ec.message());
        // Best-effort restore backup if we created one.
        std::error_code ec2;
        if (std::filesystem::exists(bak, ec2) && !ec2) {
            std::filesystem::rename(bak, selfNroPath, ec2);
        }
        out.error = "Apply failed.";
        return out;
    }

    if (logFn) logFn("Self-update applied successfully.");
    removeFileBestEffort(pendingPath);
    out.pendingCleared = true;
    out.applied = true;
    return out;
}

} // namespace romm

