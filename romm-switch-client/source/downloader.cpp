#include "romm/downloader.hpp"
#include "romm/logger.hpp"
#include "romm/filesystem.hpp"
#include "romm/api.hpp"
#include "romm/util.hpp"
#include "romm/raii.hpp"
#include "romm/http_common.hpp"
#include "romm/manifest.hpp"
#include "romm/queue_store.hpp"
#include "romm/speed_test.hpp"
#include <switch.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <chrono>
#include <thread>
#include <fstream>
#include <sys/iosupport.h>
#include <switch/runtime/devices/fs_dev.h> // fsdevSetConcatenationFileAttribute

namespace romm {

namespace {

constexpr uint64_t kDbiPartSizeBytes = 0xFFFF0000ULL; // DBI/Tinfoil split size
constexpr uint64_t kFreeSpaceMarginBytes = 200ULL * 1024ULL * 1024ULL; // ~200MB margin
constexpr size_t kStreamBufferBytes = 256 * 1024;
constexpr int kMaxRetryBackoffMs = 2000;

struct DownloadContext {
    std::thread worker;
    std::atomic<bool> stopRequested{false};
    Status* status{nullptr};
    Config cfg;
    std::atomic<int> activeSocketFd{-1};
};

DownloadContext gCtx; // global download context shared with worker

static void recomputeTotals(Status& st) {
    uint64_t remaining = 0;
    for (auto& q : st.downloadQueue) {
        uint64_t sz = q.bundle.totalSize();
        if (sz == 0) sz = q.game.sizeBytes;
        remaining += sz;
    }
    st.totalDownloadBytes.store(st.totalDownloadedBytes.load() + remaining);
}

// Best-effort recursive directory delete used for cleaning stale temp folders.
static void removeDirRecursive(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return;
    std::filesystem::remove_all(path, ec);
    if (ec) {
        logLine("Warning: failed to remove dir " + path + " err=" + ec.message());
    } else {
        logLine("Removed dir " + path);
    }
}

// Remove empty parent directories up to (but not including) stopDir.
static void removeEmptyParents(std::filesystem::path p, const std::filesystem::path& stopDir) {
    std::error_code ec;
    while (p != stopDir && p.has_parent_path()) {
        if (!std::filesystem::exists(p, ec)) break;
        if (!std::filesystem::is_empty(p, ec)) break;
        std::filesystem::remove(p, ec);
        if (ec) break;
        p = p.parent_path();
    }
}

// Check (best effort) if there is enough free space at path for neededBytes + margin.
static bool queryFreeSpace(const std::string& path, uint64_t& outFreeBytes) {
    struct statvfs vfs{};
    if (statvfs(path.c_str(), &vfs) != 0) return false;
    outFreeBytes = static_cast<uint64_t>(vfs.f_bavail) * static_cast<uint64_t>(vfs.f_frsize);
    return true;
}

static bool ensureFreeSpace(const std::string& path, uint64_t neededBytes, uint64_t* outFreeBytes = nullptr) {
    uint64_t freeBytes = 0;
    if (!queryFreeSpace(path, freeBytes)) return true; // best effort
    if (outFreeBytes) *outFreeBytes = freeBytes;
    return freeBytes >= neededBytes + kFreeSpaceMarginBytes;
}

static void setDownloadFailureState(Status& status, bool failed, const std::string& message) {
    withStatusLock(status, [&]() {
        status.lastDownloadFailed.store(failed);
        status.lastDownloadError = message;
        return 0;
    });
    postWorkerEvent(status, WorkerEvent{WorkerEventType::DownloadFailureState, failed, message});
}

// Sanitize a string for filesystem use; strip disallowed chars and optionally shorten later.
static std::string safeName(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if (c >= 32 && c < 127 && c != '/' && c != '\\' && c != ':') out.push_back(static_cast<char>(c));
    }
    if (out.empty()) out = "rom";
    return out;
}

static std::string romFolderName(const Game& g) {
    std::string idSafe = safeName(!g.id.empty() ? g.id : g.fileId);
    if (idSafe.empty() && !g.fsName.empty()) idSafe = safeName(g.fsName);
    if (idSafe.empty()) idSafe = "rom";
    std::string titleSafe = safeName(g.title);
    if (titleSafe.empty()) return idSafe;
    return titleSafe + "_" + idSafe;
}

struct PreflightInfo {
    bool supportsRanges{false};
    uint64_t contentLength{0};
};

#ifdef UNIT_TEST
static void parseLengthAndRanges(const std::string& headers, PreflightInfo& info) {
    std::istringstream hs(headers);
    std::string line;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colonPos = line.find(':');
        if (colonPos == std::string::npos) continue;
        std::string key = line.substr(0, colonPos);
        std::string val = line.substr(colonPos + 1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
        std::string keyLower = key;
        for (auto& c : keyLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string valLower = val;
        for (auto& c : valLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (keyLower == "accept-ranges" && valLower.find("bytes") != std::string::npos) {
            info.supportsRanges = true;
        } else if (keyLower == "content-length") {
            info.contentLength = static_cast<uint64_t>(std::strtoull(val.c_str(), nullptr, 10));
        } else if (keyLower == "content-range") {
            auto slash = val.find('/');
            if (slash != std::string::npos) {
                std::string totalStr = val.substr(slash + 1);
                uint64_t total = static_cast<uint64_t>(std::strtoull(totalStr.c_str(), nullptr, 10));
                if (total > 0) info.contentLength = total;
            }
        }
    }
}
#endif

static uint64_t partSizeFor(const Config& cfg, uint64_t totalSize) {
    return cfg.fat32Safe ? kDbiPartSizeBytes : totalSize;
}

static Manifest buildManifestFor(const Game& g, uint64_t totalSize, uint64_t partSize) {
    Manifest m;
    m.rommId = g.id;
    m.fileId = g.fileId;
    m.fsName = g.fsName.empty() ? safeName(g.title) : g.fsName;
    m.url = g.downloadUrl;
    m.totalSize = totalSize;
    m.partSize = partSize == 0 ? totalSize : partSize;
    partSize = m.partSize;
    uint64_t remaining = totalSize;
    int idx = 0;
    while (remaining > 0) {
        uint64_t sz = (remaining > partSize) ? partSize : remaining;
        m.parts.push_back(ManifestPart{idx, sz, ""});
        remaining -= sz;
        idx++;
    }
    return m;
}

static bool writeManifestFile(const std::string& path, const Manifest& m) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << manifestToJson(m);
    return true;
}

static bool readManifestFile(const std::string& path, Manifest& m) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string err;
    return manifestFromJson(content, m, err);
}

static bool parsePartIndex(const std::string& name, int& outIdx) {
    if (name.size() <= 5 || name.substr(name.size() - 5) != ".part") return false;
    std::string stem = name.substr(0, name.size() - 5);
    if (stem.empty()) return false;
    for (char c : stem) {
        if (c < '0' || c > '9') return false;
    }
    outIdx = std::atoi(stem.c_str());
    return true;
}

// Preflight: try HEAD first; if it fails or is rejected, fall back to Range: 0-0 GET.
static bool preflight(const std::string& url, const std::string& authBasic, int timeoutSec, PreflightInfo& info) {
    info = {};
    auto doRequest = [&](const std::string& method,
                         bool addRange00,
                         int& outCode,
                         ParsedHttpResponse& outParsed) -> bool {
        outCode = 0;
        outParsed = ParsedHttpResponse{};
        std::vector<std::pair<std::string, std::string>> headers;
        if (!authBasic.empty()) headers.emplace_back("Authorization", "Basic " + authBasic);
        if (addRange00) headers.emplace_back("Range", "bytes=0-0");

        HttpRequestOptions opts;
        opts.timeoutSec = timeoutSec;
        opts.keepAlive = false;
        opts.decodeChunked = true;
        opts.activeSocketFd = &gCtx.activeSocketFd;

        HttpTransaction tx;
        std::string reqErr;
        if (!httpRequestBuffered(method, url, headers, opts, tx, reqErr)) {
            return false;
        }
        outParsed = tx.parsed;
        outCode = outParsed.statusCode;
        if (outCode >= 300 && outCode < 400 && !outParsed.location.empty()) {
            logLine("Redirect not supported (" + std::to_string(outCode) + ") to " + outParsed.location);
        }
        return true;
    };

    int code = 0;
    ParsedHttpResponse parsed{};
    // Try HEAD
    if (doRequest("HEAD", false, code, parsed) && code >= 200 && code < 300) {
        info.contentLength = parsed.contentLength;
        info.supportsRanges = parsed.acceptRanges;
        return info.contentLength > 0;
    }
    if (code != 0 && (code < 200 || code >= 300)) {
        logLine("Preflight HEAD returned HTTP " + std::to_string(code));
    }

    // Fallback: GET with Range 0-0
    if (!doRequest("GET", true, code, parsed)) return false;
    if (code == 206 || parsed.acceptRanges) info.supportsRanges = true;
    if (!(code == 200 || code == 206)) {
        logLine("Preflight Range GET returned HTTP " + std::to_string(code));
        return false;
    }
    uint64_t crTotal = parsed.hasContentRangeTotal ? parsed.contentRangeTotal : 0;
    if (crTotal > 0) {
        info.contentLength = crTotal;
    } else if (parsed.hasContentLength && parsed.contentLength > 0) {
        info.contentLength = parsed.contentLength;
    }
    return info.contentLength > 0;
}

#ifdef UNIT_TEST
bool parseLengthAndRangesForTest(const std::string& headers, bool& supportsRanges, uint64_t& contentLength) {
    PreflightInfo info;
    info.supportsRanges = false;
    info.contentLength = 0;
    parseLengthAndRanges(headers, info);
    supportsRanges = info.supportsRanges;
    contentLength = info.contentLength;
    return contentLength > 0;
}
#endif

// Stream a continuous HTTP GET (optionally with Range) and split into FAT32-friendly parts.
// TODO(manifest/hashes): track expected parts/sizes/hashes to validate resume beyond size-only.
static bool streamDownload(const std::string& url,
                           const std::string& authBasic,
                           bool useRange,
                           uint64_t startOffset,
                           uint64_t totalSize,
                           uint64_t partSize,
                           const std::string& tmpDir,
                           Status& status,
                           const Config& cfg,
                           std::string& err) {
    int timeoutSec = cfg.httpTimeoutSeconds > 0 ? cfg.httpTimeoutSeconds : 10;
    if (timeoutSec > 30) timeoutSec = 30;
    uint64_t expectedBody = totalSize - startOffset;
    const uint64_t kProbeBytes = 10ULL * 1024ULL * 1024ULL; // 10 MB probe for throughput log
    bool probeLogged = false;
    auto transferStart = std::chrono::steady_clock::now();
    logLine("Stream start: url=" + url + " range=" + (useRange ? "true" : "false") +
            " start=" + std::to_string(startOffset) + " expect=" + std::to_string(expectedBody));

    // Buffered writer that keeps one part file open at a time.
    FILE* partFile = nullptr;
    int currentPart = -1;
    auto closePart = [&]() {
        if (partFile) {
            fclose(partFile);
            partFile = nullptr;
            currentPart = -1;
        }
    };
    auto writeSpan = [&](uint64_t& globalOffset, const char* data, size_t len) -> bool {
        size_t remaining = len;
        size_t idx = 0;
        while (remaining > 0) {
            uint64_t partIdx = globalOffset / partSize;
            uint64_t partOff = globalOffset % partSize;
            uint64_t space = partSize - partOff;
            size_t toWrite = static_cast<size_t>(std::min<uint64_t>(space, remaining));
            if (currentPart != static_cast<int>(partIdx)) {
                uint64_t received = (globalOffset >= startOffset) ? (globalOffset - startOffset) : 0;
                uint64_t remainingBytes = (expectedBody > received) ? (expectedBody - received) : 0;
                uint64_t freeBytes = 0;
                if (!ensureFreeSpace(tmpDir, remainingBytes, &freeBytes)) {
                    err = "Not enough free space (need " + std::to_string(remainingBytes) +
                          " bytes + margin, have " + std::to_string(freeBytes) + ")";
                    logLine("Free-space recheck failed in stream: " + err);
                    closePart();
                    return false;
                }
                closePart();
                std::string partName = (partIdx < 10 ? "0" : "") + std::to_string(partIdx);
                std::string partPath = tmpDir + "/" + partName + ".part";
                partFile = fopen(partPath.c_str(), "r+b");
                if (!partFile) partFile = fopen(partPath.c_str(), "w+b");
                if (!partFile) { err = "Open part failed"; logLine("Open part failed: " + partPath); return false; }
                // Large stdio buffer to reduce syscalls.
                static thread_local char ioBuf[kStreamBufferBytes];
                setvbuf(partFile, ioBuf, _IOFBF, sizeof(ioBuf));
                if (fseek(partFile, static_cast<long>(partOff), SEEK_SET) != 0) {
                    err = "Seek failed";
                    logLine("Seek failed in part " + partPath + " offset=" + std::to_string(partOff));
                    closePart();
                    return false;
                }
                currentPart = static_cast<int>(partIdx);
            }
            size_t wn = fwrite(data + idx, 1, toWrite, partFile);
            if (wn != toWrite) { err = "Write failed"; closePart(); return false; }
            globalOffset += toWrite;
            idx += toWrite;
            remaining -= toWrite;
        }
        return true;
    };

    uint64_t globalOffset = startOffset;
    auto lastBeat = std::chrono::steady_clock::now();
    uint64_t bytesSinceBeat = 0;
    const uint64_t kLogEvery = 100ULL * 1024ULL * 1024ULL; // ~100MB
    ParsedHttpResponse parsedHeaders{};
    bool headersValidated = false;
    auto validateHeaders = [&]() -> bool {
        if (headersValidated) return true;
        const int statusCode = parsedHeaders.statusCode;
        if (statusCode >= 300 && statusCode < 400) {
            err = "Redirect not supported (HTTP " + std::to_string(statusCode) +
                  (parsedHeaders.location.empty() ? "" : " to " + parsedHeaders.location) + ")";
            return false;
        }
        if (useRange && statusCode != 206) {
            err = "Range not honored (status " + std::to_string(statusCode) + ")";
            return false;
        }
        if (!useRange && statusCode != 200) {
            err = "HTTP status " + std::to_string(statusCode);
            return false;
        }
        if (parsedHeaders.chunked) {
            err = "Chunked transfer not supported for streaming downloads";
            return false;
        }
        if (parsedHeaders.hasContentRange) {
            if (parsedHeaders.contentRangeStart != startOffset) {
                err = "Content-Range start mismatch";
                return false;
            }
            expectedBody = (parsedHeaders.contentRangeEnd - parsedHeaders.contentRangeStart) + 1;
        } else if (!useRange && parsedHeaders.hasContentLength && parsedHeaders.contentLength > 0) {
            expectedBody = parsedHeaders.contentLength;
        }
        if (parsedHeaders.hasContentLength && expectedBody && parsedHeaders.contentLength < expectedBody) {
            err = "Short body (Content-Length " + std::to_string(parsedHeaders.contentLength) +
                  " < expected " + std::to_string(expectedBody) + ")";
            return false;
        }
        logLine("Stream headers ok: status=" + std::to_string(statusCode) +
                " clen=" + std::to_string(parsedHeaders.contentLength) +
                " expected=" + std::to_string(expectedBody) +
                (useRange ? " (range)" : ""));
        headersValidated = true;
        return true;
    };

    std::vector<std::pair<std::string, std::string>> headers;
    if (!authBasic.empty()) headers.emplace_back("Authorization", "Basic " + authBasic);
    if (useRange && startOffset > 0) {
        headers.emplace_back("Range", "bytes=" + std::to_string(startOffset) + "-");
    }

    HttpRequestOptions opts;
    opts.timeoutSec = timeoutSec;
    opts.keepAlive = false;
    opts.decodeChunked = false;
    opts.cancelRequested = &gCtx.stopRequested;
    opts.activeSocketFd = &gCtx.activeSocketFd;

    std::string streamErr;
    bool ok = httpRequestStreamed(
        "GET", url, headers, opts, parsedHeaders,
        [&](const char* data, size_t len) -> bool {
            if (!validateHeaders()) return false;
            if (len == 0) return true;
            uint64_t receivedBefore = globalOffset - startOffset;
            if (receivedBefore >= expectedBody) return true;
            size_t toUse = static_cast<size_t>(
                std::min<uint64_t>(static_cast<uint64_t>(len), expectedBody - receivedBefore));
            if (toUse == 0) return true;
            if (!writeSpan(globalOffset, data, toUse)) return false;
            status.currentDownloadedBytes.fetch_add(toUse);
            status.totalDownloadedBytes.fetch_add(toUse);
            bytesSinceBeat += toUse;

            uint64_t received = globalOffset - startOffset;
            if (!probeLogged && received >= kProbeBytes) {
                double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - transferStart).count();
                if (secs <= 0.0) secs = 1e-6;
                double mbps = (received / (1024.0 * 1024.0)) / secs; // MB/s
                logLine("Throughput estimate ~" + std::to_string(mbps) + " MB/s (first 10MB)");
                {
                    std::lock_guard<std::mutex> lock(status.mutex);
                    status.lastSpeedMBps = mbps;
                }
                probeLogged = true;
            }

            auto now = std::chrono::steady_clock::now();
            if (bytesSinceBeat >= kLogEvery || now - lastBeat > std::chrono::seconds(10)) {
                double beatSecs = std::chrono::duration<double>(now - lastBeat).count();
                if (beatSecs > 0.0) {
                    double mbps = (bytesSinceBeat / (1024.0 * 1024.0)) / beatSecs; // MB/s
                    {
                        std::lock_guard<std::mutex> lock(status.mutex);
                        status.lastSpeedMBps = mbps;
                    }
                }
                std::string titleCopy;
                {
                    std::lock_guard<std::mutex> lock(status.mutex);
                    titleCopy = status.currentDownloadTitle;
                }
                logDebug("Heartbeat: " + titleCopy +
                         " cur=" + std::to_string(status.currentDownloadedBytes.load()) + "/" +
                         std::to_string(status.currentDownloadSize.load()) +
                         " total=" + std::to_string(status.totalDownloadedBytes.load()) + "/" +
                         std::to_string(status.totalDownloadBytes.load()),
                         "DL");
                lastBeat = now;
                bytesSinceBeat = 0;
            }
            return true;
        },
        streamErr);

