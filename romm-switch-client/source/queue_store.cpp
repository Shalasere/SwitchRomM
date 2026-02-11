#include "romm/queue_store.hpp"

#include "romm/filesystem.hpp"
#include "mini/json.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace romm {

namespace {

std::string escapeJson(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

bool shouldPersistState(QueueState s) {
    return s == QueueState::Pending ||
           s == QueueState::Downloading ||
           s == QueueState::Finalizing ||
           s == QueueState::Resumable;
}

std::string queueToJson(const std::vector<QueueItem>& items) {
    std::ostringstream oss;
    oss << "{\"version\":1,\"items\":[";
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& q = items[i];
        if (i > 0) oss << ",";
        oss << "{";
        oss << "\"game\":{";
        oss << "\"id\":\"" << escapeJson(q.game.id) << "\",";
        oss << "\"title\":\"" << escapeJson(q.game.title) << "\",";
        oss << "\"platform_id\":\"" << escapeJson(q.game.platformId) << "\",";
        oss << "\"platform_slug\":\"" << escapeJson(q.game.platformSlug) << "\",";
        oss << "\"fs_name\":\"" << escapeJson(q.game.fsName) << "\",";
        oss << "\"file_id\":\"" << escapeJson(q.game.fileId) << "\",";
        oss << "\"cover_url\":\"" << escapeJson(q.game.coverUrl) << "\",";
        oss << "\"download_url\":\"" << escapeJson(q.game.downloadUrl) << "\",";
        oss << "\"size_bytes\":" << static_cast<unsigned long long>(q.game.sizeBytes);
        oss << "},";

        oss << "\"bundle\":{";
        oss << "\"rom_id\":\"" << escapeJson(q.bundle.romId) << "\",";
        oss << "\"title\":\"" << escapeJson(q.bundle.title) << "\",";
        oss << "\"platform_slug\":\"" << escapeJson(q.bundle.platformSlug) << "\",";
        oss << "\"mode\":\"" << escapeJson(q.bundle.mode) << "\",";
        oss << "\"files\":[";
        for (size_t j = 0; j < q.bundle.files.size(); ++j) {
            const auto& f = q.bundle.files[j];
            if (j > 0) oss << ",";
            oss << "{";
            oss << "\"file_id\":\"" << escapeJson(f.fileId) << "\",";
            oss << "\"name\":\"" << escapeJson(f.name) << "\",";
            oss << "\"url\":\"" << escapeJson(f.url) << "\",";
            oss << "\"size_bytes\":" << static_cast<unsigned long long>(f.sizeBytes) << ",";
            oss << "\"relative_path\":\"" << escapeJson(f.relativePath) << "\",";
            oss << "\"category\":\"" << escapeJson(f.category) << "\"";
            oss << "}";
        }
        oss << "]";
        oss << "}";
        oss << "}";
    }
    oss << "]}";
    return oss.str();
}

std::string valToString(const mini::Value& v) {
    if (v.type == mini::Value::Type::String) return v.str;
    if (v.type == mini::Value::Type::Number) {
        const double n = v.number;
        if (!std::isfinite(n)) return {};
        const double r = std::round(n);
        if (std::fabs(n - r) < 1e-9 && std::fabs(r) <= 9e15) {
            return std::to_string(static_cast<int64_t>(r));
        }
        std::ostringstream oss;
        oss << n;
        return oss.str();
    }
    return {};
}

uint64_t valToU64(const mini::Value& v) {
    if (v.type != mini::Value::Type::Number) return 0;
    if (!std::isfinite(v.number) || v.number < 0) return 0;
    return static_cast<uint64_t>(v.number);
}

void parseFileSpec(const mini::Object& o, DownloadFileSpec& f) {
    if (auto it = o.find("file_id"); it != o.end()) f.fileId = valToString(it->second);
    if (auto it = o.find("name"); it != o.end()) f.name = valToString(it->second);
    if (auto it = o.find("url"); it != o.end()) f.url = valToString(it->second);
    if (auto it = o.find("size_bytes"); it != o.end()) f.sizeBytes = valToU64(it->second);
    if (auto it = o.find("relative_path"); it != o.end()) f.relativePath = valToString(it->second);
    if (auto it = o.find("category"); it != o.end()) f.category = valToString(it->second);
}

void parseBundle(const mini::Object& o, DownloadBundle& b) {
    if (auto it = o.find("rom_id"); it != o.end()) b.romId = valToString(it->second);
    if (auto it = o.find("title"); it != o.end()) b.title = valToString(it->second);
    if (auto it = o.find("platform_slug"); it != o.end()) b.platformSlug = valToString(it->second);
    if (auto it = o.find("mode"); it != o.end()) b.mode = valToString(it->second);
    if (auto it = o.find("files"); it != o.end() && it->second.type == mini::Value::Type::Array) {
        for (const auto& v : it->second.array) {
            if (v.type != mini::Value::Type::Object) continue;
            DownloadFileSpec f{};
            parseFileSpec(v.object, f);
            if (!f.url.empty() && !f.name.empty() && f.sizeBytes > 0) {
                b.files.push_back(std::move(f));
            }
        }
    }
}

void parseGame(const mini::Object& o, Game& g) {
    if (auto it = o.find("id"); it != o.end()) g.id = valToString(it->second);
    if (auto it = o.find("title"); it != o.end()) g.title = valToString(it->second);
    if (auto it = o.find("platform_id"); it != o.end()) g.platformId = valToString(it->second);
    if (auto it = o.find("platform_slug"); it != o.end()) g.platformSlug = valToString(it->second);
    if (auto it = o.find("fs_name"); it != o.end()) g.fsName = valToString(it->second);
    if (auto it = o.find("file_id"); it != o.end()) g.fileId = valToString(it->second);
    if (auto it = o.find("cover_url"); it != o.end()) g.coverUrl = valToString(it->second);
    if (auto it = o.find("download_url"); it != o.end()) g.downloadUrl = valToString(it->second);
    if (auto it = o.find("size_bytes"); it != o.end()) g.sizeBytes = valToU64(it->second);
}

bool sameIdentity(const QueueItem& a, const QueueItem& b) {
    if (!a.game.id.empty() && !b.game.id.empty() && a.game.id == b.game.id) return true;
    if (!a.game.fileId.empty() && !b.game.fileId.empty() && a.game.fileId == b.game.fileId) return true;
    if (!a.game.fsName.empty() && !b.game.fsName.empty() && a.game.fsName == b.game.fsName) return true;
    return false;
}

bool hasIdentityMatch(const std::vector<QueueItem>& list, const QueueItem& item) {
    return std::any_of(list.begin(), list.end(), [&](const QueueItem& existing) {
        return sameIdentity(existing, item);
    });
}

bool hasTerminalHistoryMatch(const std::vector<QueueItem>& history, const QueueItem& item) {
    return std::any_of(history.begin(), history.end(), [&](const QueueItem& existing) {
        if (!sameIdentity(existing, item)) return false;
        return existing.state == QueueState::Completed || existing.state == QueueState::Cancelled;
    });
}

} // namespace

