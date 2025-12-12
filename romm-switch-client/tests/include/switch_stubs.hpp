#pragma once

// Minimal stubs to let api.cpp compile in UNIT_TEST mode on host.

#include <cstdint>

using Result = int;

inline void svcSleepThread(int64_t /*ns*/) {
    // No-op in tests.
}