    if (!headersValidated && !validateHeaders()) {
        closePart();
        return false;
    }
    closePart();

    if (!ok) {
        if (streamErr == "Cancelled" || gCtx.stopRequested.load()) {
            err = "Stopped";
            return false;
        }
        if (err.empty()) {
            err = !streamErr.empty() && streamErr != "Sink aborted" ? streamErr : "Stream failed";
        }
        logLine("Stream recv error: " + err);
        return false;
    }

    const uint64_t received = globalOffset - startOffset;
    if (gCtx.stopRequested.load()) { err = "Stopped"; return false; }
    if (received < expectedBody) { err = "Short read"; return false; }
    if (received > expectedBody) { err = "Overflow"; return false; }
    return true;
}

// Rename *.part -> 00/01... then move tmpDir to finalDir (archive bit set for multi-part).
static bool finalizeParts(const std::string& tmpDir, const std::string& finalDir) {
    // Drop manifest (avoid carrying metadata into the final folder).
    std::error_code rmManifestEc;
    std::filesystem::remove(tmpDir + "/manifest.json", rmManifestEc);
    // rename *.part -> 00/01... and move dir + set archive bit
    DIR* d = opendir(tmpDir.c_str());
    if (!d) return false;
    struct dirent* ent;
    std::vector<std::string> partFiles;
    while ((ent = readdir(d)) != nullptr) {
        std::string n = ent->d_name;
        if (n.size() > 5 && n.substr(n.size() - 5) == ".part") {
            partFiles.push_back(n);
        }
    }
    closedir(d);
    std::sort(partFiles.begin(), partFiles.end());

    // If only one part and it fits in a single file, emit a plain XCI/NSP file instead of a folder.
    if (partFiles.size() == 1) {
        std::string src = tmpDir + "/" + partFiles[0];
        struct stat st{};
        uint64_t sz = 0;
        if (stat(src.c_str(), &st) == 0) sz = static_cast<uint64_t>(st.st_size);
        char magicBuf[8] = {};
        std::ifstream mf(src, std::ios::binary);
        mf.read(magicBuf, sizeof(magicBuf));
        std::ostringstream magicHex;
        for (int i = 0; i < (int)mf.gcount(); ++i) {
            if (i) magicHex << " ";
            magicHex << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                     << (static_cast<int>(static_cast<unsigned char>(magicBuf[i])) & 0xFF);
        }
        logLine("Finalize single-part: " + src +
                " size=" + std::to_string(sz) +
                " magic=" + magicHex.str());
        mf.close(); // ensure handle closed before we move the file
        // best-effort remove any stale file
        std::error_code rmEc;
        std::filesystem::remove(finalDir, rmEc);
        if (rmEc) {
            logLine("Warning: could not remove existing " + finalDir + " err=" + rmEc.message());
        }
        std::error_code renEc;
        std::filesystem::rename(src, finalDir, renEc);
        if (renEc) {
            logLine("Failed to move single part " + src + " -> " + finalDir +
                    " err=" + renEc.message() + " errno=" + std::to_string(errno) +
                    " strerror=" + std::string(strerror(errno)));
            // fallback: copy then remove source
            std::error_code cpEc;
            std::filesystem::copy_file(src, finalDir,
                                       std::filesystem::copy_options::overwrite_existing, cpEc);
            if (cpEc) {
                logLine("Copy fallback failed " + src + " -> " + finalDir + " err=" + cpEc.message());
                return false;
            }
            std::error_code delEc;
            std::filesystem::remove(src, delEc);
            if (delEc) {
                logLine("Warning: failed to remove source after copy " + src + " err=" + delEc.message());
            }
        }
        // remove now-empty temp dir
        removeDirRecursive(tmpDir);
        logLine("Finalize complete (single part) for " + finalDir);
        return true;
    }

    logLine("Finalize multi-part: parts=" + std::to_string(partFiles.size()) + " dst=" + finalDir);
    for (auto& f : partFiles) {
        std::string src = tmpDir + "/" + f;
        std::string dst = tmpDir + "/" + f.substr(0, f.size() - 5); // strip .part
        if (rename(src.c_str(), dst.c_str()) != 0) {
            logLine("Failed to rename part " + src + " -> " + dst + " errno=" + std::to_string(errno));
            return false;
        }
    }
    // If a previous finalDir exists (stale/failed), clear it before moving new data in.
    removeDirRecursive(finalDir);
    if (rename(tmpDir.c_str(), finalDir.c_str()) != 0) {
        logLine("Failed to move " + tmpDir + " -> " + finalDir);
        return false;
    }
    // Best-effort: set concatenation (archive) attribute so DBI treats this as a packaged title folder.
    Result rc = fsdevSetConcatenationFileAttribute(finalDir.c_str());
    if (R_FAILED(rc)) {
        logLine("Warning: failed to set concatenation/archive bit on " + finalDir + " rc=" + std::to_string(rc));
    }
    logLine("Finalize complete for " + finalDir);
    return true;
}

