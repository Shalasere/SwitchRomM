#pragma once

#include <atomic>
#include <thread>
#include "romm/config.hpp"
#include "romm/status.hpp"

namespace romm {

// Starts the background download worker if not already running.
void startDownloadWorker(Status& status, const Config& cfg);

// Signals the worker to stop and waits for it to finish.
void stopDownloadWorker();

// If a previous worker has finished, join and release its resources.
void reapDownloadWorkerIfDone();

// Scan temp manifests under download_dir to seed history of resumable items.
bool loadLocalManifests(Status& status, const Config& cfg, std::string& outError);

#ifdef UNIT_TEST
// Test helper: parse HTTP headers for content length and range support.
bool parseLengthAndRangesForTest(const std::string& headers, bool& supportsRanges, uint64_t& contentLength);
#endif

} // namespace romm
