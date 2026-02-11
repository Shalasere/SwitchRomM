#pragma once

#include "romm/models.hpp"
#include "romm/errors.hpp"
#include "romm/platform_prefs.hpp"
#include "romm/planner.hpp"
#include <atomic>
#include <string>
#include <vector>
#include <mutex>
#include <utility>

namespace romm {

enum class QueueState { Pending, Downloading, Finalizing, Completed, Resumable, Failed, Cancelled };
enum class RomFilter { All, Queued, Resumable, Failed, Completed, NotQueued };
enum class RomSort { TitleAsc, TitleDesc, SizeDesc, SizeAsc };

struct QueueItem {
    Game game;
    DownloadBundle bundle;
    QueueState state{QueueState::Pending};
    std::string error;

    QueueItem() = default;
    QueueItem(const Game& g, QueueState s, const std::string& errStr = std::string())
        : game(g), state(s), error(errStr) {}
};

struct Status {
    // TODO(thread-safety): use this mutex (or an event queue) to guard shared state between UI and worker.
    mutable std::mutex mutex;

    bool validHost{false};
    bool validCredentials{false};

    // Current UI/view state
    enum class View { PLATFORMS, ROMS, DETAIL, QUEUE, ERROR, DOWNLOADING, DIAGNOSTICS } currentView{View::PLATFORMS};

    // Data loaded from API
    std::vector<Platform> platforms;
    std::vector<Game> roms;      // active (filtered/sorted) list used by UI
    std::vector<Game> romsAll;   // master list fetched from server for indexing
    uint64_t romsRevision{0}; // bump when `roms` changes to let UI caches avoid O(N) per-frame rebuilds
    uint64_t romsAllRevision{0};
    std::string romSearchQuery;
    RomFilter romFilter{RomFilter::All};
    RomSort romSort{RomSort::TitleAsc};
    uint64_t romListOptionsRevision{0};
    PlatformPrefs platformPrefs;

    // Selection indices for views
    int selectedPlatformIndex{0};
    int selectedRomIndex{0};
    int selectedQueueIndex{0};
    // Selected platform identity (used to keep UI and behavior correlated even if indices drift).
    std::string currentPlatformId;
    std::string currentPlatformSlug;
    std::string currentPlatformName;
    std::vector<View> navStack; // simple stack for PLATFORMS -> ROMS -> DETAIL navigation
    View prevQueueView{View::ROMS}; // where to return when leaving queue
    View prevDiagnosticsView{View::PLATFORMS}; // where to return when leaving diagnostics

    // Download queue and progress
    std::vector<QueueItem> downloadQueue;
    std::vector<QueueItem> downloadHistory; // completed/failed items for UI display
    uint64_t downloadQueueRevision{0};   // bump when queue contents or states change
    uint64_t downloadHistoryRevision{0}; // bump when history changes
    std::atomic<size_t> currentDownloadIndex{0};
    std::atomic<uint64_t> currentDownloadSize{0};
    std::atomic<uint64_t> currentDownloadedBytes{0};
    std::atomic<uint64_t> totalDownloadBytes{0};
    std::atomic<uint64_t> totalDownloadedBytes{0};
    std::string currentDownloadTitle;
    double lastSpeedMBps{0.0}; // last measured throughput in MB/s, updated by worker
    std::atomic<bool> downloadWorkerRunning{false};
    std::atomic<bool> lastDownloadFailed{false};
    std::string lastDownloadError;
    bool downloadCompleted{false};

    // Async flags (unused/legacy)
    std::atomic<bool> platformsReady{false};
    std::atomic<bool> romsReady{false};
    std::atomic<bool> downloadInProgress{false};

    // Network/IO busy indicator for UI throbber.
    std::atomic<bool> netBusy{false};
    std::atomic<uint32_t> netBusySinceMs{0};
    std::string netBusyWhat;
    uint64_t romFetchGeneration{0}; // increments to cancel/ignore stale ROM fetches

    std::string lastError;
    ErrorInfo lastErrorInfo{};

    // Diagnostics probe state.
    bool diagnosticsServerReachableKnown{false};
    bool diagnosticsServerReachable{false};
    bool diagnosticsProbeInFlight{false};
    uint64_t diagnosticsProbeGeneration{0};
    uint32_t diagnosticsLastProbeMs{0};
    std::string diagnosticsLastProbeDetail;
};

// Helper to run a callable while holding the status mutex, returning its result.
// Keeps locking policy consistent across UI and worker paths.
template <typename F>
auto withStatusLock(Status& st, F&& fn) -> decltype(fn()) {
    std::lock_guard<std::mutex> lock(st.mutex);
    return fn();
}

} // namespace romm