// Download a single Game into FAT32-safe parts. Resumes completed parts; deletes partial fragments.
// Sanitize a relative path (may include directories) for output.
static std::string sanitizeRelativePath(const std::string& rel) {
    // Split on separators, drop "." and "..", collapse empties, clamp segment length.
    std::vector<std::string> segments;
    std::string cur;
    auto flush = [&]() {
        if (cur.empty()) return;
        if (cur == "." || cur == "..") {
            cur.clear();
            return;
        }
        if (cur.size() > 80) cur = cur.substr(0, 80);
        segments.push_back(cur);
        cur.clear();
    };
    for (char c : rel) {
        if (c == '/' || c == '\\') {
            flush();
        } else if (c >= 32 && c < 127 && c != ':') {
            cur.push_back(c);
        }
    }
    flush();
    std::string out;
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i) out.push_back('/');
        out += segments[i];
    }
    return out;
}

// Download a single file (Game-compatible) into FAT32-safe parts. Resumes completed parts; deletes partial fragments.
static bool downloadOneFile(Game g, const DownloadFileSpec* spec, Status& status, const Config& cfg) {
    std::string auth;
    if (!cfg.username.empty() || !cfg.password.empty()) {
        auth = romm::util::base64Encode(cfg.username + ":" + cfg.password);
    }
    std::string platformSlug = g.platformSlug.empty() ? "unknown" : g.platformSlug;
    std::string platSafe = safeName(platformSlug);
    std::string romSafe = safeName(!g.id.empty() ? g.id : g.fileId);
    std::string fileSafe = safeName(g.fileId);
    if (romSafe.empty()) romSafe = "rom";
    if (fileSafe.empty()) fileSafe = "file";
    // Final outputs live under <downloadDir>/<platform>/<title__id.ext>
    std::string baseDir = cfg.downloadDir + "/" + platSafe + "/" + romFolderName(g);
    ensureDirectory(baseDir);
    // Temps live under <downloadDir>/temp/<platform>/<romId>/<fileId>/...
    std::string tempRoot = cfg.downloadDir + "/temp/" + platSafe + "/" + romSafe + "/" + fileSafe;
    ensureDirectory(tempRoot);

    // free space check for full ROM upfront (best effort)
    uint64_t freeBytes = 0;
    if (!ensureFreeSpace(baseDir, g.sizeBytes, &freeBytes)) {
        std::string msg = "Not enough free space (need " + std::to_string(g.sizeBytes) +
                          " bytes + margin, have " + std::to_string(freeBytes) + ")";
        logLine(msg + " for " + g.title);
        setDownloadFailureState(status, true, msg);
        return false;
    }

    if (g.downloadUrl.empty()) {
        logLine("No download URL for " + g.title);
        setDownloadFailureState(status, true, "No download URL");
        return false;
    }

    std::string tmpName = safeName(g.title);
    if (tmpName.empty() && !g.fsName.empty()) tmpName = safeName(g.fsName);
    if (tmpName.size() > 12) tmpName = tmpName.substr(0, 12);
    std::string idSuffix = safeName(!g.id.empty() ? g.id : g.fileId);
    if (idSuffix.size() > 8) idSuffix = idSuffix.substr(0, 8);
    if (tmpName.empty()) tmpName = idSuffix.empty() ? "rom" : idSuffix;
    std::string tmpDir = tempRoot + "/" + tmpName;
    if (!idSuffix.empty()) tmpDir += "_" + idSuffix;
    tmpDir += ".tmp";
    ensureDirectory(tmpDir);
    logLine("Using temp dir: " + tmpDir);
    logLine("Download URL: " + g.downloadUrl);
    PreflightInfo pf;
    uint64_t originalSize = g.sizeBytes;
    if (!preflight(g.downloadUrl, auth, cfg.httpTimeoutSeconds, pf)) {
        logLine("Preflight failed for " + g.title + " (HEAD/Range probe). Aborting download.");
        setDownloadFailureState(status, true, "Preflight failed");
        // Persist a manifest with failure reason so restart shows the failure.
        Manifest failManifest = buildManifestFor(g, g.sizeBytes, partSizeFor(cfg, g.sizeBytes));
        failManifest.failureReason = "Preflight failed (HEAD/Range)";
        writeManifestFile(tmpDir + "/manifest.json", failManifest);
        return false;
    } else {
        logLine("Preflight for " + g.title + " len=" + std::to_string(pf.contentLength) +
                " ranges=" + (pf.supportsRanges ? "true" : "false"));
    }
    uint64_t effectiveSize = pf.contentLength ? pf.contentLength : g.sizeBytes;
    if (pf.contentLength != 0 && pf.contentLength != g.sizeBytes) {
        logLine("Warning: server size " + std::to_string(pf.contentLength) + " differs from metadata " + std::to_string(g.sizeBytes));
    }
    if (effectiveSize == 0) {
        effectiveSize = g.sizeBytes;
    }
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        status.currentDownloadSize.store(effectiveSize);
        g.sizeBytes = effectiveSize;
        if (!status.downloadQueue.empty()) {
            // Replace the queued size so recomputeTotals uses the effective size.
            status.downloadQueue.front().game.sizeBytes = effectiveSize;
        }
        uint64_t curTotal = status.totalDownloadBytes.load();
        if (curTotal >= originalSize) {
            status.totalDownloadBytes.store(curTotal - originalSize + effectiveSize);
        } else {
            status.totalDownloadBytes.store(effectiveSize);
        }
    }

    uint64_t totalSize = status.currentDownloadSize.load();
    uint64_t partSize = partSizeFor(cfg, totalSize);
    bool refreshedMetadata = false;
    const uint64_t kTinyContentThreshold = 1024ULL * 1024ULL; // 1 MB

    // Prepare manifest (load existing or create new) to drive resume decisions.
    const std::string manifestPath = tmpDir + "/manifest.json";
    Manifest manifest;
    bool haveManifest = readManifestFile(manifestPath, manifest);
    bool needRewrite = true;
    if (haveManifest) {
        // Reuse only if consistent with current download parameters.
        if (manifestCompatible(manifest, g, totalSize, partSize) &&
            manifest.failureReason.empty()) {
            needRewrite = false;
        }
    }
    if (needRewrite) {
        manifest = buildManifestFor(g, totalSize, partSize);
        writeManifestFile(manifestPath, manifest);
    }

    // Inspect existing parts for resume; only count parts matching manifest sizes.
    std::vector<std::pair<int, uint64_t>> observedParts;
    DIR* d = opendir(tmpDir.c_str());
    if (d) {
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            int idx = -1;
            if (!parsePartIndex(ent->d_name, idx)) continue;
            std::string p = tmpDir + "/" + ent->d_name;
            struct stat st{};
            if (stat(p.c_str(), &st) == 0) {
                observedParts.push_back({idx, static_cast<uint64_t>(st.st_size)});
            }
        }
        closedir(d);
    }
    auto resumePlan = planResume(manifest, observedParts);
    logLine("Resume plan: valid=" + std::to_string(resumePlan.validParts.size()) +
            " partial=" + std::to_string(resumePlan.partialIndex) +
            " bytesHave=" + std::to_string(resumePlan.bytesHave) +
            " bytesNeed=" + std::to_string(resumePlan.bytesNeed));
    // Drop any invalid parts so we never append onto bad data.
    for (int idx : resumePlan.invalidParts) {
        std::string p = tmpDir + "/";
        if (idx < 10) p += "0";
        p += std::to_string(idx) + ".part";
        std::error_code ec;
        std::filesystem::remove(p, ec);
        if (ec) {
            logLine("Warning: failed to remove invalid part " + p + " err=" + ec.message());
        }
    }
    if (resumePlan.partialIndex >= 0 && resumePlan.partialBytes > 0) {
        logLine("Resuming partial part idx=" + std::to_string(resumePlan.partialIndex) +
                " bytes=" + std::to_string(resumePlan.partialBytes));
    }
    // Mark validated parts as completed in manifest and persist.
    if (!resumePlan.validParts.empty()) {
        for (auto& part : manifest.parts) {
            if (std::find(resumePlan.validParts.begin(), resumePlan.validParts.end(), part.index) != resumePlan.validParts.end()) {
                part.completed = true;
            }
        }
        writeManifestFile(manifestPath, manifest);
    }

    uint64_t haveBytes = std::min<uint64_t>(resumePlan.bytesHave, totalSize);
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        status.currentDownloadSize.store(totalSize);
        status.currentDownloadedBytes.store(haveBytes);
        status.currentDownloadTitle = g.title;
    }
    logLine("Resume state: haveBytes=" + std::to_string(haveBytes) +
            " total=" + std::to_string(totalSize) +
            " ranges=" + (pf.supportsRanges ? "true" : "false"));
    // Count any already-present bytes toward total downloaded so the bar doesn't regress
    uint64_t creditedExisting = 0;
    if (haveBytes > 0) {
        status.totalDownloadedBytes.fetch_add(haveBytes);
        creditedExisting = haveBytes;
    }

    if (haveBytes >= totalSize) {
        logLine("Already have full size for " + g.title);
    }

    const int maxAttempts = 3;
    int attempt = 0;
    std::string err;
    bool okStream = false;
    auto refreshMetadata = [&]() mutable -> bool {
        if (refreshedMetadata) return false;
        refreshedMetadata = true;
        logLine("Refreshing metadata for " + g.title + " after bad response");
        std::string enrichErr;
        if (!enrichGameWithFiles(cfg, g, enrichErr)) {
            logLine("Metadata refresh failed: " + enrichErr);
            err = enrichErr;
            return false;
        }
        removeDirRecursive(tmpDir);
        ensureDirectory(tmpDir);
        manifest = buildManifestFor(g, g.sizeBytes, partSizeFor(cfg, g.sizeBytes));
        writeManifestFile(manifestPath, manifest);
        haveBytes = 0;
        creditedExisting = 0;
        status.currentDownloadedBytes.store(0);
        status.totalDownloadedBytes.store(0);
        status.currentDownloadSize.store(g.sizeBytes);
        {
            std::lock_guard<std::mutex> lock(status.mutex);
            status.currentDownloadTitle = g.title;
        }
        if (!preflight(g.downloadUrl, auth, cfg.httpTimeoutSeconds, pf)) {
            logLine("Preflight after refresh failed");
            err = "Preflight after refresh failed";
            return false;
        }
        logLine("Refresh succeeded; new URL=" + g.downloadUrl + " len=" + std::to_string(pf.contentLength));
        totalSize = pf.contentLength ? pf.contentLength : g.sizeBytes;
        status.currentDownloadSize.store(totalSize);
        return true;
    };
    // If preflight returned an implausibly tiny length (e.g., HTML error page), try one refresh up front.
    if (pf.contentLength > 0 && pf.contentLength < kTinyContentThreshold) {
        logLine("Tiny Content-Length (" + std::to_string(pf.contentLength) + " bytes) for " + g.title + "; attempting metadata refresh");
        if (refreshMetadata()) {
            totalSize = status.currentDownloadSize.load();
        } else {
            err = "Server returned tiny Content-Length (" + std::to_string(pf.contentLength) + " bytes)";
            return false;
        }
    }

    while (attempt < maxAttempts && !okStream && !gCtx.stopRequested.load()) {
        bool useRange = pf.supportsRanges && haveBytes > 0;
        if (!pf.supportsRanges && haveBytes > 0) {
            logLine("Server does not support Range; restarting download for " + g.title);
            removeDirRecursive(tmpDir);
            ensureDirectory(tmpDir);
            if (creditedExisting > 0) {
                uint64_t curTotal = status.totalDownloadedBytes.load();
                if (curTotal >= creditedExisting) {
                    status.totalDownloadedBytes.fetch_sub(creditedExisting);
                } else {
                    status.totalDownloadedBytes.store(0);
                }
                creditedExisting = 0;
            }
            status.currentDownloadedBytes.store(0);
            haveBytes = 0;
            useRange = false;
        }
        err.clear();
        uint64_t totalBefore = status.totalDownloadedBytes.load();
        logLine("Begin stream attempt " + std::to_string(attempt + 1) +
                " range=" + (useRange ? "true" : "false") +
                " haveBytes=" + std::to_string(haveBytes) +
                " totalSize=" + std::to_string(totalSize));
        okStream = streamDownload(g.downloadUrl, auth, useRange, haveBytes, totalSize, partSize, tmpDir, status, cfg, err);
        if (!okStream) {
            logLine("Download attempt " + std::to_string(attempt + 1) + " failed: " + err);
            // Roll back bytes credited during this failed attempt so overall doesn't exceed 100%.
            uint64_t totalAfter = status.totalDownloadedBytes.load();
            if (totalAfter > totalBefore) {
                uint64_t delta = totalAfter - totalBefore;
                if (delta > totalAfter) delta = totalAfter; // safety
                status.totalDownloadedBytes.fetch_sub(delta);
            }
            status.currentDownloadedBytes.store(haveBytes);
            attempt++;
            if (gCtx.stopRequested.load()) break;
            // Backoff between attempts (capped) to be friendlier on slow/unstable links.
            int backoffMs = 500 * attempt;
            if (backoffMs > kMaxRetryBackoffMs) backoffMs = kMaxRetryBackoffMs;
            std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
            if (!err.empty() && err.find("HTTP status 404") != std::string::npos) {
                // Stale URL (manifest pointing to old file_id). Try to refresh once.
                if (refreshMetadata()) {
                    attempt = 0; // fresh attempts after refresh
                    continue;
                }
            }
            // If ranges unsupported, reset counters so UI reflects restart
            if (!pf.supportsRanges) {
                status.currentDownloadedBytes.store(0);
                haveBytes = 0;
            } else {
                // Recompute haveBytes from disk for resume
                uint64_t newHave = 0;
                DIR* d = opendir(tmpDir.c_str());
                if (d) {
                    struct dirent* ent;
                    while ((ent = readdir(d)) != nullptr) {
                        std::string n = ent->d_name;
                        if (n.size() > 5 && n.substr(n.size() - 5) == ".part") {
                            std::string p = tmpDir + "/" + n;
                            struct stat st{};
                            if (stat(p.c_str(), &st) == 0) newHave += (uint64_t)st.st_size;
                        }
                    }
                    closedir(d);
                }
                haveBytes = std::min<uint64_t>(newHave, totalSize);
                status.currentDownloadedBytes.store(haveBytes);
                // Keep overall in sync with on-disk bytes after a retry: do not let overall drop below haveBytes.
                uint64_t curTotal = status.totalDownloadedBytes.load();
                if (curTotal < haveBytes) {
                    status.totalDownloadedBytes.store(haveBytes);
                }
            }
        }
    }
    if (!okStream) {
        std::string errCopy = err.empty() ? "Download failed" : err;
        setDownloadFailureState(status, true, errCopy);
        logLine("Download failed: " + errCopy);
        return false;
    }

    // Build final output path
    std::string relOut;
    if (spec && !spec->relativePath.empty()) {
        relOut = sanitizeRelativePath(spec->relativePath);
    } else if (!g.fsName.empty()) {
        relOut = sanitizeRelativePath(g.fsName);
    } else if (spec) {
        relOut = sanitizeRelativePath(spec->name);
    } else {
        relOut = sanitizeRelativePath(g.title);
        if (relOut.empty()) relOut = "rom";
        if (!idSuffix.empty()) relOut += "_" + idSuffix;
        relOut += ".nsp";
    }
    if (relOut.empty()) relOut = "rom_" + idSuffix + ".nsp";
    std::filesystem::path finalPath = std::filesystem::path(baseDir) / std::filesystem::path(relOut);
    ensureDirectory(finalPath.parent_path().string());
    // If the final path already exists and matches expected size, treat as complete.
    if (spec && spec->sizeBytes > 0) {
        std::error_code sizeEc;
        if (std::filesystem::exists(finalPath, sizeEc) && std::filesystem::is_regular_file(finalPath, sizeEc)) {
            uint64_t existing = static_cast<uint64_t>(std::filesystem::file_size(finalPath, sizeEc));
            if (!sizeEc && existing == spec->sizeBytes) {
                logLine("Skipping existing complete file " + finalPath.string());
                status.totalDownloadedBytes.fetch_add(existing);
                status.currentDownloadedBytes.store(existing);
                return true;
            }
        }
    }
    // Avoid overwriting an existing file/dir by disambiguating if path already exists.
    std::error_code finalEc;
    if (std::filesystem::exists(finalPath, finalEc)) {
        std::string suffix = idSuffix.empty() ? "dup" : idSuffix;
        finalPath += ("." + suffix);
    }
    logLine("Finalize: moving temp to " + finalPath.string());
    bool okFinal = finalizeParts(tmpDir, finalPath.string());
    if (!okFinal) {
        setDownloadFailureState(status, true, "Finalize failed");
        return false;
    }
    // Clean up temp root for this fileId now that finalize succeeded.
    removeDirRecursive(tempRoot);
    // Remove any empty parent directories under <downloadDir>/temp/<platform>/<romId>/...
    std::filesystem::path stop = std::filesystem::path(cfg.downloadDir) / "temp";
    removeEmptyParents(std::filesystem::path(tempRoot).parent_path(), stop);
    removeEmptyParents(std::filesystem::path(tempRoot).parent_path().parent_path(), stop);
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        // Keep UI counters aligned with the completed file.
        status.currentDownloadedBytes.store(totalSize);
        status.totalDownloadedBytes.store(status.totalDownloadedBytes.load()); // unchanged, already includes current
        status.lastDownloadError.clear();
        status.lastDownloadFailed.store(false);
    }
    logLine("Download complete: " + g.title);
    return true;
}

