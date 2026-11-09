#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

namespace AgcDriver::DriverDetail {
std::uint64_t NullPixelProgramAddress();
}

namespace {
using namespace AgcDriver::GuestMemory;
constexpr std::size_t Unit = 65536;
constexpr std::size_t ChunkGuestBytes = 256ull << 20u;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Range {
public:
    explicit Range(std::size_t bytes) : bytes(bytes) {
        data = static_cast<std::byte*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, ChunkGuestBytes));
        Require(data != nullptr, "cannot reserve page-state fixture");
    }

    ~Range() {
        {
            GuestAllocations::Mutation mutation;
            for (const auto offset : committed) mutation.Remove(data + offset);
        }
        GuestArena::GuestArenaReset_nid_postfix(data, bytes);
        GuestArena::GuestArenaRelease_nid_postfix(data, bytes);
    }

    std::byte* Commit(std::size_t offset, std::size_t length = Unit) {
        auto* at = data + offset;
        GuestArena::GuestArenaCommit_nid_postfix(at, length, PAGE_READWRITE, Unit);
        std::memset(at, 0x5b, length);
        GuestAllocations::Mutation mutation;
        mutation.Add(at, length, true, true, true);
        committed.push_back(offset);
        return at;
    }

    std::byte* Data() const { return data; }

private:
    std::byte* data = nullptr;
    std::size_t bytes;
    std::vector<std::size_t> committed;
};

std::size_t PrivateBytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    Require(K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)), "cannot query page-state fixture private commit");
    return counters.PrivateUsage;
}

void CheckMetadata() {
    Range range(3 * ChunkGuestBytes);
    auto* data = range.Commit(0);
    const auto before = PrivateBytes();
    Require(Accessible(data, Unit, true), "committed fixture is inaccessible");
    const auto after = PrivateBytes();
    const auto added = after > before ? after - before : 0;
    std::printf("page-permission initialization: %zu bytes of private commit\n", added);
    Require(added < (16ull << 20u), "page permissions committed a dense arena table");
    const auto queried = PrivateBytes();
    Require(!Accessible(range.Data() + 2 * ChunkGuestBytes, Unit), "reserved fixture tail is accessible");
    Require(PrivateBytes() < queried + (4ull << 20u), "querying reserved pages allocated untouched metadata chunks");
}

