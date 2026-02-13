#include "romm/api.hpp"
#include "romm/logger.hpp"
#include "romm/util.hpp"
#include "romm/raii.hpp"
#include "romm/http_common.hpp"
#include "mini/json.hpp"
// TODO(http): centralize HTTP client with structured errors/timeouts and optional token auth.

#ifndef UNIT_TEST
#include <switch.h>
#else
#include "switch_stubs.hpp"
#endif
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <functional>
#include <cerrno>
#include <cmath>
#include <algorithm>
#include <array>
#include <optional>
#ifndef UNIT_TEST
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/time.h>
#endif

namespace romm {

namespace {
constexpr size_t kApiRecvBuf = 8192;
constexpr int64_t kRetryDelayFastNs = 250'000'000LL; // 250ms
constexpr int64_t kRetryDelaySlowNs = 1'000'000'000LL; // 1s
constexpr size_t kDefaultApiPageLimit = 300;
constexpr size_t kDefaultRemoteSearchLimit = 250;
constexpr size_t kMaxDigestItems = 10000;

void setApiError(std::string& outError,
                 ErrorInfo* outInfo,
                 const std::string& detail,
                 ErrorCategory hint) {
    outError = detail;
    if (outInfo) *outInfo = classifyError(detail, hint);
}

static uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

static std::string hex64(uint64_t v) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << v;
    return oss.str();
}

static std::string stableDigest(const std::vector<std::string>& tokens) {
    std::vector<std::string> sorted = tokens;
    std::sort(sorted.begin(), sorted.end());
    uint64_t h = 1469598103934665603ULL;
    for (const auto& t : sorted) {
        h ^= fnv1a64(t);
        h *= 1099511628211ULL;
    }
    return hex64(h);
}

static std::string valueToken(const mini::Value& v) {
    switch (v.type) {
        case mini::Value::Type::String:
            return v.str;
        case mini::Value::Type::Number: {
            std::ostringstream oss;
            oss << std::setprecision(17) << v.number;
            return oss.str();
        }
        case mini::Value::Type::Bool:
            return v.boolean ? "1" : "0";
        case mini::Value::Type::Null:
            return "null";
        case mini::Value::Type::Array:
            return "array(" + std::to_string(v.array.size()) + ")";
        case mini::Value::Type::Object:
            return "object(" + std::to_string(v.object.size()) + ")";
    }
    return {};
}

static bool parseIdentifiersDigestBody(const std::string& body, std::string& outDigest, std::string& err) {
    mini::Array arr;
    mini::Object obj;
    std::vector<std::string> tokens;
    auto addToken = [&](const std::string& t) {
        if (!t.empty() && tokens.size() < kMaxDigestItems) tokens.push_back(t);
    };

    if (mini::parse(body, arr)) {
        for (const auto& v : arr) {
            if (v.type == mini::Value::Type::Object) {
                const auto& o = v.object;
                std::string id;
                std::array<const char*, 7> idKeys{
                    "id", "rom_id", "platform_id", "slug", "name", "value", "key"
                };
                for (const char* k : idKeys) {
                    auto it = o.find(k);
                    if (it != o.end()) {
                        id = valueToken(it->second);
                        if (!id.empty()) break;
                    }
                }
                std::string ver;
                std::array<const char*, 8> verKeys{
                    "updated_at", "modified_at", "mtime", "timestamp", "version", "checksum", "hash", "etag"
                };
                for (const char* k : verKeys) {
                    auto it = o.find(k);
                    if (it != o.end()) {
                        ver = valueToken(it->second);
                        if (!ver.empty()) break;
                    }
                }
                if (!id.empty() || !ver.empty()) {
                    addToken(id + "|" + ver);
                } else {
                    std::vector<std::string> kv;
                    kv.reserve(o.size());
                    for (const auto& kvp : o) kv.push_back(kvp.first + "=" + valueToken(kvp.second));
                    addToken(stableDigest(kv));
                }
            } else {
                addToken(valueToken(v));
            }
        }
        outDigest = stableDigest(tokens);
        return true;
    }

    if (!mini::parse(body, obj)) {
        err = "Failed to parse identifiers JSON";
        return false;
    }

    auto collectArray = [&](const char* key) -> bool {
        auto it = obj.find(key);
        if (it == obj.end() || it->second.type != mini::Value::Type::Array) return false;
        for (const auto& v : it->second.array) addToken(valueToken(v));
        return true;
    };

    if (collectArray("items") || collectArray("identifiers") || collectArray("results") || collectArray("ids")) {
        outDigest = stableDigest(tokens);
        return true;
    }

    for (const auto& kv : obj) {
        addToken(kv.first + "=" + valueToken(kv.second));
    }
    outDigest = stableDigest(tokens);
    return true;
}

#ifndef UNIT_TEST
struct KeepAliveConn {
    int fd{-1};
    std::string host;
    std::string port;
    int timeoutSec{0};
};

thread_local KeepAliveConn gKeepAliveConn;

static void closeKeepAliveConn() {
    if (gKeepAliveConn.fd >= 0) {
        close(gKeepAliveConn.fd);
    }
    gKeepAliveConn = KeepAliveConn{};
}

static bool openKeepAliveConn(const std::string& host,
                              const std::string& port,
                              int timeoutSec,
                              std::string& err) {
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    int ret = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (ret != 0 || !res) {
        err = "DNS lookup failed for host: " + host;
        if (res) freeaddrinfo(res);
        return false;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        err = "Socket creation failed";
        freeaddrinfo(res);
        return false;
    }

    if (timeoutSec > 0) {
        timeval tv{};
        tv.tv_sec = timeoutSec;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        err = std::string("Connect failed: ") + std::strerror(errno);
        freeaddrinfo(res);
        close(fd);
        return false;
    }
    freeaddrinfo(res);

    closeKeepAliveConn();
    gKeepAliveConn.fd = fd;
    gKeepAliveConn.host = host;
    gKeepAliveConn.port = port;
    gKeepAliveConn.timeoutSec = timeoutSec;
    return true;
}

static bool ensureKeepAliveConn(const std::string& host,
                                const std::string& port,
                                int timeoutSec,
                                std::string& err) {
    if (gKeepAliveConn.fd >= 0 &&
        gKeepAliveConn.host == host &&
        gKeepAliveConn.port == port &&
        gKeepAliveConn.timeoutSec == timeoutSec) {
        return true;
    }
    return openKeepAliveConn(host, port, timeoutSec, err);
}

static bool readHttpResponseKeepAlive(int fd, HttpResponse& resp, std::string& err, bool& shouldCloseConn) {
    resp = HttpResponse{};
    shouldCloseConn = false;

    std::string raw;
    raw.reserve(16 * 1024);
    char buf[kApiRecvBuf];
    size_t hdrEnd = std::string::npos;
    while (hdrEnd == std::string::npos) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            err = (errno == EAGAIN || errno == EWOULDBLOCK) ? "Recv timed out" :
                  (std::string("Recv failed: ") + std::strerror(errno));
            return false;
        }
        if (n == 0) {
            err = "Connection closed before HTTP headers";
            return false;
        }
        raw.append(buf, buf + n);
        hdrEnd = raw.find("\r\n\r\n");
    }

    std::string headerBlock = raw.substr(0, hdrEnd);
    ParsedHttpResponse parsed{};
    if (!parseHttpResponseHeaders(headerBlock, parsed, err)) {
        return false;
    }
    resp.statusCode = parsed.statusCode;
    resp.statusText = parsed.statusText;
    resp.headersRaw = parsed.headersRaw;

    std::string headersLower = resp.headersRaw;
    for (auto& c : headersLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (headersLower.find("connection: close") != std::string::npos) {
        shouldCloseConn = true;
    }

    std::string body = raw.substr(hdrEnd + 4);
    if (parsed.chunked) {
        // Conservative handling: read until we can decode a complete chunked body.
        std::string decoded;
        while (!decodeChunkedBody(body, decoded)) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n < 0) {
                err = (errno == EAGAIN || errno == EWOULDBLOCK) ? "Recv timed out" :
                      (std::string("Recv failed: ") + std::strerror(errno));
                return false;
            }
            if (n == 0) break;
            body.append(buf, buf + n);
        }
        if (!decodeChunkedBody(body, decoded)) {
            err = "Failed to decode chunked HTTP body";
            return false;
        }
        resp.body.swap(decoded);
        shouldCloseConn = true; // simplify connection lifecycle for chunked replies
        return true;
    }

    if (parsed.contentLength > 0) {
        while (body.size() < parsed.contentLength) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n < 0) {
                err = (errno == EAGAIN || errno == EWOULDBLOCK) ? "Recv timed out" :
                      (std::string("Recv failed: ") + std::strerror(errno));
                return false;
            }
            if (n == 0) {
                err = "Short HTTP body";
                return false;
            }
            body.append(buf, buf + n);
        }
        if (body.size() > parsed.contentLength) {
            body.resize(static_cast<size_t>(parsed.contentLength));
        }
        resp.body.swap(body);
        return true;
    }

    // No framing headers: read to EOF and close.
    while (true) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            err = (errno == EAGAIN || errno == EWOULDBLOCK) ? "Recv timed out" :
                  (std::string("Recv failed: ") + std::strerror(errno));
            return false;
        }
        if (n == 0) break;
        body.append(buf, buf + n);
    }
    resp.body.swap(body);
    shouldCloseConn = true;
    return true;
}

