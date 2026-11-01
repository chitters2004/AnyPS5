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
    Require(!ResidentGuardsClear_nid_no_patch(guardAddress, 64u), "a snapshot taken before a registration was accepted after it");
    Require(ResidentGuardOverlaps_nid_no_patch(guardAddress, 64u) == ColorGuardOverlap, "a read of a color guard was not classified as a color overlap only");
    Require(ResidentGuardOverlaps_nid_no_patch(distant, 64u) == 0u, "a distant read was classified as an overlap");
    Require(ResidentGuardOverlaps_nid_no_patch(std::numeric_limits<std::uint64_t>::max() - 15u, 32u) == (ColorGuardOverlap | DepthGuardOverlap), "an overflowing read was not sent to both exact paths");
    Refuses([&] { RequireNoMultisampleColorTarget(guardAddress, 64u, "a CPU read"); }, "FMASK");
    Refuses([&] { RequireNoMultisampleTarget(guardAddress + FmaskBytes - 1u, 1u, "a CPU read"); }, "FMASK");
    Refuses([&] { RequireNoMultisampleTarget(guardAddress - 64u, 65u, "a CPU read"); }, "FMASK");
    Require(ResidentGuardsClear_nid_no_patch(guardAddress - 64u, 64u), "the range ending at a guard was not proven clear");
    Refuses([&] { RequireNoMultisampleTarget(guardAddress - 64u, 128u, "a CPU read"); }, "FMASK");
    Require(ResidentGuardsClear_nid_no_patch(guardAddress + FmaskBytes, 1u), "the range starting at a guard end was not proven clear");
    Refuses([&] { RequireNoMultisampleTarget(guardAddress, 64u, "a shader buffer access", std::pair<std::uint64_t, std::size_t>{guardAddress, FmaskBytes}); }, "FMASK");
    RequireNoMultisampleTarget(distant, 64u, "a shader buffer access", std::pair<std::uint64_t, std::size_t>{distant, 64u});
    Require(!ResidentGuardsClear_nid_no_patch(std::numeric_limits<std::uint64_t>::max() - 15u, 32u), "an overflowing range was proven clear");
    Refuses([&] { RequireNoMultisampleColorTarget(std::numeric_limits<std::uint64_t>::max() - 15u, 32u, "a CPU read"); }, "overflows");
    Require(ResidentGuardsClear_nid_no_patch(distant, 4096u), "a distant range was not proven clear after a registration");
    GuardOneFragmentFmask(context, Fmask(guardAddress));
    Require(ResidentGuardGrowth_nid_no_patch() == registered, "guarding an already guarded FMASK advanced the guard growth");
    WithoutSharedWrites([&] {
        for (int round = 0; round < 1000; ++round) {
            Require(ResidentGuardsClear_nid_no_patch(distant + round, 64u), "a range outside the snapshot was not proven clear");
            RequireNoMultisampleColorTarget(distant + round, 64u, "a CPU read");
            RequireNoMultisampleTarget(distant + round, 64u, "a CPU read");
            AgcDriver::GuestMemory::GuardRead(distant + round, 64u);
        }
    });
    ClearMultisampleTargets(context.device);
    Require(ResidentGuardGrowth_nid_no_patch() == registered, "removing guards advanced the guard growth");
    WithoutSharedWrites([&] { Require(ResidentGuardsClear_nid_no_patch(distant, 64u), "a range outside the snapshot was not proven clear after a removal"); });
    RequireNoMultisampleTarget(guardAddress, 64u, "a CPU read");
    std::puts("headless: registration is seen, repeated and removed guards keep the snapshot, hits write nothing in the driver image");
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
            ResidentGuardProof proof;
            AgcDriver::GuestMemory::RangeProof rangeProof;
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
                    const auto growth = ResidentGuardGrowth_nid_no_patch();
                    const bool proved = proof.Holds(growth, current) || (ResidentGuardsClear_nid_no_patch(address + 128u, 64u) && (proof.Record(growth, current), true));
                    if (after && proved) stale.fetch_add(1, std::memory_order_relaxed);
                    try {
                        if (RequireNoMultisampleTargetProved(&rangeProof, address + 192u, 64u, "a vertex buffer read") && after) stale.fetch_add(1, std::memory_order_relaxed);
                    } catch (const std::runtime_error&) {
                    }
                    if (current > 8u) {
                        try {
                            RequireNoMultisampleColorTarget(Base + 0x10000000ull + static_cast<std::uint64_t>(current - 8u) * 0x40000u, 64u, "a CPU read");
                        } catch (const std::runtime_error&) {
                        }
                    }
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
    Require(stale.load() == 0u, "a stale snapshot was accepted " + std::to_string(stale.load()) + " times after a registration");
    std::printf("stress: %u rounds of %u readers with concurrent rebuilds and proofs, no stale snapshot or proof accepted after a registration\n", Rounds, Readers);
}

