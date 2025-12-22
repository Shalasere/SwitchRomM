#include "romm/cover_loader.hpp"
#include "stb_image.h"
#include <chrono>

namespace romm {

CoverLoader::CoverLoader() = default;
CoverLoader::~CoverLoader() { stop(); }

void CoverLoader::start(FetchFn fetchFn) {
    if (worker_.joinable()) return;
    stop_.store(false);
    fetch_ = fetchFn;
    worker_ = std::thread(&CoverLoader::workerLoop, this);
}

void CoverLoader::stop() {
    stop_.store(true);
    cv_.notify_one();
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job_.reset();
        result_.reset();
    }
}

void CoverLoader::request(const CoverJob& job, const std::string& currentTextureUrl) {
    if (job.url.empty()) return;
    // Dedup: avoid requesting if it matches the current texture URL or queued job URL.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!currentTextureUrl.empty() && currentTextureUrl == job.url) return;
        if (job_ && job_->url == job.url) return;
        job_ = job;
    }
    cv_.notify_one();
}

std::optional<CoverResult> CoverLoader::poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (result_) {
        auto out = result_;
        result_.reset();
        return out;
    }
    return std::nullopt;
}

void CoverLoader::workerLoop() {
    while (!stop_.load()) {
        CoverJob job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return stop_.load() || job_.has_value(); });
            if (stop_.load()) break;
            // Coalesce rapid-fire requests: wait a short window for newer jobs, then take the latest.
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
            while (!stop_.load()) {
                if (cv_.wait_until(lock, deadline, [&]{ return stop_.load(); })) break;
                if (std::chrono::steady_clock::now() >= deadline) break;
            }
            if (stop_.load()) break;
            job = *job_;
            job_.reset();
        }
        CoverResult res;
        res.url = job.url;
        res.title = job.title;
        if (!fetch_) {
            res.ok = false;
            res.error = "no fetch function";
        } else {
            std::vector<unsigned char> data;
            std::string err;
            if (fetch_(job.url, job.cfg, data, err)) {
#ifdef UNIT_TEST
                // In tests, skip real image decoding; treat payload as raw RGBA if present.
                res.ok = true;
                res.w = 1;
                res.h = 1;
                if (data.size() >= 4) {
                    res.pixels.assign(data.begin(), data.begin() + 4);
                } else {
                    res.pixels.assign({0xFF, 0, 0, 0xFF}); // opaque red fallback
                }
#else
                int w = 0, h = 0, channels = 0;
                unsigned char* pixels = stbi_load_from_memory(
                    reinterpret_cast<const unsigned char*>(data.data()),
                    static_cast<int>(data.size()), &w, &h, &channels, STBI_rgb_alpha);
                if (pixels) {
                    res.ok = true;
                    res.w = w;
                    res.h = h;
                    res.pixels.assign(pixels, pixels + (w * h * 4));
                    stbi_image_free(pixels);
                } else {
                    res.ok = false;
                    res.error = "decode failed";
                }
#endif
            } else {
                res.ok = false;
                res.error = err.empty() ? "fetch failed" : err;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            result_ = res;
        }
    }
}

} // namespace romm
