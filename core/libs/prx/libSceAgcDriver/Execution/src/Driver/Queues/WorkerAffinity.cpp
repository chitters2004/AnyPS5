#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/WorkerAffinity.hpp"
#include "prx/libSceAgcDriver/Execution/include/TracyZone.hpp"
#include "prx/libc/include/CpuTopology.hpp"
#include "prx/libc/include/general/ThreadName.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

namespace AgcDriver::DriverDetail {

std::uint64_t WorkerAffinityMask() {
    static const std::uint64_t mask = [] {
        if (std::getenv("APS5_NO_WORKER_AFFINITY") != nullptr) return std::uint64_t{0};
        const auto requested = CpuTopology::MaskFromEnvironment("APS5_WORKER_AFFINITY_MASK");
        if (requested != 0) return requested;
#ifndef __linux__
        if (std::getenv("APS5_WORKER_AFFINITY") == nullptr) return std::uint64_t{0};
#endif
        const auto& layout = CpuTopology::Get();
        return layout.hybrid ? layout.performant : std::uint64_t{0};
    }();
    return mask;
}

void NameDriverThread(const char* name) {
#ifdef _WIN32
    const HRESULT nameResult = ThreadName::SetCurrent(name);
    if (FAILED(nameResult)) std::fprintf(stderr, "[thread-name] could not name %s (0x%08lx)\n", name, static_cast<unsigned long>(nameResult));
#elif defined(__APPLE__)
    const int nameResult = pthread_setname_np(name);
    if (nameResult != 0) std::fprintf(stderr, "[thread-name] could not name %s (%s)\n", name, std::strerror(nameResult));
#else
    const int nameResult = pthread_setname_np(pthread_self(), name);
    if (nameResult != 0) std::fprintf(stderr, "[thread-name] could not name %s (%s)\n", name, std::strerror(nameResult));
#endif
}

std::uint64_t ThreadCpuNanos() {
#ifdef _WIN32
    FILETIME creation{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exited, &kernel, &user)) return 0;
    const auto ticks = [](const FILETIME& time) { return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32u) | time.dwLowDateTime; };
    return (ticks(kernel) + ticks(user)) * 100;
#else
    timespec now{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0) return 0;
    return static_cast<std::uint64_t>(now.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(now.tv_nsec);
#endif
}

void PinWorkerThread(const char* role) {
    TracyZone::NameThread(role);
    const auto mask = WorkerAffinityMask();
    if (mask == 0) return;
    static std::once_flag summary;
    std::call_once(summary, [mask] {
        const auto& layout = CpuTopology::Get();
        std::fprintf(stderr, "[affinity] queue workers and presenter -> 0x%llx (hybrid=%d efficient=0x%llx performant=0x%llx process=0x%llx)\n", static_cast<unsigned long long>(mask), layout.hybrid ? 1 : 0, static_cast<unsigned long long>(layout.efficient), static_cast<unsigned long long>(layout.performant), static_cast<unsigned long long>(layout.process));
    });
    CpuTopology::PinTraced(role, nullptr, mask);
}

}
