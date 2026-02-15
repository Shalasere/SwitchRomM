#include "catch.hpp"

#include "romm/update.hpp"

TEST_CASE("Version compare", "[update]") {
    REQUIRE(romm::compareVersions("0.2.6", "0.2.6") == 0);
    REQUIRE(romm::compareVersions("v0.2.6", "0.2.6") == 0);
    REQUIRE(romm::compareVersions("0.2.7", "0.2.6") > 0);
    REQUIRE(romm::compareVersions("0.10.0", "0.2.99") > 0);
    REQUIRE(romm::compareVersions("0.2.6-alpha.1", "0.2.6") == 0); // suffix ignored for now
    REQUIRE(romm::compareVersions("1", "1.0.0") == 0);
}

TEST_CASE("Parse GitHub latest release JSON and pick NRO", "[update]") {
    const std::string json = R"JSON(
{
  "tag_name": "v0.2.7",
  "name": "Release v0.2.7",
  "html_url": "https://github.com/Shalasere/SwitchRomM/releases/tag/v0.2.7",
  "published_at": "2026-02-15T00:00:00Z",
  "assets": [
    { "name": "something.txt", "browser_download_url": "https://example.com/a.txt", "size": 10 },
    { "name": "romm-switch-client.nro", "browser_download_url": "https://example.com/romm-switch-client.nro", "size": 1234 }
  ]
}
)JSON";

    romm::GitHubRelease rel;
    std::string err;
    REQUIRE(romm::parseGitHubLatestReleaseJson(json, rel, err));
    REQUIRE(rel.tagName == "v0.2.7");
    REQUIRE(!rel.assets.empty());

    romm::GitHubAsset asset;
    REQUIRE(romm::pickReleaseNroAsset(rel, asset, err, "romm-switch-client.nro"));
    REQUIRE(asset.name == "romm-switch-client.nro");
    REQUIRE(asset.downloadUrl.find(".nro") != std::string::npos);
    REQUIRE(asset.sizeBytes == 1234);
}

