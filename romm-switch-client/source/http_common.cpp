#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif

#include "romm/http_common.hpp"
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>

#ifndef UNIT_TEST
#include <curl/curl.h>
#endif

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/time.h>
#endif

namespace romm {

namespace {
constexpr size_t kHttpRecvBuf = 8192;
constexpr long kCurlBufferSize = 256L * 1024L;

struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::string port;
    std::string path;
};

struct KeepAliveConn {
    int fd{-1};
    std::string host;
    std::string port;
    int timeoutSec{0};
};

thread_local KeepAliveConn gKeepAliveConn;

static void closeSocketFd(int fd) {
#if defined(_WIN32)
    closesocket(fd);
#else
    close(fd);
#endif
}

struct UniqueSocket {
    int fd{-1};
    ~UniqueSocket() { if (fd >= 0) closeSocketFd(fd); }
    explicit operator bool() const { return fd >= 0; }
    int release() {
        int out = fd;
        fd = -1;
        return out;
    }
};

struct ActiveSocketScope {
    std::atomic<int>* slot{nullptr};
    int fd{-1};
    ActiveSocketScope(std::atomic<int>* s, int f) : slot(s), fd(f) {
        if (slot) slot->store(fd, std::memory_order_release);
    }
    ~ActiveSocketScope() {
        if (!slot) return;
        int expected = fd;
        (void)slot->compare_exchange_strong(expected, -1, std::memory_order_acq_rel);
    }
};

static bool isCancelled(const HttpRequestOptions& options) {
    return options.cancelRequested && options.cancelRequested->load(std::memory_order_acquire);
}

[[maybe_unused]] static bool parseHttpUrlInternal(const std::string& url, ParsedUrl& out, std::string& err) {
    out = ParsedUrl{};
    size_t schemeLen = 0;
    if (url.rfind("http://", 0) == 0) {
        out.scheme = "http";
        schemeLen = 7;
    } else if (url.rfind("https://", 0) == 0) {
        out.scheme = "https";
        schemeLen = 8;
    } else {
        err = "URL must start with http:// or https://";
        return false;
    }

    std::string rest = url.substr(schemeLen);
    auto slash = rest.find('/');
    std::string hostport;
    if (slash == std::string::npos) {
        hostport = rest;
        out.path = "/";
    } else {
        hostport = rest.substr(0, slash);
        out.path = rest.substr(slash);
    }
    if (out.path.empty()) out.path = "/";

    out.host = hostport;
    out.port = (out.scheme == "https") ? "443" : "80";
    auto colon = hostport.find(':');
    if (colon != std::string::npos) {
        out.host = hostport.substr(0, colon);
        out.port = hostport.substr(colon + 1);
        if (out.port.empty()) out.port = (out.scheme == "https") ? "443" : "80";
    }
    if (out.host.empty()) {
        err = "Bad URL: missing host";
        return false;
    }
    return true;
}

[[maybe_unused]] static bool decodeChunkedBodyInternal(const std::string& body, std::string& decoded) {
    decoded.clear();
    size_t pos = 0;
    while (pos < body.size()) {
        size_t lineEnd = body.find("\r\n", pos);
        if (lineEnd == std::string::npos) return false;
        std::string lenLine = body.substr(pos, lineEnd - pos);
        auto semicolon = lenLine.find(';');
        if (semicolon != std::string::npos) lenLine = lenLine.substr(0, semicolon);
        while (!lenLine.empty() && std::isspace(static_cast<unsigned char>(lenLine.front()))) lenLine.erase(lenLine.begin());
        while (!lenLine.empty() && std::isspace(static_cast<unsigned char>(lenLine.back()))) lenLine.pop_back();
        errno = 0;
        char* endptr = nullptr;
        long chunkSize = std::strtol(lenLine.c_str(), &endptr, 16);
        if (endptr == lenLine.c_str() || errno == ERANGE || chunkSize < 0) return false;
        pos = lineEnd + 2;
        if (chunkSize == 0) {
            if (pos + 2 > body.size()) return false;
            if (body[pos] != '\r' || body[pos + 1] != '\n') return false;
            return true;
        }
        if (pos + static_cast<size_t>(chunkSize) > body.size()) return false;
        decoded.append(body, pos, static_cast<size_t>(chunkSize));
        pos += static_cast<size_t>(chunkSize);
        if (pos + 2 > body.size()) return false;
        if (body[pos] != '\r' || body[pos + 1] != '\n') return false;
        pos += 2;
    }
    return false;
}

[[maybe_unused]] static bool isNoBodyStatus(int statusCode) {
    return (statusCode >= 100 && statusCode < 200) || statusCode == 204 || statusCode == 304;
}

[[maybe_unused]] static bool isHeadMethod(const std::string& method) {
    return method == "HEAD" || method == "head" || method == "Head";
}

static bool setSocketTimeouts(int fd, int timeoutSec) {
    if (timeoutSec <= 0) return true;
#if defined(_WIN32)
    const DWORD timeoutMs = static_cast<DWORD>(timeoutSec * 1000);
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeoutMs),
                   static_cast<int>(sizeof(timeoutMs))) != 0) {
        return false;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeoutMs),
                   static_cast<int>(sizeof(timeoutMs))) != 0) {
        return false;
    }