static DownloadBundle bundleFromGame(const Game& g) {
    DownloadBundle b;
    b.romId = g.id;
    b.title = g.title;
    b.platformSlug = g.platformSlug;
    DownloadFileSpec f;
    f.fileId = g.fileId;
    f.name = g.fsName.empty() ? g.title : g.fsName;
    f.url = g.downloadUrl;
    f.sizeBytes = g.sizeBytes;
    b.files.push_back(std::move(f));
    return b;
}

// Download a bundle (supports multiple files iteratively).
static bool downloadBundle(const DownloadBundle& bundle, Status& status, const Config& cfg) {
    DownloadBundle b = bundle;
    if (b.files.empty()) {
        logLine("Bundle has no files; falling back to single file from game metadata");
        return false;
    }
    status.currentDownloadFileCount.store(b.files.size());
    bool ok = true;
    for (size_t i = 0; i < b.files.size(); ++i) {
        const auto& f = b.files[i];
        Game g;
        g.id = b.romId;
        g.title = b.title;
        g.platformSlug = b.platformSlug;
        g.fsName = f.name;
        g.fileId = f.fileId;
        g.downloadUrl = f.url;
        g.sizeBytes = f.sizeBytes;
        status.currentDownloadIndex.store(static_cast<size_t>(i));
        if (!downloadOneFile(g, &f, status, cfg)) {
            ok = false;
            break;
        }
    }
    return ok;
}

