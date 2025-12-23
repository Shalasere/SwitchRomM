#include "romm/planner.hpp"
#include "romm/logger.hpp"
#include <algorithm>
#include <cctype>
#include <map>

namespace romm {

static std::string toLowerStr(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}

DownloadBundle buildBundleFromGame(const Game& g, const PlatformPrefs& prefs) {
    DownloadBundle bundle;
    bundle.romId = g.id;
    bundle.title = g.title;
    bundle.platformSlug = g.platformSlug;
    std::string slugLower = g.platformSlug;
    std::transform(slugLower.begin(), slugLower.end(), slugLower.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    bundle.mode = prefs.defaultMode;
    if (auto it = prefs.bySlug.find(slugLower); it != prefs.bySlug.end()) {
        if (!it->second.mode.empty()) bundle.mode = it->second.mode;
    }
    // Filter files by category=game
    std::vector<RomFile> gameFiles;
    for (const auto& rf : g.files) {
        std::string cat = rf.category;
        std::transform(cat.begin(), cat.end(), cat.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        if (cat.empty() || cat == "game") gameFiles.push_back(rf);
    }
    if (gameFiles.empty() && !g.downloadUrl.empty()) {
        RomFile rf;
        rf.id = g.fileId;
        rf.name = g.fsName.empty() ? g.title : g.fsName;
        rf.url = g.downloadUrl;
        rf.sizeBytes = g.sizeBytes;
        rf.category = "game";
        gameFiles.push_back(rf);
    }

    auto isIgnoredExt = [&](const std::string& name, const std::vector<std::string>& ignore) {
        auto dot = name.rfind('.');
        if (dot == std::string::npos) return false;
        std::string ext = toLowerStr(name.substr(dot));
        return std::find(ignore.begin(), ignore.end(), ext) != ignore.end();
    };
    std::vector<std::string> ignore = prefs.defaultIgnoreExt;
    if (auto it = prefs.bySlug.find(slugLower); it != prefs.bySlug.end()) {
        ignore.insert(ignore.end(), it->second.ignoreExt.begin(), it->second.ignoreExt.end());
    }
    // Remove ignored
    gameFiles.erase(std::remove_if(gameFiles.begin(), gameFiles.end(),
                    [&](const RomFile& f){ return isIgnoredExt(f.name, ignore); }),
                    gameFiles.end());

    std::vector<std::string> prefer;
    std::vector<std::string> avoidTokens;
    if (auto it = prefs.bySlug.find(slugLower); it != prefs.bySlug.end()) {
        prefer = it->second.preferExt;
        avoidTokens = it->second.avoidNameTokens;
    }
    auto hasAvoidToken = [&](const std::string& name) {
        std::string lower = toLowerStr(name);
        for (const auto& tok : avoidTokens) {
            if (!tok.empty() && lower.find(tok) != std::string::npos) return true;
        }
        return false;
    };

    if (bundle.mode == "all_files") {
        for (const auto& rf : gameFiles) {
            DownloadFileSpec spec;
            spec.fileId = rf.id;
            spec.name = rf.name;
            spec.relativePath = rf.path;
            spec.url = rf.url;
            spec.sizeBytes = rf.sizeBytes;
            spec.category = rf.category;
            bundle.files.push_back(std::move(spec));
        }
    } else if (bundle.mode == "bundle_best") {
        // Group by parent directory (if provided), pick the best-scoring group, download all files in that group.
        struct Group {
            std::vector<RomFile> files;
            int bestScore{0};
            uint64_t totalSize{0};
        };
        auto scoreFile = [&](const RomFile& rf) -> int {
            int sc = 0;
            auto dot = rf.name.rfind('.');
            if (dot != std::string::npos) {
                std::string ext = toLowerStr(rf.name.substr(dot));
                for (size_t i = 0; i < prefer.size(); ++i) {
                    if (ext == prefer[i]) {
                        sc = static_cast<int>(prefer.size() - i);
                        break;
                    }
                }
                if (ext == ".cue" || ext == ".gdi" || ext == ".m3u") sc += 50; // index files are strong signals
            }
            if (hasAvoidToken(rf.name)) sc -= 1000;
            return sc;
        };
        std::map<std::string, Group> groups;
        for (const auto& rf : gameFiles) {
            std::string dir = rf.path;
            auto slash = dir.find_last_of("/\\");
            if (slash != std::string::npos) dir = dir.substr(0, slash);
            dir = toLowerStr(dir);
            Group& ggrp = groups[dir];
            ggrp.files.push_back(rf);
            ggrp.totalSize += rf.sizeBytes;
            ggrp.bestScore = std::max(ggrp.bestScore, scoreFile(rf));
        }
        const Group* bestGroup = nullptr;
        for (const auto& kv : groups) {
            if (!bestGroup || kv.second.bestScore > bestGroup->bestScore ||
                (kv.second.bestScore == bestGroup->bestScore && kv.second.totalSize > bestGroup->totalSize)) {
                bestGroup = &kv.second;
            }
        }
        if (bestGroup) {
            for (const auto& rf : bestGroup->files) {
                DownloadFileSpec spec;
                spec.fileId = rf.id;
                spec.name = rf.name;
                spec.relativePath = rf.path.empty() ? rf.name : rf.path;
                spec.url = rf.url;
                spec.sizeBytes = rf.sizeBytes;
                spec.category = rf.category;
                bundle.files.push_back(std::move(spec));
            }
        }
    } else { // single_best
        auto score = [&](const RomFile& rf) -> int {
            auto dot = rf.name.rfind('.');
            if (dot == std::string::npos) return -1;
            std::string ext = toLowerStr(rf.name.substr(dot));
            for (size_t i = 0; i < prefer.size(); ++i) {
                if (ext == prefer[i]) return static_cast<int>(prefer.size() - i);
            }
            int sc = 0;
            if (hasAvoidToken(rf.name)) sc -= 1000;
            return sc;
        };
        const RomFile* best = nullptr;
        int bestScore = -1;
        uint64_t bestSize = 0;
        for (const auto& rf : gameFiles) {
            int sc = score(rf);
            if (sc > bestScore || (sc == bestScore && rf.sizeBytes > bestSize)) {
                best = &rf;
                bestScore = sc;
                bestSize = rf.sizeBytes;
            }
        }
        if (best) {
            DownloadFileSpec spec;
            spec.fileId = best->id;
            spec.name = best->name;
            spec.relativePath = best->path;
            spec.url = best->url;
            spec.sizeBytes = best->sizeBytes;
            spec.category = best->category;
            bundle.files.push_back(std::move(spec));
        }
    }

    if (bundle.files.empty()) {
        romm::logLine("buildBundleFromGame: no selectable files for game " + g.id);
    }
    return bundle;
}

} // namespace romm
