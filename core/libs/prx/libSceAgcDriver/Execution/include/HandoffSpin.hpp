#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_HANDOFFSPIN_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_HANDOFFSPIN_HPP

#include "prx/libc/include/CpuTopology.hpp"
#include <algorithm>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include "prx/libc/include/PreciseWait.hpp"

namespace AgcDriver {

inline void SpinPause() {
    PreciseSpinPause();
}

inline unsigned UsableProcessors() {
    const auto count = static_cast<unsigned>(std::bitset<64>(CpuTopology::Get().process).count());
    return count != 0 ? count : std::thread::hardware_concurrency();
}

inline std::chrono::nanoseconds HandoffSpinWindowFor(const char* configured, unsigned usableProcessors) {
    constexpr std::chrono::microseconds automaticWindow{150};
    constexpr unsigned automaticProcessorsAbove = 8;
    constexpr long long longestWindowUs = 10000;
    if (configured != nullptr) return std::chrono::microseconds(std::clamp<long long>(std::strtoll(configured, nullptr, 10), 0, longestWindowUs));
    return usableProcessors > automaticProcessorsAbove ? std::chrono::nanoseconds(automaticWindow) : std::chrono::nanoseconds::zero();
}

inline std::chrono::nanoseconds HandoffSpinWindow() {
    static const std::chrono::nanoseconds window = HandoffSpinWindowFor(std::getenv("APS5_HANDOFF_SPIN_US"), UsableProcessors());
    return window;
}

template<typename TReady>
bool SpinUntil(std::chrono::nanoseconds window, TReady&& ready) {
    if (ready()) return true;
    if (window.count() <= 0) return false;
    const auto until = std::chrono::steady_clock::now() + window;
    do {
        SpinPause();
        if (ready()) return true;
    } while (std::chrono::steady_clock::now() < until);
    return false;
}

template<typename TMutex>
void LockSpinning(TMutex& mutex, std::chrono::nanoseconds window) {
    if (window.count() > 0 && SpinUntil(window, [&] { return mutex.try_lock(); })) return;
    mutex.lock();
}

struct HandoffSpin {
    bool waited = false;
    bool ready = true;
    std::chrono::steady_clock::time_point start{};
    std::chrono::steady_clock::time_point end{};
};

class HandoffSite {
public:
    HandoffSite(const char* name, std::chrono::nanoseconds cap) : name(name), cap(cap) {}
    HandoffSite(const HandoffSite&) = delete;
    HandoffSite& operator=(const HandoffSite&) = delete;

    const char* Name() const noexcept { return name; }
    std::chrono::nanoseconds Window() const noexcept { return cap; }

    template<typename TReady>
    HandoffSpin Spin(TReady&& ready) {
        if (ready()) return {};
        const auto window = Window();
        const auto start = std::chrono::steady_clock::now();
        if (window.count() <= 0) return {true, false, start, start};
        const auto deadline = start + window;
        auto now = start;
        do {
            SpinPause();
            if (ready()) return {true, true, start, std::chrono::steady_clock::now()};
            now = std::chrono::steady_clock::now();
        } while (now < deadline);
        return {true, false, start, now};
    }

private:
    const char* name;
    std::chrono::nanoseconds cap;
};

template<typename TMutex>
HandoffSpin LockSpinning(TMutex& mutex, HandoffSite& site) {
    const auto spin = site.Spin([&] { return mutex.try_lock(); });
    if (!spin.ready) mutex.lock();
    return spin;
}

}

#endif