// Background worker: processes the downloadQueue sequentially, updating Status.
static void workerLoop() {
    Status* st = gCtx.status;
    Config cfg = gCtx.cfg;
    if (!st) return;
    // NOTE: this thread mutates Status; use Status::mutex to guard non-atomic fields.
    st->downloadWorkerRunning.store(true);
    {
        std::lock_guard<std::mutex> lock(st->mutex);
        st->downloadCompleted = false;
        st->totalDownloadBytes.store(0);
        st->totalDownloadedBytes.store(0);
        for (auto& q : st->downloadQueue) {
            uint64_t sz = q.bundle.totalSize();
            if (sz == 0) sz = q.game.sizeBytes;
            st->totalDownloadBytes.fetch_add(sz);
        }
        for (auto& q : st->downloadQueue) q.state = QueueState::Pending;
        st->downloadQueueRevision++;
    }
    setDownloadFailureState(*st, false, "");
    st->currentDownloadFileCount.store(0);
    logLine("Worker start, total bytes=" + std::to_string(st->totalDownloadBytes.load()));
    while (true) {
        QueueItem next;
            {
                std::lock_guard<std::mutex> lock(st->mutex);
                if (st->downloadQueue.empty() || gCtx.stopRequested.load()) break;
                st->currentDownloadIndex.store(0);
                next = st->downloadQueue.front(); // copy
                if (next.bundle.files.empty()) {
                    next.bundle = bundleFromGame(next.game);
                    st->downloadQueue.front().bundle = next.bundle;
                }
                // Prime UI fields for the next item.
                uint64_t bundleSize = next.bundle.totalSize();
                if (bundleSize == 0) bundleSize = next.game.sizeBytes;
                st->currentDownloadTitle = next.bundle.title.empty() ? next.game.title : next.bundle.title;
                st->currentDownloadSize.store(bundleSize);
                st->currentDownloadedBytes.store(0);
                st->currentDownloadFileCount.store(next.bundle.files.empty() ? 1 : next.bundle.files.size());
                st->downloadQueue.front().state = QueueState::Downloading;
                st->downloadQueueRevision++;
            }
        setDownloadFailureState(*st, false, "");
        if (!downloadBundle(next.bundle, *st, cfg)) {
            bool wasStopped = gCtx.stopRequested.load();
            logLine(std::string("Download failed or stopped for ") + next.game.title +
                    (wasStopped ? " (stop requested)" : ""));
            if (wasStopped) {
                setDownloadFailureState(*st, false, "");
            } else {
                std::string errCopy;
                {
                    std::lock_guard<std::mutex> lock(st->mutex);
                    errCopy = st->lastDownloadError.empty() ? "Download failed" : st->lastDownloadError;
                }
                setDownloadFailureState(*st, true, errCopy);
            }
            bool queueChanged = false;
            {
                std::lock_guard<std::mutex> lock(st->mutex);
                if (!st->downloadQueue.empty()) {
                    if (wasStopped) {
                        // Preserve interrupted item in active queue so restart/next run can resume.
                        st->downloadQueue.front().state = QueueState::Resumable;
                        st->downloadQueue.front().error = "Interrupted";
                        st->downloadQueueRevision++;
                        queueChanged = true;
                    } else {
                        st->downloadQueue.front().state = QueueState::Failed;
                        st->downloadQueue.front().error = st->lastDownloadError;
                        st->downloadHistory.push_back(st->downloadQueue.front());
                        st->downloadQueue.erase(st->downloadQueue.begin());
                        st->downloadQueueRevision++;
                        st->downloadHistoryRevision++;
                        queueChanged = true;
                    }
                }
                recomputeTotals(*st);
            }
            if (queueChanged) {
                std::string qerr;
                if (!saveQueueState(*st, qerr) && !qerr.empty()) {
                    logLine("Queue state save warning: " + qerr);
                }
            }
            continue;
        }
        bool queueChanged = false;
        {
            std::lock_guard<std::mutex> lock(st->mutex);
            if (!st->downloadQueue.empty()) {
                st->downloadQueue.front().state = QueueState::Completed;
                st->downloadHistory.push_back(st->downloadQueue.front());
                st->downloadQueue.erase(st->downloadQueue.begin());
                st->downloadQueueRevision++;
                st->downloadHistoryRevision++;
                queueChanged = true;
            }
            recomputeTotals(*st);
        }
        if (queueChanged) {
            std::string qerr;
            if (!saveQueueState(*st, qerr) && !qerr.empty()) {
                logLine("Queue state save warning: " + qerr);
            }
        }
    }
    st->downloadWorkerRunning.store(false);
    st->currentDownloadFileCount.store(0);
    bool postCompletion = false;
    {
        std::lock_guard<std::mutex> lock(st->mutex);
        if (st->downloadQueue.empty() && !gCtx.stopRequested.load() && !st->lastDownloadFailed.load()) {
            postCompletion = true;
        }
    }
    if (postCompletion) {
        postWorkerEvent(*st, WorkerEvent{WorkerEventType::DownloadCompletion, false, ""});
        logLine("All downloads complete.");
    }
    logLine("Worker done.");
}

} // namespace

