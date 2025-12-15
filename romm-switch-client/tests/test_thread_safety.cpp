#include "catch.hpp"
#include "romm/status.hpp"
#include "romm/models.hpp"
#include <thread>
#include <atomic>

// Basic multithreaded smoke to ensure mutex-guarded access patterns hold up under concurrent reads/writes.
TEST_CASE("concurrent access to Status guarded by mutex") {
    romm::Status st;
    std::atomic<bool> stop{false};

    // Writer thread: append games under the mutex.
    std::thread writer([&](){
        for (int i = 0; i < 500; ++i) {
            if (stop.load()) break;
            std::lock_guard<std::mutex> lock(st.mutex);
            romm::Game g;
            g.id = std::to_string(i);
            g.title = "Game " + std::to_string(i);
            g.sizeBytes = static_cast<uint64_t>(i * 1024);
            st.downloadQueue.push_back(romm::QueueItem{g, romm::QueueState::Pending, ""});
            // Clamp selection to end.
            st.selectedQueueIndex = static_cast<int>(st.downloadQueue.size()) - 1;
        }
    });

    // Reader thread: snapshot sizes under the mutex and verify indices stay in range.
    std::thread reader([&](){
        for (int j = 0; j < 500; ++j) {
            std::vector<romm::QueueItem> snap;
            int sel = 0;
            {
                std::lock_guard<std::mutex> lock(st.mutex);
                snap = st.downloadQueue;
                sel = st.selectedQueueIndex;
            }
            if (!snap.empty()) {
                REQUIRE(sel >= 0);
                REQUIRE(sel < static_cast<int>(snap.size()));
            }
        }
        stop.store(true);
    });

    writer.join();
    reader.join();

    std::lock_guard<std::mutex> lock(st.mutex);
    REQUIRE(st.downloadQueue.size() <= 500);
    if (!st.downloadQueue.empty()) {
        REQUIRE(st.selectedQueueIndex >= 0);
        REQUIRE(st.selectedQueueIndex < static_cast<int>(st.downloadQueue.size()));
    }
}
