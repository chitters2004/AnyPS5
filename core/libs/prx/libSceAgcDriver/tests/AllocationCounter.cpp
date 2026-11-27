#include "AllocationCounter.hpp"
#include <cstdint>
#include <new>

extern "C" void* __real__Znwy(std::size_t bytes);

namespace {

struct AllocationHooks {
    bool armed = false;
    bool counting = false;
    std::size_t counted = 0;
    std::size_t beforeFailure = SIZE_MAX;
    bool persistent = false;
};

thread_local AllocationHooks hooks;

void rearm(AllocationHooks& state) noexcept {
    state.armed = state.counting || state.beforeFailure != SIZE_MAX;
}

[[gnu::noinline]] void observe(AllocationHooks& state) {
    if (state.beforeFailure == 0) {
        if (!state.persistent) {
            state.beforeFailure = SIZE_MAX;
            rearm(state);
        }
        throw std::bad_alloc();
    }
    if (state.beforeFailure != SIZE_MAX) --state.beforeFailure;
    if (state.counting) ++state.counted;
}

}

extern "C" void BeginAllocationCount_nid_no_patch() noexcept {
    hooks.counted = 0;
    hooks.counting = true;
    rearm(hooks);
}

extern "C" std::size_t EndAllocationCount_nid_no_patch() noexcept {
    hooks.counting = false;
    rearm(hooks);
    return hooks.counted;
}

extern "C" void AllocateForCount_nid_no_patch(std::size_t bytes) {
    void* (*volatile allocate)(std::size_t) = &::operator new;
    void* allocation = allocate(bytes);
    ::operator delete(allocation);
}

extern "C" void FailAllocationsAfter_nid_no_patch(std::size_t allocations) noexcept {
    hooks.beforeFailure = allocations;
    hooks.persistent = false;
    rearm(hooks);
}

extern "C" void FailEveryAllocationAfter_nid_no_patch(std::size_t allocations) noexcept {
    hooks.beforeFailure = allocations;
    hooks.persistent = true;
    rearm(hooks);
}

extern "C" void StopAllocationFailures_nid_no_patch() noexcept {
    hooks.beforeFailure = SIZE_MAX;
    hooks.persistent = false;
    rearm(hooks);
}

extern "C" bool AllocationFailurePending_nid_no_patch() noexcept {
    return hooks.beforeFailure != SIZE_MAX;
}

extern "C" void* __wrap__Znwy(std::size_t bytes) {
    auto& state = hooks;
    if (state.armed) [[unlikely]] observe(state);
    return __real__Znwy(bytes);
}
