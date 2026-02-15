#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace romm {

struct GitHubAsset {
    std::string name;
    std::string downloadUrl; // browser_download_url
    uint64_t sizeBytes{0};
};

struct GitHubRelease {
    std::string tagName;     // e.g. "v0.2.7"
    std::string name;        // release title
    std::string body;        // release notes (may be large)
    std::string htmlUrl;     // release page
    std::string publishedAt; // ISO timestamp
    std::vector<GitHubAsset> assets;
};

// Parse GitHub release JSON (as returned by /repos/:owner/:repo/releases/latest).
bool parseGitHubLatestReleaseJson(const std::string& json, GitHubRelease& out, std::string& outError);

// Normalize "v0.2.7" -> "0.2.7" (and trim).
std::string normalizeVersionTag(const std::string& tagOrVersion);

// Returns:
// -1 if a < b
//  0 if a == b
//  1 if a > b
// Comparison is numeric across dot-separated integer components. Non-numeric suffixes are ignored.
int compareVersions(const std::string& a, const std::string& b);

// Find the most appropriate .nro asset URL. Prefers exact name match if provided, otherwise any *.nro.
bool pickReleaseNroAsset(const GitHubRelease& rel,
                         GitHubAsset& out,
                         std::string& outError,
                         const std::string& preferredName = "romm-switch-client.nro");

} // namespace romm