bool loadLocalManifests(Status& status, const Config& cfg, std::string& outError) {
    namespace fs = std::filesystem;
    outError.clear();
    const fs::path tempRoot = fs::path(cfg.downloadDir) / "temp";
    if (!fs::exists(tempRoot)) return true; // nothing to load

    struct Found {
        Manifest manifest;
        fs::path dirPath;
        std::string platformSlug;
    };
    std::vector<Found> manifests;
    for (const auto& entry : fs::recursive_directory_iterator(tempRoot)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().filename() != "manifest.json") continue;
        Manifest m;
        if (!readManifestFile(entry.path().string(), m)) continue;
        fs::path parent = entry.path().parent_path();
        // temp/<platform>/<rom>/<file>/... -> extract platform slug as first element after tempRoot
        std::string platSlug = "unknown";
        fs::path rel;
        std::error_code ec;
        rel = fs::relative(parent, tempRoot, ec);
        if (!ec && rel.has_parent_path()) {
            auto it = rel.begin();
            if (it != rel.end()) {
                platSlug = it->string();
            }
        }
        manifests.push_back({m, parent, platSlug});
    }

    std::lock_guard<std::mutex> lock(status.mutex);
    for (const auto& found : manifests) {
        const Manifest& m = found.manifest;
        auto alreadyQ = std::find_if(status.downloadQueue.begin(), status.downloadQueue.end(),
            [&](const QueueItem& qi) {
                if (!m.rommId.empty()) {
                    if (qi.game.id != m.rommId) return false;
                } else if (qi.game.fsName != m.fsName) {
                    return false;
                }
                if (!m.fileId.empty()) return qi.game.fileId == m.fileId;
                return true;
            });
        if (alreadyQ != status.downloadQueue.end()) continue;
        auto already = std::find_if(status.downloadHistory.begin(), status.downloadHistory.end(),
            [&](const QueueItem& qi) {
                if (!m.rommId.empty()) {
                    if (qi.game.id != m.rommId) return false;
                } else if (qi.game.fsName != m.fsName) {
                    return false;
                }
                if (!m.fileId.empty()) return qi.game.fileId == m.fileId;
                return true;
            });
        if (already != status.downloadHistory.end()) continue;
        QueueItem qi;
        qi.game.id = m.rommId;
        qi.game.fileId = m.fileId;
        qi.game.fsName = m.fsName;
        qi.game.downloadUrl = m.url;
        qi.game.sizeBytes = m.totalSize;
        qi.game.platformSlug = found.platformSlug;
        qi.bundle = bundleFromGame(qi.game);
        qi.state = QueueState::Resumable;
        qi.error = "Resume available";
        bool allDone = !m.parts.empty() &&
                       std::all_of(m.parts.begin(), m.parts.end(),
                                   [](const ManifestPart& p){ return p.completed; });
        if (allDone) {
            qi.state = QueueState::Completed;
            qi.error.clear();
        }
        if (!m.failureReason.empty()) {
            qi.state = QueueState::Failed;
            qi.error = m.failureReason;
        }
        status.downloadHistory.push_back(qi);
    }
    if (!manifests.empty()) status.downloadHistoryRevision++;
    return true;
}