bool saveQueueState(const Status& status, std::string& outError, const std::string& path) {
    outError.clear();

    std::vector<QueueItem> snapshot;
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        snapshot.reserve(status.downloadQueue.size());
        for (const auto& q : status.downloadQueue) {
            if (shouldPersistState(q.state)) snapshot.push_back(q);
        }
    }

    std::error_code ec;
    const std::filesystem::path statePath(path);
    if (snapshot.empty()) {
        std::filesystem::remove(statePath, ec); // best effort clear stale queue snapshot
        return true;
    }

    std::filesystem::path parent = statePath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            outError = "Failed to create queue state dir: " + parent.string() + " err=" + ec.message();
            return false;
        }
    }

    const std::string payload = queueToJson(snapshot);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        outError = "Failed to open queue state file for write: " + path;
        return false;
    }
    out << payload;
    if (!out.good()) {
        outError = "Failed writing queue state file: " + path;
        return false;
    }
    return true;
}

bool loadQueueState(Status& status, const Config& cfg, std::string& outError, const std::string& path) {
    outError.clear();

    std::ifstream in(path, std::ios::binary);
    if (!in) return true; // no snapshot yet
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (json.empty()) return true;

    mini::Object root;
    if (!mini::parse(json, root)) {
        outError = "Invalid queue state JSON.";
        return false;
    }
    auto itemsIt = root.find("items");
    if (itemsIt == root.end() || itemsIt->second.type != mini::Value::Type::Array) {
        outError = "Queue state missing items array.";
        return false;
    }

    std::vector<QueueItem> recovered;
    for (const auto& v : itemsIt->second.array) {
        if (v.type != mini::Value::Type::Object) continue;
        QueueItem qi{};
        qi.state = QueueState::Pending;
        auto gameIt = v.object.find("game");
        if (gameIt != v.object.end() && gameIt->second.type == mini::Value::Type::Object) {
            parseGame(gameIt->second.object, qi.game);
        }
        auto bundleIt = v.object.find("bundle");
        if (bundleIt != v.object.end() && bundleIt->second.type == mini::Value::Type::Object) {
            parseBundle(bundleIt->second.object, qi.bundle);
        }

        if (qi.bundle.romId.empty()) qi.bundle.romId = qi.game.id;
        if (qi.bundle.title.empty()) qi.bundle.title = qi.game.title;
        if (qi.bundle.platformSlug.empty()) qi.bundle.platformSlug = qi.game.platformSlug;

        // Fallback for legacy/minimal snapshot entries.
        if (qi.bundle.files.empty() &&
            !qi.game.downloadUrl.empty() &&
            !qi.game.fileId.empty() &&
            !qi.game.fsName.empty() &&
            qi.game.sizeBytes > 0) {
            DownloadFileSpec f{};
            f.fileId = qi.game.fileId;
            f.name = qi.game.fsName;
            f.url = qi.game.downloadUrl;
            f.sizeBytes = qi.game.sizeBytes;
            qi.bundle.files.push_back(std::move(f));
        }

        if (qi.game.id.empty() || qi.bundle.files.empty()) continue;
        recovered.push_back(std::move(qi));
    }

    if (recovered.empty()) return true;

    std::lock_guard<std::mutex> lock(status.mutex);
    size_t added = 0;
    for (const auto& qi : recovered) {
        if (isGameCompletedOnDisk(qi.game, cfg)) continue;
        if (hasIdentityMatch(status.downloadQueue, qi)) continue;
        if (hasTerminalHistoryMatch(status.downloadHistory, qi)) continue;
        status.downloadQueue.push_back(qi);
        added++;
    }
    if (added > 0) {
        status.downloadQueueRevision++;
        status.downloadCompleted = false;
    }
    return true;
}

} // namespace romm
