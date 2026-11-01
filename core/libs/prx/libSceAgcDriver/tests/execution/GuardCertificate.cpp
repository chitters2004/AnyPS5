#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTarget.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

using namespace AgcDriver::Graphics;

constexpr std::uint64_t Base = 0x7c0000000000ull;
constexpr std::uint64_t FmaskBytes = 0x10000u;

template<typename Call>
void Refuses(Call&& call, const std::string& expected) {
    try {
        call();
    } catch (const std::runtime_error& error) {
        Require(std::string(error.what()).find(expected) != std::string::npos, std::string("unexpected refusal: ") + error.what());
        return;
    }
    throw std::runtime_error("a guarded range was read without a refusal: " + expected);
}

Context FakeContext(std::uintptr_t device) {
    Context context{};
    context.device = reinterpret_cast<VkDevice>(device);
    return context;
}

ColorTarget Fmask(std::uint64_t address) {
    ColorTarget target{};
    target.extent = {256u, 256u};
    target.coverageSamples = 4u;
    target.samples = 8u;
    target.fmaskAddress = address;
    return target;
}

#ifndef _WIN32
class FrozenImage {
public:
    FrozenImage() {
        Require(threads() == 1u, "the frozen-image check needs a single-threaded process");
        dl_iterate_phdr(&FrozenImage::collect, this);
        Require(!segments.empty(), "the driver image has no writable segment");
        for (const auto& [begin, end] : segments) Require(mprotect(reinterpret_cast<void*>(begin), end - begin, PROT_READ) == 0, "cannot freeze the driver image");
    }

    ~FrozenImage() {
        for (const auto& [begin, end] : segments) mprotect(reinterpret_cast<void*>(begin), end - begin, PROT_READ | PROT_WRITE);
    }

    FrozenImage(const FrozenImage&) = delete;
    FrozenImage& operator=(const FrozenImage&) = delete;

private:
    static std::size_t threads() {
        std::size_t count = 0;
        if (auto* directory = opendir("/proc/self/task")) {
            while (const auto* entry = readdir(directory)) count += entry->d_name[0] != '.';
            closedir(directory);
        }
        return count;
    }

    static int collect(dl_phdr_info* info, std::size_t, void* data) {
        auto& self = *static_cast<FrozenImage*>(data);
        const auto code = reinterpret_cast<std::uintptr_t>(&ResidentGuardsClear_nid_no_patch);
        bool owner = false;
        for (std::size_t index = 0; index < info->dlpi_phnum; ++index) {
            const auto& header = info->dlpi_phdr[index];
            const auto begin = info->dlpi_addr + header.p_vaddr;
            if (header.p_type == PT_LOAD && (header.p_flags & PF_X) != 0 && code >= begin && code < begin + header.p_memsz) owner = true;
        }
        if (!owner) return 0;
        const auto page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
        for (std::size_t index = 0; index < info->dlpi_phnum; ++index) {
            const auto& header = info->dlpi_phdr[index];
            if (header.p_type != PT_LOAD || (header.p_flags & PF_W) == 0) continue;
            const auto begin = (info->dlpi_addr + header.p_vaddr) & ~(page - 1u);
            const auto end = (info->dlpi_addr + header.p_vaddr + header.p_memsz + page - 1u) & ~(page - 1u);
            self.segments.emplace_back(begin, end);
        }
        return 1;
    }

    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> segments;
};
#endif

template<typename Reads>
void WithoutSharedWrites(Reads&& reads) {
#ifndef _WIN32
    reads();
    FrozenImage frozen;
    reads();
#else
    reads();
#endif
}