void CheckStaleRebuild() {
    const auto context = FakeContext(0x8000u);
    const auto retired = Base + 0x30000000ull;
    ClearMultisampleTargets(context.device);
    GuardOneFragmentFmask(context, Fmask(retired));
    Refuses([&] { RequireNoMultisampleTarget(retired, 64u, "a CPU read"); }, "FMASK");
    ClearMultisampleTargets(context.device);
    for (int round = 0; round < 256; ++round) {
        Require(ResidentGuardsClear_nid_no_patch(retired + round, 64u), "a removed range was not proven clear");
        RequireNoMultisampleTarget(retired + round, 64u, "a CPU read");
    }
    WithoutSharedWrites([&] {
        for (int round = 0; round < 1000; ++round) {
            Require(ResidentGuardsClear_nid_no_patch(retired + round, 64u), "a removed range was not proven clear after a rebuild");
            RequireNoMultisampleTarget(retired + round, 64u, "a CPU read");
        }
    });
    std::puts("stale: reads of removed guards rebuild the snapshot, after which they write nothing in the driver image");
}

void CheckOverflow() {
    const auto context = FakeContext(0x9000u);
    const auto first = Base + 0x40000000ull;
    const auto distant = Base + 0x60000000ull;
    constexpr std::uint64_t Count = 300;
    ClearMultisampleTargets(context.device);
    const auto at = [&](std::uint64_t index) { return first + index * 2u * FmaskBytes; };
    for (std::uint64_t index = 0; index < Count; ++index) GuardOneFragmentFmask(context, Fmask(at(index)));
    for (const std::uint64_t index : {std::uint64_t{0}, Count / 2u, Count - 1u}) {
        Refuses([&] { RequireNoMultisampleTarget(at(index) + 64u, 64u, "a CPU read"); }, "FMASK");
        Require(ResidentGuardOverlaps_nid_no_patch(at(index), 1u) == ColorGuardOverlap, "an overflowing snapshot lost a color guard");
    }
    for (std::uint64_t index = 0; index < Count; ++index) Require(ResidentGuardsClear_nid_no_patch(at(index) + FmaskBytes, FmaskBytes), "a gap between guards was not proven clear while the snapshot overflowed");
    Require(ResidentGuardsClear_nid_no_patch(distant, 64u), "a distant range was not proven clear while the snapshot overflowed");
    ClearMultisampleTargets(context.device);
    for (int round = 0; round < 256; ++round) Require(ResidentGuardsClear_nid_no_patch(at(round), 64u), "a removed range was not proven clear after an overflow");
    WithoutSharedWrites([&] {
        for (int round = 0; round < 1000; ++round) Require(ResidentGuardsClear_nid_no_patch(distant + round, 64u), "a distant range was not proven clear after the snapshot recovered");
    });
    std::puts("overflow: an overflowing snapshot sends reads to the exact path and recovers after removals");
}

