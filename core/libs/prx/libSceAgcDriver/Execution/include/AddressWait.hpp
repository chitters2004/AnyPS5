#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ADDRESSWAIT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ADDRESSWAIT_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#elif defined(__APPLE__) && __has_include(<os/os_sync_wait_on_address.h>)
#include <os/clock.h>
#include <os/os_sync_wait_on_address.h>
#endif

namespace AgcDriver {

template<typename TValue>
concept AddressWord = sizeof(TValue) == 4 && std::is_trivially_copyable_v<TValue> && std::atomic<TValue>::is_always_lock_free && sizeof(std::atomic<TValue>) == sizeof(TValue);

namespace AddressWaitDetail {

using Clock = std::chrono::steady_clock;

template<AddressWord TValue>
std::uint32_t Bits(TValue value) noexcept {
    return std::bit_cast<std::uint32_t>(value);
}

template<AddressWord TValue>
void* Address(const std::atomic<TValue>& word) noexcept {
    return const_cast<void*>(static_cast<const volatile void*>(&word));
}

inline void Pause() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

#if defined(_WIN32)

inline constexpr bool Native = true;
inline constexpr std::uint32_t Forever = 0xffffffffu;

void WaitOnWord(void* address, const void* old, std::size_t bytes, std::uint32_t milliseconds) noexcept;
void WakeOneOnWord(void* address) noexcept;
void WakeAllOnWord(void* address) noexcept;

inline std::uint32_t Milliseconds(Clock::duration remaining) noexcept {
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
    return static_cast<std::uint32_t>(std::clamp<long long>(ms, 1, static_cast<long long>(Forever) - 1));
}

template<AddressWord TValue>
void Wait(const std::atomic<TValue>& word, TValue old) noexcept {
    WaitOnWord(Address(word), &old, sizeof(TValue), Forever);
}

template<AddressWord TValue>
void WaitFor(const std::atomic<TValue>& word, TValue old, Clock::duration remaining) noexcept {
    WaitOnWord(Address(word), &old, sizeof(TValue), Milliseconds(remaining));
}

template<AddressWord TValue>
void WakeOne(const std::atomic<TValue>& word) noexcept {
    WakeOneOnWord(Address(word));
}

template<AddressWord TValue>
void WakeAll(const std::atomic<TValue>& word) noexcept {
    WakeAllOnWord(Address(word));
}

#elif defined(__linux__)

inline constexpr bool Native = true;

static_assert(std::endian::native == std::endian::little);

inline void Futex(void* address, int operation, std::uint32_t value, const timespec* timeout) noexcept {
    syscall(SYS_futex, address, operation, value, timeout, nullptr, 0);
}

template<AddressWord TValue>
void Wait(const std::atomic<TValue>& word, TValue old) noexcept {
    Futex(Address(word), FUTEX_WAIT_PRIVATE, Bits(old), nullptr);
}

template<AddressWord TValue>
void WaitFor(const std::atomic<TValue>& word, TValue old, Clock::duration remaining) noexcept {
    const auto ns = std::max<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count(), 1);
    const timespec timeout{static_cast<time_t>(ns / 1000000000), static_cast<long>(ns % 1000000000)};
    Futex(Address(word), FUTEX_WAIT_PRIVATE, Bits(old), &timeout);
}

template<AddressWord TValue>
void WakeOne(const std::atomic<TValue>& word) noexcept {
    Futex(Address(word), FUTEX_WAKE_PRIVATE, 1, nullptr);
}

template<AddressWord TValue>
void WakeAll(const std::atomic<TValue>& word) noexcept {
    Futex(Address(word), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr);
}

#elif defined(__APPLE__) && __has_include(<os/os_sync_wait_on_address.h>)

inline bool NativeAvailable() noexcept {
    if (__builtin_available(macOS 14.4, *)) return true;
    return false;
}

inline const bool Native = NativeAvailable();

template<AddressWord TValue>
void Wait(const std::atomic<TValue>& word, TValue old) noexcept {
    if (__builtin_available(macOS 14.4, *)) {
        os_sync_wait_on_address(Address(word), Bits(old), sizeof(TValue), OS_SYNC_WAIT_ON_ADDRESS_NONE);
        return;
    }
    word.wait(old, std::memory_order_relaxed);
}

template<AddressWord TValue>
void WaitFor(const std::atomic<TValue>& word, TValue old, Clock::duration remaining) noexcept {
    if (__builtin_available(macOS 14.4, *)) {
        const auto ns = std::max<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count(), 1);
        os_sync_wait_on_address_with_timeout(Address(word), Bits(old), sizeof(TValue), OS_SYNC_WAIT_ON_ADDRESS_NONE, OS_CLOCK_MACH_ABSOLUTE_TIME, static_cast<std::uint64_t>(ns));
        return;
    }
    std::this_thread::sleep_for(std::min<Clock::duration>(remaining, std::chrono::microseconds(100)));
}

