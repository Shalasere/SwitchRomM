#include "romm/http_common.hpp"
#include <cerrno>
#include <sstream>
#include <cctype>
#include <algorithm>
#include <cstdlib>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#endif

namespace romm {

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
    std::getline(sl, out.statusText);
    if (!out.statusText.empty() && out.statusText.front() == ' ') out.statusText.erase(out.statusText.begin());

    std::istringstream hs(headerBlock);
    std::string line;
    std::getline(hs, line); // discard status line
    std::ostringstream raw;
    bool firstHeader = true;
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
        std::string keyLower = key;
        for (auto& c : keyLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string valLower = val;
        for (auto& c : valLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (keyLower == "content-length") {
            out.contentLength = static_cast<uint64_t>(std::strtoull(val.c_str(), nullptr, 10));
        } else if (keyLower == "transfer-encoding" && valLower.find("chunked") != std::string::npos) {
            out.chunked = true;
        } else if (keyLower == "accept-ranges" && valLower.find("bytes") != std::string::npos) {
            out.acceptRanges = true;
        } else if (keyLower == "location") {
            out.location = val;
        }
    }
    out.headersRaw = raw.str();
    return true;
}

} // namespace romm