static bool httpRequestKeepAlive(const std::string& method,
                                 const std::string& url,
                                 const std::vector<std::pair<std::string, std::string>>& extraHeaders,
                                 int timeoutSec,
                                 HttpResponse& resp,
                                 std::string& err) {
    std::string host, portStr, path;
    if (!parseHttpUrl(url, host, portStr, path, err)) return false;

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!ensureKeepAliveConn(host, portStr, timeoutSec, err)) return false;

        std::ostringstream req;
        req << method << " " << path << " HTTP/1.1\r\n";
        req << "Host: " << host << "\r\n";
        req << "Connection: keep-alive\r\n";
        for (const auto& kv : extraHeaders) {
            req << kv.first << ": " << kv.second << "\r\n";
        }
        req << "\r\n";
        std::string reqStr = req.str();

        if (!romm::sendAll(gKeepAliveConn.fd, reqStr.data(), reqStr.size())) {
            closeKeepAliveConn();
            err = "Send failed";
            if (attempt == 0) continue;
            return false;
        }

        bool shouldCloseConn = false;
        if (!readHttpResponseKeepAlive(gKeepAliveConn.fd, resp, err, shouldCloseConn)) {
            closeKeepAliveConn();
            if (attempt == 0) continue;
            return false;
        }
        if (shouldCloseConn) closeKeepAliveConn();
        return true;
    }
    return false;
}
#endif
}

bool parseHttpUrl(const std::string& url,
                  std::string& host,
                  std::string& portStr,
                  std::string& path,
                  std::string& err)
{
    // TODO: support https:// via TLS in future; currently http-only.
    if (url.rfind("http://", 0) != 0) {
        err = "Only http:// URLs are supported (TLS not implemented)";
        return false;
    }

    std::string rest = url.substr(7); // after "http://"
    std::string hostport;
    auto slash = rest.find('/');
    if (slash == std::string::npos) {
        hostport = rest;
        path = "/";
    } else {
        hostport = rest.substr(0, slash);
        path = rest.substr(slash); // includes '/'
    }

    host = hostport;
    portStr = "80";
    auto colon = hostport.find(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        portStr = hostport.substr(colon + 1);
        if (portStr.empty()) portStr = "80";
    }

    if (host.empty()) {
        err = "Bad URL: missing host";
        return false;
    }
    if (path.empty()) path = "/";

    return true;
}

