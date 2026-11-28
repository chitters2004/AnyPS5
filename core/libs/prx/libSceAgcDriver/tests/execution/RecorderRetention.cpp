#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#ifdef APS5_AGC_ALLOCATION_COUNT
#include "../AllocationCounter.hpp"
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace Graphics = AgcDriver::Graphics;
using Graphics::Require;

std::vector<std::string> failures;

void Fail(std::string failure) {
    failures.push_back(std::move(failure));
}

constexpr std::uint64_t HeldWrite = 0x7f0000000000ull;

struct Hold {
    explicit Hold(const Graphics::Context& context) : context(context) {
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
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
    void Release() {
        std::lock_guard lock(mutex);
        if (released) return;
        VkSemaphoreSignalInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        signalInfo.semaphore = semaphore;
        signalInfo.value = 1;
        if (signal(context.device, &signalInfo) != VK_SUCCESS) std::terminate();
        released = true;
    }
    const Graphics::Context& context;
    PFN_vkSignalSemaphore signal = nullptr;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    std::mutex mutex;
    bool released = false;
};

struct ReleaseOnExit {
    Hold& hold;
    ~ReleaseOnExit() { hold.Release(); }
};

template <typename Case>
void Timed(const char* name, Case&& run) {
    const auto start = std::chrono::steady_clock::now();
    run();
    std::printf("%s: %.0f ms\n", name, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
}

std::weak_ptr<int> KeepSentinel(Graphics::Recorder& recorder, std::size_t bytes = 0) {
    auto sentinel = std::make_shared<int>(0);
    std::weak_ptr<int> watch = sentinel;
    recorder.Keep(std::move(sentinel), bytes);
    return watch;
}

void RecordMarker(const Graphics::Context& context, Graphics::Recorder& recorder, Graphics::Buffer& marker) {
    auto* commands = recorder.Commands();
    context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, marker.Handle(), 0, 4, 0x5a5a5a5au);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

struct Marker {
    explicit Marker(const Graphics::Context& context) : buffer(context, 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT) { std::memset(buffer.Bytes().data(), 0, 4); }
    bool Executed() {
        buffer.Invalidate();
        std::uint32_t value = 0;
        std::memcpy(&value, buffer.Bytes().data(), sizeof(value));
        return value == 0x5a5a5a5au;
    }
    Graphics::Buffer buffer;
};

struct Outcome {
    bool threw = false;
    bool executed = false;
    bool lostWhileHeld = false;
    bool failed = false;
};

void ArmAllocationFailure(std::size_t allocations) {
#ifdef APS5_AGC_ALLOCATION_COUNT
    FailAllocationsAfter_nid_no_patch(allocations);
#else
    static_cast<void>(allocations);
#endif
}

bool DisarmAllocationFailure() {
#ifdef APS5_AGC_ALLOCATION_COUNT
    const bool failed = !AllocationFailurePending_nid_no_patch();
    StopAllocationFailures_nid_no_patch();
    return failed;
#else
    return false;
#endif
}

void Drain(Graphics::Recorder& recorder, const std::string& what) {
    try {
        recorder.Sync();
    } catch (const std::exception& error) {
        Fail(what + ": the recorder did not drain after the case: " + error.what());
    }
    recorder.FlushDeferredReleases();
}

Outcome HeldSubmit(const Graphics::Context& context, std::size_t failAfter) {
    Outcome outcome;
    Hold hold(context);
    Marker marker(context);
    std::weak_ptr<int> watch;
    {
        Graphics::Recorder recorder(context, true);
        ReleaseOnExit release{hold};
        recorder.Activate();
        watch = KeepSentinel(recorder);
        recorder.NotePendingWrite(HeldWrite, 256);
        RecordMarker(context, recorder, marker.buffer);
        recorder.HoldNextSubmit(hold.semaphore, 1);
        ArmAllocationFailure(failAfter);
        try {
            recorder.Submit();
        } catch (const std::exception&) {
            outcome.threw = true;
        }
        outcome.failed = DisarmAllocationFailure();
        outcome.lostWhileHeld = watch.expired();
        hold.Release();
        static_cast<void>(context.Function<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(context.device));
        outcome.executed = marker.Executed();
        Drain(recorder, "a held submit");
    }
    if (!watch.expired()) Fail("a held submit, failing allocation " + std::to_string(failAfter) + ": the kept object outlived its recorder");
    return outcome;
}

void SubmitSweep(const Graphics::Context& context) {
    const std::string what = "a submit of a batch held on the GPU";
#ifdef APS5_AGC_ALLOCATION_COUNT
    std::size_t cases = 0;
    std::size_t failedAfterSubmit = 0;
    for (std::size_t failAfter = 0;; ++failAfter) {
        Require(failAfter < 4096, what + ": the allocation sweep did not end");
        const auto outcome = HeldSubmit(context, failAfter);
        if (!outcome.failed) break;
        ++cases;
        const auto at = what + ", failing allocation " + std::to_string(failAfter);
        if (outcome.executed && outcome.lostWhileHeld) Fail(at + ": the kept object was destroyed while the batch was still held on the GPU");
        if (outcome.executed && outcome.threw) ++failedAfterSubmit;
        if (!outcome.executed && !outcome.threw) Fail(at + ": the submit neither ran the batch nor reported the failure");
    }
    if (cases == 0) Fail(what + ": the submit made no allocation to fail");
    if (failedAfterSubmit != 0) Fail(what + ": " + std::to_string(failedAfterSubmit) + " failed allocations threw after the batch reached the GPU");
    std::printf("%s: %zu allocations failed one at a time, %zu threw after the batch reached the GPU\n", what.c_str(), cases, failedAfterSubmit);
#else
    const auto outcome = HeldSubmit(context, 0);
    if (outcome.lostWhileHeld || !outcome.executed) Fail(what + ": the batch did not run while keeping its objects");
    std::printf("%s: skipped the allocation sweep, the driver's allocator is wrapped only in MinGW test builds\n", what.c_str());
#endif
}

#ifdef APS5_AGC_ALLOCATION_COUNT
struct Growth {
    std::size_t submit = 0;
    std::size_t allocations = 0;
};

Growth FindInFlightGrowth(const Graphics::Context& context) {
    Hold hold(context);
    Growth growth;
    {
        Graphics::Recorder recorder(context, true);
        ReleaseOnExit release{hold};
        std::size_t steady = 0;
        for (std::size_t submit = 1; submit <= 1024 && growth.submit == 0; ++submit) {
            KeepSentinel(recorder);
            recorder.HoldNextSubmit(hold.semaphore, 1);
            BeginAllocationCount_nid_no_patch();
            recorder.Submit();
            const auto made = EndAllocationCount_nid_no_patch();
            if (submit == 2) steady = made;
            if (submit > 2 && made > steady) growth = {submit, made};
        }
        hold.Release();
        Drain(recorder, "the in-flight growth probe");
    }
    return growth;
}

Outcome GrowingSubmit(const Graphics::Context& context, std::size_t submits, std::size_t failAfter, std::size_t& earlierLost) {
    Outcome outcome;
    Hold hold(context);
    Marker marker(context);
    std::vector<std::weak_ptr<int>> earlier;
    std::weak_ptr<int> watch;
    {
        Graphics::Recorder recorder(context, true);
        ReleaseOnExit release{hold};
        recorder.Activate();
        for (std::size_t submit = 1; submit < submits; ++submit) {
            earlier.push_back(KeepSentinel(recorder));
            recorder.HoldNextSubmit(hold.semaphore, 1);
            recorder.Submit();
        }
        watch = KeepSentinel(recorder);
        recorder.NotePendingWrite(HeldWrite, 256);
        RecordMarker(context, recorder, marker.buffer);
        recorder.HoldNextSubmit(hold.semaphore, 1);
        ArmAllocationFailure(failAfter);
        try {
            recorder.Submit();
        } catch (const std::exception&) {
            outcome.threw = true;
        }
        outcome.failed = DisarmAllocationFailure();
        outcome.lostWhileHeld = watch.expired();
        earlierLost = static_cast<std::size_t>(std::count_if(earlier.begin(), earlier.end(), [](const auto& object) { return object.expired(); }));
        hold.Release();
        static_cast<void>(context.Function<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(context.device));
        outcome.executed = marker.Executed();
        Drain(recorder, "a submit growing the in-flight list");
    }
    if (!watch.expired()) Fail("a submit growing the in-flight list: the kept object outlived its recorder");
    return outcome;
}
#endif

void InFlightGrowth(const Graphics::Context& context) {
    const std::string what = "a submit that grows the in-flight list behind held batches";
#ifdef APS5_AGC_ALLOCATION_COUNT
    const auto growth = FindInFlightGrowth(context);
    if (growth.submit == 0) {
        Fail(what + ": no submit grew the in-flight list");
        return;
    }
    std::size_t cases = 0;
    for (std::size_t failAfter = 0; failAfter < growth.allocations; ++failAfter) {
        std::size_t earlierLost = 0;
        const auto outcome = GrowingSubmit(context, growth.submit, failAfter, earlierLost);
        const auto at = what + ", failing allocation " + std::to_string(failAfter) + " of submit " + std::to_string(growth.submit);
        if (!outcome.failed) {
            Fail(at + ": the allocation was not reached");
            continue;
        }
        ++cases;
        if (outcome.executed && outcome.lostWhileHeld) Fail(at + ": the kept object was destroyed while the batch was still held on the GPU");
        if (outcome.executed && outcome.threw) Fail(at + ": the submit threw after the batch reached the GPU");
        if (!outcome.executed && !outcome.threw) Fail(at + ": the submit neither ran the batch nor reported the failure");
        if (earlierLost != 0) Fail(at + ": " + std::to_string(earlierLost) + " objects kept by earlier held batches were destroyed");
    }
    std::printf("%s: submit %zu grew the list with %zu allocations, each failed in turn\n", what.c_str(), growth.submit, cases);
#else
    static_cast<void>(context);
    std::printf("%s: skipped, the driver's allocator is wrapped only in MinGW test builds\n", what.c_str());
#endif
}

}

int main() {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        const auto context = device->graphicsContext();
        Timed("submit sweep", [&] { SubmitSweep(context); });
        Timed("in-flight growth", [&] { InFlightGrowth(context); });
        for (const auto& failure : failures) std::cerr << failure << '\n';
        return failures.empty() ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
