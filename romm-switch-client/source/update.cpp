#include "romm/update.hpp"

#include "mini/json.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace romm {

static std::string trimCopy(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

bool parseGitHubLatestReleaseJson(const std::string& json, GitHubRelease& out, std::string& outError) {
    out = GitHubRelease{};
    outError.clear();

    mini::Object obj;
    if (!mini::parse(json, obj)) {
        outError = "Failed to parse GitHub release JSON.";
        return false;
    }

    auto getStr = [&](const char* key, std::string& dst) {
        auto it = obj.find(key);
        if (it != obj.end() && it->second.type == mini::Value::Type::String) dst = it->second.str;
    };

    getStr("tag_name", out.tagName);
    getStr("name", out.name);
    getStr("body", out.body);
    getStr("html_url", out.htmlUrl);
    getStr("published_at", out.publishedAt);

    auto itAssets = obj.find("assets");
    if (itAssets != obj.end() && itAssets->second.type == mini::Value::Type::Array) {
        for (const auto& v : itAssets->second.array) {
            if (v.type != mini::Value::Type::Object) continue;
            GitHubAsset a;
            auto itN = v.object.find("name");
            if (itN != v.object.end() && itN->second.type == mini::Value::Type::String) a.name = itN->second.str;
            auto itU = v.object.find("browser_download_url");
            if (itU != v.object.end() && itU->second.type == mini::Value::Type::String) a.downloadUrl = itU->second.str;
            auto itS = v.object.find("size");
            if (itS != v.object.end() && itS->second.type == mini::Value::Type::Number) {
                if (itS->second.number > 0) a.sizeBytes = static_cast<uint64_t>(itS->second.number);
            }
            if (!a.name.empty() || !a.downloadUrl.empty()) out.assets.push_back(std::move(a));
        }
    }

    if (out.tagName.empty()) {
        outError = "GitHub release JSON missing tag_name.";
        return false;
    }
    return true;
}

std::string normalizeVersionTag(const std::string& tagOrVersion) {
    std::string s = trimCopy(tagOrVersion);
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    return trimCopy(s);
}

static std::vector<int64_t> parseVersionParts(const std::string& s) {
    std::vector<int64_t> parts;
    std::string v = normalizeVersionTag(s);
    // Keep digits/dots until we hit something else (e.g. "-alpha").
    std::string clean;
    clean.reserve(v.size());
    for (char c : v) {
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') clean.push_back(c);
        else break;
    }
    std::stringstream ss(clean);
    std::string tok;
    while (std::getline(ss, tok, '.')) {
        if (tok.empty()) {
            parts.push_back(0);
            continue;
        }
        char* endp = nullptr;
        long long n = std::strtoll(tok.c_str(), &endp, 10);
        if (endp == tok.c_str()) n = 0;
        parts.push_back(static_cast<int64_t>(n));
    }
    while (!parts.empty() && parts.back() == 0) parts.pop_back();
    return parts;
}

int compareVersions(const std::string& a, const std::string& b) {
    auto pa = parseVersionParts(a);
    auto pb = parseVersionParts(b);
    size_t n = std::max(pa.size(), pb.size());
    pa.resize(n, 0);
    pb.resize(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (pa[i] < pb[i]) return -1;
        if (pa[i] > pb[i]) return 1;
    }
    return 0;
}

static bool endsWithCaseInsensitive(const std::string& s, const std::string& suf) {
    if (s.size() < suf.size()) return false;
    size_t off = s.size() - suf.size();
    for (size_t i = 0; i < suf.size(); ++i) {
        char a = s[off + i];
        char b = suf[i];
        if (std::tolower(static_cast<unsigned char>(a)) != std::tolower(static_cast<unsigned char>(b))) return false;
    }
    return true;
}

bool pickReleaseNroAsset(const GitHubRelease& rel,
                         GitHubAsset& out,
                         std::string& outError,
                         const std::string& preferredName) {
    out = GitHubAsset{};
    outError.clear();
    if (rel.assets.empty()) {
        outError = "Release has no assets.";
        return false;
    }
    // Prefer exact-name match if present.
    for (const auto& a : rel.assets) {
        if (!preferredName.empty() && a.name == preferredName && !a.downloadUrl.empty()) {
            out = a;
            return true;
        }
    }
    // Otherwise pick the first .nro asset.
    for (const auto& a : rel.assets) {
        if (endsWithCaseInsensitive(a.name, ".nro") && !a.downloadUrl.empty()) {
            out = a;
            return true;
        }
    }
    outError = "No .nro asset found in release.";
    return false;
}

} // namespace romm

