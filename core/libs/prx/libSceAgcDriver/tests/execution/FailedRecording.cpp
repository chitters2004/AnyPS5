#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "VulkanTestDevice.hpp"
#ifdef APS5_AGC_ALLOCATION_COUNT
#include "prx/libSceAgcDriver/tests/AllocationCounter.hpp"
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace AgcDriver::DriverDetail {

struct DriverTestAccess {
    static void UseDevice(std::shared_ptr<VulkanDevice> device) { Driver::Get().device = std::move(device); }
};

}

namespace {

using AgcDriver::DriverDetail::DriverTestAccess;
using AgcDriver::Graphics::Context;
using AgcDriver::Graphics::Recorder;

constexpr VkDeviceSize BufferBytes = 256;
constexpr std::uint32_t Pattern = 0x5a5a5a5au;
constexpr std::size_t BlockBytes = 65536;
constexpr std::size_t FillBytes = 4096;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("failed recording: " + message);
}

AgcDriver::GuestMemory::GpuMutexType& Gpu() { return AgcDriver::GuestMemory::GpuMutex(); }

void RecordFillBuffer(const Context& context, VkCommandBuffer commands, VkBuffer buffer) {
    context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, buffer, 0, BufferBytes, Pattern);
}

class HostBuffer {
public:
    explicit HostBuffer(const Context& context) : context(context) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = BufferBytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Require(context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &buffer) == VK_SUCCESS, "vkCreateBuffer");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, buffer, &requirements);
        constexpr VkMemoryPropertyFlags hostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        std::uint32_t type = context.memory.memoryTypeCount;
        for (std::uint32_t candidate = 0; candidate < context.memory.memoryTypeCount; ++candidate) {
            if ((requirements.memoryTypeBits & (1u << candidate)) != 0u && (context.memory.memoryTypes[candidate].propertyFlags & hostVisible) == hostVisible) {
                type = candidate;
                break;
            }
        }
        Require(type != context.memory.memoryTypeCount, "no host-visible coherent memory type");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        Require(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &memory) == VK_SUCCESS, "vkAllocateMemory");
        Require(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, buffer, memory, 0) == VK_SUCCESS, "vkBindBufferMemory");
        Require(context.Function<PFN_vkMapMemory>("vkMapMemory")(context.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS, "vkMapMemory");
        *static_cast<std::uint32_t*>(mapped) = 0;
    }

    ~HostBuffer() {
        context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
        context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
    }

    HostBuffer(const HostBuffer&) = delete;
    HostBuffer& operator=(const HostBuffer&) = delete;

    VkBuffer Handle() const { return buffer; }
    void RecordFill(VkCommandBuffer commands) const { RecordFillBuffer(context, commands, buffer); }

    std::uint32_t First() const { return *static_cast<const std::uint32_t*>(mapped); }

private:
    Context context;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
};

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint32_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint32_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "cannot allocate a guest block");
        std::memset(block, 0, BlockBytes);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        VirtualFree(block, 0, MEM_RELEASE);
#else
        std::free(block);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(block); }
    std::uint32_t First() const { return *static_cast<const volatile std::uint32_t*>(block); }

private:
    std::uint32_t* block = nullptr;
};

std::array<std::uint32_t, 4> Uniform(std::uint32_t value) { return {value, value, value, value}; }

struct InjectedCompletionFailure {};

void SyncLocked(Recorder& recorder) {
    std::lock_guard gpu(Gpu());
    recorder.Sync();
}

bool Fill(AgcDriver::VulkanDevice& device, const GuestBlock& block, const std::array<std::uint32_t, 4>& pattern) {
    return device.FillBuffer(block.Address(), FillBytes, pattern);
}

bool Imported(AgcDriver::VulkanDevice& device, Recorder& recorder, const GuestBlock& block) {
    bool imported = false;
    {
        std::lock_guard gpu(Gpu());
        imported = Fill(device, block, Uniform(0));
        recorder.Sync();
    }
    device.WaitIdle();
    return imported && block.First() == 0;
}

