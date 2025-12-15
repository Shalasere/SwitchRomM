#include "catch.hpp"
#include "romm/status.hpp"
#include "romm/models.hpp"

TEST_CASE("queue indices stay in range under mutex") {
    romm::Status st;
    {
        std::lock_guard<std::mutex> lock(st.mutex);
        st.downloadQueue.clear();
        st.selectedQueueIndex = 0;
    }

    // Add items under lock and ensure index clamps.
    {
        std::lock_guard<std::mutex> lock(st.mutex);
        st.downloadQueue.push_back(romm::Game{});
        st.downloadQueue.push_back(romm::Game{});
        st.selectedQueueIndex = 5;
        if (st.selectedQueueIndex >= (int)st.downloadQueue.size()) {
            st.selectedQueueIndex = (int)st.downloadQueue.size() - 1;
        }
    }
    {
        std::lock_guard<std::mutex> lock(st.mutex);
        REQUIRE(st.downloadQueue.size() == 2);
        REQUIRE(st.selectedQueueIndex == 1);
    }

    // Remove one and ensure index clamps again.
    {
        std::lock_guard<std::mutex> lock(st.mutex);
        st.downloadQueue.pop_back();
        if (st.selectedQueueIndex >= (int)st.downloadQueue.size()) {
            st.selectedQueueIndex = st.downloadQueue.empty() ? 0 : (int)st.downloadQueue.size() - 1;
        }
    }
    {
        std::lock_guard<std::mutex> lock(st.mutex);
        REQUIRE(st.downloadQueue.size() == 1);
        REQUIRE(st.selectedQueueIndex == 0);
    }
}

TEST_CASE("enqueue dedup by fileId/fsName") {
    romm::Status st;
    romm::Game a; a.fileId = "123"; a.fsName = "foo.nsp";
    romm::Game b; b.fileId = "123"; b.fsName = "bar.nsp"; // same id, different name
    romm::Game c; c.fileId = "456"; c.fsName = "foo.nsp"; // same name, different id

    auto enqueue = [&](const romm::Game& g) {
        std::lock_guard<std::mutex> lock(st.mutex);
        auto it = std::find_if(st.downloadQueue.begin(), st.downloadQueue.end(), [&](const romm::Game& existing) {
            return (!g.fileId.empty() && g.fileId == existing.fileId) ||
                   (!g.fsName.empty() && g.fsName == existing.fsName);
        });
        if (it == st.downloadQueue.end()) {
            st.downloadQueue.push_back(g);
            return true;
        }
        return false;
    };

    REQUIRE(enqueue(a));
    REQUIRE_FALSE(enqueue(b)); // duplicate by fileId
    REQUIRE_FALSE(enqueue(c)); // duplicate by fsName
    {
        std::lock_guard<std::mutex> lock(st.mutex);
        REQUIRE(st.downloadQueue.size() == 1);
    }
}

TEST_CASE("progress counters accumulate safely") {
    romm::Status st;
    st.currentDownloadedBytes.store(0);
    st.totalDownloadedBytes.store(0);
    st.currentDownloadSize.store(100);
    st.totalDownloadBytes.store(200);

    // Simulate two chunks written.
    st.currentDownloadedBytes.fetch_add(40);
    st.totalDownloadedBytes.fetch_add(40);
    st.currentDownloadedBytes.fetch_add(60);
    st.totalDownloadedBytes.fetch_add(60);

    REQUIRE(st.currentDownloadedBytes.load() == 100);
    REQUIRE(st.totalDownloadedBytes.load() == 100);
    // Totals can exceed current size if multiple items are considered; here they match.
}

TEST_CASE("withStatusLock guards mutations and returns values") {
    romm::Status st;
    int snapshotSize = romm::withStatusLock(st, [&]() {
        st.downloadQueue.push_back(romm::Game{});
        st.downloadQueue.push_back(romm::Game{});
        st.selectedQueueIndex = 1;
        return static_cast<int>(st.downloadQueue.size());
    });
    REQUIRE(snapshotSize == 2);
    // Verify under lock that state persisted.
    romm::withStatusLock(st, [&]() {
        REQUIRE(st.downloadQueue.size() == 2);
        REQUIRE(st.selectedQueueIndex == 1);
        st.downloadQueue.clear();
        st.selectedQueueIndex = 0;
        return 0;
    });
    // After clearing, size should be 0.
    romm::withStatusLock(st, [&]() {
        REQUIRE(st.downloadQueue.empty());
        REQUIRE(st.selectedQueueIndex == 0);
        return 0;
    });
}
