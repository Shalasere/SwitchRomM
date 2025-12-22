#pragma once

#include <string>
#include <vector>
#include <atomic>

namespace romm {

struct Platform {
    std::string id;
    std::string name;
    std::string slug;
    int romCount{0};
};

struct RomFile {
    std::string id;
    std::string name;
    std::string path;    // optional relative path from API
    std::string url;
    uint64_t sizeBytes{0};
    std::string category; // e.g., "game", "dlc", "update"
};

struct Game {
    std::string id;
    std::string title;
    std::string platformId;
    std::string platformSlug;
    std::string fsName;
    std::string fileId; // preferred RomM file id (xci/nsp)
    std::string coverUrl;
    uint64_t sizeBytes{0};
    std::string downloadUrl;
    std::vector<RomFile> files; // full file list from API detail
    bool isLocal{false};
};

} // namespace romm
