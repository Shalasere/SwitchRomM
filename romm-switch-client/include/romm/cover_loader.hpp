#pragma once

#include <optional>
#include <string>
#include <vector>
#include "romm/config.hpp"
#include "romm/job_manager.hpp"

namespace romm {

struct CoverJob {
    std::string url;
    std::string title;
    Config cfg;
};

struct CoverResult {
    bool ok{false};
    std::string url;
    std::string title;
    int w{0};
    int h{0};
    std::vector<unsigned char> pixels; // RGBA
    std::string error;
};

// Simple async cover fetch/decode worker. Fetch/HTTP logic is expected to be provided
// via a callback so tests can stub it.
class CoverLoader {
public:
    using FetchFn = bool(*)(const std::string& url, const Config& cfg, std::vector<unsigned char>& outData, std::string& err);

    CoverLoader();
    ~CoverLoader();

    // Start the worker thread if not already running.
    void start(FetchFn fetchFn);
    // Stop and join the worker thread.
    void stop();

    // Enqueue a cover job (deduped by URL). No-op if URL matches the last texture URL.
    void request(const CoverJob& job, const std::string& currentTextureUrl);

    // Poll for a completed result; returns nullopt if none ready.
    std::optional<CoverResult> poll();

private:
    CoverResult runJob(const CoverJob& job);

    FetchFn fetch_{nullptr};
    LatestJobWorker<CoverJob, CoverResult> worker_;
};

} // namespace romm
