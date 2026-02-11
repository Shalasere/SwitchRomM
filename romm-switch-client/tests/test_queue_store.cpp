#include "catch.hpp"

#include "romm/queue_store.hpp"

#include <filesystem>
#include <fstream>

namespace {

romm::QueueItem makeQueueItem(const std::string& id,
                              const std::string& title,
                              const std::string& slug,
                              const std::string& fsName) {
    romm::QueueItem qi;
    qi.state = romm::QueueState::Pending;
    qi.game.id = id;
    qi.game.title = title;
    qi.game.platformSlug = slug;
    qi.game.fsName = fsName;
    qi.game.fileId = "file_" + id;
    qi.game.downloadUrl = "http://example.com/" + fsName;
    qi.game.sizeBytes = 1024;

    qi.bundle.romId = qi.game.id;
    qi.bundle.title = qi.game.title;
    qi.bundle.platformSlug = qi.game.platformSlug;
    qi.bundle.mode = "single_best";
    romm::DownloadFileSpec file{};
    file.fileId = qi.game.fileId;
    file.name = qi.game.fsName;
    file.url = qi.game.downloadUrl;
    file.sizeBytes = qi.game.sizeBytes;
    qi.bundle.files.push_back(std::move(file));
    return qi;
}

} // namespace

TEST_CASE("queue store save/load roundtrip") {
    namespace fs = std::filesystem;
    const fs::path tempDir = fs::current_path() / "tmp_queue_store_roundtrip";
    const fs::path queuePath = tempDir / "queue_state.json";
    fs::create_directories(tempDir);

    romm::Status st;
    {
        std::lock_guard<std::mutex> lock(st.mutex);
        st.downloadQueue.push_back(makeQueueItem("100", "Roundtrip", "switch", "roundtrip.xci"));
    }

    std::string err;
    REQUIRE(romm::saveQueueState(st, err, queuePath.string()));
    REQUIRE(err.empty());

    romm::Status loaded;
    romm::Config cfg;
    cfg.downloadDir = (tempDir / "downloads").string();
    REQUIRE(romm::loadQueueState(loaded, cfg, err, queuePath.string()));
    REQUIRE(err.empty());
    REQUIRE(loaded.downloadQueue.size() == 1);
    REQUIRE(loaded.downloadQueue[0].game.id == "100");
    REQUIRE(loaded.downloadQueue[0].bundle.files.size() == 1);

    std::error_code ec;
    fs::remove_all(tempDir, ec);
}

TEST_CASE("queue store load skips completed-on-disk items") {
    namespace fs = std::filesystem;
    const fs::path tempDir = fs::current_path() / "tmp_queue_store_completed";
    const fs::path queuePath = tempDir / "queue_state.json";
    const fs::path downloadRoot = tempDir / "downloads";
    fs::create_directories(tempDir);

    romm::Status st;
    {
        std::lock_guard<std::mutex> lock(st.mutex);
        st.downloadQueue.push_back(makeQueueItem("42", "Complete Me", "switch", "complete.xci"));
    }
    std::string err;
    REQUIRE(romm::saveQueueState(st, err, queuePath.string()));
    REQUIRE(err.empty());

    // Simulate completed output: <downloadDir>/<platform>/<title_id>/...
    const fs::path completedDir = downloadRoot / "switch" / "Complete Me_42";
    fs::create_directories(completedDir);
    std::ofstream(completedDir / "dummy.bin").put('x');

    romm::Status loaded;
    romm::Config cfg;
    cfg.downloadDir = downloadRoot.string();
    REQUIRE(romm::loadQueueState(loaded, cfg, err, queuePath.string()));
    REQUIRE(err.empty());
    REQUIRE(loaded.downloadQueue.empty());

    std::error_code ec;
    fs::remove_all(tempDir, ec);
}

TEST_CASE("queue store load skips duplicates only for terminal history states") {
    namespace fs = std::filesystem;
    const fs::path tempDir = fs::current_path() / "tmp_queue_store_history";
    const fs::path queuePath = tempDir / "queue_state.json";
    fs::create_directories(tempDir);

    romm::Status src;
    {
        std::lock_guard<std::mutex> lock(src.mutex);
        src.downloadQueue.push_back(makeQueueItem("900", "Dup", "switch", "dup.xci"));
    }

    std::string err;
    REQUIRE(romm::saveQueueState(src, err, queuePath.string()));
    REQUIRE(err.empty());

    romm::Status loaded;
    {
        std::lock_guard<std::mutex> lock(loaded.mutex);
        auto qi = makeQueueItem("900", "Dup", "switch", "dup.xci");
        qi.state = romm::QueueState::Resumable;
        loaded.downloadHistory.push_back(std::move(qi));
    }
    romm::Config cfg;
    cfg.downloadDir = (tempDir / "downloads").string();

    REQUIRE(romm::loadQueueState(loaded, cfg, err, queuePath.string()));
    REQUIRE(err.empty());
    REQUIRE(loaded.downloadQueue.size() == 1);
    REQUIRE(loaded.downloadHistory.size() == 1);

    {
        std::lock_guard<std::mutex> lock(loaded.mutex);
        loaded.downloadQueue.clear();
        auto done = makeQueueItem("900", "Dup", "switch", "dup.xci");
        done.state = romm::QueueState::Completed;
        loaded.downloadHistory.push_back(std::move(done));
    }
    REQUIRE(romm::loadQueueState(loaded, cfg, err, queuePath.string()));
    REQUIRE(err.empty());
    REQUIRE(loaded.downloadQueue.empty());

    std::error_code ec;
    fs::remove_all(tempDir, ec);
}