bool decodeChunkedBody(const std::string& body, std::string& decoded) {
    decoded.clear();
    size_t pos = 0;
    while (pos < body.size()) {
        size_t lineEnd = body.find("\r\n", pos);
        if (lineEnd == std::string::npos) {
            // malformed
            return false;
        }
        std::string lenLine = body.substr(pos, lineEnd - pos);
        // strip optional chunk extensions
        auto sc = lenLine.find(';');
        if (sc != std::string::npos) {
            lenLine = lenLine.substr(0, sc);
        }
        // trim spaces
        while (!lenLine.empty() && isspace(static_cast<unsigned char>(lenLine.front())))
            lenLine.erase(lenLine.begin());
        while (!lenLine.empty() && isspace(static_cast<unsigned char>(lenLine.back())))
            lenLine.pop_back();

        char* endptr = nullptr;
        errno = 0;
        long chunkSize = std::strtol(lenLine.c_str(), &endptr, 16);
        bool badNumber = (endptr == lenLine.c_str()) || (errno == ERANGE);
        if (badNumber) {
            return false;
        }
        if (chunkSize == 0) {
            // Require trailing CRLF after the zero-size chunk.
            if (lineEnd + 4 > body.size()) return false;
            if (body[lineEnd + 2] != '\r' || body[lineEnd + 3] != '\n') return false;
            return true;
        }
        pos = lineEnd + 2; // skip CRLF
        if (pos + static_cast<size_t>(chunkSize) > body.size()) {
            // incomplete chunk
            return false;
        }
        decoded.append(body, pos, static_cast<size_t>(chunkSize));
        pos += static_cast<size_t>(chunkSize);
        // skip trailing CRLF after chunk data
        if (pos + 2 > body.size()) return false;
        if (body[pos] != '\r' || body[pos + 1] != '\n') return false;
        pos += 2;
    }
    return true;
}

#ifndef UNIT_TEST
// Low-level HTTP request: no JSON assumptions.
// Returns true if we got *any* HTTP response (even 4xx/5xx).
// resp.statusCode will be 0 on protocol/parse failure.
bool httpRequest(const std::string& method,
                 const std::string& url,
                 const std::vector<std::pair<std::string, std::string>>& extraHeaders,
                 int timeoutSec,
                 HttpResponse& resp,
                 std::string& err)
{
    return httpRequestKeepAlive(method, url, extraHeaders, timeoutSec, resp, err);
}

// Streaming variant: delivers body via onData, does not keep the payload in memory.
bool httpRequestStream(const std::string& method,
                       const std::string& url,
                       const std::vector<std::pair<std::string, std::string>>& extraHeaders,
                       int timeoutSec,
                       HttpResponse& resp,
                       const std::function<bool(const char*, size_t)>& onData,
                       std::string& err)
{
    resp = HttpResponse{};
    std::string host, portStr, path;
    if (!parseHttpUrl(url, host, portStr, path, err)) {
        return false;
    }

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;

    int ret = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
    if (ret != 0 || !res) {
        err = "DNS lookup failed for host: " + host;
        if (res) freeaddrinfo(res);
        return false;
    }

    romm::UniqueFd sockFd(socket(res->ai_family, res->ai_socktype, res->ai_protocol));
    if (!sockFd) {
        err = "Socket creation failed";
        freeaddrinfo(res);
        return false;
    }

    if (timeoutSec > 0) {
        timeval tv{}; tv.tv_sec = timeoutSec; tv.tv_usec = 0;
        setsockopt(sockFd.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sockFd.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    if (connect(sockFd.fd, res->ai_addr, res->ai_addrlen) != 0) {
        err = std::string("Connect failed: ") + std::strerror(errno);
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);

    std::ostringstream req;
    req << method << " " << path << " HTTP/1.1\r\n";
    req << "Host: " << host << "\r\n";
    req << "Connection: close\r\n";
    for (const auto& kv : extraHeaders) {
        req << kv.first << ": " << kv.second << "\r\n";
    }
    req << "\r\n";
    std::string reqStr = req.str();

    if (!romm::sendAll(sockFd.fd, reqStr.data(), reqStr.size())) {
        err = "Send failed";
        return false;
    }

    std::string headerBuf; // only used until headers parsed
    char buf[kApiRecvBuf];
    bool headersDone = false;
    size_t bytesSentToSink = 0;
    bool chunked = false;
    uint64_t contentLength = 0;

    while (true) {
        ssize_t n = recv(sockFd.fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                err = "Recv timed out";
            } else {
                err = std::string("Recv failed: ") + std::strerror(errno);
            }
            return false;
        } else if (n == 0) {
            break; // EOF
        }

        if (!headersDone) {
            headerBuf.append(buf, buf + n);
            auto hdrEnd = headerBuf.find("\r\n\r\n");
            if (hdrEnd == std::string::npos) {
                continue; // need more header data
            }
            headersDone = true;
            size_t bodyStart = hdrEnd + 4;

            std::string headerBlock = headerBuf.substr(0, hdrEnd);
            ParsedHttpResponse parsed{};
            if (!parseHttpResponseHeaders(headerBlock, parsed, err)) {
                return false;
            }
            resp.statusCode = parsed.statusCode;
            resp.statusText = parsed.statusText;
            resp.headersRaw = parsed.headersRaw;
            contentLength = parsed.contentLength;
            chunked = parsed.chunked;
            if (chunked) {
                err = "Chunked encoding not supported for streaming downloads";
                return false;
            }

            // Send any body bytes already received with the headers.
            if (bodyStart < headerBuf.size()) {
                size_t bodyBytes = headerBuf.size() - bodyStart;
                if (!onData(headerBuf.data() + bodyStart, bodyBytes)) {
                    err = "Sink aborted";
                    return false;
                }
                bytesSentToSink += bodyBytes;
            }
            headerBuf.clear(); // free header accumulation to avoid buffering whole body
            continue;
        }

        // After headers: stream straight to sink without buffering.
        if (!onData(buf, static_cast<size_t>(n))) {
            err = "Sink aborted";
            return false;
        }
        bytesSentToSink += static_cast<size_t>(n);
    }

    if (contentLength > 0 && bytesSentToSink < contentLength) {
        err = "Short read";
        return false;
    }

    return true;
}
#else
// Stubbed httpRequest for UNIT_TEST builds (network not exercised).
bool httpRequest(const std::string&,
                 const std::string&,
                 const std::vector<std::pair<std::string, std::string>>&,
                 int,
                 HttpResponse&,
                 std::string& err) {
    err = "httpRequest not available in UNIT_TEST build";
    return false;
}

bool httpRequestStream(const std::string&,
                       const std::string&,
                       const std::vector<std::pair<std::string, std::string>>&,
                       int,
                       HttpResponse&,
                       const std::function<bool(const char*, size_t)>&,
                       std::string& err) {
    err = "httpRequestStream not available in UNIT_TEST build";
    return false;
}

// Test helper: simulate streaming from a raw HTTP response string.
bool httpRequestStreamMock(const std::string& rawResponse,
                           HttpResponse& resp,
                           const std::function<bool(const char*, size_t)>& sink,
                           std::string& err) {
    resp = HttpResponse{};
    auto hdrEnd = rawResponse.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) {
        err = "Malformed response";
        return false;
    }
    std::string headerBlock = rawResponse.substr(0, hdrEnd);
    std::string body = rawResponse.substr(hdrEnd + 4);

    auto firstCrLf = headerBlock.find("\r\n");
    if (firstCrLf == std::string::npos) {
        err = "Malformed status line";
        return false;
    }
    resp.headersRaw = headerBlock.substr(firstCrLf + 2);
    std::string statusLine = headerBlock.substr(0, firstCrLf);
    if (!statusLine.empty() && statusLine.back() == '\r') statusLine.pop_back();
    std::istringstream sl(statusLine);
    std::string httpVer;
    sl >> httpVer >> resp.statusCode;
    std::getline(sl, resp.statusText);
    if (!resp.statusText.empty() && resp.statusText.front() == ' ') resp.statusText.erase(resp.statusText.begin());

    // Parse headers: reject chunked, track content-length for short-read detection.
    uint64_t contentLength = 0;
    std::istringstream hs(headerBlock.substr(firstCrLf + 2));
    std::string line;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
        std::string keyLower = key;
        for (auto& c : keyLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (keyLower == "transfer-encoding" && val.find("chunked") != std::string::npos) {
            err = "Chunked not supported in mock";
            return false;
        } else if (keyLower == "content-length") {
            contentLength = static_cast<uint64_t>(std::strtoull(val.c_str(), nullptr, 10));
        }
    }

    if (!body.empty()) {
        if (!sink(body.data(), body.size())) {
            err = "Sink aborted";
            return false;
        }
        // Only enforce short-read if a body was present; header-only mocks (preflight) are allowed.
        if (contentLength > 0 && body.size() < contentLength) {
            err = "Short read";
            return false;
        }
    }
    return true;
}
#endif

