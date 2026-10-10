#include "protocol/TimeUtil.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

namespace p2p {

uint64_t monotonicMs() {
#ifdef _WIN32
    return GetTickCount64();
#else
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000ull + static_cast<uint64_t>(ts.tv_nsec / 1000000ull);
#endif
}

}  // namespace p2p
