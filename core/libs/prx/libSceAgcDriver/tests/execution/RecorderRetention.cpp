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
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
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

void Fail(std::string failure) {
    failures.push_back(std::move(failure));
}

constexpr std::uint64_t HeldWrite = 0x7f0000000000ull;
constexpr std::uint64_t OuterWrite = 0x7f0000010000ull;
constexpr std::uint64_t InnerWrite = 0x7f0000020000ull;

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
    bool waited = false;
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

struct WaitProbe {
    std::mutex mutex;
    std::condition_variable changed;
    std::thread::id audited;
    bool waiting = false;
    bool returned = false;
};

PFN_vkGetDeviceProcAddr originalProc = nullptr;
PFN_vkWaitSemaphoresKHR originalWaitSemaphores = nullptr;
std::atomic<WaitProbe*> probe{nullptr};
PFN_vkWaitForFences originalWaitForFences = nullptr;
PFN_vkDeviceWaitIdle originalDeviceWaitIdle = nullptr;
std::atomic<bool> failWaits{false};
std::atomic<bool> missingWait{false};
std::atomic<unsigned> failedWaits{0};
std::atomic<unsigned> failedIdles{0};
std::atomic<unsigned> missingLookups{0};

VKAPI_ATTR VkResult VKAPI_CALL WaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo* info, std::uint64_t timeout) {
    if (auto* watched = probe.load(); watched != nullptr && std::this_thread::get_id() == watched->audited) {
        {
            std::lock_guard lock(watched->mutex);
            watched->waiting = true;
        }
        watched->changed.notify_all();
    }
    return originalWaitSemaphores(device, info, timeout);
}

