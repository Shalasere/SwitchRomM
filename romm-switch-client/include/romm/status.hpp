#pragma once

#include "romm/models.hpp"
#include <atomic>
#include <string>
#include <vector>

namespace romm {

struct Status {
    bool validHost{false};
    bool validCredentials{false};

    // Current UI/view state
    enum class View { PLATFORMS, ROMS, DETAIL, QUEUE, ERROR, DOWNLOADING } currentView{View::PLATFORMS};

    // Data loaded from API
    std::vector<Platform> platforms;
    std::vector<Game> roms;

    // Selection indices for views
    int selectedPlatformIndex{0};
    int selectedRomIndex{0};
    int selectedQueueIndex{0};
    std::vector<View> navStack; // simple stack for PLATFORMS -> ROMS -> DETAIL navigation
    View prevQueueView{View::ROMS}; // where to return when leaving queue

    // Download queue and progress
    std::vector<Game> downloadQueue;
    bool isDownloading{false};
    size_t currentDownloadIndex{0};
    std::atomic<uint64_t> currentDownloadSize{0};
    std::atomic<uint64_t> currentDownloadedBytes{0};
    std::atomic<uint64_t> totalDownloadBytes{0};
    std::atomic<uint64_t> totalDownloadedBytes{0};
    std::string currentDownloadTitle;
    std::atomic<bool> downloadWorkerRunning{false};
    std::atomic<bool> lastDownloadFailed{false};
    std::string lastDownloadError;
    bool downloadCompleted{false};

    // Async flags (unused/legacy)
    std::atomic<bool> platformsReady{false};
    std::atomic<bool> romsReady{false};
    std::atomic<bool> downloadInProgress{false};

    std::string lastError;
};

} // namespace romm
