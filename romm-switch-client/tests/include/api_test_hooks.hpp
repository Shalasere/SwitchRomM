#pragma once

#include <string>

namespace romm {
// Helpers are defined in source/api.cpp (not exposed via a header in production).
bool parseHttpUrl(const std::string& url,
                  std::string& host,
                  std::string& port,
                  std::string& path,
                  std::string& err);

bool decodeChunkedBody(const std::string& body, std::string& decoded);
} // namespace romm