#else
    timeval tv{};
    tv.tv_sec = timeoutSec;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) return false;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) return false;
#endif
    return true;
}

static void closeKeepAliveConn() {
    if (gKeepAliveConn.fd >= 0) closeSocketFd(gKeepAliveConn.fd);
    gKeepAliveConn = KeepAliveConn{};
}

static bool openSocketConnected(const ParsedUrl& url, int timeoutSec, int& outFd, std::string& err) {
    outFd = -1;
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    int ret = getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &res);
    if (ret != 0 || !res) {
        err = "DNS lookup failed for host: " + url.host;
        if (res) freeaddrinfo(res);
        return false;
    }

    UniqueSocket sock;
    sock.fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (!sock) {
        err = "Socket creation failed";
        freeaddrinfo(res);
        return false;
    }
    if (!setSocketTimeouts(sock.fd, timeoutSec)) {
        err = "Failed to set socket timeout";
        freeaddrinfo(res);
        return false;
    }
    if (connect(sock.fd, res->ai_addr, res->ai_addrlen) != 0) {
        err = std::string("Connect failed: ") + std::strerror(errno);
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);
    outFd = sock.release();
    return true;
}

[[maybe_unused]] static bool ensureKeepAliveConn(const ParsedUrl& url, const HttpRequestOptions& options, std::string& err) {
    if (gKeepAliveConn.fd >= 0 &&
        gKeepAliveConn.host == url.host &&
        gKeepAliveConn.port == url.port &&
        gKeepAliveConn.timeoutSec == options.timeoutSec) {
        return true;
    }
    int fd = -1;
    if (!openSocketConnected(url, options.timeoutSec, fd, err)) return false;
    closeKeepAliveConn();
    gKeepAliveConn.fd = fd;
    gKeepAliveConn.host = url.host;
    gKeepAliveConn.port = url.port;
    gKeepAliveConn.timeoutSec = options.timeoutSec;
    return true;
}

static bool recvWithCancel(int fd, char* buf, size_t len, const HttpRequestOptions& options, ssize_t& outN, std::string& err) {
    if (isCancelled(options)) {
        err = "Cancelled";
        return false;
    }
    while (true) {
        outN = recv(fd, buf, len, 0);
        if (outN >= 0) return true;
        if (errno == EINTR) continue;
        if ((errno == EAGAIN || errno == EWOULDBLOCK) && isCancelled(options)) {
            err = "Cancelled";
            return false;
        }
        err = (errno == EAGAIN || errno == EWOULDBLOCK) ? "Recv timed out"
                                                         : (std::string("Recv failed: ") + std::strerror(errno));
        return false;
    }
}

[[maybe_unused]] static bool sendAllWithCancel(int fd, const std::string& req, const HttpRequestOptions& options, std::string& err) {
    if (isCancelled(options)) {
        err = "Cancelled";
        return false;
    }
    if (!sendAll(fd, req.data(), req.size())) {
        err = "Send failed";
        return false;
    }
    return true;
}

