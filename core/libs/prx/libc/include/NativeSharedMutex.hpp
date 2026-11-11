#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_NATIVESHAREDMUTEX_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_NATIVESHAREDMUTEX_HPP

#ifdef _WIN32

#include <synchapi.h>

class NativeSharedMutex {
public:
    constexpr NativeSharedMutex() noexcept = default;
    NativeSharedMutex(const NativeSharedMutex&) = delete;
    NativeSharedMutex& operator=(const NativeSharedMutex&) = delete;

    void lock() noexcept { AcquireSRWLockExclusive(&srw); }
    bool try_lock() noexcept { return TryAcquireSRWLockExclusive(&srw) != 0; }
    void unlock() noexcept { ReleaseSRWLockExclusive(&srw); }
    void lock_shared() noexcept { AcquireSRWLockShared(&srw); }
    bool try_lock_shared() noexcept { return TryAcquireSRWLockShared(&srw) != 0; }
    void unlock_shared() noexcept { ReleaseSRWLockShared(&srw); }

private:
    SRWLOCK srw = SRWLOCK_INIT;
};

#else

#include <shared_mutex>

using NativeSharedMutex = std::shared_mutex;

#endif

#endif
