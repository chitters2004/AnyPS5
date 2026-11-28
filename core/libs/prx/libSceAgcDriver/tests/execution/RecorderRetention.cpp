#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#ifdef APS5_AGC_ALLOCATION_COUNT
#include "../AllocationCounter.hpp"
#endif
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace Graphics = AgcDriver::Graphics;
using Graphics::Require;

std::vector<std::string> failures;

struct Hold {
    Hold(const Graphics::Context& context) : context(context) {
        try {
            signal = context.Function<PFN_vkSignalSemaphore>("vkSignalSemaphore");
        } catch (const std::runtime_error&) {
            signal = context.Function<PFN_vkSignalSemaphoreKHR>("vkSignalSemaphoreKHR");
        }
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        const VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        Require(context.Function<PFN_vkCreateSemaphore>("vkCreateSemaphore")(context.device, &info, nullptr, &semaphore) == VK_SUCCESS, "the device has no timeline semaphores to hold a batch on the GPU");
    }
    ~Hold() {
        Release();
        static_cast<void>(context.Function<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(context.device));
        context.Function<PFN_vkDestroySemaphore>("vkDestroySemaphore")(context.device, semaphore, nullptr);
    }
    void Release() {
        if (released.exchange(true)) return;
        VkSemaphoreSignalInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        signalInfo.semaphore = semaphore;
        signalInfo.value = 1;
        static_cast<void>(signal(context.device, &signalInfo));
    }
    const Graphics::Context& context;
    PFN_vkSignalSemaphore signal = nullptr;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    std::atomic<bool> released{false};
};

struct Outcome {
    bool threw = false;
    bool executed = false;
    bool lostWhileHeld = false;
    bool failed = false;
};

void RecordMarker(const Graphics::Context& context, Graphics::Recorder& recorder, Graphics::Buffer& marker) {
    auto* commands = recorder.Commands();
    context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, marker.Handle(), 0, 4, 0x5a5a5a5au);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

bool Executed(Graphics::Buffer& marker) {
    marker.Invalidate();
    std::uint32_t value = 0;
    std::memcpy(&value, marker.Bytes().data(), sizeof(value));
    return value == 0x5a5a5a5au;
}