template<AddressWord TValue>
void WakeOne(const std::atomic<TValue>& word) noexcept {
    if (__builtin_available(macOS 14.4, *)) {
        os_sync_wake_by_address_any(Address(word), sizeof(TValue), OS_SYNC_WAKE_BY_ADDRESS_NONE);
        return;
    }
    const_cast<std::atomic<TValue>&>(word).notify_one();
}

template<AddressWord TValue>
void WakeAll(const std::atomic<TValue>& word) noexcept {
    if (__builtin_available(macOS 14.4, *)) {
        os_sync_wake_by_address_all(Address(word), sizeof(TValue), OS_SYNC_WAKE_BY_ADDRESS_NONE);
        return;
    }
    const_cast<std::atomic<TValue>&>(word).notify_all();
}

#else

inline constexpr bool Native = false;

template<AddressWord TValue>
void Wait(const std::atomic<TValue>& word, TValue old) noexcept {
    word.wait(old, std::memory_order_relaxed);
}

template<AddressWord TValue>
void WaitFor(const std::atomic<TValue>&, TValue, Clock::duration remaining) noexcept {
    std::this_thread::sleep_for(std::min<Clock::duration>(remaining, std::chrono::microseconds(100)));
}

template<AddressWord TValue>
void WakeOne(const std::atomic<TValue>& word) noexcept {
    const_cast<std::atomic<TValue>&>(word).notify_one();
}

template<AddressWord TValue>
void WakeAll(const std::atomic<TValue>& word) noexcept {
    const_cast<std::atomic<TValue>&>(word).notify_all();
}

#endif

struct alignas(64) ParkedBucket {
    std::mutex mutex;
    std::condition_variable parked;
};

inline ParkedBucket& ParkedBucketOf(const void* address) noexcept {
    static auto* const buckets = new std::array<ParkedBucket, 64>;
    const auto bits = reinterpret_cast<std::uintptr_t>(address);
    return (*buckets)[(bits >> 2 ^ bits >> 8) % buckets->size()];
}

inline bool ParkedOff() noexcept {
    static const bool off = [] {
        const char* value = std::getenv("APS5_NO_NATIVE_WAITS");
        return value != nullptr && std::strcmp(value, "0") != 0;
    }();
    return off;
}

inline void ParkedWait(const void* address, bool (*unchanged)(const void* address, std::uint32_t old), std::uint32_t old, const Clock::time_point* deadline) noexcept {
    auto& bucket = ParkedBucketOf(address);
    std::unique_lock lock(bucket.mutex);
    if (!unchanged(address, old)) return;
    if (deadline == nullptr) {
        bucket.parked.wait(lock);
    } else {
        bucket.parked.wait_until(lock, *deadline);
    }
}

inline void ParkedWake(const void* address) noexcept {
    auto& bucket = ParkedBucketOf(address);
    { std::lock_guard lock(bucket.mutex); }
    bucket.parked.notify_all();
}

template<AddressWord TValue>
bool Unchanged(const void* address, std::uint32_t old) {
    return Bits(static_cast<const std::atomic<TValue>*>(address)->load(std::memory_order_seq_cst)) == old;
}

}

inline bool NativeWaitsOff() noexcept {
    return AddressWaitDetail::ParkedOff();
}

inline bool NativeAddressWait() noexcept {
    return AddressWaitDetail::Native && !NativeWaitsOff();
}

inline constexpr int AddressWaitSpins = 64;

template<AddressWord TValue>
void AddressWait(const std::atomic<TValue>& word, std::type_identity_t<TValue> old) noexcept {
    if (NativeWaitsOff()) {
        AddressWaitDetail::ParkedWait(&word, &AddressWaitDetail::Unchanged<TValue>, AddressWaitDetail::Bits(old), nullptr);
        return;
    }
    for (int spin = 0; spin < AddressWaitSpins; ++spin) {
        if (AddressWaitDetail::Bits(word.load(std::memory_order_relaxed)) != AddressWaitDetail::Bits(old)) return;
        AddressWaitDetail::Pause();
    }
    if (AddressWaitDetail::Bits(word.load(std::memory_order_relaxed)) != AddressWaitDetail::Bits(old)) return;
    AddressWaitDetail::Wait(word, old);
}

