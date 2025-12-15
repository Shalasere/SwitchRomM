#include "catch.hpp"
#include "romm/downloader.hpp"
#include "romm/status.hpp"
#include "romm/models.hpp"
#include <atomic>
#include <thread>
#include <chrono>

// NOTE: This is a small host-side smoke test for the worker lifecycle.
// It does not perform network I/O; it only checks that starting a worker after
// a completed run is allowed (i.e., the old thread was joined and reset).
// We simulate completion by toggling downloadWorkerRunning directly.

TEST_CASE("worker can be re-started after completion") {
    romm::Status st;
    romm::Config cfg;
    cfg.downloadDir = "sdmc:/romm_cache/switch"; // unused here

    // Simulate a previous completed session: mark the worker as not running and ensure
    // startDownloadWorker will join/reap any old thread before starting anew.
    st.downloadWorkerRunning.store(false);

    // Call reap first (no-op if nothing to join).
    romm::reapDownloadWorkerIfDone();

    // Start should spawn a worker thread even if a previous one had finished.
    romm::startDownloadWorker(st, cfg);
    REQUIRE(st.downloadWorkerRunning.load() == true);

    // Simulate worker finishing.
    st.downloadWorkerRunning.store(false);
    romm::reapDownloadWorkerIfDone();

    // Start again should be allowed (no stale joinable thread blocks it).
    romm::startDownloadWorker(st, cfg);
    REQUIRE(st.downloadWorkerRunning.load() == true);

    // Clean up
    romm::stopDownloadWorker();
}
