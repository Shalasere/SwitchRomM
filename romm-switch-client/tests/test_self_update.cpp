#include "catch.hpp"

#include "romm/self_update.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

static std::string readAll(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string out;
    char buf[4096];
    for (;;) {
        size_t n = std::fread(buf, 1, sizeof(buf), f);
        if (n == 0) break;
        out.append(buf, buf + n);
    }
    std::fclose(f);
    return out;
}

static void writeAll(const std::string& path, const std::string& data) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
}

static std::filesystem::path makeTempDir() {
    auto base = std::filesystem::temp_directory_path();
    for (int i = 0; i < 1000; ++i) {
        auto p = base / ("romm_self_update_test_" + std::to_string(std::rand()) + "_" + std::to_string(i));
        std::error_code ec;
        if (std::filesystem::create_directories(p, ec)) return p;
    }
    // Fall back; test run will fail if we can't create directories.
    auto p = base / "romm_self_update_test_fallback";
    std::filesystem::create_directories(p);
    return p;
}

} // namespace

TEST_CASE("self_update: canonicalSelfNroPath enforces sdmc:/switch/.nro") {
    REQUIRE(romm::canonicalSelfNroPath("") ==
            "sdmc:/switch/romm_switch_client/romm-switch-client.nro");
    REQUIRE(romm::canonicalSelfNroPath("sdmc:/switch/foo/bar.nro") ==
            "sdmc:/switch/foo/bar.nro");
    REQUIRE(romm::canonicalSelfNroPath("sdmc:/romm_cache/bar.nro") ==
            "sdmc:/switch/romm_switch_client/romm-switch-client.nro");
    REQUIRE(romm::canonicalSelfNroPath("romfs:/romm-switch-client.nro") ==
            "sdmc:/switch/romm_switch_client/romm-switch-client.nro");
}

TEST_CASE("self_update: computeUpdateDirFromDownloadDir") {
    REQUIRE(romm::computeUpdateDirFromDownloadDir("") ==
            "sdmc:/switch/romm_switch_client/app_update");
    REQUIRE(romm::computeUpdateDirFromDownloadDir("sdmc:/romm_cache") ==
            "sdmc:/romm_cache/app_update");
    REQUIRE(romm::computeUpdateDirFromDownloadDir("sdmc:/romm_cache/") ==
            "sdmc:/romm_cache/app_update");
}