[[maybe_unused]] static bool readUntilHeaders(int fd,
                             const HttpRequestOptions& options,
                             std::string& raw,
                             std::string& headerBlock,
                             size_t& bodyStart,
                             std::string& err) {
    raw.clear();
    headerBlock.clear();
    bodyStart = 0;
    char buf[kHttpRecvBuf];
    while (true) {
        ssize_t n = 0;
        if (!recvWithCancel(fd, buf, sizeof(buf), options, n, err)) return false;
        if (n == 0) {
            err = "Connection closed before HTTP headers";
            return false;
        }
        raw.append(buf, buf + n);
        auto hdrEnd = raw.find("\r\n\r\n");
        if (hdrEnd == std::string::npos) continue;
        headerBlock = raw.substr(0, hdrEnd);
        bodyStart = hdrEnd + 4;
        return true;
    }
}

[[maybe_unused]] static bool readExactBody(int fd,
                          uint64_t targetBytes,
                          const HttpRequestOptions& options,
                          std::string& ioBody,
                          std::string& err) {
    char buf[kHttpRecvBuf];
    while (ioBody.size() < targetBytes) {
        ssize_t n = 0;
        if (!recvWithCancel(fd, buf, sizeof(buf), options, n, err)) return false;
        if (n == 0) {
            err = "Short HTTP body";
            return false;
        }
        ioBody.append(buf, buf + n);
        if (options.maxBodyBytes > 0 && ioBody.size() > options.maxBodyBytes) {
            err = "HTTP body exceeds configured max size";
            return false;
        }
    }
    if (ioBody.size() > targetBytes) ioBody.resize(static_cast<size_t>(targetBytes));
    return true;
}

[[maybe_unused]] static bool readToEof(int fd, const HttpRequestOptions& options, std::string& ioBody, std::string& err) {
    char buf[kHttpRecvBuf];
    while (true) {
        ssize_t n = 0;
        if (!recvWithCancel(fd, buf, sizeof(buf), options, n, err)) return false;
        if (n == 0) return true;
        ioBody.append(buf, buf + n);
        if (options.maxBodyBytes > 0 && ioBody.size() > options.maxBodyBytes) {
            err = "HTTP body exceeds configured max size";
            return false;
        }
    }
}

[[maybe_unused]] static bool parseAndFillResponse(const std::string& headerBlock,
                                 ParsedHttpResponse& parsed,
                                 std::string& err) {
    return parseHttpResponseHeaders(headerBlock, parsed, err);
}

#ifndef UNIT_TEST
struct CurlEasyHandle {
    CURL* handle{nullptr};
    bool owned{false};
    ~CurlEasyHandle() {
        if (owned && handle) curl_easy_cleanup(handle);
    }
};

thread_local CURL* gCurlKeepAliveEasy = nullptr;

struct CurlHeaderState {
    std::string raw;
};

struct CurlProgressState {
    const HttpRequestOptions* options{nullptr};
    bool cancelled{false};
};

struct CurlBufferedWriteState {
    const HttpRequestOptions* options{nullptr};
    std::string* body{nullptr};
    bool sizeExceeded{false};
};

struct CurlStreamWriteState {
    const HttpRequestOptions* options{nullptr};
    const std::function<bool(const char*, size_t)>* onData{nullptr};
    CurlHeaderState* headers{nullptr};
    ParsedHttpResponse* parsed{nullptr};
    bool headersParsed{false};
    bool parseFailed{false};
    bool chunkedRejected{false};
    bool sinkAborted{false};
    bool sizeExceeded{false};
    uint64_t streamed{0};
    std::string parseErr;
};

struct CurlSlist {
    curl_slist* head{nullptr};
    ~CurlSlist() { if (head) curl_slist_free_all(head); }
};

