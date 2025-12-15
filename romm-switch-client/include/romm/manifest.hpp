#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace romm {

struct ManifestPart {
    int index{0};
    uint64_t size{0};
    std::string sha256; // optional
    bool completed{false}; // true if part finished and flushed
};

struct Manifest {
    std::string rommId;
    std::string fileId;
    std::string fsName;
    std::string url;
    uint64_t totalSize{0};
    uint64_t partSize{0};
    std::vector<ManifestPart> parts;
};

// Serialize/deserialize manifest as JSON strings (host-testable).
std::string manifestToJson(const Manifest& m);
bool manifestFromJson(const std::string& json, Manifest& out, std::string& err);

// Given a manifest and observed parts (sizes/hashes), decide what to resume/delete.
struct ResumePlan {
    std::vector<int> validParts;
    std::vector<int> invalidParts;
    uint64_t bytesHave{0};
    uint64_t bytesNeed{0};
    int partialIndex{-1};
    uint64_t partialBytes{0};
};

ResumePlan planResume(const Manifest& m,
                      const std::vector<std::pair<int, uint64_t>>& observedParts);

} // namespace romm