TEST_CASE("self_update: readTextFileTrim trims whitespace") {
    auto dir = makeTempDir();
    auto p = (dir / "pending.txt").string();
    writeAll(p, "  hello world \r\n");
    std::string out;
    REQUIRE(romm::readTextFileTrim(p, out));
    REQUIRE(out == "hello world");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("self_update: writeTextFileEnsureParent creates parent directories") {
    auto dir = makeTempDir();
    auto nested = dir / "a" / "b" / "c" / "pending.txt";
    // Ensure the parent doesn't exist.
    std::error_code ec;
    std::filesystem::remove_all(dir / "a", ec);
    REQUIRE_FALSE(std::filesystem::exists(nested.parent_path(), ec));

    REQUIRE(romm::writeTextFileEnsureParent(nested.string(), "staged_path_here"));
    REQUIRE(std::filesystem::exists(nested, ec));

    std::string got;
    REQUIRE(romm::readTextFileTrim(nested.string(), got));
    REQUIRE(got == "staged_path_here");

    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("self_update: applyPendingSelfUpdate ignores missing pending file") {
    auto dir = makeTempDir();
    auto self = (dir / "self.nro").string();
    writeAll(self, "SELF");

    auto pending = (dir / "update_pending.txt").string();
    auto res = romm::applyPendingSelfUpdate(self, pending, nullptr);
    REQUIRE_FALSE(res.hadPending);
    REQUIRE_FALSE(res.applied);
    REQUIRE(readAll(self) == "SELF");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("self_update: applyPendingSelfUpdate clears invalid pointer") {
    auto dir = makeTempDir();
    auto self = (dir / "self.nro").string();
    writeAll(self, "SELF");

    auto pending = (dir / "update_pending.txt").string();
    writeAll(pending, (dir / "missing.nro.new").string() + "\n");

    auto res = romm::applyPendingSelfUpdate(self, pending, nullptr);
    REQUIRE(res.hadPending);
    REQUIRE_FALSE(res.applied);
    REQUIRE(res.pendingCleared);
    REQUIRE_FALSE(std::filesystem::exists(pending));
    REQUIRE(readAll(self) == "SELF");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("self_update: applyPendingSelfUpdate clears pointer for non-nro staged file") {
    auto dir = makeTempDir();
    auto self = (dir / "self.nro").string();
    writeAll(self, "SELF");

    auto staged = (dir / "app_update" / "romm-switch-client.nro.new").string();
    writeAll(staged, "NOTNRO");
    writeAll(staged + ".part", "partial");

    auto pending = (dir / "update_pending.txt").string();
    writeAll(pending, staged + "\n");

    auto res = romm::applyPendingSelfUpdate(self, pending, nullptr);
    REQUIRE(res.hadPending);
    REQUIRE_FALSE(res.applied);
    REQUIRE(res.pendingCleared);
    REQUIRE_FALSE(std::filesystem::exists(pending));
    REQUIRE(std::filesystem::exists(staged));
    REQUIRE_FALSE(std::filesystem::exists(staged + ".part"));
    REQUIRE(readAll(self) == "SELF");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("self_update: applyPendingSelfUpdate applies staged file and keeps only last backup") {
    auto dir = makeTempDir();
    auto updateDir = (dir / "app_update");
    std::filesystem::create_directories(updateDir);

    auto self = (dir / "romm-switch-client.nro").string();
    writeAll(self, "OLD_SELF");

    // Pre-existing backup should be overwritten (keep only last backup).
    auto bak = (updateDir / "romm-switch-client.nro.bak").string();
    writeAll(bak, "OLD_BAK");

    auto staged = (updateDir / "romm-switch-client.nro.new").string();
    writeAll(staged, std::string("NRO0") + "NEW_SELF");
    writeAll(staged + ".part", "partial");

    auto pending = (dir / "update_pending.txt").string();
    writeAll(pending, staged + "\n");

    std::vector<std::string> logs;
    auto res = romm::applyPendingSelfUpdate(self, pending, [&](const std::string& m) { logs.push_back(m); });
    REQUIRE(res.hadPending);
    REQUIRE(res.applied);
    REQUIRE(res.pendingCleared);
    REQUIRE_FALSE(std::filesystem::exists(pending));
    REQUIRE_FALSE(std::filesystem::exists(staged));
    REQUIRE_FALSE(std::filesystem::exists(staged + ".part"));

    // Self now has the NRO magic.
    REQUIRE(readAll(self).rfind("NRO0", 0) == 0);
    // Backup contains the previous self content, not the old backup content.
    REQUIRE(readAll(bak) == "OLD_SELF");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("self_update: applyPendingSelfUpdate can apply even if self doesn't exist") {
    auto dir = makeTempDir();
    auto updateDir = (dir / "app_update");
    std::filesystem::create_directories(updateDir);

    auto self = (dir / "romm-switch-client.nro").string();
    // self does not exist

    auto staged = (updateDir / "romm-switch-client.nro.new").string();
    writeAll(staged, std::string("NRO0") + "NEW_SELF");

    auto pending = (dir / "update_pending.txt").string();
    writeAll(pending, staged + "\n");

    auto res = romm::applyPendingSelfUpdate(self, pending, nullptr);
    REQUIRE(res.hadPending);
    REQUIRE(res.applied);
    REQUIRE_FALSE(std::filesystem::exists(pending));
    REQUIRE(std::filesystem::exists(self));
    REQUIRE(readAll(self).rfind("NRO0", 0) == 0);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