static bool ensureCurlGlobalInit(std::string& err) {
    static bool initAttempted = false;
    static bool initOk = false;
    if (!initAttempted) {
        initAttempted = true;
        initOk = (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    }
    if (!initOk) err = "curl_global_init failed";
    return initOk;
}

static bool acquireCurlHandle(bool keepAlive, CurlEasyHandle& out, std::string& err) {
    if (keepAlive) {
        if (!gCurlKeepAliveEasy) gCurlKeepAliveEasy = curl_easy_init();
        if (!gCurlKeepAliveEasy) {
            err = "curl_easy_init failed";
            return false;
        }
        out.handle = gCurlKeepAliveEasy;
        out.owned = false;
        curl_easy_reset(out.handle);
        return true;
    }
    out.handle = curl_easy_init();
    if (!out.handle) {
        err = "curl_easy_init failed";
        return false;
    }
    out.owned = true;
    return true;
}

static bool extractFinalHeaderBlock(const std::string& raw, std::string& out) {
    out.clear();
    size_t pos = 0;
    bool found = false;
    while (pos < raw.size()) {
        size_t end = raw.find("\r\n\r\n", pos);
        if (end == std::string::npos) break;
        std::string block = raw.substr(pos, end - pos);
        if (!block.empty() && block.rfind("HTTP/", 0) == 0) {
            out = std::move(block);
            found = true;
        }
        pos = end + 4;
    }
    if (found) return true;
    if (!raw.empty() && raw.rfind("HTTP/", 0) == 0) {
        out = raw;
        while (!out.empty() && (out.back() == '\r' || out.back() == '\n')) out.pop_back();
        return true;
    }
    return false;
}

static bool parseCurlResponseHeaders(const CurlHeaderState& headerState,
                                     ParsedHttpResponse& out,
                                     std::string& err) {
    std::string block;
    if (!extractFinalHeaderBlock(headerState.raw, block)) {
        err = "Missing HTTP response headers";
        return false;
    }
    return parseAndFillResponse(block, out, err);
}

static size_t curlHeaderCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* state = static_cast<CurlHeaderState*>(userdata);
    if (!state || !ptr) return 0;
    size_t n = size * nmemb;
    state->raw.append(ptr, n);
    return n;
}

static int curlProgressCallback(void* clientp,
                                curl_off_t /*dltotal*/,
                                curl_off_t /*dlnow*/,
                                curl_off_t /*ultotal*/,
                                curl_off_t /*ulnow*/) {
    auto* state = static_cast<CurlProgressState*>(clientp);
    if (!state || !state->options) return 0;
    if (isCancelled(*state->options)) {
        state->cancelled = true;
        return 1;
    }
    return 0;
}

static size_t curlBufferedWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* state = static_cast<CurlBufferedWriteState*>(userdata);
    if (!state || !state->body || !ptr) return 0;
    size_t n = size * nmemb;
    if (state->options && state->options->maxBodyBytes > 0 &&
        state->body->size() + n > state->options->maxBodyBytes) {
        state->sizeExceeded = true;
        return 0;
    }
    state->body->append(ptr, n);
    return n;
}

static size_t curlStreamWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* state = static_cast<CurlStreamWriteState*>(userdata);
    if (!state || !ptr) return 0;
    size_t n = size * nmemb;
    if (n == 0) return 0;

    if (!state->headersParsed) {
        std::string err;
        if (!parseCurlResponseHeaders(*state->headers, *state->parsed, err)) {
            state->parseFailed = true;
            state->parseErr = err;
            return 0;
        }
        state->headersParsed = true;
        if (state->parsed->chunked) {
            state->chunkedRejected = true;
            return 0;
        }
    }

    if (state->options && state->options->maxBodyBytes > 0 &&
        state->streamed + n > state->options->maxBodyBytes) {
        state->sizeExceeded = true;
        return 0;
    }

    if (state->onData && !(*state->onData)(ptr, n)) {
        state->sinkAborted = true;
        return 0;
    }

    state->streamed += static_cast<uint64_t>(n);
    return n;
}

