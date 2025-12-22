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
    resp = HttpResponse{};
    std::string host, portStr, path;
    if (!parseHttpUrl(url, host, portStr, path, err)) {
        return false;
    }

    struct addrinfo hints{};
    hints.ai_family   = AF_INET;
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

    // Set recv/send timeout
    if (timeoutSec > 0) {
        timeval tv{};
        tv.tv_sec  = timeoutSec;
        tv.tv_usec = 0;
        setsockopt(sockFd.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sockFd.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    if (connect(sockFd.fd, res->ai_addr, res->ai_addrlen) != 0) {
        err = "Connect failed";
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);

    // Build request
    std::ostringstream req;
    req << method << " " << path << " HTTP/1.1\r\n";
    req << "Host: " << host << "\r\n";
    req << "Connection: close\r\n";
    for (const auto& kv : extraHeaders) {
        req << kv.first << ": " << kv.second << "\r\n";
    }
    req << "\r\n";
    std::string reqStr = req.str();

    // Send all
    size_t totalSent = 0;
    while (totalSent < reqStr.size()) {
        ssize_t n = send(sockFd.fd, reqStr.data() + totalSent,
                         reqStr.size() - totalSent, 0);
        if (n <= 0) {
            err = "Send failed";
            return false;
        }
        totalSent += static_cast<size_t>(n);
    }

    // Receive response
    std::string raw;
    char buf[8192];
    while (true) {
        ssize_t n = recv(sockFd.fd, buf, sizeof(buf), 0);
        if (n > 0) {
            raw.append(buf, buf + n);
        } else if (n == 0) {
            break; // EOF
        } else {
            // timeout or error
            err = "Recv failed or timed out";
            return false;
        }
    }

    if (raw.empty()) {
        err = "Empty HTTP response";
        return false;
    }

    // Split headers/body
    auto hdrEnd = raw.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) {
        err = "Malformed HTTP response (no header/body separator)";
        return false;
    }
    std::string headerBlock = raw.substr(0, hdrEnd);
    std::string body        = raw.substr(hdrEnd + 4);

    // Parse status line
    auto firstCRLF = headerBlock.find("\r\n");
    if (firstCRLF == std::string::npos) {
        err = "Malformed HTTP response (no status line CRLF)";
        return false;
    }
    std::string statusLine = headerBlock.substr(0, firstCRLF);
    std::istringstream sl(statusLine);
    std::string httpVer;
    sl >> httpVer;
    sl >> resp.statusCode;
    std::getline(sl, resp.statusText);
    if (!resp.statusText.empty() && resp.statusText.front() == ' ')
        resp.statusText.erase(resp.statusText.begin());

    resp.headersRaw = headerBlock.substr(firstCRLF + 2); // everything after status line

    // Detect chunked encoding
    std::string headersLower = resp.headersRaw;
    for (auto& c : headersLower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    bool chunked = (headersLower.find("transfer-encoding: chunked") != std::string::npos);

    if (chunked) {
        std::string decoded;
        if (!decodeChunkedBody(body, decoded)) {
            err = "Failed to decode chunked HTTP body";
            return false;
        }
        resp.body.swap(decoded);
    } else {
        resp.body.swap(body);
    }

    return true;
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
    hints.ai_family   = AF_INET;
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

// Simple retry wrapper for JSON GET requests.
// Retries on transport errors/timeouts, not on HTTP 4xx.
static bool httpGetJsonWithRetry(const std::string& url,
                                 const std::string& authBasic,
                                 int timeoutSec,
                                 HttpResponse& resp,
                                 std::string& err)
{
    const int maxAttempts = 3;
    std::string lastErr;

    std::vector<std::pair<std::string,std::string>> headers;
    headers.emplace_back("Accept", "application/json");
    if (!authBasic.empty()) {
        headers.emplace_back("Authorization", "Basic " + authBasic);
    }

    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        HttpResponse r;
        std::string e;
        if (httpRequest("GET", url, headers, timeoutSec, r, e)) {
            // Got a response, even if it's 4xx/5xx
            resp = std::move(r);
            if (resp.statusCode >= 200 && resp.statusCode < 300) {
                return true;
            }
            // Non-2xx: don't retry, bubble up error and snippet.
            err = "HTTP " + std::to_string(resp.statusCode) +
                  (resp.statusText.empty() ? "" : (" " + resp.statusText));
            if (!resp.body.empty()) {
                err += " body: " + resp.body.substr(0, resp.body.size() > 256 ? 256 : resp.body.size());
            }
            return false;
        }

        lastErr = e.empty() ? "HTTP transport failure" : e;
        if (attempt < maxAttempts) {
            // basic backoff: 250ms, 1s
            int64_t delayNs = (attempt == 1) ? kRetryDelayFastNs : kRetryDelaySlowNs;
            svcSleepThread(delayNs);
        }
    }

    err = "HTTP request failed after retries: " + lastErr;
    return false;
}

static std::string valToString(const mini::Value& v) {
    if (v.type == mini::Value::Type::String) return v.str;
    if (v.type == mini::Value::Type::Number) return std::to_string(v.number);
    return {};
}

static std::string sanitizeAscii(const std::string& s, size_t maxlen = 64) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c >= 32 && c < 127)
            out.push_back(static_cast<char>(c));
        else
            out.push_back('?');
        if (out.size() >= maxlen) break;
    }
    if (out.size() < s.size()) out.append("...");
    return out;
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

