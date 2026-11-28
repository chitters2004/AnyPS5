#include "AllocationCounter.hpp"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <new>

namespace {

thread_local std::size_t allocationsBeforeFailure = SIZE_MAX;

bool CalledFromDriver(const void* caller) noexcept {
    Dl_info module{};
    return dladdr(caller, &module) != 0 && module.dli_fname != nullptr && std::strstr(module.dli_fname, "libSceAgcDriver") != nullptr;
}

void* Allocate(std::size_t bytes, const void* caller) {
    if (allocationsBeforeFailure != SIZE_MAX && CalledFromDriver(caller)) {
        if (allocationsBeforeFailure == 0) {
            allocationsBeforeFailure = SIZE_MAX;
            throw std::bad_alloc();
        }
        --allocationsBeforeFailure;
    }
    if (auto* allocation = std::malloc(bytes == 0 ? 1 : bytes)) return allocation;
    throw std::bad_alloc();
}

}

extern "C" void FailAllocationsAfter_nid_no_patch(std::size_t allocations) noexcept {
    allocationsBeforeFailure = allocations;
}

extern "C" void StopAllocationFailures_nid_no_patch() noexcept {
    allocationsBeforeFailure = SIZE_MAX;
}

extern "C" bool AllocationFailurePending_nid_no_patch() noexcept {
    return allocationsBeforeFailure != SIZE_MAX;
}

void* operator new(std::size_t bytes) { return Allocate(bytes, __builtin_return_address(0)); }
void* operator new[](std::size_t bytes) { return Allocate(bytes, __builtin_return_address(0)); }
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete[](void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept { std::free(allocation); }
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept { return std::malloc(bytes == 0 ? 1 : bytes); }
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept { return std::malloc(bytes == 0 ? 1 : bytes); }
void operator delete(void* allocation, const std::nothrow_t&) noexcept { std::free(allocation); }
void operator delete[](void* allocation, const std::nothrow_t&) noexcept { std::free(allocation); }
