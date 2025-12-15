#include "romm/downloader.hpp"
#include "romm/events.hpp"
#include <sstream>
#include <algorithm>
#include <cctype>

namespace romm {

// Host-side stub so unit tests can link without bringing in libnx-dependent downloader.
bool pollDownloadEvent(DownloadEvent& ev) {
    (void)ev;
    return false;
}

// No-op stubs for worker control; used by lifecycle tests.
void startDownloadWorker(Status& status, const Config& cfg) {
    (void)cfg;
    status.downloadWorkerRunning.store(true);
}

void stopDownloadWorker() {
    // nothing
}

void reapDownloadWorkerIfDone() {
    // nothing
}

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
        std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
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

} // namespace romm
