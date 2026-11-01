#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTarget.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "VulkanTestDevice.hpp"
#include <sys/mman.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;
namespace GuestMemory = AgcDriver::GuestMemory;

constexpr std::uint64_t Base = 0x7c0000000000ull;
std::array<std::byte, 0x30000> imageStorage{};
struct alignas(64) Sink {
    volatile std::uint64_t value = 0;
};

std::array<Sink, 8> sinks{};

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

double Time(unsigned threads, std::uint64_t iterations, const std::function<void(unsigned, std::uint64_t)>& body) {
    std::atomic<bool> go{false};
    std::vector<double> ns(threads);
    std::vector<std::thread> pool;
    for (unsigned thread = 0; thread < threads; ++thread) {
        pool.emplace_back([&, thread] {
            while (!go.load(std::memory_order_acquire)) {}
            const auto start = std::chrono::steady_clock::now();
            for (std::uint64_t index = 0; index < iterations; ++index) body(thread, index);
            ns[thread] = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / static_cast<double>(iterations);
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& thread : pool) thread.join();
    double total = 0;
    for (const auto value : ns) total += value;
    return total / threads;
}

void Report(const char* name, unsigned threads, double ns) {
    std::printf("cost,%s,%u,%.1f\n", name, threads, ns);
}

}

int main() {
    const auto context = FakeContext(0xd000u);
    ClearMultisampleTargets(context.device);
    const auto aligned = (reinterpret_cast<std::uintptr_t>(imageStorage.data()) + 0xffffu) & ~std::uintptr_t{0xffff};
    auto* image = reinterpret_cast<std::byte*>(aligned);
    GuestAllocations::Mutation().Add(image, 0x20000, true, true, true);
    constexpr std::size_t MappedBytes = 0x20000;
    auto* mapped = static_cast<std::byte*>(mmap(nullptr, MappedBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    Require(mapped != MAP_FAILED, "cannot map the outside block");
    GuestAllocations::Mutation().Add(mapped, MappedBytes, true, true, true);
    const auto imageAddress = reinterpret_cast<std::uint64_t>(image);
    const auto mappedAddress = reinterpret_cast<std::uint64_t>(mapped);
    std::printf("cost,name,threads,ns_per_op\n");
    const auto unguarded = Time(1, 2000000, [&](unsigned thread, std::uint64_t index) { sinks[thread].value = RequireNoMultisampleTarget(imageAddress + (index & 63u) * 64u, 64u, "a CPU read"); });
    Report("RequireNoMultisampleTarget, no guard resident", 1, unguarded);
    for (std::uint64_t guard = 0; guard < 12u; ++guard) GuardOneFragmentFmask(context, Fmask(Base + guard * 0x100000u));
    for (const unsigned threads : {1u, 4u}) {
        Report("RequireNoMultisampleTarget, 12 guards, snapshot hit", threads, Time(threads, 2000000, [&](unsigned thread, std::uint64_t index) { sinks[thread].value = RequireNoMultisampleTarget(imageAddress + thread * 4096u + (index & 63u) * 64u, 64u, "a CPU read"); }));
        Report("ResidentGuardOverlaps, 12 guards, snapshot hit", threads, Time(threads, 2000000, [&](unsigned thread, std::uint64_t index) { sinks[thread].value = ResidentGuardOverlaps_nid_no_patch(imageAddress + thread * 4096u + (index & 63u) * 64u, 64u); }));
        Report("GuardRead, 12 guards, snapshot hit", threads, Time(threads, 2000000, [&](unsigned thread, std::uint64_t index) { GuestMemory::GuardRead(imageAddress + thread * 4096u + (index & 63u) * 64u, 64u); }));
        std::vector<GuestMemory::RangeProof> proofs(threads);
        Report("RequireNoMultisampleTargetProved, held proof", threads, Time(threads, 2000000, [&](unsigned thread, std::uint64_t) { sinks[thread].value = RequireNoMultisampleTargetProved(&proofs[thread], imageAddress + thread * 4096u, 64u, "a CPU read"); }));
        Report("ResidentGuardOverlaps, overlapping a guard (scan and exact)", threads, Time(threads, 200000, [&](unsigned thread, std::uint64_t index) { sinks[thread].value = ResidentGuardOverlaps_nid_no_patch(Base + (index % 12u) * 0x100000u + 64u, 64u); }));
    }
    GuestMemory::RangeProof stale;
    Require(RequireNoMultisampleTargetProved(&stale, imageAddress, 64u, "a CPU read"), "the stale proof range is not clear");
    const auto proved = ResidentGuardGrowth_nid_no_patch();
    for (std::uint64_t guard = 0; guard < 8u; ++guard) GuardOneFragmentFmask(context, Fmask(Base + 0x4000000u + guard * 0x100000u));
    const auto growth = ResidentGuardGrowth_nid_no_patch();
    Report("ResidentGuardsUnchangedSince, 8 registrations behind", 1, Time(1, 2000000, [&](unsigned thread, std::uint64_t) { sinks[thread].value = ResidentGuardsUnchangedSince_nid_no_patch(proved, imageAddress, 64u, growth); }));
    for (const std::size_t bytes : {std::size_t{64}, std::size_t{4096}}) {
        const auto suffix = std::string(", ") + std::to_string(bytes) + " bytes";
        Report(("CheckRange, page-cached image" + suffix).c_str(), 1, Time(1, 500000, [&](unsigned, std::uint64_t) { GuestMemory::CheckRange(image, bytes, 1); }));
        Report(("CheckRange, uncached mapping" + suffix).c_str(), 1, Time(1, 200000, [&](unsigned, std::uint64_t) { GuestMemory::CheckRange(mapped, bytes, 1); }));
        GuestMemory::RangeProof mappingProof;
        Report(("CheckRangeProved, held proof" + suffix).c_str(), 1, Time(1, 2000000, [&](unsigned, std::uint64_t) { GuestMemory::CheckRangeProved(&mappingProof, image, bytes, 1); }));
        Report(("FlushGpuWrites" + suffix).c_str(), 1, Time(1, 2000000, [&](unsigned, std::uint64_t) { GuestMemory::FlushGpuWrites(imageAddress, bytes); }));
        std::vector<std::byte> destination(bytes);
        Report(("GuestMemory::Read, page-cached image" + suffix).c_str(), 1, Time(1, 500000, [&](unsigned, std::uint64_t) { GuestMemory::Read(imageAddress, destination); }));
        Report(("GuestMemory::Read, uncached mapping" + suffix).c_str(), 1, Time(1, 200000, [&](unsigned, std::uint64_t) { GuestMemory::Read(mappedAddress, destination); }));
        Report(("memcpy" + suffix).c_str(), 1, Time(1, 2000000, [&](unsigned, std::uint64_t) { std::memcpy(destination.data(), image, bytes); sinks[0].value = static_cast<std::uint64_t>(destination[0]); }));
    }
    ClearMultisampleTargets(context.device);
    GuestAllocations::Mutation().Remove(mapped);
    GuestAllocations::Mutation().Remove(image);
    munmap(mapped, MappedBytes);
    return 0;
}