static std::string headerValue(const std::string& headersRaw, const std::string& wantedLower) {
    std::istringstream hs(headersRaw);
    std::string line;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (key != wantedLower) continue;
        std::string val = line.substr(colon + 1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
        return val;
    }
    return {};
}

static bool shouldRetryHttpStatus(int status) {
    return status == 408 || status == 425 || status == 429 || (status >= 500 && status <= 599);
}

static std::string buildHttpFailure(const HttpResponse& resp) {
    std::string err = "HTTP " + std::to_string(resp.statusCode) +
                      (resp.statusText.empty() ? "" : (" " + resp.statusText));
    if (resp.statusCode >= 300 && resp.statusCode < 400) {
        std::string location = headerValue(resp.headersRaw, "location");
        if (!location.empty()) {
            err += " redirect to " + location +
                   " (redirects disabled; auth is not forwarded cross-host)";
        } else {
            err += " redirect (redirects disabled; auth is not forwarded cross-host)";
        }
    }
    if (!resp.body.empty()) {
        err += " body: " + resp.body.substr(0, resp.body.size() > 256 ? 256 : resp.body.size());
    }
    return err;
}

// Simple retry wrapper for JSON GET requests.
// Retries on transport errors/timeouts and retryable HTTP statuses (408/425/429/5xx).
static bool httpGetJsonWithRetry(const std::string& url,
                                 const std::string& authBasic,
                                 int timeoutSec,
                                 HttpResponse& resp,
                                 std::string& err)
{
    const int maxAttempts = 3;
    std::string lastErr;
    bool hadHttpResponse = false;

    std::vector<std::pair<std::string,std::string>> headers;
    headers.emplace_back("Accept", "application/json");
    if (!authBasic.empty()) {
        headers.emplace_back("Authorization", "Basic " + authBasic);
    }

    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        HttpResponse r;
        std::string e;
        if (httpRequest("GET", url, headers, timeoutSec, r, e)) {
            hadHttpResponse = true;
            resp = std::move(r);
            if (resp.statusCode >= 200 && resp.statusCode < 300) {
                return true;
            }

            lastErr = buildHttpFailure(resp);
            if (shouldRetryHttpStatus(resp.statusCode) && attempt < maxAttempts) {
                int64_t delayNs = (attempt == 1) ? kRetryDelayFastNs : kRetryDelaySlowNs;
                svcSleepThread(delayNs);
                continue;
            }
            err = lastErr;
            return false;
        }

        lastErr = e.empty() ? "HTTP transport failure" : e;
        if (attempt < maxAttempts) {
            // basic backoff: 250ms, 1s
            int64_t delayNs = (attempt == 1) ? kRetryDelayFastNs : kRetryDelaySlowNs;
            svcSleepThread(delayNs);
        }
    }

    if (hadHttpResponse) {
        err = lastErr;
    } else {
        err = "HTTP request failed after retries: " + lastErr;
    }
    return false;
}

static std::string valToString(const mini::Value& v) {
    if (v.type == mini::Value::Type::String) return v.str;
    if (v.type == mini::Value::Type::Number) {
        // JSON parsers commonly store integers as doubles; avoid "2.000000" IDs, which can break API queries.
        const double n = v.number;
        if (std::isfinite(n)) {
            const double r = std::round(n);
            if (std::fabs(n - r) < 1e-9 && std::fabs(r) <= 9e15) {
                return std::to_string(static_cast<int64_t>(r));
            }
        }
        // Fallback: preserve value with more precision than std::to_string (which rounds to 6 decimals).
        std::ostringstream oss;
        oss << std::setprecision(17) << n;
        return oss.str();
    }
    return {};
}

static std::string previewText(const std::string& s, size_t maxlen = 64) {
    if (s.size() <= maxlen) return s;
    return s.substr(0, maxlen) + "...";
}