void CheckBoundaries() {
    constexpr auto span = 3 * ChunkGuestBytes;
    Range range(span);
    const std::array<std::size_t, 4> offsets{0, ChunkGuestBytes - Unit, ChunkGuestBytes, 2 * ChunkGuestBytes + Unit};
    for (const auto offset : offsets) range.Commit(offset);
    auto* block = range.Data();
    std::atomic<bool> begin{false};
    std::atomic<unsigned> errors{0};
    std::vector<std::thread> readers;
    for (unsigned reader = 0; reader < 8; ++reader) {
        readers.emplace_back([&] {
            while (!begin.load(std::memory_order_acquire)) std::this_thread::yield();
            for (unsigned round = 0; round < 500; ++round) {
                if (!Accessible(block + offsets.back(), Unit, true)) errors.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    begin.store(true, std::memory_order_release);
    for (auto& reader : readers) reader.join();
    Require(errors.load() == 0, "concurrent page-state publication changed access");
    for (const auto offset : offsets) {
        std::array<std::byte, 16> copy{};
        Read(reinterpret_cast<std::uint64_t>(block + offset), copy);
        Require(std::all_of(copy.begin(), copy.end(), [](std::byte value) { return value == std::byte{0x5b}; }), "page cache changed guest bytes");
    }
    Require(Accessible(block + ChunkGuestBytes - Unit, 2 * Unit, true), "adjacent chunks lost their boundary pages");
    const auto described = DescribeCommitted(reinterpret_cast<std::uint64_t>(block), span, true);
    std::uint64_t total = 0;
    for (const auto& [first, last] : described.ranges) total += last - first;
    Require(!described.whole && total == offsets.size() * Unit, "reserved holes were reported committed");
    auto* changed = block + offsets[1];
    const auto protect = [&](bool writable) {
        GuestAllocations::Mutation mutation;
        mutation.Protect(changed, Unit, true, writable, true, [&] {
            const DWORD protection = writable ? PAGE_READWRITE : PAGE_READONLY;
            DWORD previous = 0;
            Require(VirtualProtect(changed, Unit, protection, &previous) != 0, "cannot protect page-state fixture");
            GuestArena::GuestArenaSetProtection_nid_postfix(reinterpret_cast<std::uintptr_t>(changed), Unit, protection);
        });
    };
    protect(false);
    Require(Accessible(changed, Unit) && !Accessible(changed, Unit, true), "protection invalidation retained writable state");
    protect(true);
    Require(Accessible(changed, Unit, true), "restored writable state was not discovered");
    GuestArena::GuestArenaReset_nid_postfix(block, Unit);
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(reinterpret_cast<std::uintptr_t>(block), Unit);
    Require(!Accessible(block, Unit), "decommitted pages remained readable");
    GuestArena::GuestArenaCommit_nid_postfix(block, Unit, PAGE_READWRITE, Unit);
    Require(Accessible(block, Unit, true), "access observer did not invalidate inaccessible state after commit");
    std::puts("page-state boundaries, publication, holes and invalidation passed");
}

void CheckRuns() {
    constexpr std::size_t page = 4096;
    constexpr std::size_t count = 1536;
    constexpr auto bytes = count * page;
    using Ranges = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
    Range range(3 * ChunkGuestBytes);
    auto* guest = range.Commit(ChunkGuestBytes - 512 * page, bytes);
    const auto base = reinterpret_cast<std::uint64_t>(guest);
    const auto expected = [&](std::size_t index, std::size_t length = 1) {
        Ranges result;
        if (index != 0) result.emplace_back(base, base + index * page);
        if (index + length < count) result.emplace_back(base + (index + length) * page, base + bytes);
        return result;
    };
    const auto protect = [&](std::size_t index, bool writable) {
        auto* at = guest + index * page;
        GuestAllocations::Mutation mutation;
        mutation.Protect(at, page, true, writable, true, [&] {
            const DWORD protection = writable ? PAGE_READWRITE : PAGE_READONLY;
            DWORD previous = 0;
            Require(VirtualProtect(at, page, protection, &previous) != 0, "cannot protect run fixture page");
            GuestArena::GuestArenaSetProtection_nid_postfix(reinterpret_cast<std::uintptr_t>(at), page, protection);
        });
    };
    const Ranges whole{{base, base + bytes}};
    Require(DescribeCommitted(base, bytes, true).whole, "run fixture was not fully writable");
    for (const std::size_t index : {0, 63, 64, 511, 512, 513, 700, 1023, 1024, 1535}) {
        protect(index, false);
        Require(!Accessible(guest, bytes, true) && Accessible(guest, bytes), "a protected page retained writable access in a summarized run");
        Require(CommittedRanges(base, bytes, true) == expected(index), "a block or chunk boundary retained a stale summary");
        Require(CommittedRanges(base, bytes) == whole, "readable ranges changed around a read-only page");
        Require(CommittedRanges(base + 1, bytes - 2, true) == [&] {
            auto clipped = expected(index);
            if (!clipped.empty() && clipped.front().first == base) ++clipped.front().first;
            if (!clipped.empty() && clipped.back().second == base + bytes) --clipped.back().second;
            return clipped;
        }(), "an unaligned run crossed a protected page");
        protect(index, true);
        Require(DescribeCommitted(base, bytes, true).whole, "restored run was not writable");
    }
    auto* hole = guest + 512 * page;
    GuestArena::GuestArenaReset_nid_postfix(hole, Unit);
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(reinterpret_cast<std::uintptr_t>(hole), Unit);
    Require(!Accessible(hole, page), "a decommitted page retained cached access");
    Require(CommittedRanges(base, bytes) == expected(512, Unit / page), "decommitted pages retained a block summary");
    GuestArena::GuestArenaCommit_nid_postfix(hole, Unit, PAGE_READWRITE, Unit);
    Require(DescribeCommitted(base, bytes, true).whole, "recommit did not clear inaccessible state and its summary");
    std::atomic<bool> done{false};
    std::atomic<std::size_t> epoch{0};
    std::atomic<std::size_t> changed{0};
    std::array<std::atomic<std::size_t>, 2> observed{};
    std::atomic<unsigned> errors{0};
    std::vector<std::thread> readers;
    for (std::size_t reader = 0; reader < observed.size(); ++reader) {
        readers.emplace_back([&, reader] {
            while (!done.load(std::memory_order_acquire)) {
                const auto phase = epoch.load(std::memory_order_acquire);
                if (phase > observed[reader].load(std::memory_order_relaxed)) {
                    const auto index = changed.load(std::memory_order_relaxed);
                    if (CommittedRanges(base, bytes, true) != ((phase & 1) != 0 ? expected(index) : whole)) errors.fetch_add(1, std::memory_order_relaxed);
                    observed[reader].store(phase, std::memory_order_release);
                } else static_cast<void>(DescribeCommitted(base, bytes, true));
            }
        });
    }
    const auto publish = [&](std::size_t phase, std::size_t index) {
        changed.store(index, std::memory_order_relaxed);
        epoch.store(phase, std::memory_order_release);
        for (auto& seen : observed) {
            while (seen.load(std::memory_order_acquire) < phase) std::this_thread::yield();
        }
    };
    for (std::size_t round = 0; round < 128; ++round) {
        const auto index = (round * 37 + 511) % count;
        protect(index, false);
        publish(2 * round + 1, index);
        protect(index, true);
        publish(2 * round + 2, index);
    }
    for (std::size_t round = 0; round < 2000; ++round) {
        const auto index = 448 + round % 128;
        protect(index, false);
        protect(index, true);
    }
    protect(512, false);
    publish(257, 512);
    done.store(true, std::memory_order_release);
    for (auto& reader : readers) reader.join();
    Require(errors.load() == 0 && CommittedRanges(base, bytes, true) == expected(512), "concurrent scans retained a stale block summary");
    protect(512, true);
    std::array<std::byte, 16> copy{};
    Read(base, copy);
    Require(std::all_of(copy.begin(), copy.end(), [](std::byte value) { return value == std::byte{0x5b}; }), "summary scans changed guest data");
    Range edge(2 * ChunkGuestBytes);
    constexpr std::size_t edgePages = 512;
    const auto edgeFirst = reinterpret_cast<std::uint64_t>(edge.Commit(ChunkGuestBytes - edgePages * page, edgePages * page));
    const Ranges edgeRun{{edgeFirst, edgeFirst + edgePages * page}};
    Require(CommittedRanges(edgeFirst, edgePages * page, true) == edgeRun, "the run before an unallocated chunk was not writable");
    Require(!DescribeCommitted(edgeFirst, edgePages * page + Unit, true).whole && CommittedRanges(edgeFirst, edgePages * page + Unit, true) == edgeRun, "a run into an unallocated chunk was reported past its committed pages");
    std::puts("page-state block scans across 64-page, 512-page and sparse-chunk boundaries passed");
}

std::atomic<std::uint64_t> queries{0};
decltype(&VirtualQuery) realQuery = nullptr;

SIZE_T WINAPI CountedQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION information, SIZE_T length) {
    queries.fetch_add(1, std::memory_order_relaxed);
    return realQuery(address, information, length);
}

const IMAGE_NT_HEADERS* Headers(std::uintptr_t base) {
    return reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
}

bool ReadOnlySection(const IMAGE_SECTION_HEADER& section) {
    return (section.Characteristics & IMAGE_SCN_MEM_READ) != 0 && (section.Characteristics & IMAGE_SCN_MEM_WRITE) == 0;
}

void CountQueries(std::uintptr_t base) {
    std::vector<FARPROC> targets;
    for (const auto* name : {L"kernel32.dll", L"kernelbase.dll"}) {
        if (const auto module = GetModuleHandleW(name)) {
            if (const auto target = GetProcAddress(module, "VirtualQuery")) targets.push_back(target);
        }
    }
    Require(!targets.empty(), "cannot resolve VirtualQuery");
    realQuery = reinterpret_cast<decltype(&VirtualQuery)>(targets.front());
    const auto& directory = Headers(base)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    unsigned patched = 0;
    for (auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress); descriptor->Name != 0; ++descriptor) {
        for (auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk); thunk->u1.Function != 0; ++thunk) {
            if (std::find(targets.begin(), targets.end(), reinterpret_cast<FARPROC>(thunk->u1.Function)) == targets.end()) continue;
            DWORD previous = 0;
            Require(VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), PAGE_READWRITE, &previous) != 0, "cannot open the driver import table");
            thunk->u1.Function = reinterpret_cast<ULONG_PTR>(&CountedQuery);
            Require(VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), previous, &previous) != 0, "cannot restore the driver import table");
            ++patched;
        }
    }
    Require(patched != 0, "the driver does not import VirtualQuery");
}