static bool parseGames(const std::string& body,
                       const std::string& platformId,
                       Status& status,
                       const std::string& serverUrl,
                       std::string& err)
{
    auto encodeSpaces = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == ' ')
                out += "%20";
            else
                out.push_back(c);
        }
        return out;
    };
    mini::Array arr;
    mini::Object obj;
    if (!mini::parse(body, arr)) {
        if (mini::parse(body, obj)) {
            auto it = obj.find("items");
            if (it != obj.end() && it->second.type == mini::Value::Type::Array) {
                arr = it->second.array;
            } else {
                err = "Games JSON missing items array";
                return false;
            }
        } else {
            err = "Failed to parse games JSON";
            return false;
        }
    }

    status.roms.clear();
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
            if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) return encodeSpaces(url);
            if (!serverUrl.empty() && url.front() == '/') {
                if (serverUrl.back() == '/')
                    return encodeSpaces(serverUrl.substr(0, serverUrl.size() - 1) + url);
                return encodeSpaces(serverUrl + url);
            }
            return encodeSpaces(url);
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

        if (auto it = o.find("platform_slug"); it != o.end() && it->second.type == mini::Value::Type::String)
            g.platformSlug = it->second.str;

        if (auto it = o.find("path_cover_small"); it != o.end() && it->second.type == mini::Value::Type::String)
            g.coverUrl = absolutizeUrl(it->second.str);
        else if (auto it1 = o.find("cover_url"); it1 != o.end() && it1->second.type == mini::Value::Type::String)
            g.coverUrl = absolutizeUrl(it1->second.str);
        else if (auto it2 = o.find("assets"); it2 != o.end() && it2->second.type == mini::Value::Type::Object) {
            auto cov = it2->second.object.find("cover");
            if (cov != it2->second.object.end() && cov->second.type == mini::Value::Type::String)
                g.coverUrl = absolutizeUrl(cov->second.str);
        }

        g.platformId = platformId;

        if (!g.id.empty()) {
            if (!g.title.empty())
                g.title = sanitizeAscii(g.title, 80);
            status.roms.push_back(g);
        }
    }

    status.romsReady = true;

    if (!status.roms.empty()) {
        std::string first  = sanitizeAscii(status.roms[0].title);
        std::string second = status.roms.size() > 1 ? sanitizeAscii(status.roms[1].title) : "";
        std::string third  = status.roms.size() > 2 ? sanitizeAscii(status.roms[2].title) : "";
        logLine("Parsed ROMs: " + std::to_string(status.roms.size()) + " first=" + first);
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
    Status st;
    bool ok = parseGames(body, platformId, st, serverUrl, err);
    outGames = st.roms;
    return ok;
}
#endif

bool fetchPlatforms(const Config& cfg, Status& status, std::string& outError) {
    std::string auth;
    if (!cfg.username.empty() || !cfg.password.empty()) {
        auth = romm::util::base64Encode(cfg.username + ":" + cfg.password);
    }

    HttpResponse resp;
    std::string err;
    std::string url = cfg.serverUrl + "/api/platforms";

    if (!httpGetJsonWithRetry(url, auth, cfg.httpTimeoutSeconds, resp, err)) {
        outError = err;
        return false;
    }

    if (!parsePlatforms(resp.body, status, err)) {
        outError = err;
        return false;
    }

    logLine("API: fetched platforms (" + std::to_string(status.platforms.size()) + ")");
    return true;
}

bool fetchGamesForPlatform(const Config& cfg,
                           const std::string& platformId,
                           Status& status,
                           std::string& outError)
{
    std::string auth;
    if (!cfg.username.empty() || !cfg.password.empty()) {
        auth = romm::util::base64Encode(cfg.username + ":" + cfg.password);
    }

    HttpResponse resp;
    std::string err;
    std::string url = cfg.serverUrl +
                      "/api/roms?platform_id=" + platformId +
                      "&order_by=name&order_dir=asc&limit=10000";

    if (!httpGetJsonWithRetry(url, auth, cfg.httpTimeoutSeconds, resp, err)) {
        outError = err;
        return false;
    }

    if (!parseGames(resp.body, platformId, status, cfg.serverUrl, err)) {
        romm::logLine("ROMs response (first 256 bytes): " +
                      resp.body.substr(0, resp.body.size() > 256 ? 256 : resp.body.size()));
        outError = err;
        return false;
    }

    logLine("API: fetched games (" + std::to_string(status.roms.size()) +
            ") for platform " + platformId);
    return true;
}

