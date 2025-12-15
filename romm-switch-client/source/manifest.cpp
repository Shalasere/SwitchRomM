#include "romm/manifest.hpp"
#include "mini/json.hpp"
#include <algorithm>
#include <sstream>

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
    oss << "]}";
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
    // Map observed sizes by index; allow one partial (incomplete) part to be resumed.
    int partialIdx = -1;
    uint64_t partialBytes = 0;
    for (const auto& pr : observedParts) {
        int idx = pr.first;
        uint64_t sz = pr.second;
        auto it = std::find_if(m.parts.begin(), m.parts.end(), [&](const ManifestPart& p){ return p.index == idx; });
        if (it != m.parts.end()) {
            if (it->size == sz) {
                plan.validParts.push_back(idx);
                plan.bytesHave += sz;
            } else if (sz > 0 && sz < it->size) {
                // treat a single partial part as resumable
                if (partialIdx < 0 || idx > partialIdx) {
                    partialIdx = idx;
                    partialBytes = sz;
                }
            } else {
                plan.invalidParts.push_back(idx);
            }
        } else {
            plan.invalidParts.push_back(idx);
        }
    }
    if (partialIdx >= 0 && partialBytes > 0) {
        plan.partialIndex = partialIdx;
        plan.partialBytes = partialBytes;
        plan.bytesHave += partialBytes;
    }
    plan.bytesNeed = (m.totalSize > plan.bytesHave) ? (m.totalSize - plan.bytesHave) : 0;
    return plan;
}

} // namespace romm