static bool parsePlatforms(const std::string& body, Status& status, std::string& err) {
    mini::Array arr;
    mini::Object obj;
    if (!mini::parse(body, arr)) {
        if (mini::parse(body, obj)) {
            auto it = obj.find("items");
            if (it != obj.end() && it->second.type == mini::Value::Type::Array) {
                arr = it->second.array;
            } else {
                err = "Platforms JSON missing items array";
                return false;
            }
        } else {
            err = "Failed to parse platforms JSON";
            return false;
        }
    }

    status.platforms.clear();
    for (auto& v : arr) {
        if (v.type != mini::Value::Type::Object) continue;
        auto& o = v.object;
        Platform p;
        if (auto it = o.find("id"); it != o.end()) p.id = valToString(it->second);
        if (auto it = o.find("display_name"); it != o.end() && it->second.type == mini::Value::Type::String)
            p.name = it->second.str;
        else if (auto itn = o.find("name"); itn != o.end() && itn->second.type == mini::Value::Type::String)
            p.name = itn->second.str;
        if (auto it = o.find("slug"); it != o.end() && it->second.type == mini::Value::Type::String)
            p.slug = it->second.str;
        if (auto it = o.find("rom_count"); it != o.end() && it->second.type == mini::Value::Type::Number)
            p.romCount = static_cast<int>(it->second.number);

        if (!p.id.empty()) {
            status.platforms.push_back(p);
        }
    }

    status.platformsReady = true;
    return true;
}

struct ParsedGamesPayload {
    std::vector<Game> games;
    size_t total{0};
    bool totalKnown{false};
};

static bool parseGamesPayload(const std::string& body,
                              const std::string& platformId,
                              const std::string& serverUrl,
                              ParsedGamesPayload& out,
                              std::string& err) {
    auto encodePath = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (unsigned char c : s) {
            if (c == ' ') { out += "%20"; continue; }
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/' || c == ':'
                || c == '?' || c == '&' || c == '=' || c == '%') {
                out.push_back(static_cast<char>(c));
            } else {
                char buf[4];
                std::snprintf(buf, sizeof(buf), "%%%02X", c);
                out.append(buf);
            }
        }
        return out;
    };
    out = ParsedGamesPayload{};
    mini::Array arr;
    mini::Object obj;
    if (!mini::parse(body, arr)) {
        if (mini::parse(body, obj)) {
            auto setTotalIfNumber = [&](const char* key) {
                auto it = obj.find(key);
                if (it != obj.end() && it->second.type == mini::Value::Type::Number) {
                    if (it->second.number >= 0) {
                        out.total = static_cast<size_t>(it->second.number);
                        out.totalKnown = true;
                    }
                }
            };
            setTotalIfNumber("total");
            setTotalIfNumber("count");
            setTotalIfNumber("num_results");
            setTotalIfNumber("total_count");
            auto adoptArray = [&](const char* key) -> bool {
                auto it = obj.find(key);
                if (it != obj.end() && it->second.type == mini::Value::Type::Array) {
                    arr = it->second.array;
                    return true;
                }
                return false;
            };
            if (!adoptArray("items") && !adoptArray("results") && !adoptArray("roms")) {
                err = "Games JSON missing items array";
                return false;
            }
        } else {
            err = "Failed to parse games JSON";
            return false;
        }
    }

    out.games.clear();
    for (auto& v : arr) {
        if (v.type != mini::Value::Type::Object) continue;
        auto& o = v.object;
        Game g;

        auto stripLeadingSlash = [](std::string s) {
            while (!s.empty() && s.front() == '/') s.erase(s.begin());
            return s;
        };
        auto absolutizeUrl = [&](const std::string& url) -> std::string {
            if (url.empty()) return url;
        if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) return encodePath(url);
        if (!serverUrl.empty() && url.front() == '/') {
            if (serverUrl.back() == '/')
                return encodePath(serverUrl.substr(0, serverUrl.size() - 1) + url);
            return encodePath(serverUrl + url);
        }
        return encodePath(url);
    };

        if (auto it = o.find("id"); it != o.end())
            g.id = valToString(it->second);

        if (auto it = o.find("name"); it != o.end() && it->second.type == mini::Value::Type::String)
            g.title = it->second.str;
        if (g.title.empty()) {
            auto it = o.find("title");
            if (it != o.end() && it->second.type == mini::Value::Type::String)
                g.title = it->second.str;
        }

        if (auto it = o.find("fs_size_bytes"); it != o.end() && it->second.type == mini::Value::Type::Number)
            g.sizeBytes = static_cast<uint64_t>(it->second.number);
        if (g.sizeBytes == 0) {
            auto it = o.find("fs_size");
            if (it != o.end() && it->second.type == mini::Value::Type::Number)
                g.sizeBytes = static_cast<uint64_t>(it->second.number);
        }

        if (auto it = o.find("fs_name"); it != o.end() && it->second.type == mini::Value::Type::String)
            g.fsName = stripLeadingSlash(it->second.str);

        if (auto it = o.find("platform_id"); it != o.end()) {
            g.platformId = valToString(it->second);
        }
        if (auto it = o.find("platform_slug"); it != o.end() && it->second.type == mini::Value::Type::String) {
            g.platformSlug = it->second.str;
        }
        if (auto it = o.find("platform"); it != o.end() && it->second.type == mini::Value::Type::Object) {
            const auto& po = it->second.object;
            if (g.platformId.empty()) {
                if (auto pid = po.find("id"); pid != po.end()) g.platformId = valToString(pid->second);
            }
            if (g.platformSlug.empty()) {
                if (auto pslug = po.find("slug"); pslug != po.end() && pslug->second.type == mini::Value::Type::String)
                    g.platformSlug = pslug->second.str;
            }
        }

        if (auto it = o.find("path_cover_small"); it != o.end() && it->second.type == mini::Value::Type::String)
            g.coverUrl = absolutizeUrl(it->second.str);
        else if (auto it1 = o.find("cover_url"); it1 != o.end() && it1->second.type == mini::Value::Type::String)
            g.coverUrl = absolutizeUrl(it1->second.str);
        else if (auto it2 = o.find("assets"); it2 != o.end() && it2->second.type == mini::Value::Type::Object) {
            auto cov = it2->second.object.find("cover");
            if (cov != it2->second.object.end() && cov->second.type == mini::Value::Type::String)
                g.coverUrl = absolutizeUrl(cov->second.str);
        }

        // If the list API doesn't include per-ROM platform metadata, fall back to the selected platform.
        if (g.platformId.empty()) g.platformId = platformId;

        if (!g.id.empty()) out.games.push_back(g);
    }

    if (!out.totalKnown) out.total = out.games.size();

    if (!out.games.empty()) {
        std::string first  = previewText(out.games[0].title);
        std::string second = out.games.size() > 1 ? previewText(out.games[1].title) : "";
        std::string third  = out.games.size() > 2 ? previewText(out.games[2].title) : "";
        logLine("Parsed ROMs: " + std::to_string(out.games.size()) + " first=" + first);
        if (!second.empty()) logLine(" second=" + second);
        if (!third.empty())  logLine(" third=" + third);
    }

    return true;
}

