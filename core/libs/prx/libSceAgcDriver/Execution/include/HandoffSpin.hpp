#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_HANDOFFSPIN_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_HANDOFFSPIN_HPP

#include "prx/libc/include/CpuTopology.hpp"
#include <algorithm>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

inline bool HandoffAdaptiveFor(const char* configured) {
    return configured == nullptr || std::strcmp(configured, "0") != 0;
}

inline bool HandoffAdaptive() {
    static const bool adaptive = HandoffAdaptiveFor(std::getenv("APS5_HANDOFF_ADAPTIVE"));
    return adaptive;
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
    static constexpr std::chrono::nanoseconds Floor{1000};
    static constexpr std::uint32_t ProbeEvery = 32;
    static constexpr std::int64_t Reach = 4;

    HandoffSite(const char* name, std::chrono::nanoseconds cap, bool adaptive = HandoffAdaptive()) : name(name), cap(cap), adaptive(adaptive), window(cap.count()) {}
    HandoffSite(const HandoffSite&) = delete;
    HandoffSite& operator=(const HandoffSite&) = delete;

    const char* Name() const noexcept { return name; }
    bool Adaptive() const noexcept { return adaptive; }
    std::chrono::nanoseconds Window() const noexcept { return adaptive ? std::chrono::nanoseconds(window.load(std::memory_order_relaxed)) : cap; }

    void Learn(bool caught, std::chrono::nanoseconds spun) noexcept {
        if (!adaptive || cap.count() <= 0) return;
        if (caught) {
            const auto previous = typical.load(std::memory_order_relaxed);
            const auto next = previous == 0 ? spun.count() : previous + (spun.count() - previous) / 8;
            typical.store(std::max<std::int64_t>(next, 1), std::memory_order_relaxed);
            window.store(reach(next), std::memory_order_relaxed);
            return;
        }
        const auto half = window.load(std::memory_order_relaxed) / 2;
        window.store(half < Floor.count() ? 0 : half, std::memory_order_relaxed);
    }

    template<typename TReady>
    HandoffSpin Spin(TReady&& ready) {
        if (ready()) return {};
        auto limit = Window();
        if (adaptive && limit.count() <= 0 && cap.count() > 0 && probes.fetch_add(1, std::memory_order_relaxed) % ProbeEvery == ProbeEvery - 1) {
            const auto known = typical.load(std::memory_order_relaxed);
            limit = std::chrono::nanoseconds(known != 0 ? reach(known) : cap.count() / Reach);
        }
        const auto start = std::chrono::steady_clock::now();
        if (limit.count() <= 0) return {true, false, start, start};
        const auto deadline = start + limit;
        auto now = start;
        do {
            SpinPause();
            if (ready()) {
                now = std::chrono::steady_clock::now();
                Learn(true, now - start);
                return {true, true, start, now};
            }
            now = std::chrono::steady_clock::now();
        } while (now < deadline);
        Learn(false, now - start);
        return {true, false, start, now};
    }

private:
    std::int64_t reach(std::int64_t spun) const noexcept {
        return std::clamp<std::int64_t>(spun * Reach, Floor.count(), cap.count());
    }

    const char* name;
    std::chrono::nanoseconds cap;
    bool adaptive;
    std::atomic<std::int64_t> window;
    std::atomic<std::int64_t> typical{0};
    std::atomic<std::uint32_t> probes{0};
};

template<typename TMutex>
HandoffSpin LockSpinning(TMutex& mutex, HandoffSite& site) {
    const auto spin = site.Spin([&] { return mutex.try_lock(); });
    if (!spin.ready) mutex.lock();
    return spin;
}

}

#endif
