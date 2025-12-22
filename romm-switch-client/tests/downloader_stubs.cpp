#include "romm/downloader.hpp"
#include "romm/manifest.hpp"
#include <fstream>
#include <sstream>
#include <mutex>
#include <filesystem>
#include <algorithm>

namespace romm {

// Minimal stubs to satisfy linker for unit tests (no libnx). Events are unused.
struct DownloadEvent {};
bool pollDownloadEvent(DownloadEvent&) { return false; }

void startDownloadWorker(Status& status, const Config& cfg) {
    (void)cfg;
    status.downloadWorkerRunning.store(true);
}
void stopDownloadWorker() {}
void reapDownloadWorkerIfDone() {}

bool loadLocalManifests(Status& status, const Config& cfg, std::string& outError) {
    namespace fs = std::filesystem;
    outError.clear();
    const fs::path tempRoot = fs::path(cfg.downloadDir) / "temp";
    if (!fs::exists(tempRoot)) return true;
    std::lock_guard<std::mutex> lock(status.mutex);
    status.downloadHistory.clear();
    for (const auto& entry : fs::directory_iterator(tempRoot)) {
        if (!entry.is_directory()) continue;
        fs::path manifestPath = entry.path() / "manifest.json";
        if (!fs::exists(manifestPath)) continue;
        std::ifstream in(manifestPath.string(), std::ios::binary);
        if (!in) continue;
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        romm::Manifest m;
        std::string err;
        if (!romm::manifestFromJson(content, m, err)) continue;
        romm::QueueItem qi;
        qi.game.id = m.rommId;
        qi.game.fileId = m.fileId;
        qi.game.fsName = m.fsName;
        qi.game.downloadUrl = m.url;
        qi.game.sizeBytes = m.totalSize;
        qi.state = romm::QueueState::Resumable;
        qi.error = "Resume available";
        bool allDone = !m.parts.empty() &&
                       std::all_of(m.parts.begin(), m.parts.end(),
                                   [](const romm::ManifestPart& p){ return p.completed; });
        if (allDone) {
            qi.state = romm::QueueState::Completed;
            qi.error.clear();
        }
        status.downloadHistory.push_back(qi);
    }
    return true;
}

#ifdef UNIT_TEST
bool parseLengthAndRangesForTest(const std::string& headers, bool& supportsRanges, uint64_t& contentLength) {
    supportsRanges = false;
    contentLength = 0;
    std::istringstream hs(headers);
    std::string line;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colonPos = line.find(':');
        if (colonPos == std::string::npos) continue;
        std::string key = line.substr(0, colonPos);
        std::string val = line.substr(colonPos + 1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
        std::string keyLower = key;
        for (auto& c : keyLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (keyLower == "content-length" && contentLength == 0) {
            contentLength = static_cast<uint64_t>(std::strtoull(val.c_str(), nullptr, 10));
        } else if (keyLower == "accept-ranges" && val.find("bytes") != std::string::npos) {
            supportsRanges = true;
        } else if (keyLower == "content-range") {
            auto slash = val.find('/');
            if (slash != std::string::npos) {
                std::string totalStr = val.substr(slash + 1);
                contentLength = static_cast<uint64_t>(std::strtoull(totalStr.c_str(), nullptr, 10));
            }
        }
    }
    return contentLength > 0;
}
#endif

} // namespace romm