#ifdef UNIT_TEST
bool parseGamesTest(const std::string& body,
                    const std::string& platformId,
                    const std::string& serverUrl,
                    std::vector<Game>& outGames,
                    std::string& err)
{
    ParsedGamesPayload parsed;
    bool ok = parseGamesPayload(body, platformId, serverUrl, parsed, err);
    outGames = std::move(parsed.games);
    return ok;
}

bool parsePlatformsTest(const std::string& body,
                        std::vector<Platform>& outPlatforms,
                        std::string& err)
{
    Status st;
    bool ok = parsePlatforms(body, st, err);
    outPlatforms = st.platforms;
    return ok;
}

bool parseIdentifiersDigestTest(const std::string& body,
                                std::string& outDigest,
                                std::string& err) {
    return parseIdentifiersDigestBody(body, outDigest, err);
}
#endif

static std::string buildBasicAuth(const Config& cfg) {
    if (cfg.username.empty() && cfg.password.empty()) return {};
    return romm::util::base64Encode(cfg.username + ":" + cfg.password);
}

static std::string buildPlatformRomsQuery(const std::string& serverUrl,
                                          const std::string& platformId,
                                          size_t limit,
                                          size_t offset) {
    std::string encodedPlatformId = romm::util::urlEncode(platformId);
    std::ostringstream q;
    q << serverUrl
      << "/api/roms?platform_ids=" << encodedPlatformId
      << "&platform_id=" << encodedPlatformId
      << "&with_char_index=false"
      << "&with_filter_values=false"
      << "&order_by=name&order_dir=asc"
      << "&limit=" << limit
      << "&offset=" << offset;
    return q.str();
}

bool fetchPlatformsIdentifiersDigest(const Config& cfg,
                                     std::string& outDigest,
                                     std::string& outError,
                                     ErrorInfo* outInfo) {
    if (outInfo) *outInfo = ErrorInfo{};
    outDigest.clear();
    HttpResponse resp;
    std::string err;
    if (!httpGetJsonWithRetry(cfg.serverUrl + "/api/platforms/identifiers",
                              buildBasicAuth(cfg), cfg.httpTimeoutSeconds, resp, err)) {
        setApiError(outError, outInfo, err, ErrorCategory::Network);
        return false;
    }
    if (!parseIdentifiersDigestBody(resp.body, outDigest, err)) {
        setApiError(outError, outInfo, err, ErrorCategory::Parse);
        return false;
    }
    outError.clear();
    return true;
}

bool fetchRomsIdentifiersDigest(const Config& cfg,
                                const std::string& platformId,
                                std::string& outDigest,
                                std::string& outError,
                                ErrorInfo* outInfo) {
    if (outInfo) *outInfo = ErrorInfo{};
    outDigest.clear();
    if (platformId.empty()) {
        setApiError(outError, outInfo, "Missing platform id for ROM identifiers probe.", ErrorCategory::Data);
        return false;
    }
    std::string encodedPlatformId = romm::util::urlEncode(platformId);
    std::string url = cfg.serverUrl + "/api/roms/identifiers?platform_ids=" + encodedPlatformId +
                      "&platform_id=" + encodedPlatformId;
    HttpResponse resp;
    std::string err;
    if (!httpGetJsonWithRetry(url, buildBasicAuth(cfg), cfg.httpTimeoutSeconds, resp, err)) {
        setApiError(outError, outInfo, err, ErrorCategory::Network);
        return false;
    }
    if (!parseIdentifiersDigestBody(resp.body, outDigest, err)) {
        setApiError(outError, outInfo, err, ErrorCategory::Parse);
        return false;
    }
    outError.clear();
    return true;
}

bool fetchPlatforms(const Config& cfg, Status& status, std::string& outError, ErrorInfo* outInfo) {
    if (outInfo) *outInfo = ErrorInfo{};
    static std::string sLastPlatformsDigest;
    if (!status.platforms.empty()) {
        std::string digest;
        std::string probeErr;
        if (fetchPlatformsIdentifiersDigest(cfg, digest, probeErr, nullptr) &&
            !digest.empty() &&
            digest == sLastPlatformsDigest) {
            outError.clear();
            logLine("API: platforms unchanged via identifiers probe; reusing in-memory list");
            return true;
        }
    }

    HttpResponse resp;
    std::string err;
    std::string url = cfg.serverUrl + "/api/platforms";

    if (!httpGetJsonWithRetry(url, buildBasicAuth(cfg), cfg.httpTimeoutSeconds, resp, err)) {
        setApiError(outError, outInfo, err, ErrorCategory::Network);
        return false;
    }

    if (!parsePlatforms(resp.body, status, err)) {
        setApiError(outError, outInfo, err, ErrorCategory::Parse);
        return false;
    }

    std::string digest;
    if (parseIdentifiersDigestBody(resp.body, digest, err) && !digest.empty()) {
        sLastPlatformsDigest = digest;
    } else {
        std::string probeErr;
        if (fetchPlatformsIdentifiersDigest(cfg, digest, probeErr, nullptr) && !digest.empty()) {
            sLastPlatformsDigest = digest;
        }
    }

    logLine("API: fetched platforms (" + std::to_string(status.platforms.size()) + ")");
    return true;
}