void CheckProof() {
    const auto context = FakeContext(0xa000u);
    const auto guardAddress = Base + 0x50000000ull;
    ClearMultisampleTargets(context.device);
    ResidentGuardProof proof;
    const auto start = ResidentGuardGrowth_nid_no_patch();
    Require(!proof.Holds(start, 1u), "an empty proof held");
    Require(RequireNoMultisampleTarget(guardAddress, 64u, "a CPU read"), "an unguarded read was not reported clear");
    proof.Record(start, 1u);
    Require(proof.Holds(start, 1u), "a recorded proof did not hold");
    Require(!proof.Holds(start, 2u), "a proof held for another key");
    Require(!proof.Holds(start, std::uint64_t{1} << ResidentGuardProof::KeyBits), "a proof held for a key wider than its bits");
    Require(!proof.Holds(std::uint64_t{1} << (64u - ResidentGuardProof::KeyBits), 1u), "a proof held for a growth wider than its bits");
    GuardOneFragmentFmask(context, Fmask(guardAddress));
    const auto registered = ResidentGuardGrowth_nid_no_patch();
    Require(registered >= start + 2u, "a registration did not advance the growth when it started and when it finished");
    Require(!proof.Holds(registered, 1u), "a proof held after a registration");
    Refuses([&] { RequireNoMultisampleTarget(guardAddress + FmaskBytes - 64u, 64u, "a CPU read"); }, "FMASK");
    proof.Record(registered, 1u);
    ClearMultisampleTargets(context.device);
    Require(proof.Holds(ResidentGuardGrowth_nid_no_patch(), 1u), "removing guards invalidated a proof");
    WithoutSharedWrites([&] {
        for (int round = 0; round < 1000; ++round) Require(proof.Holds(ResidentGuardGrowth_nid_no_patch(), 1u), "a held proof stopped holding");
    });
    std::puts("proof: a recorded proof holds until a registration, survives removals, and checking it writes nothing in the driver image");
}

