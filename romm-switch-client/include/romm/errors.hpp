#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace romm {

enum class ErrorCategory {
    None,
    Config,
    Network,
    Auth,
    Http,
    Parse,
    Filesystem,
    Data,
    Unsupported,
    Internal
};

enum class ErrorCode {
    None,
    Unknown,
    ConfigMissing,
    ConfigInvalid,
    ConfigUnsupported,
    MissingRequiredField,
    TransportFailure,
    Timeout,
    DnsFailure,
    ConnectFailure,
    HttpStatus,
    HttpUnauthorized,
    HttpForbidden,
    HttpNotFound,
    ParseFailure,
    UnsupportedFeature,
    InvalidData
};

struct ErrorInfo {
    ErrorCategory category{ErrorCategory::None};
    ErrorCode code{ErrorCode::None};
    int httpStatus{0};
    bool retryable{false};
    std::string userMessage;
    std::string detail;
};

inline const char* errorCategoryLabel(ErrorCategory c) {
    switch (c) {
        case ErrorCategory::None: return "None";
        case ErrorCategory::Config: return "Config";
        case ErrorCategory::Network: return "Network";
        case ErrorCategory::Auth: return "Auth";
        case ErrorCategory::Http: return "HTTP";
        case ErrorCategory::Parse: return "Parse";
        case ErrorCategory::Filesystem: return "Filesystem";
        case ErrorCategory::Data: return "Data";
        case ErrorCategory::Unsupported: return "Unsupported";
        case ErrorCategory::Internal: return "Internal";
        default: return "Unknown";
    }
}

inline const char* errorCodeLabel(ErrorCode c) {
    switch (c) {
        case ErrorCode::None: return "None";
        case ErrorCode::Unknown: return "Unknown";
        case ErrorCode::ConfigMissing: return "ConfigMissing";
        case ErrorCode::ConfigInvalid: return "ConfigInvalid";
        case ErrorCode::ConfigUnsupported: return "ConfigUnsupported";
        case ErrorCode::MissingRequiredField: return "MissingRequiredField";
        case ErrorCode::TransportFailure: return "TransportFailure";
        case ErrorCode::Timeout: return "Timeout";
        case ErrorCode::DnsFailure: return "DnsFailure";
        case ErrorCode::ConnectFailure: return "ConnectFailure";
        case ErrorCode::HttpStatus: return "HttpStatus";
        case ErrorCode::HttpUnauthorized: return "HttpUnauthorized";
        case ErrorCode::HttpForbidden: return "HttpForbidden";
        case ErrorCode::HttpNotFound: return "HttpNotFound";
        case ErrorCode::ParseFailure: return "ParseFailure";
        case ErrorCode::UnsupportedFeature: return "UnsupportedFeature";
        case ErrorCode::InvalidData: return "InvalidData";
        default: return "Unknown";
    }
}

inline std::string toLowerCopy(const std::string& s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return out;
}

inline int parseHttpStatusFromMessage(const std::string& msg) {
    // Accept simple forms like "HTTP 401 ..." or "(HTTP 404)".
    auto pos = msg.find("HTTP ");
    if (pos == std::string::npos) pos = msg.find("HTTP");
    if (pos == std::string::npos) return 0;
    pos = msg.find_first_of("0123456789", pos);
    if (pos == std::string::npos) return 0;
    int code = 0;
    int digits = 0;
    while (pos < msg.size() && std::isdigit(static_cast<unsigned char>(msg[pos])) && digits < 3) {
        code = code * 10 + (msg[pos] - '0');
        ++pos;
        ++digits;
    }
    return digits == 3 ? code : 0;
}

