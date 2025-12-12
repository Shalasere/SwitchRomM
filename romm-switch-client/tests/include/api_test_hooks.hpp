#pragma once

#include <string>

namespace romm::detail {
bool parseHttpUrlTest(const std::string& url,
                      std::string& host,
                      std::string& port,
                      std::string& path,
                      std::string& err);

bool decodeChunkedBodyTest(const std::string& body, std::string& decoded);
} // namespace romm::detail