std::string Escape(const std::function<void()>& work) {
    try {
        work();
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

void EscapeDiscardsHold(AgcDriver::VulkanDevice& device, Recorder& recorder, const GuestBlock& earlier, const GuestBlock& later) {
    SyncLocked(recorder);
    {
        std::lock_guard gpu(Gpu());
        Require(Fill(device, earlier, Uniform(Pattern + 1u)), "the earlier fill was not recorded");
    }
    Require(recorder.Recording(), "the earlier fill left no open batch");
    const auto submitted = recorder.Submissions();
    const auto reason = Escape([&] {
        std::lock_guard gpu(Gpu());
        Require(Fill(device, later, Uniform(Pattern + 2u)), "the later fill was not recorded");
        {
            std::lock_guard nested(Gpu());
            static_cast<void>(recorder.Commands());
        }
        throw std::runtime_error("injected escape");
    });
    Require(reason == "injected escape", "the escape threw '" + reason + "'");
    Require(!recorder.Recording(), "an exception that left the GPU mutex after its holder recorded kept the batch open");
    SyncLocked(recorder);
    device.WaitIdle();
    Require(recorder.Submissions() == submitted, "the batch an exception left the GPU mutex through was submitted");
    Require(later.First() == 0, "the discarded fill ran");
    std::puts("an exception that leaves the GPU mutex after its holder recorded, from a nested hold too, discards the batch before the mutex is released");
}

void RefusalKeepsBatch(AgcDriver::VulkanDevice& device, Recorder& recorder, const GuestBlock& block) {
    SyncLocked(recorder);
    {
        std::lock_guard gpu(Gpu());
        Require(Fill(device, block, Uniform(Pattern + 3u)), "the fill was not recorded");
    }
    const auto submitted = recorder.Submissions();
    const auto reason = Escape([] {
        std::lock_guard gpu(Gpu());
        throw std::runtime_error("injected refusal");
    });
    Require(reason == "injected refusal", "the refusal threw '" + reason + "'");
    Require(recorder.Recording(), "an exception from a hold that recorded nothing discarded the batch an earlier hold recorded");
    SyncLocked(recorder);
    device.WaitIdle();
    Require(recorder.Submissions() == submitted + 1 && block.First() == Pattern + 3u, "the batch kept through a refusal did not run");
    std::puts("an exception from a hold that recorded nothing keeps the batch an earlier hold left open");
}

void FailedCopyNote(AgcDriver::VulkanDevice& device, Recorder& recorder, const GuestBlock& source, const GuestBlock& destination) {
    {
        std::lock_guard gpu(Gpu());
        Require(Fill(device, source, Uniform(Pattern + 4u)), "the copy source fill was not recorded");
        Require(Fill(device, destination, Uniform(0)), "the copy destination fill was not recorded");
        recorder.Sync();
    }
    device.WaitIdle();
    const auto submitted = recorder.Submissions();
    AgcDriver::VulkanDevice::CopyOutcome outcome{};
    const auto reason = Escape([&] {
        std::lock_guard gpu(Gpu());
        outcome = device.CopyBuffer(destination.Address(), source.Address(), FillBytes, 0, std::numeric_limits<std::size_t>::max(), 0, 0, 0, [](std::span<const std::byte>, std::uint64_t) { throw std::runtime_error("injected writer note failure"); });
    });
    Require(reason == "injected writer note failure", "the copy whose writer note failed threw '" + reason + "' (path " + std::to_string(outcome.path) + ")");
    Require(!recorder.Recording(), "a copy whose bookkeeping failed after it recorded kept the batch open");
    SyncLocked(recorder);
    device.WaitIdle();
    Require(recorder.Submissions() == submitted, "the batch of a copy whose bookkeeping failed was submitted");
    Require(destination.First() == 0, "the discarded copy ran");
    std::puts("a copy whose bookkeeping fails after it recorded discards its batch");
}

void FailedRunCallback(AgcDriver::VulkanDevice& device, Recorder& recorder, const GuestBlock& block) {
    SyncLocked(recorder);
    const auto submitted = recorder.Submissions();
    const int owner = 0;
    const void* const owners[] = {&owner};
    const auto reason = Escape([&] {
        std::lock_guard gpu(Gpu());
        Require(Fill(device, block, Uniform(Pattern + 5u)), "the fill was not recorded");
        recorder.DeferDispatchBarrier(1, owners, {});
        recorder.AfterRun([](Recorder&) { throw std::runtime_error("injected after-run failure"); });
        try {
            recorder.Submit();
        } catch (...) {
            Require(!recorder.Recording(), "a submit whose run callback failed kept the batch open while the GPU mutex was held");
            throw;
        }
    });
    Require(reason == "injected after-run failure", "the submit whose run callback failed threw '" + reason + "'");
    SyncLocked(recorder);
    device.WaitIdle();
    Require(recorder.Submissions() == submitted, "the batch whose run callback failed was submitted");
    Require(block.First() == 0, "the discarded fill ran");
    std::puts("a run callback that fails at submit discards its batch");
}

void FailedCompletionDrain(AgcDriver::VulkanDevice& device, Recorder& recorder, const GuestBlock& first, const GuestBlock& second) {
    SyncLocked(recorder);
    bool thrown = false;
    {
        std::lock_guard gpu(Gpu());
        Require(Fill(device, first, Uniform(Pattern + 6u)), "the first fill was not recorded");
        recorder.OnComplete([] { throw InjectedCompletionFailure{}; });
        recorder.Submit();
        Require(Fill(device, second, Uniform(Pattern + 7u)), "the second fill was not recorded");
        recorder.Submit();
        Require(recorder.InFlightBatches() >= 2, "the two batches are not in flight");
        try {
            recorder.Sync();
        } catch (const InjectedCompletionFailure&) {
            thrown = true;
        }
    }
    Require(thrown, "the sync whose first batch's completion failed did not rethrow the failure");
    Require(recorder.InFlightBatches() == 0, "a sync stopped finishing batches at the first completion failure");
    device.WaitIdle();
    Require(second.First() == Pattern + 7u, "the later batch did not run");
    std::puts("a sync whose batch fails to complete finishes every later batch and rethrows the first failure");
}

#ifdef APS5_AGC_ALLOCATION_COUNT
void FillFailures(AgcDriver::VulkanDevice& device, Recorder& recorder, const GuestBlock& block) {
    const auto start = std::chrono::steady_clock::now();
    std::size_t escapes = 0;
    for (std::uint32_t allocation = 0;; ++allocation) {
        Require(allocation < 4096u, "the fill never ran out of allocations to fail");
        SyncLocked(recorder);
        const auto submitted = recorder.Submissions();
        const std::array<std::uint32_t, 4> pattern{0x10000000u + allocation, 0x20000000u + allocation, 0x30000000u + allocation, 0x40000000u + allocation};
        bool escaped = false;
        try {
            std::lock_guard gpu(Gpu());
            FailAllocationsAfter_nid_no_patch(allocation);
            Require(Fill(device, block, pattern), "the pattern fill was not recorded");
        } catch (const std::bad_alloc&) {
            escaped = true;
        }
        const bool fired = !AllocationFailurePending_nid_no_patch();
        StopAllocationFailures_nid_no_patch();
        if (!fired) break;
        if (!escaped) continue;
        ++escapes;
        Require(!recorder.Recording(), "a pattern fill whose allocation " + std::to_string(allocation) + " failed left a batch open");
        SyncLocked(recorder);
        Require(recorder.Submissions() == submitted, "a pattern fill whose allocation " + std::to_string(allocation) + " failed submitted a batch");
    }
    SyncLocked(recorder);
    device.WaitIdle();
    Require(escapes != 0, "no failed allocation reached the caller of a pattern fill");
    const auto spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("a pattern fill whose allocation fails leaves no batch open: %zu injected failures reached the caller (%.2f s)\n", escapes, spent);
}
#endif

void FailedCompletionAtTeardown(const Context& context, Recorder& recorder) {
    std::lock_guard gpu(Gpu());
    auto buffer = std::make_shared<HostBuffer>(context);
    buffer->RecordFill(recorder.Commands());
    recorder.Keep(buffer);
    recorder.OnComplete([] { throw InjectedCompletionFailure{}; });
    recorder.Submit();
    std::puts("a batch whose completion fails is left in flight for the device's teardown");
}

class GpuHold {
public:
    explicit GpuHold(const Context& context) : context(context) {
        try {
            signal = context.Function<PFN_vkSignalSemaphore>("vkSignalSemaphore");
        } catch (const std::runtime_error&) {
            signal = context.Function<PFN_vkSignalSemaphoreKHR>("vkSignalSemaphoreKHR");
        }
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        const VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        Require(context.Function<PFN_vkCreateSemaphore>("vkCreateSemaphore")(context.device, &info, nullptr, &semaphore) == VK_SUCCESS, "the device has no timeline semaphore to hold a batch on the GPU");
    }

    ~GpuHold() {
        Release();
        static_cast<void>(context.Function<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(context.device));
        context.Function<PFN_vkDestroySemaphore>("vkDestroySemaphore")(context.device, semaphore, nullptr);
    }

    GpuHold(const GpuHold&) = delete;
    GpuHold& operator=(const GpuHold&) = delete;

    VkSemaphore Semaphore() const { return semaphore; }
    bool Released() const { return released.load(std::memory_order_acquire); }

    void Release() {
        if (released.exchange(true, std::memory_order_acq_rel)) return;
        VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        info.semaphore = semaphore;
        info.value = 1;
        static_cast<void>(signal(context.device, &info));
    }

private:
    Context context;
    PFN_vkSignalSemaphore signal = nullptr;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    std::atomic<bool> released{false};
};

void TeardownWithBatchesOutstanding(const Context& context) {
    GpuHold hold(context);
    std::atomic<bool> destroyed{false};
    bool destroyedWhileHeld = false;
    {
        std::lock_guard gpu(Gpu());
        std::jthread releaser([&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
            while (!destroyed.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            hold.Release();
        });
        Recorder recorder(context, true);
        auto first = std::make_shared<HostBuffer>(context);
        first->RecordFill(recorder.Commands());
        recorder.Keep(first);
        recorder.OnComplete([] { throw InjectedCompletionFailure{}; });
        recorder.Submit();
        recorder.HoldNextSubmit(hold.Semaphore(), 1);
        auto held = std::make_shared<HostBuffer>(context);
        held->RecordFill(recorder.Commands());
        recorder.Keep(held);
        recorder.Keep(std::shared_ptr<int>(new int(0), [&](int* sentinel) {
            delete sentinel;
            destroyedWhileHeld = !hold.Released();
            hold.Release();
            destroyed.store(true, std::memory_order_release);
        }));
        recorder.Submit();
        auto last = std::make_shared<HostBuffer>(context);
        last->RecordFill(recorder.Commands());
        recorder.Keep(last);
        recorder.FailNextSubmitsForTest(1);
    }
    Require(destroyed.load(std::memory_order_acquire), "the held batch's kept object outlived its recorder");
    Require(!destroyedWhileHeld, "the recorder's teardown released a batch still held on the GPU after an older batch's completion and the last submit failed");
    std::puts("a recorder torn down with a failing completion, a batch held on the GPU and a failing submit waits for the GPU before releasing");
}

void FailedPassCallback(const Context& context, Recorder& recorder) {
    std::lock_guard gpu(Gpu());
    auto older = std::make_shared<HostBuffer>(context);
    older->RecordFill(recorder.Commands());
    recorder.Keep(older);
    recorder.Submit();
    Require(recorder.InFlightBatches() != 0, "the older batch is not in flight");
    const auto submitted = recorder.Submissions();
    auto owned = std::make_unique<HostBuffer>(context);
    const auto start = recorder.StartDrawPass(1, false, false, {});
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {16, 16};
    rendering.layerCount = 1;
    context.Function<PFN_vkCmdBeginRenderingKHR>("vkCmdBeginRenderingKHR")(start.commands, &rendering);
    recorder.LeaveRenderPassOpen(1, Recorder::NoTiming, false, {}, [context, named = owned->Handle()](AgcDriver::Graphics::CommandState& pass) {
        RecordFillBuffer(context, pass.Commands(), named);
        throw std::bad_alloc();
    });
    bool thrown = false;
    try {
        recorder.Sync();
    } catch (const std::bad_alloc&) {
        thrown = true;
    }
    Require(thrown, "the pass callback's failure did not reach the syncing caller");
    Require(recorder.InFlightBatches() == 0, "the sync that failed in a pass callback left the older batch unfinished");
    owned.reset();
    recorder.Sync();
    Require(recorder.Submissions() == submitted, "the batch whose pass callback failed was submitted after the object it named was destroyed");
    std::puts("a pass callback that fails before keeping what it recorded discards its batch, after the older batch finished");
}

std::exception_ptr InjectedDriverFailure() { return std::make_exception_ptr(std::runtime_error("injected driver failure")); }

void DriverFailure(const AgcDriver::VulkanDevice& device, Recorder& recorder) {
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    auto unfinished = std::make_shared<HostBuffer>(context);
    std::uint64_t submitted = 0;
    bool discarded = false;
    {
        std::lock_guard gpu(Gpu());
        auto older = std::make_shared<HostBuffer>(context);
        older->RecordFill(recorder.Commands());
        recorder.Keep(older);
        recorder.Submit();
        Require(recorder.InFlightBatches() != 0, "the older batch is not in flight");
        submitted = recorder.Submissions();
        unfinished->RecordFill(recorder.Commands());
        recorder.Keep(unfinished);
        recorder.OnDiscard([&discarded] { discarded = true; });
    }
    AgcDriverReportFailure_nid_postfix(InjectedDriverFailure());
    const auto kept = Escape([&] {
        std::lock_guard gpu(Gpu());
        recorder.ReserveKept(1);
        recorder.Keep(std::make_shared<int>(0));
    });
    Require(kept.empty(), "a keep reserved in the batch open when the driver failed threw '" + kept + "'");
    const auto recordedAfter = Escape([&] {
        std::lock_guard gpu(Gpu());
        unfinished->RecordFill(recorder.Commands());
    });
    Require(recordedAfter == "injected driver failure", "recording into the batch open when the driver failed threw '" + recordedAfter + "' instead of the failure");
    Require(discarded && !recorder.Recording(), "recording into the batch open when the driver failed did not discard it");
    const auto synced = Escape([&] { SyncLocked(recorder); });
    Require(synced.empty(), "a sync after the driver failed, with no batch open, threw '" + synced + "'");
    Require(recorder.Submissions() == submitted, "a batch was submitted after the driver failed");
    Require(recorder.InFlightBatches() == 0, "the sync after the driver failed left the older batch unfinished");
    Require(unfinished->First() == 0, "the discarded batch ran");
    const auto opened = Escape([&] {
        std::lock_guard gpu(Gpu());
        static_cast<void>(recorder.Commands());
    });
    Require(opened == "injected driver failure", "opening a batch after the driver failed threw '" + opened + "' instead of the failure");
    std::puts("after a driver failure the open batch is discarded at its next recording, the older batch finishes and no batch opens");
}

void ReplacementAfterFailure(std::shared_ptr<AgcDriver::VulkanDevice> device) {
    const auto* failed = Recorder::Active();
    DriverTestAccess::UseDevice(nullptr);
    device.reset();
    std::shared_ptr<AgcDriver::VulkanDevice> replacement = OpenVulkanTestDevice();
    Require(replacement != nullptr, "the replacement device did not open");
    DriverTestAccess::UseDevice(replacement);
    auto* later = Recorder::Active();
    Require(later != nullptr && later != failed, "the replacement device has no recorder of its own");
    const auto replaced = Escape([&] {
        std::lock_guard gpu(Gpu());
        static_cast<void>(later->Commands());
    });
    Require(replaced == "injected driver failure", "a recorder created after the driver failed threw '" + replaced + "' instead of the failure");
    std::puts("a device installed after the driver failed refuses to record");
}

void SubmitRace(const std::shared_ptr<AgcDriver::VulkanDevice>& device) {
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(*device);
    auto& recorder = *Recorder::Active();
    auto unfinished = std::make_shared<HostBuffer>(context);
    const int owner = 0;
    const void* const owners[] = {&owner};
    std::uint64_t submitted = 0;
    const auto reason = Escape([&] {
        std::lock_guard gpu(Gpu());
        submitted = recorder.Submissions();
        unfinished->RecordFill(recorder.Commands());
        recorder.Keep(unfinished);
        recorder.DeferDispatchBarrier(1, owners, {});
        recorder.AfterRun([](Recorder&) { AgcDriverReportFailure_nid_postfix(InjectedDriverFailure()); });
        recorder.Submit();
    });
    Require(reason == "injected driver failure", "a submit during which the driver failed threw '" + reason + "' instead of the failure");
    Require(recorder.Submissions() == submitted, "a batch was submitted after the driver failed during its submit");
    Require(!recorder.Recording(), "the batch whose submit the driver failure interrupted stayed open");
    device->WaitIdle();
    Require(unfinished->First() == 0, "the batch whose submit the driver failure interrupted ran");
    std::puts("a driver failure reported while a batch is being submitted stops that submit");
}

int Run(std::string_view mode) {
    bool failures = false;
    const auto check = [&failures](const char* name, const std::function<void()>& test) {
        try {
            test();
        } catch (const std::exception& error) {
            std::cerr << name << ": " << error.what() << '\n';
            failures = true;
        }
    };
    std::shared_ptr<AgcDriver::VulkanDevice> device = OpenVulkanTestDevice();
    if (!device) return VulkanTestSkipped;
    DriverTestAccess::UseDevice(device);
    auto* recorder = Recorder::Active();
    Require(recorder != nullptr, "the device has no active recorder");
    if (mode == "--driver-failure") {
        DriverFailure(*device, *recorder);
        ReplacementAfterFailure(std::move(device));
    } else if (mode == "--submit-race") {
        SubmitRace(device);
    } else {
        {
            GuestBlock first, second;
            if (SkipWithoutSharedMemoryStores() || !Imported(*device, *recorder, first) || !Imported(*device, *recorder, second)) {
                std::puts("skipped: the hold, copy, run, drain and fill cases need host imports of guest memory");
            } else {
                check("escape", [&] { EscapeDiscardsHold(*device, *recorder, first, second); });
                check("refusal", [&] { RefusalKeepsBatch(*device, *recorder, first); });
                check("copy", [&] { FailedCopyNote(*device, *recorder, first, second); });
                check("run callback", [&] { FailedRunCallback(*device, *recorder, second); });
                check("drain", [&] { FailedCompletionDrain(*device, *recorder, first, second); });
#ifdef APS5_AGC_ALLOCATION_COUNT
                check("fill", [&] { FillFailures(*device, *recorder, second); });
#else
                std::puts("skipped: the fill allocation failures need a build that can fail allocations inside the driver");
#endif
            }
            device->WaitIdle();
        }
        {
            const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(*device);
            check("teardown with batches outstanding", [&] { TeardownWithBatchesOutstanding(context); });
            if (context.graphicsPipelineLibrary) {
                check("pass callback", [&] { FailedPassCallback(context, *recorder); });
            } else {
                std::puts("skipped: a failing pass callback is not checked, the device records render passes, not dynamic rendering");
            }
            device->WaitIdle();
            check("teardown", [&] { FailedCompletionAtTeardown(context, *recorder); });
        }
    }
    DriverTestAccess::UseDevice(nullptr);
    return failures ? 1 : 0;
}

}

int main(int argc, char** argv) {
    int result = 1;
    try {
        result = Run(argc > 1 ? std::string_view(argv[1]) : std::string_view());
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    try {
        AgcDriverShutdown_nid_postfix();
    } catch (...) {
    }
    return result;
}