bool fetchGamesPageForPlatform(const Config& cfg,
                               const std::string& platformId,
                               size_t offset,
                               size_t limit,
                               GamesPage& outPage,
                               std::string& outError,
                               ErrorInfo* outInfo) {
    if (outInfo) *outInfo = ErrorInfo{};
    outPage = GamesPage{};
    if (platformId.empty()) {
        setApiError(outError, outInfo, "Missing platform id.", ErrorCategory::Data);
        return false;
    }
    if (limit == 0) limit = kDefaultApiPageLimit;

    HttpResponse resp;
    std::string err;
    std::string url = buildPlatformRomsQuery(cfg.serverUrl, platformId, limit, offset);

    if (!httpGetJsonWithRetry(url, buildBasicAuth(cfg), cfg.httpTimeoutSeconds, resp, err)) {
        setApiError(outError, outInfo, err, ErrorCategory::Network);
        return false;
    }

    ParsedGamesPayload parsed;
    if (!parseGamesPayload(resp.body, platformId, cfg.serverUrl, parsed, err)) {
        romm::logLine("ROMs response (first 256 bytes): " +
                      resp.body.substr(0, resp.body.size() > 256 ? 256 : resp.body.size()));
        setApiError(outError, outInfo, err, ErrorCategory::Parse);
        return false;
    }

    outPage.games = std::move(parsed.games);
    outPage.offset = offset;
    outPage.limit = limit;
    outPage.total = parsed.total;
    outPage.totalKnown = parsed.totalKnown;
    if (parsed.totalKnown) {
        outPage.hasMore = (offset + outPage.games.size()) < parsed.total;
    } else {
        outPage.hasMore = outPage.games.size() >= limit;
    }

    logLine("API: fetched games page offset=" + std::to_string(offset) +
            " limit=" + std::to_string(limit) +
            " count=" + std::to_string(outPage.games.size()) +
            " platform=" + platformId);
    outError.clear();
    return true;
}

bool fetchGamesForPlatform(const Config& cfg,
                           const std::string& platformId,
                           Status& status,
                           std::string& outError,
                           ErrorInfo* outInfo)
{
    GamesPage page;
    if (!fetchGamesPageForPlatform(cfg, platformId, 0, 10000, page, outError, outInfo)) {
        return false;
    }
    status.roms = std::move(page.games);
    status.romsReady = true;
    return true;
}

bool searchGamesRemote(const Config& cfg,
                       const std::string& platformId,
                       const std::string& query,
                       size_t limit,
                       std::vector<Game>& outGames,
                       std::string& outError,
                       ErrorInfo* outInfo) {
    if (outInfo) *outInfo = ErrorInfo{};
    outGames.clear();
    if (platformId.empty()) {
        setApiError(outError, outInfo, "Missing platform id for remote search.", ErrorCategory::Data);
        return false;
    }
    if (query.empty()) {
        outError.clear();
        return true;
    }
    if (limit == 0) limit = kDefaultRemoteSearchLimit;

    std::string encodedPid = romm::util::urlEncode(platformId);
    std::string encodedQ = romm::util::urlEncode(query);
    std::ostringstream url;
    url << cfg.serverUrl
        << "/api/search/roms?q=" << encodedQ
        << "&query=" << encodedQ
        << "&platform_ids=" << encodedPid
        << "&platform_id=" << encodedPid
        << "&limit=" << limit
        << "&offset=0";

    HttpResponse resp;
    std::string err;
    if (!httpGetJsonWithRetry(url.str(), buildBasicAuth(cfg), cfg.httpTimeoutSeconds, resp, err)) {
        setApiError(outError, outInfo, err, ErrorCategory::Network);
        return false;
    }

    ParsedGamesPayload parsed;
    if (!parseGamesPayload(resp.body, platformId, cfg.serverUrl, parsed, err)) {
        setApiError(outError, outInfo, err, ErrorCategory::Parse);
        return false;
    }
    outGames = std::move(parsed.games);
    outError.clear();
    logLine("API: remote search query=\"" + query + "\" results=" + std::to_string(outGames.size()));
    return true;
}