void CheckHeadless() {
    const auto context = FakeContext(0x5000u);
    const auto guardAddress = Base + 0x100000u;
    const auto distant = Base + 0x4000000u;
    ClearMultisampleTargets(context.device);
    const auto start = ResidentGuardGrowth_nid_no_patch();
    Require(ResidentGuardsClear_nid_no_patch(guardAddress, 64u), "an unguarded range was not proven clear");
    Require(ResidentGuardsClear_nid_no_patch(distant, 4096u), "an unguarded distant range was not proven clear");
    GuardOneFragmentFmask(context, Fmask(guardAddress));
    const auto registered = ResidentGuardGrowth_nid_no_patch();
    Require(registered > start, "registering a guard did not advance the guard growth");
    Require(!ResidentGuardsClear_nid_no_patch(guardAddress, 64u), "a certificate taken before a registration was accepted after it");
    Refuses([&] { RequireNoMultisampleColorTarget(guardAddress, 64u, "a CPU read"); }, "FMASK");
    Refuses([&] { RequireNoMultisampleTarget(guardAddress + FmaskBytes - 1u, 1u, "a CPU read"); }, "FMASK");
    Refuses([&] { RequireNoMultisampleTarget(guardAddress - 64u, 65u, "a CPU read"); }, "FMASK");
    Require(ResidentGuardsClear_nid_no_patch(guardAddress - 64u, 64u), "the range ending at a guard was not proven clear");
    Refuses([&] { RequireNoMultisampleTarget(guardAddress - 64u, 128u, "a CPU read"); }, "FMASK");
    Require(ResidentGuardsClear_nid_no_patch(guardAddress + FmaskBytes, 1u), "the range starting at a guard end was not proven clear");
    Require(!ResidentGuardsClear_nid_no_patch(std::numeric_limits<std::uint64_t>::max() - 15u, 32u), "an overflowing range was proven clear");
    Refuses([&] { RequireNoMultisampleColorTarget(std::numeric_limits<std::uint64_t>::max() - 15u, 32u, "a CPU read"); }, "overflows");
    Require(ResidentGuardsClear_nid_no_patch(distant, 4096u), "a distant range was not proven clear after a registration");
    GuardOneFragmentFmask(context, Fmask(guardAddress));
    Require(ResidentGuardGrowth_nid_no_patch() == registered, "guarding an already guarded FMASK advanced the guard growth");
    WithoutSharedWrites([&] {
        for (int round = 0; round < 1000; ++round) {
            Require(ResidentGuardsClear_nid_no_patch(distant + round, 64u), "a certified range missed");
            RequireNoMultisampleColorTarget(distant + round, 64u, "a CPU read");
            RequireNoMultisampleTarget(distant + round, 64u, "a CPU read");
            AgcDriver::GuestMemory::GuardRead(distant + round, 64u);
        }
    });
    ClearMultisampleTargets(context.device);
    Require(ResidentGuardGrowth_nid_no_patch() == registered, "removing guards advanced the guard growth");
    WithoutSharedWrites([&] { Require(ResidentGuardsClear_nid_no_patch(distant, 64u), "a certificate did not survive a removal"); });
    RequireNoMultisampleTarget(guardAddress, 64u, "a CPU read");
    std::puts("headless: registration invalidates, repeated and removed guards keep certificates, hits write nothing in the driver image");
}