void startDownloadWorker(Status& status, const Config& cfg) {
    // If a previous worker finished but wasn't joined, clean it up now.
    if (gCtx.worker.joinable()) {
        if (status.downloadWorkerRunning.load()) {
            // Still running; do not start another.
            return;
        }
        gCtx.worker.join();
    }
    gCtx.stopRequested.store(false);
    gCtx.status = &status;
    gCtx.cfg = cfg;
    gCtx.worker = std::thread(workerLoop);
}

void stopDownloadWorker() {
    gCtx.stopRequested.store(true);
    int activeFd = gCtx.activeSocketFd.load(std::memory_order_acquire);
    if (activeFd >= 0) {
        ::shutdown(activeFd, SHUT_RDWR);
    }
    if (gCtx.worker.joinable()) {
        gCtx.worker.join();
    }
    gCtx.status = nullptr;
    gCtx.worker = std::thread(); // reset
}

void reapDownloadWorkerIfDone() {
    if (gCtx.worker.joinable()) {
        // Safe to join if the worker has finished (not running or stop requested).
        if (!gCtx.status || !gCtx.status->downloadWorkerRunning.load()) {
            gCtx.worker.join();
            gCtx.status = nullptr;
            gCtx.worker = std::thread();
        }
    }
}

} // namespace romm
