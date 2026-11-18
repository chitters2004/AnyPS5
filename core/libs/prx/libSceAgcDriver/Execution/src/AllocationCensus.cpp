#include "prx/libSceAgcDriver/Execution/include/AllocationCensus.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace AgcDriver::AllocationCensus {

namespace {

constexpr std::size_t TopSites = 40;
using CensusTable = Table<4096, TopSites>;

CensusTable& Census() noexcept {
    static CensusTable table;
    return table;
}

std::atomic<int> state{0};
std::atomic<bool> reporting{false};
std::atomic<std::int64_t> lastReport{0};

std::int64_t NowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Module {
    std::uintptr_t base = 0;
    std::uintptr_t preferred = 0;
    char path[512] = {};
};

Module FindModule() noexcept {
    Module module;
#ifdef _WIN32
    HMODULE handle = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&FindModule), &handle) && handle != nullptr) {
        module.base = reinterpret_cast<std::uintptr_t>(handle);
        GetModuleFileNameA(handle, module.path, sizeof(module.path));
        const HANDLE file = CreateFileA(module.path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            static unsigned char header[4096];
            DWORD read = 0;
            if (ReadFile(file, header, sizeof(header), &read, nullptr)) module.preferred = static_cast<std::uintptr_t>(PreferredImageBase(header, read));
            CloseHandle(file);
        }
    }
#endif
    return module;
}

void Report(std::int64_t now, std::int64_t last) noexcept {
    static const Module module = FindModule();
    static std::array<Site, TopSites> top{};
    static char line[8192];
    if (last == 0) {
        std::fprintf(stderr, "[alloc-census] counting host allocations in %s (loaded at 0x%llx, preferred base 0x%llx read from the file; 0 means unread and sites are offsets from the load address); site addresses are at the preferred base for addr2line -f -C -e on that file; new = the driver's operator new (HostAllocator), malloc/calloc/realloc = calls linked through --wrap in the driver, the HostAllocator site's malloc being its cache misses\n", module.path[0] != 0 ? module.path : "the driver", static_cast<unsigned long long>(module.base), static_cast<unsigned long long>(module.preferred));
    }
    const auto window = Census().Take(top);
    const double seconds = last != 0 ? static_cast<double>(now - last) / 1000.0 : 0.0;
    FormatWindow(line, sizeof(line), seconds, window, top.data(), module.base, module.preferred);
    std::fprintf(stderr, "%s\n", line);
}

}

bool Enabled() noexcept {
    auto current = state.load(std::memory_order_relaxed);
    if (current != 0) return current == 2;
    current = std::getenv("APS5_ALLOC_CENSUS") != nullptr ? 2 : 1;
    state.store(current, std::memory_order_relaxed);
    return current == 2;
}

void Note(Source source, const void* caller, std::size_t bytes) noexcept {
    Census().Note(source, reinterpret_cast<std::uintptr_t>(caller), bytes);
}

std::uint64_t Total(Source source) noexcept {
    return Census().Total(source);
}

void NoteDraw() noexcept {
    if (!Enabled()) return;
    auto& census = Census();
    census.NoteDraw();
    if ((census.Draws() & 1023u) != 0) return;
    const auto now = NowMs();
    auto last = lastReport.load(std::memory_order_relaxed);
    if (last != 0 && now - last < 10000) return;
    if (reporting.exchange(true, std::memory_order_acquire)) return;
    last = lastReport.load(std::memory_order_relaxed);
    if (last == 0 || now - last >= 10000) {
        Report(now, last);
        lastReport.store(now, std::memory_order_relaxed);
    }
    reporting.store(false, std::memory_order_release);
}

}