inline ErrorInfo classifyError(const std::string& detail, ErrorCategory hint = ErrorCategory::None) {
    ErrorInfo out;
    out.detail = detail;
    out.category = hint;
    out.code = ErrorCode::Unknown;

    const std::string l = toLowerCopy(detail);
    const int http = parseHttpStatusFromMessage(detail);
    if (http > 0) out.httpStatus = http;

    auto set = [&](ErrorCategory cat, ErrorCode code, const char* user, bool retryable) {
        out.category = cat;
        out.code = code;
        out.userMessage = user;
        out.retryable = retryable;
    };

    if (l.find("missing config") != std::string::npos) {
        set(ErrorCategory::Config, ErrorCode::ConfigMissing, "Configuration file is missing.", false);
    } else if (l.find("invalid config json") != std::string::npos || l.find("failed to parse env") != std::string::npos) {
        set(ErrorCategory::Config, ErrorCode::ConfigInvalid, "Configuration format is invalid.", false);
    } else if (l.find("missing server_url") != std::string::npos || l.find("missing platform id") != std::string::npos) {
        set(hint == ErrorCategory::None ? ErrorCategory::Config : hint, ErrorCode::MissingRequiredField, "Required setting or field is missing.", false);
    } else if (l.find("https:// not supported") != std::string::npos || l.find("tls not implemented") != std::string::npos ||
               l.find("not supported") != std::string::npos || l.find("chunked transfer not supported") != std::string::npos) {
        set(ErrorCategory::Unsupported, ErrorCode::UnsupportedFeature, "This feature is not supported yet.", false);
    } else if (http == 401) {
        set(ErrorCategory::Auth, ErrorCode::HttpUnauthorized, "Authentication failed (401).", false);
    } else if (http == 403) {
        set(ErrorCategory::Auth, ErrorCode::HttpForbidden, "Access denied (403).", false);
    } else if (http == 404) {
        set(ErrorCategory::Http, ErrorCode::HttpNotFound, "Requested resource was not found (404).", false);
    } else if (http >= 400 && http < 600) {
        set(ErrorCategory::Http, ErrorCode::HttpStatus, "Server returned an HTTP error.", http >= 500);
    } else if (l.find("dns") != std::string::npos || l.find("resolve") != std::string::npos) {
        set(ErrorCategory::Network, ErrorCode::DnsFailure, "DNS lookup failed.", true);
    } else if (l.find("connect failed") != std::string::npos || l.find("socket") != std::string::npos) {
        set(ErrorCategory::Network, ErrorCode::ConnectFailure, "Failed to connect to server.", true);
    } else if (l.find("timeout") != std::string::npos || l.find("timed out") != std::string::npos) {
        set(ErrorCategory::Network, ErrorCode::Timeout, "Network operation timed out.", true);
    } else if (l.find("recv failed") != std::string::npos || l.find("send failed") != std::string::npos ||
               l.find("transport") != std::string::npos || l.find("http request failed") != std::string::npos) {
        set(ErrorCategory::Network, ErrorCode::TransportFailure, "Network transport failed.", true);
    } else if (l.find("parse") != std::string::npos || l.find("malformed") != std::string::npos || l.find("json") != std::string::npos) {
        set(ErrorCategory::Parse, ErrorCode::ParseFailure, "Received malformed data.", false);
    } else if (l.find("write failed") != std::string::npos || l.find("open part failed") != std::string::npos ||
               l.find("seek failed") != std::string::npos) {
        set(ErrorCategory::Filesystem, ErrorCode::InvalidData, "Failed to write to storage.", true);
    } else if (l.find("no valid files") != std::string::npos || l.find("missing id") != std::string::npos) {
        set(ErrorCategory::Data, ErrorCode::InvalidData, "Server data is incomplete for this ROM.", false);
    }

    // Fill any missing defaults.
    if (out.category == ErrorCategory::None) out.category = hint == ErrorCategory::None ? ErrorCategory::Internal : hint;
    if (out.userMessage.empty()) {
        switch (out.category) {
            case ErrorCategory::Config: out.userMessage = "Configuration error."; break;
            case ErrorCategory::Network: out.userMessage = "Network error."; out.retryable = true; break;
            case ErrorCategory::Auth: out.userMessage = "Authentication/permission error."; break;
            case ErrorCategory::Http: out.userMessage = "Server returned an error."; break;
            case ErrorCategory::Parse: out.userMessage = "Data parsing error."; break;
            case ErrorCategory::Filesystem: out.userMessage = "Storage error."; out.retryable = true; break;
            case ErrorCategory::Data: out.userMessage = "Invalid server data."; break;
            case ErrorCategory::Unsupported: out.userMessage = "Unsupported feature."; break;
            case ErrorCategory::Internal: out.userMessage = "Internal application error."; break;
            default: out.userMessage = "Unknown error."; break;
        }
    }

    return out;
}

} // namespace romm
