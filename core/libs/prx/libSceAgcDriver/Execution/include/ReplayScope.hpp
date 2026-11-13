#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_REPLAYSCOPE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_REPLAYSCOPE_HPP

#include "prx/libc/include/HostThreadSlot.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace AgcDriver::Replay {

struct ScopeSlot {};

inline bool Replaying() noexcept {
    return HostThreadSlot<bool, ScopeSlot>::Get();
}

inline bool Guarded() noexcept {
    static const bool guarded = [] {
        const char* off = std::getenv("APS5_NO_REPLAY_GUARD");
        return off == nullptr || off[0] == '\0' || std::strcmp(off, "0") == 0;
    }();
    return guarded;
}

class Scope {
public:
    Scope() noexcept : previous(Replaying()) { HostThreadSlot<bool, ScopeSlot>::Set(true); }
    ~Scope() { HostThreadSlot<bool, ScopeSlot>::Set(previous); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    bool previous;
};

[[noreturn, gnu::cold, gnu::noinline]] inline void Refuse(const char* site) noexcept {
    std::fprintf(stderr, "AGC graphics: %s was called while replaying an encoded command; a replayed command must not reach guest memory, the active recorder, host imports or storage textures\n", site);
    std::fflush(stderr);
    std::abort();
}

inline void RefuseIn(const char* site) noexcept {
    if (Replaying() && Guarded()) [[unlikely]] Refuse(site);
}

}

#endif