Outcome HeldSubmit(const Graphics::Context& context, std::size_t failAfter, bool failSync) {
    Outcome outcome;
    Hold hold(context);
    Graphics::Buffer marker(context, 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    std::memset(marker.Bytes().data(), 0, 4);
    std::weak_ptr<int> watch;
    {
        Graphics::Recorder recorder(context, true);
        auto sentinel = std::make_shared<int>(0);
        watch = sentinel;
        recorder.Keep(std::move(sentinel));
        RecordMarker(context, recorder, marker);
        recorder.HoldNextSubmit(hold.semaphore, 1);
        std::jthread releaser;
        if (failSync) {
            recorder.Submit();
            releaser = std::jthread([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                hold.Release();
            });
        }
#ifdef APS5_AGC_ALLOCATION_COUNT
        FailAllocationsAfter_nid_no_patch(failAfter);
#endif
        try {
            if (failSync) recorder.Sync();
            else recorder.Submit();
        } catch (const std::exception&) {
            outcome.threw = true;
        }
#ifdef APS5_AGC_ALLOCATION_COUNT
        outcome.failed = !AllocationFailurePending_nid_no_patch();
        StopAllocationFailures_nid_no_patch();
#endif
        outcome.lostWhileHeld = watch.expired() && !hold.released.load();
        if (releaser.joinable()) releaser.join();
        hold.Release();
        static_cast<void>(context.Function<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(context.device));
        outcome.executed = Executed(marker);
        try {
            recorder.Sync();
        } catch (const std::exception& error) {
            failures.push_back(std::string("the recorder did not drain after the case: ") + error.what());
        }
        recorder.FlushDeferredReleases();
    }
    if (!watch.expired()) failures.push_back("the kept object outlived its recorder (failing from allocation " + std::to_string(failAfter) + ")");
    return outcome;
}

void LongParkedChain(const Graphics::Context& context) {
    constexpr std::size_t Discards = 200000;
    const std::string what = "a batch held on the GPU with " + std::to_string(Discards) + " failed submits parked behind it, each keeping a zero-byte object";
    Hold hold(context);
    std::size_t destroyed = 0;
    std::size_t discarded = 0;
    std::size_t destroyedWhileHeld = 0;
    {
        Graphics::Recorder recorder(context, true);
        recorder.Keep(std::make_shared<int>(0));
        recorder.HoldNextSubmit(hold.semaphore, 1);
        recorder.Submit();
        Require(recorder.InFlightBatches() == 1u, what + ": the held batch is not in flight");
        for (std::size_t index = 0; index < Discards; ++index) {
            recorder.Keep(std::shared_ptr<int>(new int(0), [&destroyed](int* object) {
                delete object;
                ++destroyed;
            }));
            Graphics::Recorder::FailNextSubmitsForTest(1);
            try {
                recorder.Submit();
            } catch (const std::runtime_error& error) {
                if (std::strstr(error.what(), "injected failure") != nullptr && !recorder.Recording()) ++discarded;
            }
        }
        Graphics::Recorder::FailNextSubmitsForTest(0);
        destroyedWhileHeld = destroyed;
        hold.Release();
        recorder.Sync();
        recorder.FlushDeferredReleases();
    }
    if (discarded != Discards) failures.push_back(what + ": " + std::to_string(discarded) + " submits were discarded");
    if (destroyedWhileHeld != 0u) failures.push_back(what + ": " + std::to_string(destroyedWhileHeld) + " kept objects were destroyed while the batch was held");
    if (destroyed != Discards) failures.push_back(what + ": " + std::to_string(destroyed) + " kept objects were destroyed once the held batch finished");
    std::printf("%s: %zu discarded, %zu destroyed while held, %zu destroyed after it finished\n", what.c_str(), discarded, destroyedWhileHeld, destroyed);
}

void Sweep(const Graphics::Context& context, bool failSync) {
    const std::string what = failSync ? "a sync finishing a batch held on the GPU" : "a submit of a batch held on the GPU";
#ifdef APS5_AGC_ALLOCATION_COUNT
    std::size_t failedAfterSubmit = 0;
    std::size_t sweep = 0;
    for (std::size_t failAfter = 0;; ++failAfter, ++sweep) {
        Require(failAfter < 4096, what + ": the allocation sweep did not end");
        const auto outcome = HeldSubmit(context, failAfter, failSync);
        if (!outcome.failed) break;
        const auto at = what + ", failing allocation " + std::to_string(failAfter);
        if (outcome.executed && outcome.lostWhileHeld) failures.push_back(at + ": the kept object was destroyed while the batch was still held on the GPU");
        if (outcome.executed && outcome.threw) ++failedAfterSubmit;
        if (!failSync && !outcome.executed && !outcome.threw) failures.push_back(at + ": the submit neither ran the batch nor reported the failure");
    }
    Require(sweep != 0, what + ": no allocation was made");
    std::printf("%s: %zu allocations failed one at a time, %zu threw after the batch reached the GPU\n", what.c_str(), sweep, failedAfterSubmit);
#else
    const auto outcome = HeldSubmit(context, 0, failSync);
    if (outcome.lostWhileHeld || !outcome.executed) failures.push_back(what + ": the batch did not run while keeping its objects");
    std::printf("%s: skipped the allocation sweep, the driver's allocator is wrapped only in MinGW test builds\n", what.c_str());
#endif
}

}

int main() {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        const auto context = device->graphicsContext();
        Sweep(context, false);
        Sweep(context, true);
        LongParkedChain(context);
        for (const auto& failure : failures) std::cerr << failure << '\n';
        return failures.empty() ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