bool enrichGameWithFiles(const Config& cfg, Game& g, std::string& outError, ErrorInfo* outInfo) {
    if (outInfo) *outInfo = ErrorInfo{};
    if (g.id.empty()) {
        setApiError(outError, outInfo, "Game missing id; cannot fetch files.", ErrorCategory::Data);
        return false;
    }

    std::string auth;
    if (!cfg.username.empty() || !cfg.password.empty()) {
        auth = romm::util::base64Encode(cfg.username + ":" + cfg.password);
    }

    HttpResponse resp;
    std::string err;
    std::string url = cfg.serverUrl + "/api/roms/" + g.id;

    if (!httpGetJsonWithRetry(url, auth, cfg.httpTimeoutSeconds, resp, err)) {
        setApiError(outError, outInfo, err, ErrorCategory::Network);
        return false;
    }

    mini::Object obj;
    if (!mini::parse(resp.body, obj)) {
        setApiError(outError, outInfo, "Failed to parse DetailedRom JSON", ErrorCategory::Parse);
        return false;
    }

    auto encodePath = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (unsigned char c : s) {
            if (c == ' ') { out += "%20"; continue; }
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/' || c == ':'
                || c == '?' || c == '&' || c == '=' || c == '%') {
                out.push_back(static_cast<char>(c));
            } else {
                char buf[4];
                std::snprintf(buf, sizeof(buf), "%%%02X", c);
                out.append(buf);
            }
        }
        return out;
    };

    auto absolutizeUrl = [&](const std::string& url) -> std::string {
        if (url.empty()) return url;
        if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) return encodePath(url);
        if (!cfg.serverUrl.empty() && url.front() == '/') {
            if (cfg.serverUrl.back() == '/')
                return encodePath(cfg.serverUrl.substr(0, cfg.serverUrl.size() - 1) + url);
            return encodePath(cfg.serverUrl + url);
        }
        return encodePath(url);
    };

    if (auto covp = obj.find("path_cover_small"); covp != obj.end() && covp->second.type == mini::Value::Type::String) {
        g.coverUrl = absolutizeUrl(covp->second.str);
    } else if (auto cov = obj.find("cover_url"); cov != obj.end() && cov->second.type == mini::Value::Type::String) {
        g.coverUrl = absolutizeUrl(cov->second.str);
    } else if (auto it2 = obj.find("assets"); it2 != obj.end() && it2->second.type == mini::Value::Type::Object) {
        auto cov = it2->second.object.find("cover");
        if (cov != it2->second.object.end() && cov->second.type == mini::Value::Type::String)
            g.coverUrl = absolutizeUrl(cov->second.str);
    }

    auto itf = obj.find("files");
    if (itf == obj.end() || itf->second.type != mini::Value::Type::Array) {
        setApiError(outError, outInfo, "DetailedRom has no files array", ErrorCategory::Data);
        return false;
    }

    g.files.clear();
    uint64_t bestSize = 0;
    std::string bestName;
    std::string bestId;
    std::string bestDownloadUrl;
    int fileCount = 0;

    auto stripLeadingSlash = [](std::string s) {
        while (!s.empty() && s.front() == '/') s.erase(s.begin());
        return s;
    };

    for (auto& f : itf->second.array) {
        if (f.type != mini::Value::Type::Object) continue;
        const auto& fo = f.object;
        ++fileCount;

        std::string fname;
        if (auto n = fo.find("file_name"); n != fo.end() && n->second.type == mini::Value::Type::String)
            fname = n->second.str;
        else if (auto n2 = fo.find("name"); n2 != fo.end() && n2->second.type == mini::Value::Type::String)
            fname = n2->second.str;
        else if (auto n3 = fo.find("full_path"); n3 != fo.end() && n3->second.type == mini::Value::Type::String)
            fname = n3->second.str;
        fname = stripLeadingSlash(fname);

        std::string fid;
        if (auto idit = fo.find("id"); idit != fo.end()) {
            if (idit->second.type == mini::Value::Type::Number)
                fid = std::to_string(static_cast<uint64_t>(idit->second.number));
            else if (idit->second.type == mini::Value::Type::String)
                fid = idit->second.str;
        }

        uint64_t fsize = 0;
        if (auto sz = fo.find("file_size_bytes"); sz != fo.end() && sz->second.type == mini::Value::Type::Number)
            fsize = static_cast<uint64_t>(sz->second.number);
        else if (auto sz2 = fo.find("size_bytes"); sz2 != fo.end() && sz2->second.type == mini::Value::Type::Number)
            fsize = static_cast<uint64_t>(sz2->second.number);
        else if (auto sz3 = fo.find("size"); sz3 != fo.end()) {
            if (sz3->second.type == mini::Value::Type::Number)
                fsize = static_cast<uint64_t>(sz3->second.number);
            else if (sz3->second.type == mini::Value::Type::String)
                fsize = std::strtoull(sz3->second.str.c_str(), nullptr, 10);
        }

        std::string fpath;
        if (auto p = fo.find("path"); p != fo.end() && p->second.type == mini::Value::Type::String)
            fpath = stripLeadingSlash(p->second.str);

        std::string category;
        if (auto c = fo.find("category"); c != fo.end() && c->second.type == mini::Value::Type::String)
            category = c->second.str;

        std::string downloadUrlField;
        if (auto dl = fo.find("download_url"); dl != fo.end() && dl->second.type == mini::Value::Type::String) {
            downloadUrlField = dl->second.str;
        }
        std::string finalUrl = absolutizeUrl(downloadUrlField);
        if (finalUrl.empty() && !fid.empty() && !fname.empty()) {
            finalUrl = cfg.serverUrl + "/api/romsfiles/" + fid + "/content/" + romm::util::urlEncode(fname);
        }

        if (fname.empty() || fid.empty() || fsize == 0 || finalUrl.empty()) {
            logInfo("Skipping file with missing fields (name/id/size/url) in files[]", "API");
            continue;
        }

        RomFile rf;
        rf.id = fid;
        rf.name = fname;
        rf.path = fpath;
        rf.url = finalUrl;
        rf.sizeBytes = fsize;
        rf.category = category;
        g.files.push_back(rf);

        std::string lower = fname;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool preferredExt = (lower.size() >= 4) &&
                            (lower.rfind(".xci") != std::string::npos || lower.rfind(".nsp") != std::string::npos);
        bool bestIsPreferred = (!bestName.empty() &&
                                (bestName.rfind(".xci") != std::string::npos || bestName.rfind(".nsp") != std::string::npos));

        bool better = false;
        if (preferredExt) {
            if (!bestIsPreferred || fsize > bestSize) better = true;
        } else if (bestName.empty() || (!bestIsPreferred && fsize > bestSize)) {
            better = true;
        }

        if (better) {
            bestName = fname;
            bestId   = fid;
            bestSize = fsize;
            bestDownloadUrl = finalUrl;
        }

        logDebug("files[] entry: name=" + fname + " id=" + fid +
                 " size=" + std::to_string(fsize), "API");
    }

    if (g.files.empty()) {
        setApiError(outError, outInfo, "No valid files for ROM.", ErrorCategory::Data);
        return false;
    }

    if (!bestId.empty()) {
        g.fsName = bestName;
        g.fileId = bestId;
        g.sizeBytes = bestSize;
        g.downloadUrl = bestDownloadUrl;
        logInfo("Selected file via files[] id=" + bestId + " name=" + bestName +
                " size=" + std::to_string(bestSize) + " for " + g.title, "API");
    } else {
        logInfo("No preferred (.xci/.nsp) file found; bundle selection will use full files list.", "API");
    }

    return true;
}

bool fetchBinary(const Config& cfg, const std::string& url, std::string& outData, std::string& outError, ErrorInfo* outInfo) {
    if (outInfo) *outInfo = ErrorInfo{};
    std::string auth;
    if (!cfg.username.empty() || !cfg.password.empty()) {
        auth = romm::util::base64Encode(cfg.username + ":" + cfg.password);
    }
    HttpResponse resp;
    std::string err;
    std::vector<std::pair<std::string,std::string>> headers;
    headers.emplace_back("Accept", "*/*");
    if (!auth.empty()) headers.emplace_back("Authorization", "Basic " + auth);
    if (!httpRequest("GET", url, headers, cfg.httpTimeoutSeconds, resp, err)) {
        setApiError(outError, outInfo, err, ErrorCategory::Network);
        return false;
    }
    if (resp.statusCode < 200 || resp.statusCode >= 300) {
        setApiError(outError, outInfo,
                    "HTTP " + std::to_string(resp.statusCode) + (resp.statusText.empty() ? "" : (" " + resp.statusText)),
                    ErrorCategory::Http);
        return false;
    }
    outData.swap(resp.body);
    return true;
}

} // namespace romm
