#pragma once

#include <string>
#include <cstdint>

namespace romm {

struct Config;
struct Status;

// Run a speed test against cfg.speedTestUrl (if set) up to testBytes; updates Status::lastSpeedMbps.
// Returns true on success; on failure outError is populated.
bool runSpeedTest(const Config& cfg, Status& status, uint64_t testBytes, std::string& outError);

} // namespace romm