template<AddressWord TValue>
bool AddressWaitUntil(const std::atomic<TValue>& word, std::type_identity_t<TValue> old, std::chrono::steady_clock::time_point deadline) noexcept {
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) return false;
    if (AddressWaitDetail::Bits(word.load(std::memory_order_relaxed)) != AddressWaitDetail::Bits(old)) return true;
    if (NativeWaitsOff()) {
        AddressWaitDetail::ParkedWait(&word, &AddressWaitDetail::Unchanged<TValue>, AddressWaitDetail::Bits(old), &deadline);
        return std::chrono::steady_clock::now() < deadline;
    }
    AddressWaitDetail::WaitFor(word, old, remaining);
    return std::chrono::steady_clock::now() < deadline;
}

template<AddressWord TValue, typename TRep, typename TPeriod>
bool AddressWaitFor(const std::atomic<TValue>& word, std::type_identity_t<TValue> old, std::chrono::duration<TRep, TPeriod> timeout) noexcept {
    return AddressWaitUntil(word, old, std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout));
}

template<AddressWord TValue>
void AddressWakeOne(const std::atomic<TValue>& word) noexcept {
    if (NativeWaitsOff()) {
        AddressWaitDetail::ParkedWake(&word);
        return;
    }
    AddressWaitDetail::WakeOne(word);
}

template<AddressWord TValue>
void AddressWakeAll(const std::atomic<TValue>& word) noexcept {
    if (NativeWaitsOff()) {
        AddressWaitDetail::ParkedWake(&word);
        return;
    }
    AddressWaitDetail::WakeAll(word);
}

class AddressCondition {
public:
    AddressCondition() = default;
    AddressCondition(const AddressCondition&) = delete;
    AddressCondition& operator=(const AddressCondition&) = delete;

    template<typename TLock, typename TReady>
    void Wait(TLock& lock, TReady ready) {
        if (ready()) return;
        const Waiting waiting{waiters};
        for (;;) {
            const auto seen = epoch.load(std::memory_order_seq_cst);
            if (ready()) return;
            lock.unlock();
            AddressWait(epoch, seen);
            lock.lock();
        }
    }

    template<typename TLock, typename TReady>
    bool WaitUntil(TLock& lock, std::chrono::steady_clock::time_point deadline, TReady ready) {
        if (ready()) return true;
        const Waiting waiting{waiters};
        for (;;) {
            const auto seen = epoch.load(std::memory_order_seq_cst);
            if (ready()) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            lock.unlock();
            AddressWaitUntil(epoch, seen, deadline);
            lock.lock();
        }
    }

    template<typename TLock, typename TRep, typename TPeriod, typename TReady>
    bool WaitFor(TLock& lock, std::chrono::duration<TRep, TPeriod> timeout, TReady ready) {
        return WaitUntil(lock, std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout), std::move(ready));
    }

    template<typename TLock, typename TRep, typename TPeriod>
    void WaitFor(TLock& lock, std::chrono::duration<TRep, TPeriod> timeout) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout);
        const Waiting waiting{waiters};
        const auto seen = epoch.load(std::memory_order_seq_cst);
        lock.unlock();
        AddressWaitUntil(epoch, seen, deadline);
        lock.lock();
    }

    template<typename TLock, typename TReady>
    bool Wait(TLock& lock, std::stop_token token, TReady ready) {
        if (ready()) return true;
        const std::stop_callback stopped(token, [this] { NotifyAll(); });
        Wait(lock, [&] { return token.stop_requested() || ready(); });
        return ready();
    }

    void NotifyOne() noexcept {
        epoch.fetch_add(1, std::memory_order_seq_cst);
        if (waiters.load(std::memory_order_seq_cst) != 0) AddressWakeOne(epoch);
    }

    void NotifyAll() noexcept {
        epoch.fetch_add(1, std::memory_order_seq_cst);
        if (waiters.load(std::memory_order_seq_cst) != 0) AddressWakeAll(epoch);
    }

private:
    struct Waiting {
        std::atomic<std::uint32_t>& count;
        explicit Waiting(std::atomic<std::uint32_t>& count) noexcept : count(count) { count.fetch_add(1, std::memory_order_seq_cst); }
        ~Waiting() { count.fetch_sub(1, std::memory_order_release); }
        Waiting(const Waiting&) = delete;
        Waiting& operator=(const Waiting&) = delete;
    };

    std::atomic<std::uint32_t> epoch{0};
    std::atomic<std::uint32_t> waiters{0};
};

}

#endif
