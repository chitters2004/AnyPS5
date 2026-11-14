#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_GUESTSTORE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_GUESTSTORE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>

namespace AgcDriver::GuestStore {

static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free && std::atomic_ref<std::uint32_t>::is_always_lock_free);

template <typename T>
inline void Store(void* target, T value, std::memory_order order = std::memory_order_release) {
    std::atomic_ref<T>(*static_cast<T*>(target)).store(value, order);
}

template <typename T>
inline T Load(const void* source, std::memory_order order = std::memory_order_acquire) {
    return std::atomic_ref<T>(*static_cast<T*>(const_cast<void*>(source))).load(order);
}

inline bool IsLabel(const void* address, std::size_t bytes) {
    return (bytes == sizeof(std::uint32_t) || bytes == sizeof(std::uint64_t)) && (reinterpret_cast<std::uintptr_t>(address) & (bytes - 1)) == 0;
}

inline void StoreLabel(void* target, std::span<const std::byte> value, std::memory_order order = std::memory_order_release) {
    if (!IsLabel(target, value.size())) throw std::logic_error("a guest label is an aligned 4 or 8 bytes");
    if (value.size() == sizeof(std::uint64_t)) {
        std::uint64_t word;
        std::memcpy(&word, value.data(), sizeof(word));
        Store(target, word, order);
        return;
    }
    std::uint32_t word;
    std::memcpy(&word, value.data(), sizeof(word));
    Store(target, word, order);
}
}

#endif