static bool setupCurlRequest(CURL* easy,
                             const std::string& method,
                             const std::string& url,
                             const std::vector<std::pair<std::string, std::string>>& headers,
                             const HttpRequestOptions& options,
                             CurlHeaderState& headerState,
                             CurlProgressState& progressState,
                             CurlSlist& reqHeaders,
                             std::string& err) {
    if (curl_easy_setopt(easy, CURLOPT_URL, url.c_str()) != CURLE_OK) {
        err = "Failed to set request URL";
        return false;
    }
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(easy, CURLOPT_FAILONERROR, 0L);
    // Raise libcurl's transfer buffer to reduce callback churn on large downloads.
    curl_easy_setopt(easy, CURLOPT_BUFFERSIZE, kCurlBufferSize);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, options.timeoutSec > 0 ? options.timeoutSec : 0);
    // Match prior behavior: timeout means connect/idle timeout, not full-transfer cap.
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, options.timeoutSec > 0 ? options.timeoutSec : 0);
    curl_easy_setopt(easy, CURLOPT_FORBID_REUSE, options.keepAlive ? 0L : 1L);
    curl_easy_setopt(easy, CURLOPT_FRESH_CONNECT, options.keepAlive ? 0L : 1L);

    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, curlHeaderCallback);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, &headerState);

    progressState.options = &options;
    progressState.cancelled = false;
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, curlProgressCallback);
    curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &progressState);

    for (const auto& kv : headers) {
        reqHeaders.head = curl_slist_append(reqHeaders.head, (kv.first + ": " + kv.second).c_str());
    }
    if (reqHeaders.head) curl_easy_setopt(easy, CURLOPT_HTTPHEADER, reqHeaders.head);

    curl_easy_setopt(easy, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(easy, CURLOPT_HTTPGET, 0L);
    curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, nullptr);
    if (isHeadMethod(method)) {
        curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "HEAD");
    } else if (method == "GET") {
        curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
    } else {
        curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method.c_str());
    }
    return true;
}
#endif

} // namespace

