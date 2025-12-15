#include "catch.hpp"
#include "romm/downloader.hpp"
#include "romm/manifest.hpp"
#include <filesystem>
#include <fstream>

namespace {
std::string writeTempManifest(const std::filesystem::path& root,
                              const romm::Manifest& m) {
    std::filesystem::create_directories(root / "temp" / "game.tmp");
    auto path = root / "temp" / "game.tmp" / "manifest.json";
    std::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
    out << romm::manifestToJson(m);
    return path.string();
}
} // namespace

TEST_CASE("loadLocalManifests seeds history from temp manifests") {
    std::filesystem::path tmp = std::filesystem::temp_directory_path() / "romm_manifest_test";
    std::filesystem::remove_all(tmp);
    romm::Manifest m;
    m.rommId = "123";
    m.fileId = "f1";
    m.fsName = "test.xci";
    m.url = "http://example/content";
    m.totalSize = 100;
    m.partSize = 50;
    m.parts.push_back(romm::ManifestPart{0, 50, "", true});
    m.parts.push_back(romm::ManifestPart{1, 50, "", false});
    writeTempManifest(tmp, m);

    romm::Status st;
    romm::Config cfg;
    cfg.downloadDir = tmp.string();
    std::string err;
    bool ok = romm::loadLocalManifests(st, cfg, err);
    REQUIRE(ok);
    REQUIRE(err.empty());
    REQUIRE(st.downloadHistory.size() == 1);
    REQUIRE(st.downloadHistory[0].game.id == "123");
    REQUIRE(st.downloadHistory[0].game.fsName == "test.xci");
    // Not all parts completed -> Pending badge
    REQUIRE(st.downloadHistory[0].state == romm::QueueState::Pending);

    std::filesystem::remove_all(tmp);
}
