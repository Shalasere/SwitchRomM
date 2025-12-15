#pragma once

#include <queue>
#include <mutex>
#include <optional>
#include <string>

namespace romm {

// Minimal worker -> UI event channel to decouple status updates.
enum class DownloadEventKind { BeginItem, Progress, CompletedItem, FailedItem, QueueEmpty };

struct DownloadEvent {
    DownloadEventKind kind;
    std::string title;
    std::string error; // only for FailedItem
};

class DownloadEventQueue {
public:
    void push(const DownloadEvent& ev) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(ev);
    }

    std::optional<DownloadEvent> pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return std::nullopt;
        DownloadEvent ev = queue_.front();
        queue_.pop();
        return ev;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<DownloadEvent> empty;
        std::swap(queue_, empty);
    }

private:
    std::mutex mutex_;
    std::queue<DownloadEvent> queue_;
};

} // namespace romm