void CheckRangeProof() {
    const auto context = FakeContext(0xb000u);
    const auto guardAddress = Base + 0x58000000ull;
    const auto clearAddress = Base + 0x5c000000ull;
    ClearMultisampleTargets(context.device);
    AgcDriver::GuestMemory::RangeProof proof;
    const auto start = ResidentGuardGrowth_nid_no_patch();
    Require(!proof.Holds(clearAddress, 64u, start), "an empty range proof held");
    Require(RequireNoMultisampleTargetProved(&proof, clearAddress, 64u, "a vertex buffer read"), "a clear range was not proven");
    Require(proof.Holds(clearAddress, 64u, start), "a proven range did not hold");
    Require(!proof.Holds(clearAddress, 65u, start) && !proof.Holds(clearAddress + 1u, 64u, start), "a range proof held for another range");
    GuardOneFragmentFmask(context, Fmask(clearAddress));
    Require(!proof.Holds(clearAddress, 64u, ResidentGuardGrowth_nid_no_patch()), "a range proof held after a registration over it");
    Refuses([&] { RequireNoMultisampleTargetProved(&proof, clearAddress, 64u, "a vertex buffer read"); }, "FMASK");
    GuardOneFragmentFmask(context, Fmask(guardAddress));
    Refuses([&] { RequireNoMultisampleTargetProved(&proof, guardAddress, 64u, "an index buffer read"); }, "FMASK");
    Require(!proof.Holds(guardAddress, 64u, ResidentGuardGrowth_nid_no_patch()), "a refused range was proven");
    ClearMultisampleTargets(context.device);
    Require(RequireNoMultisampleTargetProved(&proof, guardAddress, 64u, "an index buffer read"), "a removed range was not proven");
    WithoutSharedWrites([&] {
        for (int round = 0; round < 1000; ++round) Require(RequireNoMultisampleTargetProved(&proof, guardAddress, 64u, "an index buffer read"), "a held range proof did not hold");
    });
    std::atomic<bool> done{false};
    std::atomic<unsigned> mixed{0};
    std::vector<std::thread> threads;
    const auto growth = ResidentGuardGrowth_nid_no_patch();
    for (unsigned writer = 0; writer < 2; ++writer) {
        threads.emplace_back([&, writer] {
            for (unsigned round = 0; round < 2000000u; ++round) proof.Record(writer == 0 ? 0x1000u : 0x2000u, writer == 0 ? 0x10u : 0x20u, growth);
        });
    }
    for (unsigned reader = 0; reader < 2; ++reader) {
        threads.emplace_back([&] {
            while (!done.load(std::memory_order_acquire)) {
                if (proof.Holds(0x1000u, 0x20u, growth) || proof.Holds(0x2000u, 0x10u, growth)) mixed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    threads[0].join();
    threads[1].join();
    done.store(true, std::memory_order_release);
    for (std::size_t index = 2; index < threads.size(); ++index) threads[index].join();
    Require(mixed.load() == 0u, "a range proof held for a range assembled from two writers " + std::to_string(mixed.load()) + " times");
    std::puts("range proof: an exact range holds until a registration, refused ranges are never proven, concurrent writers never mix a range");
}

void CheckGuardLog() {
    const auto context = FakeContext(0xc000u);
    const auto watched = Base + 0x68000000ull;
    const auto elsewhere = Base + 0x6c000000ull;
    ClearMultisampleTargets(context.device);
    const auto proved = ResidentGuardGrowth_nid_no_patch();
    Require(ResidentGuardsUnchangedSince_nid_no_patch(proved, watched, 64u, proved), "a proof was not unchanged at its own growth");
    Require(!ResidentGuardsUnchangedSince_nid_no_patch(0u, watched, 64u, proved), "a missing proof was unchanged");
    GuardOneFragmentFmask(context, Fmask(elsewhere));
    auto growth = ResidentGuardGrowth_nid_no_patch();
    Require(growth > proved, "a registration did not advance the growth");
    Require(ResidentGuardsUnchangedSince_nid_no_patch(proved, watched, 64u, growth), "a registration elsewhere invalidated a proof");
    Require(!ResidentGuardsUnchangedSince_nid_no_patch(proved, elsewhere + FmaskBytes - 1u, 2u, growth), "a registration over a proven range left it unchanged");
    Require(!ResidentGuardsUnchangedSince_nid_no_patch(growth, watched, 64u, proved), "a proof newer than the growth was unchanged");
    AgcDriver::GuestMemory::RangeProof proof;
    Require(RequireNoMultisampleTargetProved(&proof, watched, 64u, "a vertex buffer read"), "a clear range was not proven");
    const auto stamped = ResidentGuardGrowth_nid_no_patch();
    GuardOneFragmentFmask(context, Fmask(elsewhere + 4u * FmaskBytes));
    Require(proof.Stamp(watched, 64u) == stamped && !proof.Holds(watched, 64u, ResidentGuardGrowth_nid_no_patch()), "a registration did not leave the proof behind the growth");
    Require(RequireNoMultisampleTargetProved(&proof, watched, 64u, "a vertex buffer read"), "a proof was not revalidated after a registration elsewhere");
    Require(proof.Holds(watched, 64u, ResidentGuardGrowth_nid_no_patch()), "a revalidated proof was not advanced to the growth");
    GuardOneFragmentFmask(context, Fmask(watched));
    Refuses([&] { RequireNoMultisampleTargetProved(&proof, watched, 64u, "a vertex buffer read"); }, "FMASK");
    ClearMultisampleTargets(context.device);
    Require(RequireNoMultisampleTargetProved(&proof, watched, 64u, "a vertex buffer read"), "a removed range was not proven");
    const auto beforeFlood = ResidentGuardGrowth_nid_no_patch();
    for (std::uint64_t index = 0; index < 80u; ++index) GuardOneFragmentFmask(context, Fmask(elsewhere + (index + 8u) * 2u * FmaskBytes));
    Require(!ResidentGuardsUnchangedSince_nid_no_patch(beforeFlood, watched, 64u, ResidentGuardGrowth_nid_no_patch()), "a proof older than the registration log was unchanged");
    Require(RequireNoMultisampleTargetProved(&proof, watched, 64u, "a vertex buffer read"), "a range behind a full registration log was not proven by the full check");
    ClearMultisampleTargets(context.device);
    std::puts("guard log: registrations elsewhere keep proofs, overlapping ones and an overflowing log do not");
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
    std::puts("teardown: reads during thread teardown are still checked");
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
    std::puts("targets: creation and address reuse are seen, repeated host-only acquires do not grow, retired and unmapped ranges are proven clear, claimed targets still retire");
    return true;
}

}

int main() {
    try {
        CheckHeadless();
        CheckStress();
        CheckTeardown();
        CheckStaleRebuild();
        CheckProof();
        CheckRangeProof();
        CheckGuardLog();
        CheckOverflow();
        if (!CheckTargets()) std::puts("targets: skipped, no Vulkan device with 4x color targets");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