bool sendAll(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool parseHttpResponseHeaders(const std::string& headerBlock, ParsedHttpResponse& out, std::string& err) {
    out = ParsedHttpResponse{};
    auto firstCrLf = headerBlock.find("\r\n");
    if (firstCrLf == std::string::npos) {
        err = "Malformed HTTP response (no status line CRLF)";
        return false;
    }
    std::string statusLine = headerBlock.substr(0, firstCrLf);
    if (!statusLine.empty() && statusLine.back() == '\r') statusLine.pop_back();
    std::istringstream sl(statusLine);
    std::string httpVer;
    sl >> httpVer >> out.statusCode;
    if (httpVer.rfind("HTTP/", 0) != 0 || out.statusCode < 100 || out.statusCode > 999) {
        err = "Malformed HTTP status line";
        return false;
    }
    std::getline(sl, out.statusText);
    if (!out.statusText.empty() && out.statusText.front() == ' ') out.statusText.erase(out.statusText.begin());

    std::istringstream hs(headerBlock);
    std::string line;
    std::getline(hs, line); // discard status line

    std::ostringstream raw;
    bool firstHeader = true;
    bool seenContentLength = false;
    uint64_t parsedContentLength = 0;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (!firstHeader) raw << "\r\n";
        raw << line;
        firstHeader = false;

        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
        std::string keyLower = key;
        for (auto& c : keyLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string valLower = val;
        for (auto& c : valLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (keyLower == "content-length") {
            if (val.empty()) {
                err = "Invalid Content-Length header";
                return false;
            }
            errno = 0;
            char* endptr = nullptr;
            uint64_t cl = static_cast<uint64_t>(std::strtoull(val.c_str(), &endptr, 10));
            while (endptr && *endptr == ' ') ++endptr;
            if (errno == ERANGE || endptr == val.c_str() || (endptr && *endptr != '\0')) {
                err = "Invalid Content-Length header";
                return false;
            }
            if (seenContentLength && cl != parsedContentLength) {
                err = "Conflicting Content-Length headers";
                return false;
            }
            seenContentLength = true;
            parsedContentLength = cl;
            out.hasContentLength = true;
            out.contentLength = cl;
        } else if (keyLower == "content-range") {
            std::string rangeVal = val;
            auto bytesPos = rangeVal.find("bytes");
            if (bytesPos != std::string::npos) {
                rangeVal = rangeVal.substr(bytesPos + 5);
            }
            while (!rangeVal.empty() && (rangeVal.front() == ' ' || rangeVal.front() == '\t')) {
                rangeVal.erase(rangeVal.begin());
            }
            auto dash = rangeVal.find('-');
            auto slash = rangeVal.find('/');
            if (dash != std::string::npos && slash != std::string::npos && dash < slash) {
                std::string startStr = rangeVal.substr(0, dash);
                std::string endStr = rangeVal.substr(dash + 1, slash - dash - 1);
                while (!startStr.empty() && (startStr.front() == ' ' || startStr.front() == '\t')) startStr.erase(startStr.begin());
                while (!startStr.empty() && (startStr.back() == ' ' || startStr.back() == '\t')) startStr.pop_back();
                while (!endStr.empty() && (endStr.front() == ' ' || endStr.front() == '\t')) endStr.erase(endStr.begin());
                while (!endStr.empty() && (endStr.back() == ' ' || endStr.back() == '\t')) endStr.pop_back();

                errno = 0;
                char* startEnd = nullptr;
                uint64_t startVal = static_cast<uint64_t>(std::strtoull(startStr.c_str(), &startEnd, 10));
                while (startEnd && *startEnd == ' ') ++startEnd;
                bool startOk = (errno != ERANGE && startEnd != startStr.c_str() && (!startEnd || *startEnd == '\0'));
                errno = 0;
                char* endEnd = nullptr;
                uint64_t endVal = static_cast<uint64_t>(std::strtoull(endStr.c_str(), &endEnd, 10));
                while (endEnd && *endEnd == ' ') ++endEnd;
                bool endOk = (errno != ERANGE && endEnd != endStr.c_str() && (!endEnd || *endEnd == '\0'));
                if (startOk && endOk && endVal >= startVal) {
                    out.hasContentRange = true;
                    out.contentRangeStart = startVal;
                    out.contentRangeEnd = endVal;
                }
            }
            if (slash != std::string::npos && slash + 1 < rangeVal.size()) {
                std::string totalStr = rangeVal.substr(slash + 1);
                while (!totalStr.empty() && (totalStr.front() == ' ' || totalStr.front() == '\t')) totalStr.erase(totalStr.begin());
                while (!totalStr.empty() && (totalStr.back() == ' ' || totalStr.back() == '\t')) totalStr.pop_back();
                if (!totalStr.empty() && totalStr != "*") {
                    errno = 0;
                    char* endptr = nullptr;
                    uint64_t total = static_cast<uint64_t>(std::strtoull(totalStr.c_str(), &endptr, 10));
                    while (endptr && *endptr == ' ') ++endptr;
                    if (errno != ERANGE && endptr != totalStr.c_str() && (!endptr || *endptr == '\0')) {
                        out.hasContentRangeTotal = true;
                        out.contentRangeTotal = total;
                    }
                }
            }
        } else if (keyLower == "transfer-encoding" && valLower.find("chunked") != std::string::npos) {
            out.chunked = true;
        } else if (keyLower == "accept-ranges" && valLower.find("bytes") != std::string::npos) {
            out.acceptRanges = true;
        } else if (keyLower == "connection" && valLower.find("close") != std::string::npos) {
            out.connectionClose = true;
        } else if (keyLower == "location") {
            out.location = val;
        }
    }
    out.headersRaw = raw.str();
    return true;
}

bool httpRequestBuffered(const std::string& method,
                         const std::string& url,
                         const std::vector<std::pair<std::string, std::string>>& headers,
                         const HttpRequestOptions& options,
                         HttpTransaction& out,
                         std::string& err) {
    out = HttpTransaction{};
    err.clear();
#ifndef UNIT_TEST
    ParsedUrl parsedUrl;
    if (!parseHttpUrlInternal(url, parsedUrl, err)) return false;

    if (!ensureCurlGlobalInit(err)) return false;
    CurlEasyHandle easy;
    if (!acquireCurlHandle(options.keepAlive, easy, err)) return false;

    CurlHeaderState headerState;
    CurlProgressState progressState;
    CurlSlist reqHeaders;
    if (!setupCurlRequest(easy.handle, method, url, headers, options, headerState, progressState, reqHeaders, err)) {
        return false;
    }

    if (options.activeSocketFd) options.activeSocketFd->store(-1, std::memory_order_release);

    CurlBufferedWriteState writeState;
    writeState.options = &options;
    writeState.body = &out.body;
    curl_easy_setopt(easy.handle, CURLOPT_WRITEFUNCTION, curlBufferedWriteCallback);
    curl_easy_setopt(easy.handle, CURLOPT_WRITEDATA, &writeState);

    CURLcode rc = curl_easy_perform(easy.handle);
    if (rc != CURLE_OK) {
        if (progressState.cancelled || isCancelled(options)) {
            err = "Cancelled";
        } else if (writeState.sizeExceeded) {
            err = "HTTP body exceeds configured max size";
        } else if (rc == CURLE_OPERATION_TIMEDOUT) {
            err = "Recv timed out";
        } else {
            err = std::string("CURL failed: ") + curl_easy_strerror(rc);
        }
        return false;
    }

    if (!parseCurlResponseHeaders(headerState, out.parsed, err)) return false;
    if (!options.decodeChunked && out.parsed.chunked) {
        err = "Chunked transfer not supported";
        return false;
    }
    if (options.maxBodyBytes > 0 && out.body.size() > options.maxBodyBytes) {
        err = "HTTP body exceeds configured max size";
        return false;
    }
    if (isHeadMethod(method) || isNoBodyStatus(out.parsed.statusCode)) out.body.clear();
    return true;
#else
    (void)method;
    (void)url;
    (void)headers;
    (void)options;
    err = "httpRequestBuffered unavailable in UNIT_TEST";
    return false;
#endif
}

bool httpRequestStreamed(const std::string& method,
                         const std::string& url,
                         const std::vector<std::pair<std::string, std::string>>& headers,
                         const HttpRequestOptions& options,
                         ParsedHttpResponse& outHeaders,
                         const std::function<bool(const char*, size_t)>& onData,
                         std::string& err) {
    outHeaders = ParsedHttpResponse{};
    err.clear();
#ifndef UNIT_TEST
    ParsedUrl parsedUrl;
    if (!parseHttpUrlInternal(url, parsedUrl, err)) return false;

    if (!ensureCurlGlobalInit(err)) return false;
    CurlEasyHandle easy;
    if (!acquireCurlHandle(options.keepAlive, easy, err)) return false;

    CurlHeaderState headerState;
    CurlProgressState progressState;
    CurlSlist reqHeaders;
    if (!setupCurlRequest(easy.handle, method, url, headers, options, headerState, progressState, reqHeaders, err)) {
        return false;
    }

    if (options.activeSocketFd) options.activeSocketFd->store(-1, std::memory_order_release);

    CurlStreamWriteState writeState;
    writeState.options = &options;
    writeState.onData = &onData;
    writeState.headers = &headerState;
    writeState.parsed = &outHeaders;
    curl_easy_setopt(easy.handle, CURLOPT_WRITEFUNCTION, curlStreamWriteCallback);
    curl_easy_setopt(easy.handle, CURLOPT_WRITEDATA, &writeState);

    CURLcode rc = curl_easy_perform(easy.handle);

    if (!writeState.headersParsed) {
        if (!parseCurlResponseHeaders(headerState, outHeaders, err)) {
            if (rc != CURLE_OK) {
                if (progressState.cancelled || isCancelled(options)) err = "Cancelled";
                else err = std::string("CURL failed: ") + curl_easy_strerror(rc);
            }
            return false;
        }
        writeState.headersParsed = true;
    }

    if (rc != CURLE_OK) {
        if (progressState.cancelled || isCancelled(options)) {
            err = "Cancelled";
        } else if (writeState.sinkAborted) {
            err = "Sink aborted";
        } else if (writeState.sizeExceeded) {
            err = "HTTP body exceeds configured max size";
        } else if (writeState.chunkedRejected) {
            err = "Chunked encoding not supported for streaming downloads";
        } else if (writeState.parseFailed) {
            err = writeState.parseErr;
        } else if (rc == CURLE_OPERATION_TIMEDOUT) {
            err = "Recv timed out";
        } else {
            err = std::string("CURL failed: ") + curl_easy_strerror(rc);
        }
        return false;
    }

    if (writeState.chunkedRejected || outHeaders.chunked) {
        err = "Chunked encoding not supported for streaming downloads";
        return false;
    }
    if (options.maxBodyBytes > 0 && writeState.streamed > options.maxBodyBytes) {
        err = "HTTP body exceeds configured max size";
        return false;
    }
    if (!(isHeadMethod(method) || isNoBodyStatus(outHeaders.statusCode)) &&
        outHeaders.hasContentLength &&
        writeState.streamed < outHeaders.contentLength) {
        err = "Short read";
        return false;
    }
    return true;
#else
    (void)method;
    (void)url;
    (void)headers;
    (void)options;
    (void)onData;
    err = "httpRequestStreamed unavailable in UNIT_TEST";
    return false;
#endif
}

} // namespace romm