void CheckStress() {
    const auto context = FakeContext(0x6000u);
    ClearMultisampleTargets(context.device);
    constexpr unsigned Readers = 3;
    constexpr unsigned Rounds = 1500;
    std::atomic<unsigned> round{0}, published{0}, ready{0}, finished{0}, failures{0}, stale{0};
    std::atomic<bool> done{false};
    std::vector<std::thread> readers;
    for (unsigned reader = 0; reader < Readers; ++reader) {
        readers.emplace_back([&, reader] {
            unsigned seen = 0;
            while (!done.load(std::memory_order_acquire)) {
                const auto current = round.load(std::memory_order_acquire);
                if (current == seen) continue;
                seen = current;
                const auto address = Base + 0x10000000ull + static_cast<std::uint64_t>(current) * 0x40000u;
                const auto warm = Base + 0x80000000ull + reader * 0x1000u;
                try {
                    RequireNoMultisampleColorTarget(address, 256u, "a CPU read");
                    RequireNoMultisampleTarget(warm, 64u, "a CPU read");
                } catch (const std::exception&) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
                ready.fetch_add(1, std::memory_order_acq_rel);
                bool after = false;
                for (unsigned spin = 0; !after; ++spin) {
                    after = published.load(std::memory_order_acquire) >= current;
                    try {
                        RequireNoMultisampleColorTarget(address + 128u, 64u, "a CPU read");
                        if (after) stale.fetch_add(1, std::memory_order_relaxed);
                    } catch (const std::runtime_error&) {
                    }
                    RequireNoMultisampleTarget(warm, 64u, "a CPU read");
                    if (spin > 50000000u) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
                finished.fetch_add(1, std::memory_order_acq_rel);
            }
        });
    }
    for (unsigned current = 1; current <= Rounds; ++current) {
        ready.store(0, std::memory_order_release);
        finished.store(0, std::memory_order_release);
        round.store(current, std::memory_order_release);
        while (ready.load(std::memory_order_acquire) != Readers) std::this_thread::yield();
        GuardOneFragmentFmask(context, Fmask(Base + 0x10000000ull + static_cast<std::uint64_t>(current) * 0x40000u));
        published.store(current, std::memory_order_release);
        while (finished.load(std::memory_order_acquire) != Readers) std::this_thread::yield();
        if (current % 7u == 0u) ClearMultisampleTargets(context.device);
    }
    done.store(true, std::memory_order_release);
    for (auto& thread : readers) thread.join();
    ClearMultisampleTargets(context.device);
    Require(failures.load() == 0u, "a stress reader failed " + std::to_string(failures.load()) + " times");
    Require(stale.load() == 0u, "a stale certificate was accepted " + std::to_string(stale.load()) + " times after a registration");
    std::printf("stress: %u rounds of %u readers, no stale certificate accepted after a registration\n", Rounds, Readers);
}

struct TeardownReads {
    std::uint64_t guardAddress = 0;
    std::uint64_t clearAddress = 0;
    std::atomic<unsigned>* failures = nullptr;

    ~TeardownReads() {
        if (failures == nullptr) return;
        try {
            for (int round = 0; round < 3; ++round) {
                if (!ResidentGuardsClear_nid_no_patch(clearAddress, 64u)) failures->fetch_add(1, std::memory_order_relaxed);
                RequireNoMultisampleTarget(clearAddress, 64u, "a CPU read");
                if (ResidentGuardsClear_nid_no_patch(guardAddress, 64u)) failures->fetch_add(1, std::memory_order_relaxed);
                try {
                    RequireNoMultisampleColorTarget(guardAddress, 64u, "a CPU read");
                    failures->fetch_add(1, std::memory_order_relaxed);
                } catch (const std::runtime_error&) {
                }
            }
        } catch (...) {
            failures->fetch_add(1, std::memory_order_relaxed);
        }
    }
};

void CheckTeardown() {
    const auto context = FakeContext(0x7000u);
    const auto guardAddress = Base + 0x20000000ull;
    const auto clearAddress = Base + 0x28000000ull;
    ClearMultisampleTargets(context.device);
    GuardOneFragmentFmask(context, Fmask(guardAddress));
    std::atomic<unsigned> failures{0};
    for (int thread = 0; thread < 8; ++thread) {
        std::thread([&] {
            thread_local TeardownReads reads;
            reads.guardAddress = guardAddress;
            reads.clearAddress = clearAddress;
            reads.failures = &failures;
            if (!ResidentGuardsClear_nid_no_patch(clearAddress, 64u)) failures.fetch_add(1, std::memory_order_relaxed);
            if (!ResidentGuardsClear_nid_no_patch(clearAddress + 4096u, 64u)) failures.fetch_add(1, std::memory_order_relaxed);
        }).join();
    }
    ClearMultisampleTargets(context.device);
    Require(failures.load() == 0u, "a read during thread teardown failed " + std::to_string(failures.load()) + " times");
    std::puts("teardown: reads after a thread released its certificate are still checked exactly");
}

constexpr std::size_t BlockBytes = 1u << 20u;
constexpr std::uint32_t Side = 256;

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        data = VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE, PAGE_NOACCESS);
        Require(data != nullptr, "cannot reserve the guest block");
