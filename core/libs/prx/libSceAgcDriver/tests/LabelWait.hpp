#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_LABELWAIT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_LABELWAIT_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace AgcDriverTests {

inline std::uint32_t LoadLabel(const volatile void* address) {
    return std::atomic_ref<std::uint32_t>(*static_cast<std::uint32_t*>(const_cast<void*>(address))).load(std::memory_order_acquire);
}

template <class Landed>
bool WaitForLabel(const volatile void* address, Landed&& landed, std::chrono::steady_clock::duration timeout, std::chrono::steady_clock::duration pause = {}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!landed(LoadLabel(address))) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        if (pause == std::chrono::steady_clock::duration{}) std::this_thread::yield();
        else std::this_thread::sleep_for(pause);
    }
    return true;
}

inline bool WaitForLabel(const volatile void* address, std::uint32_t value, std::chrono::steady_clock::duration timeout, std::chrono::steady_clock::duration pause = {}) {
    return WaitForLabel(address, [value](std::uint32_t word) { return word == value; }, timeout, pause);
}

}

#endif
