#include "romm/downloader.hpp"
#include "romm/logger.hpp"
#include "romm/filesystem.hpp"
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
#include <fstream>
#include <sys/iosupport.h>
#include <switch/runtime/devices/fs_dev.h> // fsdevSetConcatenationFileAttribute

namespace romm {

namespace {

constexpr uint64_t kPartSize = 0xFFFF0000ULL; // DBI/Tinfoil split size
constexpr uint64_t kFreeSpaceMarginBytes = 200ULL * 1024ULL * 1024ULL; // ~200MB margin

struct DownloadContext {
    std::thread worker;
    std::atomic<bool> stopRequested{false};
    std::mutex mutex;
    Status* status{nullptr};
    Config cfg;
};

DownloadContext gCtx; // global download context shared with worker

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

static std::string base64(const std::string& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (uint8_t c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(tbl[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(tbl[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

// Check (best effort) if there is enough free space at path for neededBytes + margin.
static bool ensureFreeSpace(const std::string& path, uint64_t neededBytes) {
    struct statvfs vfs{};
    if (statvfs(path.c_str(), &vfs) != 0) return true; // best effort
    uint64_t freeBytes = static_cast<uint64_t>(vfs.f_bavail) * static_cast<uint64_t>(vfs.f_frsize);
    return freeBytes >= neededBytes + kFreeSpaceMarginBytes;
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

struct PreflightInfo {
    bool supportsRanges{false};
    uint64_t contentLength{0};
};

// Preflight: try HEAD first; if it fails or is rejected, fall back to Range: 0-0 GET.
static bool preflight(const std::string& url, const std::string& authBasic, int timeoutSec, PreflightInfo& info) {
    info = {};
    auto doRequest = [&](const std::string& method, const std::string& extraHeader, int& outCode, std::string& headers) -> bool {
        outCode = 0;
        headers.clear();
        if (url.rfind("http://", 0) != 0) return false;
        std::string rest = url.substr(7);
        auto slash = rest.find('/');
        std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
        std::string path = (slash == std::string::npos) ? "/" : rest.substr(slash);
        std::string host = hostport;
        std::string portStr = "80";
        auto colon = hostport.find(':');
        if (colon != std::string::npos) { host = hostport.substr(0, colon); portStr = hostport.substr(colon + 1); }
        struct addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = nullptr;
        int ret = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
        if (ret != 0 || !res) { if (res) freeaddrinfo(res); return false; }
        int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock < 0) { freeaddrinfo(res); return false; }
        if (timeoutSec > 0) {
            struct timeval tv{}; tv.tv_sec = timeoutSec;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }
        if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) { freeaddrinfo(res); close(sock); return false; }
        freeaddrinfo(res);
        std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n";
        if (!authBasic.empty()) req += "Authorization: Basic " + authBasic + "\r\n";
        if (!extraHeader.empty()) req += extraHeader + "\r\n";
        req += "\r\n";
        send(sock, req.c_str(), req.size(), 0);
        std::string headerAccum;
        char buf[1024];
        while (true) {
            ssize_t n = recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) break;
            headerAccum.append(buf, buf + n);
            if (headerAccum.find("\r\n\r\n") != std::string::npos) break;
        }
        close(sock);
        auto hdrEnd = headerAccum.find("\r\n\r\n");
        if (hdrEnd == std::string::npos) return false;
        std::string headerBlock = headerAccum.substr(0, hdrEnd);
        std::istringstream hs(headerBlock);
        std::string statusLine;
        if (!std::getline(hs, statusLine)) return false;
        if (!statusLine.empty() && statusLine.back() == '\r') statusLine.pop_back();
        std::istringstream sl(statusLine);
        std::string httpVer; sl >> httpVer >> outCode;
        headers = headerBlock;
        return true;
    };

    int code = 0;
    std::string headers;
    // Try HEAD
    if (doRequest("HEAD", "", code, headers) && code >= 200 && code < 300) {
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
            if (keyLower == "content-length") {
                info.contentLength = static_cast<uint64_t>(std::strtoull(val.c_str(), nullptr, 10));
            } else if (keyLower == "accept-ranges" && val.find("bytes") != std::string::npos) {
                info.supportsRanges = true;
            }
        }
        return info.contentLength > 0;
    }

    // Fallback: GET with Range 0-0
    if (!doRequest("GET", "Range: bytes=0-0", code, headers)) return false;
    if (code == 206) info.supportsRanges = true;
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
        if (keyLower == "content-range") {
            // Expect bytes 0-0/total
            auto slash = val.find('/');
            if (slash != std::string::npos) {
                std::string totalStr = val.substr(slash + 1);
                info.contentLength = static_cast<uint64_t>(std::strtoull(totalStr.c_str(), nullptr, 10));
            }
        } else if (keyLower == "content-length" && info.contentLength == 0) {
            info.contentLength = static_cast<uint64_t>(std::strtoull(val.c_str(), nullptr, 10));
        }
    }
    return info.contentLength > 0;
}

// Stream a continuous HTTP GET (optionally with Range) and split into FAT32-friendly parts.
// TODO(manifest/hashes): track expected parts/sizes/hashes to validate resume beyond size-only.
static bool streamDownload(const std::string& url,
                           const std::string& authBasic,
                           bool useRange,
                           uint64_t startOffset,
                           uint64_t totalSize,
                           const std::string& tmpDir,
                           Status& status,
                           const Config& cfg,
                           std::string& err) {
    if (url.rfind("http://", 0) != 0) { err = "Only http:// supported"; return false; }
    std::string rest = url.substr(7);
    auto slash = rest.find('/');
    std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    std::string path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    std::string host = hostport;
    std::string portStr = "80";
    auto colon = hostport.find(':');
    if (colon != std::string::npos) { host = hostport.substr(0, colon); portStr = hostport.substr(colon + 1); }

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    int ret = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
    if (ret != 0 || !res) { if (res) freeaddrinfo(res); err = "DNS failed"; return false; }
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { freeaddrinfo(res); err = "Socket failed"; return false; }
    if (cfg.httpTimeoutSeconds > 0) {
        struct timeval tv{}; tv.tv_sec = cfg.httpTimeoutSeconds;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    // Bump receive buffer for faster pulls.
    int rcvbuf = 256 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) { freeaddrinfo(res); close(sock); err = "Connect failed"; return false; }
    freeaddrinfo(res);

    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n";
    if (!authBasic.empty()) req += "Authorization: Basic " + authBasic + "\r\n";
    if (useRange && startOffset > 0) {
        req += "Range: bytes=" + std::to_string(startOffset) + "-\r\n";
    }
    req += "\r\n";
    send(sock, req.c_str(), req.size(), 0);

    // Read headers
    std::string headerAccum;
    // Large user-space buffer for faster pulls.
    static thread_local char buf[256 * 1024];
    bool headersDone = false;
    size_t bodyStartOffset = 0;
    int statusCode = 0;
    uint64_t contentLength = 0;
    uint64_t expectedBody = totalSize - startOffset;
    while (!headersDone) {
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) { err = "No HTTP headers received"; close(sock); return false; }
        headerAccum.append(buf, buf + n);
        auto hdrEnd = headerAccum.find("\r\n\r\n");
        if (hdrEnd == std::string::npos) continue;
        headersDone = true;
        bodyStartOffset = hdrEnd + 4;
        std::string headers = headerAccum.substr(0, hdrEnd);
        std::istringstream hs(headers);
        std::string statusLine;
        std::getline(hs, statusLine);
        if (!statusLine.empty() && statusLine.back() == '\r') statusLine.pop_back();
        std::istringstream sl(statusLine);
        std::string httpVer; sl >> httpVer >> statusCode;
        if (useRange && statusCode != 206) { err = "Range not honored (status " + std::to_string(statusCode) + ")"; close(sock); return false; }
        if (!useRange && statusCode != 200) { err = "HTTP status " + std::to_string(statusCode); close(sock); return false; }
        std::string line;
        while (std::getline(hs, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto cpos = line.find(':');
            if (cpos == std::string::npos) continue;
            std::string key = line.substr(0, cpos);
            std::string val = line.substr(cpos + 1);
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
            std::string keyLower = key;
            for (auto& c : keyLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (keyLower == "content-length") {
                contentLength = static_cast<uint64_t>(std::strtoull(val.c_str(), nullptr, 10));
            } else if (keyLower == "content-range") {
                // Optional: validate total size from content-range
            }
        }
        if (contentLength && expectedBody && contentLength < expectedBody) {
            err = "Short body (Content-Length " + std::to_string(contentLength) + " < expected " + std::to_string(expectedBody) + ")";
            close(sock);
            return false;
        }
    }

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
            uint64_t partIdx = globalOffset / kPartSize;
            uint64_t partOff = globalOffset % kPartSize;
            uint64_t space = kPartSize - partOff;
            size_t toWrite = static_cast<size_t>(std::min<uint64_t>(space, remaining));
            if (currentPart != static_cast<int>(partIdx)) {
                closePart();
                std::string partName = (partIdx < 10 ? "0" : "") + std::to_string(partIdx);
                std::string partPath = tmpDir + "/" + partName + ".part";
                partFile = fopen(partPath.c_str(), "r+b");
                if (!partFile) partFile = fopen(partPath.c_str(), "w+b");
                if (!partFile) { err = "Open part failed"; return false; }
                // Large stdio buffer to reduce syscalls.
                static thread_local char ioBuf[256 * 1024];
                setvbuf(partFile, ioBuf, _IOFBF, sizeof(ioBuf));
                if (fseek(partFile, static_cast<long>(partOff), SEEK_SET) != 0) {
                    err = "Seek failed";
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
    // Write any body already read into buffer
    if (bodyStartOffset < headerAccum.size()) {
        size_t bodyBytes = headerAccum.size() - bodyStartOffset;
        size_t toUse = static_cast<size_t>(std::min<uint64_t>((uint64_t)bodyBytes, expectedBody));
        if (toUse > 0) {
            if (!writeSpan(globalOffset, headerAccum.data() + bodyStartOffset, toUse)) { close(sock); return false; }
            status.currentDownloadedBytes.fetch_add(toUse);
            status.totalDownloadedBytes.fetch_add(toUse);
        }
    }

    auto lastBeat = std::chrono::steady_clock::now();
    uint64_t bytesSinceBeat = 0;
    const uint64_t kLogEvery = 100ULL * 1024ULL * 1024ULL; // ~100MB
    while (globalOffset - startOffset < expectedBody && !gCtx.stopRequested.load()) {
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        size_t toUse = static_cast<size_t>(std::min<uint64_t>((uint64_t)n, expectedBody - (globalOffset - startOffset)));
        if (toUse > 0) {
            if (!writeSpan(globalOffset, buf, toUse)) { close(sock); return false; }
            status.currentDownloadedBytes.fetch_add(toUse);
            status.totalDownloadedBytes.fetch_add(toUse);
            bytesSinceBeat += toUse;
            auto now = std::chrono::steady_clock::now();
            if (bytesSinceBeat >= kLogEvery || now - lastBeat > std::chrono::seconds(10)) {
                logDebug("Heartbeat: " + status.currentDownloadTitle +
                         " cur=" + std::to_string(status.currentDownloadedBytes.load()) + "/" +
                         std::to_string(status.currentDownloadSize.load()) +
                         " total=" + std::to_string(status.totalDownloadedBytes.load()) + "/" +
                         std::to_string(status.totalDownloadBytes.load()),
                         "DL");
                lastBeat = now;
                bytesSinceBeat = 0;
            }
        }
    }
    close(sock);
    closePart();

    uint64_t received = globalOffset - startOffset;
    if (gCtx.stopRequested.load()) { err = "Stopped"; return false; }
    if (received < expectedBody) { err = "Short read"; return false; }
    if (received > expectedBody) { err = "Overflow"; return false; }
    return true;
}

// Rename *.part -> 00/01... then move tmpDir to finalDir (archive bit set for multi-part).
static bool finalizeParts(const std::string& tmpDir, const std::string& finalDir) {
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
static bool downloadOne(const Game& g, Status& status, const Config& cfg) {
    std::string auth;
    if (!cfg.username.empty() || !cfg.password.empty()) {
        auth = base64(cfg.username + ":" + cfg.password);
    }
    std::string baseDir = cfg.downloadDir;
    ensureDirectory(baseDir);
    std::string tempRoot = baseDir + "/temp";
    ensureDirectory(tempRoot);

    // free space check for full ROM upfront (best effort)
    if (!ensureFreeSpace(baseDir, g.sizeBytes)) {
        logLine("Not enough free space for " + g.title);
        {
            std::lock_guard<std::mutex> lock(status.mutex);
            status.lastDownloadError = "Not enough free space";
        }
        return false;
    }

    if (g.downloadUrl.empty()) {
        logLine("No download URL for " + g.title);
        {
            std::lock_guard<std::mutex> lock(status.mutex);
            status.lastDownloadError = "No download URL";
        }
        return false;
    }

    std::string tmpName = safeName(g.title);
    if (tmpName.empty() && !g.fsName.empty()) tmpName = safeName(g.fsName);
    if (tmpName.size() > 12) tmpName = tmpName.substr(0, 12);
    if (tmpName.empty()) tmpName = g.id;
    std::string tmpDir = tempRoot + "/" + tmpName + ".tmp";
    ensureDirectory(tmpDir);
    logLine("Using temp dir: " + tmpDir);
    logLine("Download URL: " + g.downloadUrl);
    status.currentDownloadSize.store(g.sizeBytes);
    status.currentDownloadedBytes.store(0);
    status.currentDownloadTitle = g.title;
    logLine("Begin download: " + g.title + " size=" + std::to_string(g.sizeBytes));

    // Calculate how many bytes we already have (for resume)
    uint64_t haveBytes = 0;
    {
        DIR* d = opendir(tmpDir.c_str());
        if (d) {
            struct dirent* ent;
            while ((ent = readdir(d)) != nullptr) {
                std::string n = ent->d_name;
                if (n.size() > 5 && n.substr(n.size() - 5) == ".part") {
                    std::string p = tmpDir + "/" + n;
                    struct stat st{};
                    if (stat(p.c_str(), &st) == 0) haveBytes += (uint64_t)st.st_size;
                }
            }
            closedir(d);
        }
    }
    if (haveBytes > g.sizeBytes) haveBytes = g.sizeBytes;

    {
        std::lock_guard<std::mutex> lock(status.mutex);
        status.currentDownloadSize.store(g.sizeBytes);
        status.currentDownloadedBytes.store(haveBytes);
        status.currentDownloadTitle = g.title;
    }
    // Count any already-present bytes toward total downloaded so the bar doesn't regress
    uint64_t creditedExisting = 0;
    if (haveBytes > 0) {
        status.totalDownloadedBytes.fetch_add(haveBytes);
        creditedExisting = haveBytes;
    }

    PreflightInfo pf;
    if (!preflight(g.downloadUrl, auth, cfg.httpTimeoutSeconds, pf)) {
        logLine("Preflight failed for " + g.title + " (HEAD/Range probe). Proceeding with metadata size=" + std::to_string(g.sizeBytes));
        pf.contentLength = g.sizeBytes;
        pf.supportsRanges = false;
    } else {
        logLine("Preflight for " + g.title + " len=" + std::to_string(pf.contentLength) +
                " ranges=" + (pf.supportsRanges ? "true" : "false"));
    }
    if (pf.contentLength != 0 && pf.contentLength != g.sizeBytes) {
        logLine("Warning: server size " + std::to_string(pf.contentLength) + " differs from metadata " + std::to_string(g.sizeBytes));
        // trust server size for transfer
        status.currentDownloadSize.store(pf.contentLength);
    } else if (pf.contentLength != 0) {
        status.currentDownloadSize.store(pf.contentLength);
    }

    uint64_t totalSize = status.currentDownloadSize.load();
    if (haveBytes >= totalSize) {
        logLine("Already have full size for " + g.title);
    }

    const int maxAttempts = 3;
    int attempt = 0;
    std::string err;
    bool okStream = false;
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
        okStream = streamDownload(g.downloadUrl, auth, useRange, haveBytes, totalSize, tmpDir, status, cfg, err);
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
        {
            std::lock_guard<std::mutex> lock(status.mutex);
            status.lastDownloadError = err.empty() ? "Download failed" : err;
        }
        logLine("Download failed: " + status.lastDownloadError);
        return false;
    }

    std::string finalBase = safeName(g.title);
    if (finalBase.empty() && !g.fsName.empty()) finalBase = safeName(g.fsName);
    if (finalBase.empty()) finalBase = g.id.empty() ? "rom" : g.id;
    std::string finalName = baseDir + "/" + finalBase;
    // ensure extension from fsName if present
    if (!g.fsName.empty()) {
        auto dot = g.fsName.rfind('.');
        if (dot != std::string::npos) finalName += g.fsName.substr(dot);
    } else {
        finalName += ".nsp";
    }
    logLine("Finalize: moving temp to " + finalName);
    bool okFinal = finalizeParts(tmpDir, finalName);
    if (!okFinal) {
        std::lock_guard<std::mutex> lock(status.mutex);
        status.lastDownloadError = "Finalize failed";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(status.mutex);
        // Keep UI counters aligned with the completed file.
        status.currentDownloadedBytes.store(g.sizeBytes);
        status.totalDownloadedBytes.store(status.totalDownloadedBytes.load()); // unchanged, already includes current
        status.lastDownloadError.clear();
    }
    logLine("Download complete: " + g.title);
    return true;
}

// Recompute totalDownloadBytes to reflect remaining queue plus what has already been downloaded.
static void recomputeTotals(Status& st) {
    uint64_t remaining = 0;
    for (auto& g : st.downloadQueue) remaining += g.sizeBytes;
    st.totalDownloadBytes.store(st.totalDownloadedBytes.load() + remaining);
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
        st->lastDownloadFailed.store(false);
        st->lastDownloadError.clear();
        st->downloadCompleted = false;
        st->totalDownloadBytes.store(0);
        st->totalDownloadedBytes.store(0);
        for (auto& g : st->downloadQueue) st->totalDownloadBytes.fetch_add(g.sizeBytes);
    }
    logLine("Worker start, total bytes=" + std::to_string(st->totalDownloadBytes.load()));
    while (true) {
        Game next;
        {
            std::lock_guard<std::mutex> lock(st->mutex);
            if (st->downloadQueue.empty() || gCtx.stopRequested.load()) break;
            st->lastDownloadFailed.store(false);
            st->lastDownloadError.clear();
            st->currentDownloadIndex = 0;
            next = st->downloadQueue.front(); // copy
            // Prime UI fields for the next item.
            st->currentDownloadTitle = next.title;
            st->currentDownloadSize.store(next.sizeBytes);
            st->currentDownloadedBytes.store(0);
        }
        if (!downloadOne(next, *st, cfg)) {
            logLine("Download failed or stopped for " + next.title);
            st->lastDownloadFailed.store(true);
            {
                std::lock_guard<std::mutex> lock(st->mutex);
                if (!st->downloadQueue.empty()) st->downloadQueue.erase(st->downloadQueue.begin());
                recomputeTotals(*st);
            }
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(st->mutex);
            if (!st->downloadQueue.empty()) st->downloadQueue.erase(st->downloadQueue.begin());
            recomputeTotals(*st);
        }
    }
    st->downloadWorkerRunning.store(false);
    {
        std::lock_guard<std::mutex> lock(st->mutex);
        if (st->downloadQueue.empty() && !gCtx.stopRequested.load() && !st->lastDownloadFailed.load()) {
            st->downloadCompleted = true;
            logLine("All downloads complete.");
        }
    }
    logLine("Worker done.");
}

} // namespace

void startDownloadWorker(Status& status, const Config& cfg) {
    if (gCtx.worker.joinable()) return;
    gCtx.stopRequested.store(false);
    gCtx.status = &status;
    gCtx.cfg = cfg;
    gCtx.worker = std::thread(workerLoop);
}

void stopDownloadWorker() {
    gCtx.stopRequested.store(true);
    if (gCtx.worker.joinable()) {
        gCtx.worker.join();
    }
    gCtx.status = nullptr;
}

} // namespace romm
