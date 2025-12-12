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
    bool isLocal{false};
};

} // namespace romm