VKAPI_ATTR VkResult VKAPI_CALL WaitForFences(VkDevice device, std::uint32_t count, const VkFence* fences, VkBool32 waitAll, std::uint64_t timeout) {
    if (failWaits.load()) {
        failedWaits.fetch_add(1);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return originalWaitForFences(device, count, fences, waitAll, timeout);
}

VKAPI_ATTR VkResult VKAPI_CALL DeviceWaitIdle(VkDevice device) {
    if (failWaits.load()) {
        failedIdles.fetch_add(1);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return originalDeviceWaitIdle(device);
}

PFN_vkVoidFunction VKAPI_CALL Resolve(VkDevice device, const char* name) {
    if (std::strcmp(name, "vkWaitSemaphoresKHR") == 0 && missingWait.load()) {
        missingLookups.fetch_add(1);
        return nullptr;
    }
    if (std::strcmp(name, "vkWaitForFences") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&WaitForFences);
    if (std::strcmp(name, "vkDeviceWaitIdle") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&DeviceWaitIdle);
    if (std::strcmp(name, "vkWaitSemaphoresKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&WaitSemaphores);
    return originalProc(device, name);
}

Graphics::Context Probed(const Graphics::Context& context) {
    originalProc = context.deviceProc;
    originalWaitSemaphores = context.Function<PFN_vkWaitSemaphoresKHR>("vkWaitSemaphoresKHR");
    originalWaitForFences = context.Function<PFN_vkWaitForFences>("vkWaitForFences");
    originalDeviceWaitIdle = context.Function<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle");
    auto probed = context;
    probed.functions = nullptr;
    probed.deviceProc = &Resolve;
    return probed;
}

Outcome HeldSync(const Graphics::Context& probed, std::size_t failAfter) {
    Outcome outcome;
    Hold hold(probed);
    std::weak_ptr<int> watch;
    WaitProbe watched;
    watched.audited = std::this_thread::get_id();
    {
        Graphics::Recorder recorder(probed, true);
        ReleaseOnExit release{hold};
        recorder.Activate();
        watch = KeepSentinel(recorder);
        recorder.NotePendingWrite(HeldWrite, 256);
        recorder.HoldNextSubmit(hold.semaphore, 1);
        recorder.Submit();
        probe.store(&watched);
        std::thread releaser([&] {
            {
                std::unique_lock lock(watched.mutex);
                watched.changed.wait(lock, [&] { return watched.waiting || watched.returned; });
                outcome.waited = watched.waiting;
            }
            outcome.lostWhileHeld = watch.expired();
            hold.Release();
        });
        struct JoinOnExit {
            WaitProbe& watched;
            std::thread& releaser;
            ~JoinOnExit() {
                {
                    std::lock_guard lock(watched.mutex);
                    watched.returned = true;
                }
                watched.changed.notify_all();
                releaser.join();
                probe.store(nullptr);
            }
        };
        {
            JoinOnExit join{watched, releaser};
            ArmAllocationFailure(failAfter);
            try {
                recorder.Sync();
            } catch (const std::exception&) {
                outcome.threw = true;
            }
            outcome.failed = DisarmAllocationFailure();
        }
        Drain(recorder, "a held sync");
    }
    if (!watch.expired()) Fail("a held sync, failing allocation " + std::to_string(failAfter) + ": the kept object outlived its recorder");
    return outcome;
}

void SyncSweep(const Graphics::Context& probed) {
    const std::string what = "a sync finishing a batch held on the GPU";
#ifdef APS5_AGC_ALLOCATION_COUNT
    std::size_t cases = 0;
    std::size_t waited = 0;
    for (std::size_t failAfter = 0;; ++failAfter) {
        Require(failAfter < 4096, what + ": the allocation sweep did not end");
        const auto outcome = HeldSync(probed, failAfter);
        if (!outcome.failed) {
            if (!outcome.waited) Fail(what + ": the sync without a failure did not wait for the held batch");
            break;
        }
        ++cases;
        if (outcome.waited) ++waited;
        if (outcome.lostWhileHeld) Fail(what + ", failing allocation " + std::to_string(failAfter) + ": the kept object was destroyed while the batch was still held on the GPU");
    }
    if (cases == 0) Fail(what + ": the sync made no allocation to fail");
    std::printf("%s: %zu allocations failed one at a time, %zu of them before the sync waited for the hold\n", what.c_str(), cases, cases - waited);
#else
    const auto outcome = HeldSync(probed, 0);
    if (outcome.lostWhileHeld || !outcome.waited) Fail(what + ": the batch was not waited for while keeping its objects");
    std::printf("%s: skipped the allocation sweep, the driver's allocator is wrapped only in MinGW test builds\n", what.c_str());
#endif
}

void NestedFinish(const Graphics::Context& context) {
    bool outerRan = false;
    bool innerRan = false;
    bool outerSeen = false;
    bool innerSeen = true;
    {
        Graphics::Recorder recorder(context, true);
        recorder.Activate();
        recorder.NotePendingWrite(OuterWrite, 256);
        recorder.OnComplete([&] {
            outerRan = true;
            recorder.Sync();
            outerSeen = Graphics::Recorder::SnapshotWriteOverlaps(OuterWrite, 256);
            innerSeen = Graphics::Recorder::SnapshotWriteOverlaps(InnerWrite, 256);
        });
        recorder.Submit();
        recorder.NotePendingWrite(InnerWrite, 256);
        recorder.OnComplete([&] { innerRan = true; });
        recorder.Submit();
        Require(Graphics::Recorder::SnapshotWriteOverlaps(OuterWrite, 256) && Graphics::Recorder::SnapshotWriteOverlaps(InnerWrite, 256), "the pending-write snapshot does not hold the writes of both submitted batches");
        recorder.Sync();
        if (Graphics::Recorder::SnapshotWriteOverlaps(OuterWrite, 256) || Graphics::Recorder::SnapshotWriteOverlaps(InnerWrite, 256)) Fail("a nested finish: the pending-write snapshot kept the writes of finished batches");
        recorder.FlushDeferredReleases();
    }
    if (!outerRan || !innerRan) Fail("a nested finish: a completion did not run");
    if (!outerSeen) Fail("a nested finish: the outer batch's writes left the snapshot while its completion was running");
    if (innerSeen) Fail("a nested finish: the inner batch's writes stayed in the snapshot after its finish");
    std::printf("a nested finish: the outer batch's writes stayed published through the inner finish\n");
}

void ReserveKeptLimits(const Graphics::Context& context) {
    Graphics::Recorder recorder(context, true);
    const auto first = KeepSentinel(recorder);
    bool refused = false;
    try {
        recorder.ReserveKept(std::numeric_limits<std::size_t>::max());
    } catch (const std::length_error&) {
        refused = true;
    }
    if (!refused) Fail("ReserveKept accepted more objects than a batch can keep");
    if (first.expired()) Fail("a refused ReserveKept released an object the batch kept");
#ifdef APS5_AGC_ALLOCATION_COUNT
    constexpr std::size_t reserved = 1000;
    recorder.ReserveKept(reserved);
    std::vector<std::shared_ptr<void>> objects;
    for (std::size_t index = 0; index < reserved; ++index) objects.push_back(std::make_shared<int>(0));
    BeginAllocationCount_nid_no_patch();
    for (auto& object : objects) recorder.Keep(std::move(object));
    const auto made = EndAllocationCount_nid_no_patch();
    if (made != 0) Fail("keeping " + std::to_string(reserved) + " reserved objects made " + std::to_string(made) + " allocations");
#endif
    Drain(recorder, "ReserveKept");
    std::printf("ReserveKept: refused an overflowing reservation; reserved keeps made no allocation\n");
}

void RetiredIntoDiscardedBatch(const Graphics::Context& context) {
    Hold hold(context);
    std::weak_ptr<Graphics::Buffer> retired;
    bool discardedCompletionRan = false;
    {
        Graphics::Recorder recorder(context, true);
        ReleaseOnExit release{hold};
        recorder.Activate();
        auto surface = std::make_shared<Graphics::Buffer>(context, 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        retired = surface;
        RecordMarker(context, recorder, *surface);
        recorder.HoldNextSubmit(hold.semaphore, 1);
        recorder.Submit();
        recorder.ReserveKept(1);
        recorder.Keep(std::move(surface), 4096);
        recorder.OnComplete([&] { discardedCompletionRan = true; });
        Graphics::Recorder::FailNextSubmitsForTest(1);
        bool threw = false;
        try {
            recorder.Submit();
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw) Fail("a retired object: the injected submit failure did not happen");
        if (retired.expired()) Fail("a retired object: a discarded batch destroyed it while a batch held on the GPU still used it");
        if (recorder.InFlightKeptBytes() != 4096) Fail("a retired object: the bytes of the parked batch are not counted in flight (" + std::to_string(recorder.InFlightKeptBytes()) + ")");
        hold.Release();
        recorder.Sync();
        if (retired.expired()) Fail("a retired object: the finish destroyed the parked batch under the mutex instead of deferring it");
        if (recorder.InFlightKeptBytes() != 0) Fail("a retired object: the finish left " + std::to_string(recorder.InFlightKeptBytes()) + " bytes in flight");
        recorder.FlushDeferredReleases();
    }
    if (!retired.expired()) Fail("a retired object: it outlived its recorder");
    if (discardedCompletionRan) Fail("a retired object: the completion of the discarded batch ran");
    std::printf("a retired object: kept by the held batch through the discard of its batch, released after its finish\n");
}

void HeldParkedChain(const Graphics::Context& context, std::size_t length) {
    Hold hold(context);
    std::vector<std::weak_ptr<int>> parked;
    {
        Graphics::Recorder recorder(context, true);
        ReleaseOnExit release{hold};
        recorder.OpenBatch();
        recorder.HoldNextSubmit(hold.semaphore, 1);
        recorder.Submit();
        parked.reserve(length);
        Graphics::Recorder::FailNextSubmitsForTest(static_cast<unsigned>(length));
        for (std::size_t index = 0; index < length; ++index) {
            parked.push_back(KeepSentinel(recorder));
            try {
                recorder.Submit();
                Fail("a parked chain: an injected submit failure did not happen");
            } catch (const std::runtime_error&) {
            }
        }
        Graphics::Recorder::FailNextSubmitsForTest(0);
        const auto lost = std::count_if(parked.begin(), parked.end(), [](const auto& object) { return object.expired(); });
        if (lost != 0) Fail("a parked chain: " + std::to_string(lost) + " objects of discarded batches were destroyed while the held batch ran");
        hold.Release();
        Drain(recorder, "a parked chain");
    }
    const auto kept = std::count_if(parked.begin(), parked.end(), [](const auto& object) { return !object.expired(); });
    if (kept != 0) Fail("a parked chain: " + std::to_string(kept) + " objects of discarded batches outlived their recorder");
    std::printf("a parked chain: %zu discarded batches parked behind a held batch\n", length);
}

void LongParkedChain(std::size_t length) {
    Graphics::Recorder::DestroyParkedChainForTest(length);
    std::printf("a parked chain of %zu batches was destroyed\n", length);
}

void FailedWait(const Graphics::Context& context, const Graphics::Context& probed, bool later, bool missing) {
    const std::string what = std::string(missing ? "a sync whose wait function is missing" : "a sync whose wait and device idle fail") + (later ? ", with a later batch in flight" : ", with no later batch in flight");
    Hold hold(context);
    std::weak_ptr<int> first;
    bool completionRan = false;
    {
        Graphics::Recorder recorder(probed, missing);
        ReleaseOnExit release{hold};
        recorder.Activate();
        first = KeepSentinel(recorder, 4096);
        recorder.OnComplete([&] { completionRan = true; });
        recorder.NotePendingWrite(HeldWrite, 256);
        recorder.HoldNextSubmit(hold.semaphore, 1);
        recorder.Submit();
        if (later) {
            KeepSentinel(recorder);
            recorder.Submit();
        }
        failedWaits = 0;
        failedIdles = 0;
        missingLookups = 0;
        (missing ? missingWait : failWaits).store(true);
        bool threw = false;
        try {
            recorder.Sync();
        } catch (const std::exception&) {
            threw = true;
        }
        missingWait.store(false);
        failWaits.store(false);
        if (!threw) Fail(what + ": the sync did not report the failure");
        if (missing ? missingLookups.load() != 1 : failedWaits.load() != 1 || failedIdles.load() != 1) Fail(what + ": the failure was not reached (" + std::to_string(missingLookups.load()) + " lookups, " + std::to_string(failedWaits.load()) + " waits, " + std::to_string(failedIdles.load()) + " idles failed)");
        if (first.expired()) Fail(what + ": the batch's objects were destroyed while it was still held on the GPU");
        hold.Release();
        static_cast<void>(context.Function<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(context.device));
        if (first.expired()) Fail(what + ": the batch's objects were destroyed before a later batch proved it complete");
        if (!later) {
            KeepSentinel(recorder);
            recorder.Submit();
        }
        Drain(recorder, what);
        if (recorder.InFlightKeptBytes() != 0) Fail(what + ": " + std::to_string(recorder.InFlightKeptBytes()) + " bytes stayed in flight");
    }
    if (!first.expired()) Fail(what + ": the batch's objects outlived the batch that carried them");
    if (completionRan) Fail(what + ": the completion of a batch no wait proved complete ran");
    std::printf("%s: kept until a later batch completed\n", what.c_str());
}

}

int main() {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        const auto context = device->graphicsContext();
        const auto probed = Probed(context);
        Timed("submit sweep", [&] { SubmitSweep(context); });
        Timed("in-flight growth", [&] { InFlightGrowth(context); });
        Timed("sync sweep", [&] { SyncSweep(probed); });
        Timed("nested finish", [&] { NestedFinish(context); });
        Timed("reserve kept", [&] { ReserveKeptLimits(context); });
        Timed("retired into a discarded batch", [&] { RetiredIntoDiscardedBatch(context); });
        Timed("held parked chain", [&] { HeldParkedChain(context, 64); });
        Timed("long parked chain", [&] { LongParkedChain(200000); });
        Timed("failed wait, later batch", [&] { FailedWait(context, probed, true, false); });
        Timed("failed wait, no later batch", [&] { FailedWait(context, probed, false, false); });
        Timed("missing wait function", [&] { FailedWait(context, probed, true, true); });
        for (const auto& failure : failures) std::cerr << failure << '\n';
        return failures.empty() ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
