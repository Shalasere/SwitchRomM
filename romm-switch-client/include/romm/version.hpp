#pragma once

namespace romm {

inline const char* appVersion() {
#ifdef ROMM_APP_VERSION
    return ROMM_APP_VERSION;
#else
    return "0.0.0";
#endif
}

} // namespace romm

