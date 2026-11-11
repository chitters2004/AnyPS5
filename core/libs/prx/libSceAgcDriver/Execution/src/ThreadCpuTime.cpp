#include "prx/libSceAgcDriver/Execution/include/ThreadCpuTime.hpp"
#include <cstdint>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

namespace AgcDriver {

std::chrono::nanoseconds ThisThreadCpuTime() noexcept {
#ifdef _WIN32
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) == 0) return {};
    const auto ticks = [](FILETIME time) { return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32u) | time.dwLowDateTime; };
    return std::chrono::nanoseconds((ticks(kernel) + ticks(user)) * 100u);
#else
    timespec at{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &at) != 0) return {};
    return std::chrono::seconds(at.tv_sec) + std::chrono::nanoseconds(at.tv_nsec);
#endif
}

}
