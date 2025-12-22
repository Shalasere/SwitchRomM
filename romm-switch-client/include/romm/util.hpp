#pragma once

#include <string>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include <cctype>

namespace romm::util {

inline std::string base64Encode(const std::string& in) {
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

inline std::string urlEncode(const std::string& in) {
    std::ostringstream oss;
    for (unsigned char c : in) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            oss << c;
        } else {
            oss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (int)c
                << std::nouppercase << std::dec;
        }
    }
    return oss.str();
}

inline std::string ellipsize(const std::string& s, size_t maxlen) {
    if (s.size() <= maxlen) return s;
    return s.substr(0, maxlen) + "...";
}

} // namespace romm::util