template <class Call>
std::uint64_t Queried(Call&& call) {
    const auto before = queries.load(std::memory_order_relaxed);
    call();
    return queries.load(std::memory_order_relaxed) - before;
}

template <class Call>
double NanosecondsPerCall(Call&& call) {
    constexpr unsigned calls = 200000;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned index = 0; index < calls; ++index) call();
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / calls;
}

std::uintptr_t BuiltinHeader(std::uintptr_t base, std::uint64_t code) {
    const auto* headers = Headers(base);
    const auto* sections = IMAGE_FIRST_SECTION(headers);
    for (unsigned index = 0; index < headers->FileHeader.NumberOfSections; ++index) {
        if (!ReadOnlySection(sections[index])) continue;
        const auto first = base + sections[index].VirtualAddress;
        for (auto at = first; at + 24 <= first + sections[index].Misc.VirtualSize; at += 8) {
            std::uint32_t magic = 0;
            std::uint64_t pointer = 0;
            std::memcpy(&magic, reinterpret_cast<const void*>(at), sizeof(magic));
            std::memcpy(&pointer, reinterpret_cast<const void*>(at + 16), sizeof(pointer));
            if (magic == 0x34333231u && pointer == code) return at;
        }
    }
    return 0;
}

void CheckConstants() {
    const auto code = AgcDriver::DriverDetail::NullPixelProgramAddress();
    HMODULE module = nullptr;
    Require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(code), &module) != 0, "built-in shader is outside a module");
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    CountQueries(base);
    const auto header = BuiltinHeader(base, code);
    Require(header != 0, "the built-in shader header is not in the driver's read-only data");
    for (const auto [address, size] : {std::pair{code, std::size_t{256}}, std::pair{static_cast<std::uint64_t>(header), std::size_t{96}}}) {
        std::vector<std::byte> bytes(size);
        std::memcpy(bytes.data(), reinterpret_cast<const void*>(address), bytes.size());
        Require(CompareMapped(address, bytes) == Compare::Equal, "built-in shader bytes differ from themselves");
        Require(Queried([&] { Require(CompareMapped(address, bytes) == Compare::Equal, "built-in shader compare changed"); }) == 0, "comparing a built-in shader queried the address space");
        auto changed = bytes;
        changed[17] ^= std::byte{1};
        Require(Queried([&] { Require(CompareMapped(address, changed) == Compare::Differs, "a changed copy of a built-in shader compared equal"); }) == 0, "a differing built-in shader compare queried the address space");
        std::vector<std::byte> copy(bytes.size());
        Require(Queried([&] { Require(CopyMapped(address, copy) == Compare::Equal && copy == bytes, "built-in shader copy differs"); }) == 0, "copying a built-in shader queried the address space");
        Require(Queried([&] { Require(Accessible(reinterpret_cast<const void*>(address), bytes.size()) && !Accessible(reinterpret_cast<const void*>(address), bytes.size(), true), "built-in shader access is not read-only"); }) == 0, "checking built-in shader access queried the address space");
    }
    const auto* headers = Headers(base);
    const auto* sections = IMAGE_FIRST_SECTION(headers);
    std::uintptr_t boundary = 0;
    for (unsigned index = 0; index + 1 < headers->FileHeader.NumberOfSections; ++index) {
        const auto& constant = sections[index];
        const auto& written = sections[index + 1];
        const auto constantEnd = (constant.VirtualAddress + constant.Misc.VirtualSize + 4095u) & ~4095u;
        if (ReadOnlySection(constant) && (written.Characteristics & IMAGE_SCN_MEM_WRITE) != 0 && written.VirtualAddress == constantEnd && written.Misc.VirtualSize >= 64) {
            boundary = base + written.VirtualAddress;
            break;
        }
    }
    Require(boundary != 0, "the driver has no read-only section followed by a writable one");
    std::vector<std::byte> across(64);
    std::memcpy(across.data(), reinterpret_cast<const void*>(boundary - 32), across.size());
    Require(Queried([&] { Require(CompareMapped(boundary - 32, across) == Compare::Equal, "a compare across a constant section end differs"); }) != 0, "a range past the driver's constants skipped the query");
    Require(Queried([&] { Require(Accessible(reinterpret_cast<const void*>(boundary), 32, true), "the writable section after the constants is not writable"); }) != 0, "a writable driver section was treated as constant");
    auto* host = static_cast<std::byte*>(VirtualAlloc(nullptr, 2 * 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Require(host != nullptr, "cannot allocate host fixture");
    std::memset(host, 0x3c, 2 * 4096);
    DWORD previous = 0;
    Require(VirtualProtect(host + 4096, 4096, PAGE_READWRITE | PAGE_GUARD, &previous) != 0, "cannot guard host fixture");
    const auto hostAddress = reinterpret_cast<std::uint64_t>(host);
    const std::vector<std::byte> page(4096, std::byte{0x3c});
    const std::vector<std::byte> pages(2 * 4096, std::byte{0x3c});
    Require(CompareMapped(hostAddress, page) == Compare::Equal, "a readable host page did not compare equal");
    Require(CompareMapped(hostAddress, pages) == Compare::Unmapped, "a guard page compared as readable");
    MEMORY_BASIC_INFORMATION guard{};
    Require(VirtualQuery(host + 4096, &guard, sizeof(guard)) == sizeof(guard) && (guard.Protect & PAGE_GUARD) != 0, "the compare consumed the guard page");
    Require(VirtualProtect(host, 4096, PAGE_NOACCESS, &previous) != 0, "cannot protect host fixture");
    Require(CompareMapped(hostAddress, page) == Compare::Unmapped, "a host page made inaccessible kept a readable answer");
    Require(VirtualProtect(host, 4096, PAGE_READONLY, &previous) != 0, "cannot restore host fixture");
    Require(CompareMapped(hostAddress, page) == Compare::Equal, "a host page made readable again kept an inaccessible answer");
    Require(VirtualFree(host, 0, MEM_RELEASE) != 0, "cannot release host fixture");
    Require(CompareMapped(hostAddress, page) == Compare::Unmapped, "a released host page kept a readable answer");
    const auto system = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"kernel32.dll"));
    const auto* systemSections = IMAGE_FIRST_SECTION(Headers(system));
    std::uintptr_t foreign = 0;
    for (unsigned index = 0; index < Headers(system)->FileHeader.NumberOfSections; ++index) {
        if (ReadOnlySection(systemSections[index]) && (systemSections[index].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 && systemSections[index].Misc.VirtualSize >= 4096) {
            foreign = system + systemSections[index].VirtualAddress;
            break;
        }
    }
    Require(foreign != 0, "kernel32 has no read-only data section");
    std::vector<std::byte> foreignBytes(96);
    std::memcpy(foreignBytes.data(), reinterpret_cast<const void*>(foreign), foreignBytes.size());
    Require(Queried([&] { Require(CompareMapped(foreign, foreignBytes) == Compare::Equal, "foreign image bytes differ"); }) != 0, "another module's constants skipped the query");
    std::vector<std::byte> headerBytes(96);
    std::memcpy(headerBytes.data(), reinterpret_cast<const void*>(header), headerBytes.size());
    const auto own = NanosecondsPerCall([&] { static_cast<void>(CompareMapped(header, headerBytes)); });
    const auto other = NanosecondsPerCall([&] { static_cast<void>(CompareMapped(foreign, foreignBytes)); });
    std::printf("CompareMapped of a 96-byte shader header: driver built-in %.1f ns/call; another module's read-only data %.1f ns/call\n", own, other);
    std::puts("driver constants, section ends, guard pages and host protection passed");
}
}

int main(int argc, char** argv) {
    try {
        Require(GuestArena::GuestArenaAvailable_nid_postfix(), "page-state fixture needs the guest arena");
        if (argc == 2 && std::string_view(argv[1]) == "--metadata") CheckMetadata();
        else if (argc == 2 && std::string_view(argv[1]) == "--runs") CheckRuns();
        else if (argc == 2 && std::string_view(argv[1]) == "--constants") CheckConstants();
        else CheckBoundaries();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