#else
        data = mmap(nullptr, BlockBytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Require(data != MAP_FAILED, "cannot reserve the guest block");
#endif
        Map();
    }

    ~GuestBlock() {
        if (mapped) Unmap();
#ifdef _WIN32
        VirtualFree(data, 0, MEM_RELEASE);
#else
        munmap(data, BlockBytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    void Map() {
#ifdef _WIN32
        Require(VirtualAlloc(data, BlockBytes, MEM_COMMIT, PAGE_READWRITE) == data, "cannot commit the guest block");
#else
        Require(mmap(data, BlockBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == data, "cannot map the guest block");
#endif
        GuestAllocations::Mutation().Add(data, BlockBytes, true, true, true);
        mapped = true;
    }

    void Unmap() {
        GuestAllocations::Mutation().Unmap(data, BlockBytes, [&](const void*, std::size_t, const void*, bool) {
#ifdef _WIN32
            Require(VirtualFree(data, BlockBytes, MEM_DECOMMIT) != 0, "cannot decommit the guest block");
#else
            Require(mmap(data, BlockBytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == data, "cannot unmap the guest block");
#endif
        });
        mapped = false;
    }

    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(data); }

private:
    void* data = nullptr;
    bool mapped = false;
};

ColorTarget Target(const GuestBlock& block) {
    ColorTarget target{};
    target.address = target.surfaceAddress = block.Address();
    target.extent = target.surfaceExtent = {Side, Side};
    target.format = VK_FORMAT_R8G8B8A8_UNORM;
    target.elementBytes = 4;
    target.samples = 4;
    target.tileMode = ColorTileMode::RenderTarget;
    target.bytes = ColorTargetLayout(Side, Side, target.tileMode, target.elementBytes).Bytes();
    return target;
}

bool CheckTargets() {
    const auto device = OpenVulkanTestDevice();
    if (!device) return false;
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(*device);
    if ((context.limits.framebufferColorSampleCounts & VK_SAMPLE_COUNT_4_BIT) == 0) return false;
    const auto settle = [&] {
        device->SubmitRecorded(true);
        device->WaitIdle();
    };
    const auto acquire = [&](const ColorTarget& target) {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        Require(MultisampleTargetView(context, target) != VK_NULL_HANDLE, "a multisampled target has no view");
    };
    const auto hostOnly = [&](std::uint64_t address) { return !HostOnlySampleRanges(address, 64u).empty(); };
    {
        GuestBlock block;
        const auto target = Target(block);
        const auto address = block.Address();
        Require(ResidentGuardsClear_nid_no_patch(address, 64u), "an unused block was not proven clear");
        const auto before = ResidentGuardGrowth_nid_no_patch();
        acquire(target);
        const auto created = ResidentGuardGrowth_nid_no_patch();
        Require(created > before, "creating a multisampled target did not advance the guard growth");
        Refuses([&] { RequireNoMultisampleTarget(address, 64u, "a CPU read"); }, "only on the host");
        acquire(target);
        acquire(target);
        Require(ResidentGuardGrowth_nid_no_patch() == created, "acquiring a resident host-only target again advanced the guard growth");
        Refuses([&] { AgcDriver::GuestMemory::GuardRead(address + 4096u, 64u); }, "only on the host");
        ClaimMultisampleTargetMemory(address, BlockBytes);
        RequireNoMultisampleTarget(address, 64u, "a CPU read");
        Require(!hostOnly(address), "a claimed target was not retired by an overlapping read");
        Require(ResidentGuardGrowth_nid_no_patch() == created, "retiring a target advanced the guard growth");
        Require(ResidentGuardsClear_nid_no_patch(address, 64u), "a retired range was not proven clear");
        acquire(target);
        Require(ResidentGuardGrowth_nid_no_patch() > created, "reusing a retired address did not advance the guard growth");
        Refuses([&] { RequireNoMultisampleTarget(address, 64u, "a CPU read"); }, "only on the host");
        settle();
        const auto beforeUnmap = ResidentGuardGrowth_nid_no_patch();
        block.Unmap();
        Require(ResidentGuardGrowth_nid_no_patch() == beforeUnmap, "an unmap advanced the guard growth");
        RequireNoMultisampleTarget(address, 64u, "a CPU read");
        Require(ResidentGuardsClear_nid_no_patch(address, 64u), "an unmapped target range was not proven clear");
        block.Map();
        acquire(target);
        Require(ResidentGuardGrowth_nid_no_patch() > beforeUnmap, "registering over a reused address did not advance the guard growth");
        Refuses([&] { RequireNoMultisampleTarget(address, 64u, "a CPU read"); }, "only on the host");
        settle();
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        ClearMultisampleTargets(context.device);
    }
    settle();
    std::puts("targets: creation and address reuse invalidate, repeated host-only acquires, retirement and unmaps keep certificates, claimed targets still retire");
    return true;
}

}

int main() {
    try {
        CheckHeadless();
        CheckStress();
        CheckTeardown();
        if (!CheckTargets()) std::puts("targets: skipped, no Vulkan device with 4x color targets");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
