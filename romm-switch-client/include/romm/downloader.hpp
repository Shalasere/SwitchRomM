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

} // namespace romm