bool enrichGameWithFiles(const Config& cfg, Game& g, std::string& outError) {
    if (g.id.empty()) {
        outError = "Game missing id; cannot fetch files.";
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
        outError = err;
        return false;
    }

    mini::Object obj;
    if (!mini::parse(resp.body, obj)) {
        outError = "Failed to parse DetailedRom JSON";
        return false;
    }

    auto encodeSpaces = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == ' ')
                out += "%20";
            else
                out.push_back(c);
        }
        return out;
    };

    auto absolutizeUrl = [&](const std::string& url) -> std::string {
        if (url.empty()) return url;
        if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) return encodeSpaces(url);
        if (!cfg.serverUrl.empty() && url.front() == '/') {
            if (cfg.serverUrl.back() == '/')
                return encodeSpaces(cfg.serverUrl.substr(0, cfg.serverUrl.size() - 1) + url);
            return encodeSpaces(cfg.serverUrl + url);
        }
        return encodeSpaces(url);
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
        outError = "DetailedRom has no files array";
        return false;
    }

    uint64_t bestSize = 0;
    std::string bestName;
    std::string bestId;
    std::string bestDownloadUrl;

    auto stripLeadingSlash = [](std::string s) {
        while (!s.empty() && s.front() == '/') s.erase(s.begin());
        return s;
    };

    int fileCount = 0;
    for (auto& f : itf->second.array) {
        if (f.type != mini::Value::Type::Object) continue;
        const auto& fo = f.object;
        ++fileCount;

        std::string fname;
        std::string fid;
        uint64_t fsize = 0;

        // File name: prefer RomM schema fields
        if (auto n = fo.find("file_name"); n != fo.end() && n->second.type == mini::Value::Type::String)
            fname = n->second.str;
        else if (auto n2 = fo.find("name"); n2 != fo.end() && n2->second.type == mini::Value::Type::String)
            fname = n2->second.str;
        else if (auto n3 = fo.find("full_path"); n3 != fo.end() && n3->second.type == mini::Value::Type::String)
            fname = n3->second.str;
        fname = stripLeadingSlash(fname);

        auto idit = fo.find("id");
        if (idit != fo.end()) {
            if (idit->second.type == mini::Value::Type::Number)
                fid = std::to_string(static_cast<uint64_t>(idit->second.number));
            else if (idit->second.type == mini::Value::Type::String)
                fid = idit->second.str;
        }
        std::string downloadUrlField;
        if (auto dl = fo.find("download_url"); dl != fo.end() && dl->second.type == mini::Value::Type::String) {
            downloadUrlField = dl->second.str;
        }

        // Size: prefer file_size_bytes
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

        if (fname.empty() || fid.empty()) continue;

        std::string lower = fname;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        bool preferredExt = (lower.rfind(".xci") != std::string::npos) || (lower.rfind(".nsp") != std::string::npos);
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
            bestDownloadUrl = downloadUrlField;
        }

        logDebug("files[] entry: name=" + fname + " id=" + fid +
                 " size=" + std::to_string(fsize), "API");
    }

    if (bestId.empty() || bestName.empty()) {
        outError = "No suitable file (.xci/.nsp) in files array (" + std::to_string(fileCount) + " entries)";
        return false;
    }

    g.fsName = bestName;
    g.fileId = bestId;
    if (bestSize > 0) g.sizeBytes = bestSize;
    if (!bestDownloadUrl.empty()) {
        g.downloadUrl = bestDownloadUrl;
        logInfo("Using download_url field for " + g.title, "API");
    } else {
        g.downloadUrl = cfg.serverUrl + "/api/roms/" + g.id +
                        "/content/" + romm::util::urlEncode(bestName) +
                        "?file_ids=" + bestId;
        logInfo("Constructed download URL for " + g.title, "API");
    }

    logInfo("Selected file via files[] id=" + bestId + " name=" + bestName +
            " size=" + std::to_string(bestSize) + " for " + g.title, "API");
    return true;
}

bool fetchBinary(const Config& cfg, const std::string& url, std::string& outData, std::string& outError) {
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
        outError = err;
        return false;
    }
    if (resp.statusCode < 200 || resp.statusCode >= 300) {
        outError = "HTTP " + std::to_string(resp.statusCode) + (resp.statusText.empty() ? "" : (" " + resp.statusText));
        return false;
    }
    outData.swap(resp.body);
    return true;
}

} // namespace romm
