#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace romm {

// Generic single-worker async job runner with "latest request wins" semantics.
// - One active job runs at a time.
// - Submitting while active replaces the pending job (single-slot queue).
// - Optional coalesce window delays pickup briefly so rapid bursts collapse.
template <typename Job, typename Result>
class LatestJobWorker {
public:
    using WorkFn = std::function<Result(const Job&)>;

    LatestJobWorker() = default;
    ~LatestJobWorker() { stop(); }

    LatestJobWorker(const LatestJobWorker&) = delete;
    LatestJobWorker& operator=(const LatestJobWorker&) = delete;

    void start(WorkFn work, uint32_t coalesceMs = 0) {
        stop();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            work_ = std::move(work);
            coalesceMs_ = coalesceMs;
            stopRequested_ = false;
            running_ = false;
            pending_.reset();
            active_.reset();
            result_.reset();
        }
        worker_ = std::thread(&LatestJobWorker::workerLoop, this);
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopRequested_ = true;
            pending_.reset();
        }
        cv_.notify_one();
        if (worker_.joinable()) worker_.join();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
            pending_.reset();
            active_.reset();
            result_.reset();
        }
    }

    void submit(const Job& job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopRequested_) return;
            pending_ = job;
        }
        cv_.notify_one();
    }

    void clearPending() {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.reset();
    }

    bool busy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_ || pending_.has_value();
    }

    bool running() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_;
    }

    std::optional<Job> pendingJob() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_;
    }

    std::optional<Job> activeJob() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_;
    }

    std::optional<Result> pollResult() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!result_) return std::nullopt;
        auto out = std::move(result_);
        result_.reset();
        return out;
    }

private:
    void workerLoop() {
        while (true) {
            Job job;
            WorkFn work;
            uint32_t coalesceMs = 0;

            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] { return stopRequested_ || pending_.has_value(); });
                if (stopRequested_) break;

                coalesceMs = coalesceMs_;
                if (coalesceMs > 0) {
                    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(coalesceMs);
                    cv_.wait_until(lock, deadline, [&] { return stopRequested_; });
                    if (stopRequested_) break;
                }

                if (!pending_) continue;
                job = *pending_;
                pending_.reset();
                active_ = job;
                running_ = true;
                work = work_;
            }

            Result out = work(job);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                running_ = false;
                active_.reset();
                result_ = std::move(out);
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stopRequested_{false};
    bool running_{false};
    uint32_t coalesceMs_{0};
    WorkFn work_{};
    std::optional<Job> pending_;
    std::optional<Job> active_;
    std::optional<Result> result_;
};

} // namespace romm
