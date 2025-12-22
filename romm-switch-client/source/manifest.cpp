#include "romm/manifest.hpp"
#include "romm/models.hpp"
#include "mini/json.hpp"
#include <algorithm>
#include <sstream>
#include <unordered_map>

namespace romm {

namespace {
// Very small JSON string escaper for manifest string fields.
// Escapes backslash and quote; other characters pass through as-is.
std::string escapeJson(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}
} // namespace

std::string manifestToJson(const Manifest& m) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"romm_id\":\"" << escapeJson(m.rommId) << "\",";
    oss << "\"file_id\":\"" << escapeJson(m.fileId) << "\",";
    oss << "\"fs_name\":\"" << escapeJson(m.fsName) << "\",";
    oss << "\"url\":\"" << escapeJson(m.url) << "\",";
    oss << "\"total_size\":" << static_cast<unsigned long long>(m.totalSize) << ",";
    oss << "\"part_size\":" << static_cast<unsigned long long>(m.partSize) << ",";
    oss << "\"parts\":[";
    for (size_t i = 0; i < m.parts.size(); ++i) {
        const auto& p = m.parts[i];
        if (i > 0) oss << ",";
        oss << "{"
            << "\"index\":" << p.index << ","
            << "\"size\":" << static_cast<unsigned long long>(p.size) << ","
            << "\"sha256\":\"" << escapeJson(p.sha256) << "\"";
        if (p.completed) {
            oss << ",\"done\":true";
        }
        oss << "}";
    }
    oss << "]";
    if (!m.failureReason.empty()) {
        oss << ",\"failure_reason\":\"" << escapeJson(m.failureReason) << "\"";
    }
    oss << "}";
    return oss.str();
}

bool manifestFromJson(const std::string& json, Manifest& out, std::string& err) {
    out = Manifest{};
    mini::Object obj;
    if (!mini::parse(json, obj)) {
        err = "Invalid manifest JSON";
        return false;
    }
    auto getStr = [&](const char* key, std::string& dst) {
        auto it = obj.find(key);
        if (it != obj.end() && it->second.type == mini::Value::Type::String) {
            dst = it->second.str;
        }
    };
    auto getNum = [&](const char* key, uint64_t& dst) {
        auto it = obj.find(key);
        if (it != obj.end() && it->second.type == mini::Value::Type::Number) {
            dst = static_cast<uint64_t>(it->second.number);
        }
    };
    getStr("romm_id", out.rommId);
    getStr("file_id", out.fileId);
    getStr("fs_name", out.fsName);
    getStr("url", out.url);
    getNum("total_size", out.totalSize);
    getNum("part_size", out.partSize);
    getStr("failure_reason", out.failureReason);

    auto pit = obj.find("parts");
    if (pit != obj.end() && pit->second.type == mini::Value::Type::Array) {
        for (const auto& v : pit->second.array) {
            if (v.type != mini::Value::Type::Object) continue;
            ManifestPart p;
            auto& o = v.object;
            if (auto it = o.find("index"); it != o.end() && it->second.type == mini::Value::Type::Number)
                p.index = static_cast<int>(it->second.number);
            if (auto it = o.find("size"); it != o.end() && it->second.type == mini::Value::Type::Number)
                p.size = static_cast<uint64_t>(it->second.number);
            if (auto it = o.find("sha256"); it != o.end() && it->second.type == mini::Value::Type::String)
                p.sha256 = it->second.str;
            if (auto it = o.find("done"); it != o.end() && it->second.type == mini::Value::Type::Bool)
                p.completed = it->second.boolean;
            out.parts.push_back(p);
        }
    }

    if (out.rommId.empty() || out.fileId.empty() || out.fsName.empty() || out.url.empty() ||
        out.totalSize == 0 || out.partSize == 0) {
        err = "Manifest missing required fields";
        return false;
    }
    return true;
}

ResumePlan planResume(const Manifest& m,
                      const std::vector<std::pair<int, uint64_t>>& observedParts) {
    ResumePlan plan;

    // Build quick lookups for expected and observed sizes.
    std::unordered_map<int, uint64_t> expected;
    expected.reserve(m.parts.size());
    for (const auto& p : m.parts) {
        expected[p.index] = p.size;
    }
    std::unordered_map<int, uint64_t> observed;
    observed.reserve(observedParts.size());
    for (const auto& pr : observedParts) {
        observed[pr.first] = pr.second;
    }

    // Walk contiguous parts from index 0. Stop at the first missing/invalid/partial.
    int idx = 0;
    while (true) {
        auto expIt = expected.find(idx);
        if (expIt == expected.end()) break; // manifest doesn't expect this index

        auto obsIt = observed.find(idx);
        if (obsIt == observed.end()) break; // missing part stops contiguity

        uint64_t expectedSize = expIt->second;
        uint64_t haveSize = obsIt->second;

        if (haveSize == expectedSize) {
            plan.validParts.push_back(idx);
            plan.bytesHave += expectedSize;
            observed.erase(obsIt);
            ++idx;
            continue;
        }

        if (haveSize > 0 && haveSize < expectedSize) {
            // Allow exactly one partial part at the first missing index.
            plan.partialIndex = idx;
            plan.partialBytes = haveSize;
            plan.bytesHave += haveSize;
            observed.erase(obsIt);
        } else {
            // Wrong size or oversized: mark invalid and stop.
            plan.invalidParts.push_back(idx);
            observed.erase(obsIt);
        }
        break; // any deviation stops contiguity
    }

    // Anything observed beyond the contiguous boundary is invalid.
    for (const auto& kv : observed) {
        plan.invalidParts.push_back(kv.first);
    }

    plan.bytesNeed = (m.totalSize > plan.bytesHave) ? (m.totalSize - plan.bytesHave) : 0;
    return plan;
}

bool manifestCompatible(const Manifest& m, const Game& g, uint64_t expectedTotalSize, uint64_t expectedPartSize) {
    if (expectedTotalSize != 0 && m.totalSize != expectedTotalSize) return false;
    if (expectedPartSize != 0 && m.partSize != expectedPartSize) return false;
    if (!g.id.empty() && !m.rommId.empty() && m.rommId != g.id) return false;
    if (!g.fileId.empty() && !m.fileId.empty() && m.fileId != g.fileId) return false;
    // If we have strong identifiers (rommId/fileId), tolerate URL changes (tokens/hosts).
    bool haveStrongId = !g.id.empty() && !m.rommId.empty();
    haveStrongId = haveStrongId || (!g.fileId.empty() && !m.fileId.empty());
    if (!haveStrongId) {
        if (!g.downloadUrl.empty() && !m.url.empty() && m.url != g.downloadUrl) return false;
    }
    return true;
}

} // namespace romm
