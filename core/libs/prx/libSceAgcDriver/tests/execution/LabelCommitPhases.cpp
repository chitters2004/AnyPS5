#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/LabelPhases.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <numeric>
#include <span>
#include <string>
#include <thread>

namespace {

using AgcDriver::Graphics::Require;
namespace Phases = AgcDriver::LabelPhases;

constexpr std::size_t BlockBytes = 65536;

struct Block {
    Block() {
#ifdef _WIN32
        bytes = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        bytes = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(bytes != nullptr, "label phases: cannot allocate a host block");
        std::memset(bytes, 0, BlockBytes);
        GuestAllocations::Mutation().Add(bytes, BlockBytes, true, true, true);
    }
    ~Block() {
        GuestAllocations::Mutation().Remove(bytes);
#ifdef _WIN32
        VirtualFree(bytes, 0, MEM_RELEASE);
#else
        std::free(bytes);
#endif
    }
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(bytes)); }
    std::uint8_t* bytes = nullptr;
};

int Store(AgcDriver::VulkanDevice& device, std::uint64_t address, std::uint32_t value, std::uint64_t stamp) {
    return device.WriteLabelOnGpu(address, std::as_bytes(std::span(&value, 1)), stamp, 0);
}

int CommitShaped(AgcDriver::VulkanDevice& device, std::uint64_t address, std::uint32_t value, std::uint64_t stamp) {
    Phases::Begin();
    const int reason = Store(device, address, value, stamp);
    Phases::Mark(Phases::Phase::Record);
    Phases::Mark(Phases::Phase::Notes);
    Phases::End();
    return reason;
}

void RequireSplit(const Phases::Totals& totals, std::uint64_t labels) {
    const std::array<const char*, Phases::PhaseCount> names{"pre", "reap", "lookup", "record", "notes", "tail"};
    Require(totals.labels == labels, ("label phases: " + std::to_string(totals.labels) + " labels counted instead of " + std::to_string(labels)).c_str());
    for (std::size_t i = 0; i < Phases::PhaseCount; ++i) {
        Require(totals.counts[i] == labels, ("label phases: phase " + std::string(names[i]) + " counted " + std::to_string(totals.counts[i]) + " times for " + std::to_string(labels) + " labels").c_str());
    }
    const auto summed = std::accumulate(totals.ns.begin(), totals.ns.end(), std::uint64_t{0});
    Require(summed == totals.holdNs, ("label phases: the phases sum to " + std::to_string(summed) + " ns, the hold to " + std::to_string(totals.holdNs) + " ns").c_str());
    Require(totals.holdNs > 0, "label phases: a label held for no time");
    const auto drawn = static_cast<std::size_t>(Phases::PreSlot::DrawnLinear);
    const auto readback = static_cast<std::size_t>(Phases::PreSlot::Readback);
    Require(totals.preCalls[drawn] == labels && totals.preCalls[readback] == labels, ("label phases: pre detail timed " + std::to_string(totals.preCalls[drawn]) + " drawn-linear and " + std::to_string(totals.preCalls[readback]) + " readback calls for " + std::to_string(labels) + " labels").c_str());
    const auto pre = totals.ns[static_cast<std::size_t>(Phases::Phase::Pre)];
    Require(totals.preNs[drawn] + totals.preNs[readback] <= pre, ("label phases: pre detail " + std::to_string(totals.preNs[drawn] + totals.preNs[readback]) + " ns exceeds the pre phase " + std::to_string(pre) + " ns").c_str());
}

}

int main() {
    try {
        Phases::SetEnabled(true);
        static_cast<void>(Phases::Take());
        Block labels;
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        const std::array<std::uint32_t, 4> pattern{1u, 2u, 3u, 4u};
        if (!device->FillBuffer(labels.Address() + 4096, 4096, pattern)) {
            std::printf("skipped, the device does not import guest memory\n");
            return VulkanTestSkipped;
        }
        device->SubmitRecorded(false);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::memcmp(labels.bytes + 4096, pattern.data(), sizeof(pattern)) != 0) {
            Require(std::chrono::steady_clock::now() < deadline, "label phases: the submitted fill never landed");
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        const std::uint32_t opening = 0x5a5a0000u;
        Require(device->WriteLabelOnGpu(labels.Address(), std::as_bytes(std::span(&opening, 1)), 100, 0, false) == 0, "label phases: the label opening a batch behind the finished one was not stored on the GPU");
        Require(CommitShaped(*device, labels.Address() + 4, 0x5a5a0001u, 1) == 0, "label phases: the label was not stored on the GPU");
        Require(Store(*device, labels.Address() + 8, 0x5a5a0002u, 2) == 0, "label phases: the unframed label was not stored on the GPU");
        Phases::CountReapTry(Phases::ReapTry::Failed, 3000);
        Phases::CountReapTry(Phases::ReapTry::Reaped, 5000000);
        const auto totals = Phases::Take();
        RequireSplit(totals, 1);
        Require(totals.labelReaps == 1 && totals.labelReapsWithWork == 1 && totals.labelReapRetired == 1 && totals.labelReapInFlight == 1, ("label phases: the in-label reap over one finished batch counted " + std::to_string(totals.labelReaps) + " reaps, " + std::to_string(totals.labelReapsWithWork) + " with work, " + std::to_string(totals.labelReapRetired) + " retired of " + std::to_string(totals.labelReapInFlight) + " in flight").c_str());
        const auto line = Phases::Line(totals);
        const bool shaped = line.find("1 labels, held ") != std::string::npos && line.find(" + lookup ") != std::string::npos && line.find(" + tail ") != std::string::npos && line.find("1 reaped (5.00 ms), 1 failed (0.00 ms)") != std::string::npos && line.find("; pre detail: drawn-linear ") != std::string::npos && line.find(" readback ") != std::string::npos && line.find("; in-label reaps: 1, 1 retired a batch (1 batches), 1 batches in flight before them") != std::string::npos;
        Require(shaped, ("label phases: unexpected report line: " + line).c_str());
        Require(Phases::Take().labels == 0, "label phases: a taken window was counted again");
        Phases::SetEnabled(false);
        Require(CommitShaped(*device, labels.Address() + 12, 0x5a5a0003u, 3) == 0, "label phases: the disabled label was not stored on the GPU");
        const auto disabled = Phases::Take();
        Require(disabled.labelReaps == 0, "label phases: an in-label reap was counted while disabled");
        Require(disabled.preCalls[0] == 0 && disabled.preCalls[1] == 0, "label phases: pre detail was timed while disabled");
        Require(disabled.labels == 0 && disabled.holdNs == 0 && std::accumulate(disabled.counts.begin(), disabled.counts.end(), std::uint64_t{0}) == 0, "label phases: a label was timed while disabled");
        device->SubmitRecorded(true);
        device->WaitIdle();
        std::fputs(line.c_str(), stdout);
        std::puts("label phases tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
