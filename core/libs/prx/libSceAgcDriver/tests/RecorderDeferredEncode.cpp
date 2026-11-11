#include "prx/libSceAgcDriver/Graphics/include/CommandEncoder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/CommandStream.hpp"
#include "prx/libSceAgcDriver/Graphics/include/QueueSubmitter.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <random>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace AgcDriver::Graphics;

namespace {

int failures = 0;

void Expect(bool condition, const std::string& what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
}

enum class BufferState { Initial, Recording, Executable };

struct Submission {
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::vector<std::string> recorded;
    std::set<std::thread::id> recordingThreads;
    std::thread::id thread;
};

struct MockDevice {
    std::mutex mutex;
    std::condition_variable changed;
    std::uintptr_t next = 0x10000;
    VkCommandPool contextPool = VK_NULL_HANDLE;
    std::set<VkCommandPool> pools;
    std::uint64_t poolsCreated = 0;
    std::uint64_t poolsDestroyed = 0;
    std::map<VkCommandBuffer, VkCommandPool> owners;
    std::map<VkCommandBuffer, BufferState> states;
    std::map<VkCommandBuffer, std::vector<std::string>> recorded;
    std::map<VkCommandBuffer, std::set<std::thread::id>> recordingThreads;
    std::uint64_t allocated = 0;
    std::uint64_t freed = 0;
    std::map<VkFence, bool> signaled;
    std::vector<Submission> submissions;
    std::vector<std::string> violations;
    bool gate = false;
    bool jitter = false;
    bool gateOpen = false;
    bool holdSubmitReturn = false;
    bool submitReturnOpen = false;
    std::set<VkFence> fencesInSubmit;
    std::set<VkCommandBuffer> buffersInSubmit;
    VkResult encoderEndResult = VK_SUCCESS;
    VkResult failedSubmitResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    std::uint32_t failSubmits = 0;
    std::uint32_t failedSubmitCalls = 0;
    std::vector<std::string> events;
    std::map<VkQueue, int> queueUsers;

    std::uintptr_t Handle() {
        next += 0x10;
        return next;
    }

    void Violation(const std::string& what) { violations.push_back(what); }
};

MockDevice* mock = nullptr;

template<typename T>
T handleOf(std::uintptr_t value) {
    return reinterpret_cast<T>(value);
}

VKAPI_ATTR VkResult VKAPI_CALL mockCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo* info, const VkAllocationCallbacks*, VkCommandPool* pool) {
    std::lock_guard lock(mock->mutex);
    if ((info->flags & VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) == 0) mock->Violation("an encoder pool cannot reset its command buffers one by one");
    *pool = handleOf<VkCommandPool>(mock->Handle());
    mock->pools.insert(*pool);
    ++mock->poolsCreated;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockDestroyCommandPool(VkDevice, VkCommandPool pool, const VkAllocationCallbacks*) {
    std::lock_guard lock(mock->mutex);
    if (pool == mock->contextPool) mock->Violation("the device's command pool was destroyed by the recorder");
    if (mock->pools.erase(pool) == 0) mock->Violation("an unknown command pool was destroyed");
    ++mock->poolsDestroyed;
    for (auto it = mock->owners.begin(); it != mock->owners.end();) {
        if (it->second != pool) {
            ++it;
            continue;
        }
        ++mock->freed;
        mock->states.erase(it->first);
        it = mock->owners.erase(it);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL mockAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* commands) {
    std::lock_guard lock(mock->mutex);
    if (mock->pools.count(info->commandPool) == 0) mock->Violation("a command buffer was allocated from an unknown pool");
    *commands = handleOf<VkCommandBuffer>(mock->Handle());
    mock->owners[*commands] = info->commandPool;
    mock->states[*commands] = BufferState::Initial;
    ++mock->allocated;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockFreeCommandBuffers(VkDevice, VkCommandPool pool, std::uint32_t count, const VkCommandBuffer* commands) {
    std::lock_guard lock(mock->mutex);
    for (std::uint32_t index = 0; index < count; ++index) {
        if (CommandStream::IsHandle(commands[index])) {
            mock->Violation("a stream handle was freed as a command buffer");
            continue;
        }
        const auto found = mock->owners.find(commands[index]);
        if (found == mock->owners.end()) {
            mock->Violation("an unknown command buffer was freed");
            continue;
        }
        if (found->second != pool) mock->Violation("a command buffer was freed into a pool that did not allocate it");
        if (mock->buffersInSubmit.count(commands[index]) != 0) mock->Violation("a command buffer was freed while its vkQueueSubmit had not returned");
        mock->owners.erase(found);
        mock->states.erase(commands[index]);
        ++mock->freed;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL mockBeginCommandBuffer(VkCommandBuffer commands, const VkCommandBufferBeginInfo*) {
    std::unique_lock lock(mock->mutex);
    if (CommandStream::IsHandle(commands)) {
        mock->Violation("vkBeginCommandBuffer was given a stream handle");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const auto owner = mock->owners.find(commands);
    if (owner == mock->owners.end()) {
        mock->Violation("an unknown command buffer was begun");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (mock->gate && owner->second != mock->contextPool) mock->changed.wait(lock, [] { return mock->gateOpen; });
    if (mock->jitter && owner->second != mock->contextPool) {
        thread_local std::mt19937 delay(static_cast<std::uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())));
        const auto pause = std::chrono::microseconds(delay() % 400);
        lock.unlock();
        std::this_thread::sleep_for(pause);
        lock.lock();
    }
    if (mock->jitter && owner->second != mock->contextPool) {
        thread_local std::mt19937 delay(static_cast<std::uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())));
        const auto pause = std::chrono::microseconds(delay() % 400);
        lock.unlock();
        std::this_thread::sleep_for(pause);
        lock.lock();
    }
    if (mock->buffersInSubmit.count(commands) != 0) mock->Violation("a command buffer was begun again while its vkQueueSubmit had not returned");
    mock->states[commands] = BufferState::Recording;
    mock->recorded[commands].clear();
    mock->recordingThreads[commands].clear();
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockEndCommandBuffer(VkCommandBuffer commands) {
    std::lock_guard lock(mock->mutex);
    if (CommandStream::IsHandle(commands)) {
        mock->Violation("vkEndCommandBuffer was given a stream handle");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (mock->states[commands] != BufferState::Recording) mock->Violation("a command buffer was ended outside recording");
    const auto owner = mock->owners.find(commands);
    if (owner != mock->owners.end() && owner->second != mock->contextPool && mock->encoderEndResult != VK_SUCCESS) return mock->encoderEndResult;
    mock->states[commands] = BufferState::Executable;
    return VK_SUCCESS;
}

void record(VkCommandBuffer commands, const std::string& op) {
    std::lock_guard lock(mock->mutex);
    if (CommandStream::IsHandle(commands)) {
        mock->Violation(op + " reached the driver with a stream handle");
        return;
    }
    if (mock->states[commands] != BufferState::Recording) mock->Violation(op + " was recorded into a command buffer that was not recording");
    mock->recorded[commands].push_back(op);
    mock->recordingThreads[commands].insert(std::this_thread::get_id());
}

VKAPI_ATTR void VKAPI_CALL mockCmdFillBuffer(VkCommandBuffer commands, VkBuffer, VkDeviceSize, VkDeviceSize, std::uint32_t value) {
    record(commands, "fill " + std::to_string(value));
}

VKAPI_ATTR void VKAPI_CALL mockCmdUpdateBuffer(VkCommandBuffer commands, VkBuffer, VkDeviceSize, VkDeviceSize size, const void* data) {
    std::uint32_t value = 0;
    if (size >= sizeof(value)) std::memcpy(&value, data, sizeof(value));
    record(commands, "update " + std::to_string(value));
}

VKAPI_ATTR void VKAPI_CALL mockCmdDispatch(VkCommandBuffer commands, std::uint32_t x, std::uint32_t, std::uint32_t) {
    record(commands, "dispatch " + std::to_string(x));
}

VKAPI_ATTR void VKAPI_CALL mockCmdPipelineBarrier(VkCommandBuffer commands, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, std::uint32_t memoryCount, const VkMemoryBarrier* memory, std::uint32_t, const VkBufferMemoryBarrier*, std::uint32_t, const VkImageMemoryBarrier*) {
    record(commands, "barrier " + std::to_string(memoryCount != 0 ? memory[0].srcAccessMask : 0u));
}

VKAPI_ATTR void VKAPI_CALL mockCmdBeginQuery(VkCommandBuffer commands, VkQueryPool, std::uint32_t, VkQueryControlFlags) {
    record(commands, "begin query");
}

VKAPI_ATTR void VKAPI_CALL mockCmdEndQuery(VkCommandBuffer commands, VkQueryPool, std::uint32_t) {
    record(commands, "end query");
}

VKAPI_ATTR void VKAPI_CALL mockCmdResetQueryPool(VkCommandBuffer commands, VkQueryPool, std::uint32_t, std::uint32_t) {
    record(commands, "reset queries");
}

VKAPI_ATTR void VKAPI_CALL mockCmdCopyQueryPoolResults(VkCommandBuffer commands, VkQueryPool, std::uint32_t, std::uint32_t, VkBuffer, VkDeviceSize, VkDeviceSize, VkQueryResultFlags) {
    record(commands, "copy queries");
}

VKAPI_ATTR void VKAPI_CALL mockCmdBindPipeline(VkCommandBuffer commands, VkPipelineBindPoint, VkPipeline) {
    record(commands, "bind pipeline");
}

VKAPI_ATTR void VKAPI_CALL mockCmdPushConstants(VkCommandBuffer commands, VkPipelineLayout, VkShaderStageFlags, std::uint32_t, std::uint32_t size, const void* values) {
    std::uint32_t value = 0;
    if (size >= sizeof(value)) std::memcpy(&value, values, sizeof(value));
    record(commands, "push " + std::to_string(value));
}

struct QueueUse {
    VkQueue queue;
    explicit QueueUse(VkQueue queue, const char* call) : queue(queue) {
        std::lock_guard lock(mock->mutex);
        if (++mock->queueUsers[queue] > 1) mock->Violation(std::string(call) + " ran on a queue another thread was using");
    }
    ~QueueUse() {
        std::lock_guard lock(mock->mutex);
        --mock->queueUsers[queue];
    }
};

VKAPI_ATTR VkResult VKAPI_CALL mockQueueBindSparse(VkQueue queue, std::uint32_t, const VkBindSparseInfo*, VkFence) {
    const QueueUse use(queue, "vkQueueBindSparse");
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    std::lock_guard lock(mock->mutex);
    mock->events.push_back("bind " + std::to_string(reinterpret_cast<std::uintptr_t>(queue)));
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockQueueSubmit(VkQueue queue, std::uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    const QueueUse use(queue, "vkQueueSubmit");
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    std::unique_lock lock(mock->mutex);
    mock->events.push_back("submit");
    if (mock->failSubmits != 0) {
        --mock->failSubmits;
        ++mock->failedSubmitCalls;
        mock->changed.notify_all();
        return mock->failedSubmitResult;
    }
    std::vector<VkCommandBuffer> submitted;
    for (std::uint32_t submit = 0; submit < count; ++submit) {
        Submission submission;
        submission.fence = fence;
        submission.thread = std::this_thread::get_id();
        if (submits[submit].commandBufferCount > 1) mock->Violation("a batch submitted more than one command buffer");
        if (submits[submit].commandBufferCount == 1) {
            const auto commands = submits[submit].pCommandBuffers[0];
            submission.commands = commands;
            if (CommandStream::IsHandle(commands)) mock->Violation("a stream handle was submitted");
            const auto owner = mock->owners.find(commands);
            submission.pool = owner != mock->owners.end() ? owner->second : VK_NULL_HANDLE;
            if (mock->states[commands] != BufferState::Executable) mock->Violation("a command buffer was submitted before it was ended");
            submission.recorded = mock->recorded[commands];
            submission.recordingThreads = mock->recordingThreads[commands];
            submitted.push_back(commands);
        }
        mock->submissions.push_back(std::move(submission));
    }
    if (fence != VK_NULL_HANDLE) mock->signaled[fence] = true;
    if (mock->holdSubmitReturn) {
        if (fence != VK_NULL_HANDLE) mock->fencesInSubmit.insert(fence);
        mock->buffersInSubmit.insert(submitted.begin(), submitted.end());
        mock->changed.notify_all();
        mock->changed.wait(lock, [] { return mock->submitReturnOpen; });
        mock->fencesInSubmit.erase(fence);
        for (const auto commands : submitted) mock->buffersInSubmit.erase(commands);
    }
    mock->changed.notify_all();
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockCreateFence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* fence) {
    std::lock_guard lock(mock->mutex);
    *fence = handleOf<VkFence>(mock->Handle());
    mock->signaled[*fence] = false;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockDestroyFence(VkDevice, VkFence fence, const VkAllocationCallbacks*) {
    std::lock_guard lock(mock->mutex);
    if (mock->fencesInSubmit.count(fence) != 0) mock->Violation("a fence was destroyed while its vkQueueSubmit had not returned");
    mock->signaled.erase(fence);
}

VKAPI_ATTR VkResult VKAPI_CALL mockGetFenceStatus(VkDevice, VkFence fence) {
    std::lock_guard lock(mock->mutex);
    return mock->signaled[fence] ? VK_SUCCESS : VK_NOT_READY;
}

VKAPI_ATTR VkResult VKAPI_CALL mockWaitForFences(VkDevice, std::uint32_t count, const VkFence* fences, VkBool32, std::uint64_t) {
    std::unique_lock lock(mock->mutex);
    const bool signaled = mock->changed.wait_for(lock, std::chrono::seconds(10), [&] {
        return std::all_of(fences, fences + count, [](VkFence fence) { return mock->signaled[fence]; });
    });
    if (signaled) return VK_SUCCESS;
    mock->Violation("a fence wait timed out: its batch was never submitted");
    return VK_ERROR_DEVICE_LOST;
}

VKAPI_ATTR VkResult VKAPI_CALL mockResetFences(VkDevice, std::uint32_t count, const VkFence* fences) {
    std::lock_guard lock(mock->mutex);
    for (std::uint32_t index = 0; index < count; ++index) {
        if (mock->fencesInSubmit.count(fences[index]) != 0) mock->Violation("a fence was reset while its vkQueueSubmit had not returned");
        mock->signaled[fences[index]] = false;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockDeviceWaitIdle(VkDevice) {
    return VK_SUCCESS;
}

PFN_vkVoidFunction VKAPI_CALL mockProc(VkDevice, const char* name) {
    static const std::map<std::string_view, PFN_vkVoidFunction> table{
        {"vkCreateCommandPool", reinterpret_cast<PFN_vkVoidFunction>(mockCreateCommandPool)},
        {"vkDestroyCommandPool", reinterpret_cast<PFN_vkVoidFunction>(mockDestroyCommandPool)},
        {"vkAllocateCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(mockAllocateCommandBuffers)},
        {"vkFreeCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(mockFreeCommandBuffers)},
        {"vkBeginCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockBeginCommandBuffer)},
        {"vkEndCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockEndCommandBuffer)},
        {"vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(mockQueueSubmit)},
        {"vkQueueBindSparse", reinterpret_cast<PFN_vkVoidFunction>(mockQueueBindSparse)},
        {"vkCreateFence", reinterpret_cast<PFN_vkVoidFunction>(mockCreateFence)},
        {"vkDestroyFence", reinterpret_cast<PFN_vkVoidFunction>(mockDestroyFence)},
        {"vkGetFenceStatus", reinterpret_cast<PFN_vkVoidFunction>(mockGetFenceStatus)},
        {"vkWaitForFences", reinterpret_cast<PFN_vkVoidFunction>(mockWaitForFences)},
        {"vkResetFences", reinterpret_cast<PFN_vkVoidFunction>(mockResetFences)},
        {"vkDeviceWaitIdle", reinterpret_cast<PFN_vkVoidFunction>(mockDeviceWaitIdle)},
        {"vkCmdFillBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockCmdFillBuffer)},
        {"vkCmdUpdateBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockCmdUpdateBuffer)},
        {"vkCmdDispatch", reinterpret_cast<PFN_vkVoidFunction>(mockCmdDispatch)},
        {"vkCmdPipelineBarrier", reinterpret_cast<PFN_vkVoidFunction>(mockCmdPipelineBarrier)},
        {"vkCmdBeginQuery", reinterpret_cast<PFN_vkVoidFunction>(mockCmdBeginQuery)},
        {"vkCmdEndQuery", reinterpret_cast<PFN_vkVoidFunction>(mockCmdEndQuery)},
        {"vkCmdResetQueryPool", reinterpret_cast<PFN_vkVoidFunction>(mockCmdResetQueryPool)},
        {"vkCmdCopyQueryPoolResults", reinterpret_cast<PFN_vkVoidFunction>(mockCmdCopyQueryPoolResults)},
        {"vkCmdBindPipeline", reinterpret_cast<PFN_vkVoidFunction>(mockCmdBindPipeline)},
        {"vkCmdPushConstants", reinterpret_cast<PFN_vkVoidFunction>(mockCmdPushConstants)},
    };
    const auto it = table.find(name);
    return it == table.end() ? nullptr : it->second;
}

std::uintptr_t nextQueue = 0x7000000;

struct Fixture {
    MockDevice device;
    Context context{};
    std::thread::id main = std::this_thread::get_id();

    explicit Fixture(VkQueue queue = VK_NULL_HANDLE) {
        mock = &device;
        device.contextPool = handleOf<VkCommandPool>(device.Handle());
        device.pools.insert(device.contextPool);
        context.device = handleOf<VkDevice>(device.Handle());
        nextQueue += 0x10;
        context.queue = queue != VK_NULL_HANDLE ? queue : handleOf<VkQueue>(nextQueue);
        context.pool = device.contextPool;
        context.deviceProc = RecordCommandStreams(context.device, mockProc);
    }

    ~Fixture() {
        std::lock_guard lock(device.mutex);
        for (const auto& violation : device.violations) Expect(false, violation);
        mock = nullptr;
    }

    void Fill(Recorder& recorder, std::uint32_t value) const {
        context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(recorder.Commands(), handleOf<VkBuffer>(0x100), 0, 4, value);
    }

    void Update(Recorder& recorder, std::uint32_t value) const {
        std::array<std::uint32_t, 4> data{value, 0, 0, 0};
        context.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer")(recorder.Commands(), handleOf<VkBuffer>(0x100), 0, sizeof(data), data.data());
        data.fill(0xdeadbeefu);
    }

    std::vector<Submission> Submissions() {
        std::lock_guard lock(device.mutex);
        return device.submissions;
    }

    void Drain() const {
        if (const auto submitter = QueueSubmitter::Existing(context.queue); submitter != nullptr) submitter->Drain();
    }
};

std::string joined(const std::vector<std::string>& ops) {
    std::string text;
    for (const auto& op : ops) text += (text.empty() ? "" : ", ") + op;
    return "[" + text + "]";
}

void DeferredBatchesEncodeOffTheRecordingThread() {
    RuntimeSwitches::Assign(RuntimeSwitch::AsyncSubmit, true);
    RuntimeSwitches::Assign(RuntimeSwitch::DeferredEncode, true);
    const auto before = CommandEncoders::Totals();
    Fixture fixture;
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Recorder recorder(fixture.context);
        fixture.Fill(recorder, 1);
        fixture.Fill(recorder, 2);
        fixture.Update(recorder, 3);
        Expect(CommandStream::IsHandle(recorder.Commands()), "a deferred batch handed out a command buffer instead of its stream handle");
        recorder.Submit();
        fixture.Fill(recorder, 4);
        recorder.Sync();
        Expect(recorder.Idle(), "a synced deferred batch is still in flight");
        auto submissions = fixture.Submissions();
        Expect(submissions.size() == 2, "two deferred batches made " + std::to_string(submissions.size()) + " submissions");
        if (submissions.size() == 2) {
            Expect(joined(submissions[0].recorded) == "[fill 1, fill 2, update 3]", "the first deferred batch encoded " + joined(submissions[0].recorded));
            Expect(joined(submissions[1].recorded) == "[fill 4]", "the second deferred batch encoded " + joined(submissions[1].recorded));
            for (const auto& submission : submissions) {
                Expect(submission.pool != fixture.context.pool && submission.pool != VK_NULL_HANDLE, "a deferred batch was encoded into the device's command pool");
                Expect(submission.thread != fixture.main, "a deferred batch was submitted on the recording thread");
                Expect(!submission.recordingThreads.empty() && submission.recordingThreads.count(fixture.main) == 0, "a deferred batch's commands reached the driver on the recording thread");
            }
        }
        for (std::uint32_t batch = 0; batch < 12; ++batch) {
            fixture.Fill(recorder, 10 + batch);
            recorder.Sync();
        }
        submissions = fixture.Submissions();
        Expect(submissions.size() == 14, "fourteen deferred batches made " + std::to_string(submissions.size()) + " submissions");
        for (std::size_t index = 2; index < submissions.size(); ++index) Expect(joined(submissions[index].recorded) == "[fill " + std::to_string(8 + index) + "]", "deferred batch " + std::to_string(index) + " encoded " + joined(submissions[index].recorded));
        std::lock_guard lock(fixture.device.mutex);
        Expect(fixture.device.poolsCreated == CommandEncoders::Count(), std::to_string(CommandEncoders::Count()) + " encoders created " + std::to_string(fixture.device.poolsCreated) + " command pools");
        Expect(fixture.device.allocated <= 2 * CommandEncoders::Count(), "fourteen deferred batches, at most two in flight, allocated " + std::to_string(fixture.device.allocated) + " command buffers instead of reusing their encoders' spares");
    }
    const auto after = CommandEncoders::Totals();
    Expect(after.streams - before.streams == 14 && after.ops - before.ops == 16, "the encode counters saw " + std::to_string(after.streams - before.streams) + " streams and " + std::to_string(after.ops - before.ops) + " ops, not 14 and 16");
    std::lock_guard lock(fixture.device.mutex);
    Expect(fixture.device.poolsDestroyed == fixture.device.poolsCreated, "the recorder's teardown left encoder command pools alive");
    Expect(fixture.device.allocated == fixture.device.freed, "the recorder's teardown leaked " + std::to_string(fixture.device.allocated - fixture.device.freed) + " command buffers");
}

void SubmitWaitsForTheEncodeAndDrainWaitsForBoth() {
    RuntimeSwitches::Assign(RuntimeSwitch::AsyncSubmit, true);
    RuntimeSwitches::Assign(RuntimeSwitch::DeferredEncode, true);
    const auto before = CommandEncoders::Totals();
    Fixture fixture;
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Recorder recorder(fixture.context);
        {
            std::lock_guard lock(fixture.device.mutex);
            fixture.device.gate = true;
        }
        fixture.Fill(recorder, 7);
        recorder.Submit();
        std::atomic<std::size_t> submittedBeforeGate{99};
        std::atomic<bool> gateOpened{false};
        std::thread opener([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            {
                std::lock_guard lock(fixture.device.mutex);
                submittedBeforeGate.store(fixture.device.submissions.size());
                fixture.device.gateOpen = true;
                gateOpened.store(true);
            }
            fixture.device.changed.notify_all();
        });
        fixture.Drain();
        const bool openedBeforeDrain = gateOpened.load();
        const auto submissions = fixture.Submissions();
        opener.join();
        Expect(submittedBeforeGate.load() == 0, "the submitter submitted a deferred batch before its encode finished");
        Expect(openedBeforeDrain, "Drain returned while a queued batch was still being encoded");
        Expect(submissions.size() == 1 && joined(submissions[0].recorded) == "[fill 7]", "the drained deferred batch was not submitted with its commands");
        {
            std::lock_guard lock(fixture.device.mutex);
            fixture.device.gate = false;
        }
        fixture.Fill(recorder, 8);
        recorder.Submit();
        {
            const QueueAccess access(fixture.context.queue);
            const auto accessed = fixture.Submissions();
            Expect(accessed.size() == 2 && joined(accessed[1].recorded) == "[fill 8]", "queue access did not wait for the queued deferred batch");
        }
        recorder.Sync();
    }
    const auto after = CommandEncoders::Totals();
    Expect(after.waits > before.waits && after.waitNs > before.waitNs, "the submitter's wait for a held encode was not counted");
}

struct Watchdog {
    std::atomic<bool> done{false};
    std::thread thread;
    explicit Watchdog(std::string what) {
        thread = std::thread([this, what = std::move(what)] {
            for (int tick = 0; tick < 150; ++tick) {
                if (done.load()) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (done.load()) return;
            std::fprintf(stderr, "FAIL: %s did not finish within 15 s\n", what.c_str());
            std::fflush(stderr);
            std::_Exit(1);
        });
    }
    ~Watchdog() {
        done.store(true);
        thread.join();
    }
};

std::string Reported(const std::function<void()>& action) {
    try {
        action();
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

std::size_t SignaledFences(Fixture& fixture) {
    std::lock_guard lock(fixture.device.mutex);
    std::size_t count = 0;
    for (const auto& [fence, signaled] : fixture.device.signaled) count += signaled ? 1 : 0;
    return count;
}

void FailedEncodeNeverCompletes() {
    RuntimeSwitches::Assign(RuntimeSwitch::AsyncSubmit, true);
    RuntimeSwitches::Assign(RuntimeSwitch::DeferredEncode, true);
    const auto before = CommandEncoders::Totals();
    Fixture fixture;
    {
        const Watchdog watchdog("a batch whose encode failed");
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Recorder recorder(fixture.context);
        bool completed = false;
        recorder.OnComplete([&] { completed = true; });
        {
            std::lock_guard lock(fixture.device.mutex);
            fixture.device.encoderEndResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        fixture.Fill(recorder, 5);
        recorder.Submit();
        const auto reported = Reported([&] { recorder.Sync(); });
        Expect(reported.find("vkEndCommandBuffer encoder") != std::string::npos, "the sync of a batch whose encode failed did not report the failure: '" + reported + "'");
        Expect(!completed, "a batch whose encode failed ran its completions");
        Expect(fixture.Submissions().empty(), "a batch whose encode failed was submitted");
        Expect(SignaledFences(fixture) == 0, "a batch whose encode failed signaled its fence");
        Expect(recorder.Idle(), "a batch whose encode failed stayed in flight after it was reported");
        {
            std::lock_guard lock(fixture.device.mutex);
            fixture.device.encoderEndResult = VK_SUCCESS;
        }
        fixture.Fill(recorder, 6);
        recorder.Sync();
        const auto submissions = fixture.Submissions();
        Expect(submissions.size() == 1 && joined(submissions[0].recorded) == "[fill 6]", "the batch after a failed encode was not submitted with its own commands");
    }
    const auto after = CommandEncoders::Totals();
    Expect(after.failures - before.failures == 1, "the failed encode was not counted");
    std::lock_guard lock(fixture.device.mutex);
    Expect(fixture.device.allocated == fixture.device.freed, "a failed encode leaked " + std::to_string(fixture.device.allocated - fixture.device.freed) + " command buffers");
}

void FailedSubmitReachesFinish() {
    RuntimeSwitches::Assign(RuntimeSwitch::AsyncSubmit, true);
    for (const bool deferred : {true, false}) {
        RuntimeSwitches::Assign(RuntimeSwitch::DeferredEncode, deferred);
        const std::string kind = deferred ? "deferred" : "direct";
        for (const bool reap : {false, true}) {
            Fixture fixture;
            {
                const Watchdog watchdog("a " + kind + " batch whose vkQueueSubmit failed");
                std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
                Recorder recorder(fixture.context);
                bool completed = false;
                recorder.OnComplete([&] { completed = true; });
                {
                    std::lock_guard lock(fixture.device.mutex);
                    fixture.device.failSubmits = 1;
                }
                fixture.Fill(recorder, 1);
                recorder.Submit();
                fixture.Drain();
                std::string reported;
                if (reap) {
                    reported = Reported([&] { recorder.Reap(); });
                    Expect(!reported.empty(), "a " + kind + " batch whose vkQueueSubmit failed was not retired by Reap");
                } else {
                    reported = Reported([&] { recorder.Sync(); });
                }
                Expect(reported.find("vkQueueSubmit recorder") != std::string::npos, "a " + kind + " batch whose vkQueueSubmit failed reported '" + reported + "'");
                Expect(!completed, "a " + kind + " batch whose vkQueueSubmit failed ran its completions");
                Expect(SignaledFences(fixture) == 0, "a " + kind + " batch whose vkQueueSubmit failed signaled its fence");
                Expect(recorder.Idle(), "a " + kind + " batch whose vkQueueSubmit failed stayed in flight after it was reported");
                fixture.Fill(recorder, 2);
                recorder.Sync();
                const auto submissions = fixture.Submissions();
                Expect(submissions.size() == 1 && joined(submissions[0].recorded) == "[fill 2]", "the " + kind + " batch after a failed vkQueueSubmit was not submitted with its own commands");
            }
            std::lock_guard lock(fixture.device.mutex);
            Expect(fixture.device.allocated == fixture.device.freed, "a failed " + kind + " submit leaked " + std::to_string(fixture.device.allocated - fixture.device.freed) + " command buffers");
        }
    }
}

void BatchesBehindAFailureAreNotSubmitted() {
    RuntimeSwitches::Assign(RuntimeSwitch::AsyncSubmit, true);
    for (const bool deferred : {true, false}) {
        RuntimeSwitches::Assign(RuntimeSwitch::DeferredEncode, deferred);
        const std::string kind = deferred ? "deferred" : "direct";
        Fixture fixture;
        {
            const Watchdog watchdog("batches queued behind a failed " + kind + " submit");
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            Recorder recorder(fixture.context);
            std::size_t completed = 0;
            {
                std::lock_guard lock(fixture.device.mutex);
                fixture.device.failSubmits = 1;
            }
            for (std::uint32_t batch = 0; batch < 3; ++batch) {
                recorder.OnComplete([&] { ++completed; });
                fixture.Fill(recorder, batch);
                recorder.Submit();
                fixture.Drain();
            }
            std::size_t reported = 0;
            while (!recorder.Idle()) {
                if (!Reported([&] { recorder.Sync(); }).empty()) ++reported;
            }
            Expect(reported == 3, "the three batches queued behind a failed " + kind + " submit reported " + std::to_string(reported) + " failures");
            Expect(completed == 0 && fixture.Submissions().empty(), "a batch queued behind a failed " + kind + " submit ran");
            fixture.Fill(recorder, 9);
            recorder.Sync();
            const auto submissions = fixture.Submissions();
            Expect(submissions.size() == 1 && joined(submissions[0].recorded) == "[fill 9]", "the " + kind + " batch after the reported failures was not submitted");
        }
    }
}

void SparseBindsShareTheQueueWithTheSubmitter() {
    RuntimeSwitches::Assign(RuntimeSwitch::AsyncSubmit, true);
    RuntimeSwitches::Assign(RuntimeSwitch::DeferredEncode, true);
    for (const bool ownQueue : {false, true}) {
        Fixture fixture;
        std::mutex sparseMutex;
        if (ownQueue) {
            fixture.context.sparseQueue = handleOf<VkQueue>(0x7fff0000);
            fixture.context.sparseQueueMutex = &sparseMutex;
        }
        {
            const Watchdog watchdog("a sparse bind next to the queue submitter");
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            Recorder recorder(fixture.context);
            {
                std::lock_guard lock(fixture.device.mutex);
                fixture.device.gate = true;
            }
            fixture.Fill(recorder, 1);
            recorder.Submit();
            std::thread opener([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                {
                    std::lock_guard lock(fixture.device.mutex);
                    fixture.device.gateOpen = true;
                }
                fixture.device.changed.notify_all();
            });
            const VkBindSparseInfo info{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
            for (int bind = 0; bind < 20; ++bind) Expect(QueueBindSparse(fixture.context, info, VK_NULL_HANDLE) == VK_SUCCESS, "a sparse bind failed");
            opener.join();
            recorder.Sync();
            std::thread binder([&] {
                for (int bind = 0; bind < 50; ++bind) QueueBindSparse(fixture.context, info, VK_NULL_HANDLE);
            });
            for (std::uint32_t batch = 0; batch < 50; ++batch) {
                fixture.Fill(recorder, 100 + batch);
                recorder.Submit();
            }
            binder.join();
            recorder.Sync();
            std::lock_guard lock(fixture.device.mutex);
            const auto& events = fixture.device.events;
            const std::string queue = std::to_string(reinterpret_cast<std::uintptr_t>(ownQueue ? fixture.context.sparseQueue : fixture.context.queue));
            Expect(events.size() == 121 && events.front() == (ownQueue ? "bind " + queue : "submit"), std::string(ownQueue ? "a sparse bind on its own queue waited for the graphics queue's submitter" : "a sparse bind on the graphics queue overtook a batch still queued in the submitter"));
            Expect(std::count(events.begin(), events.end(), "bind " + queue) == 70, "a sparse bind went to the wrong queue");
        }
    }
}

void SubmittersAreScopedToTheirDevice() {
    RuntimeSwitches::Assign(RuntimeSwitch::AsyncSubmit, true);
    RuntimeSwitches::Assign(RuntimeSwitch::DeferredEncode, true);
    VkQueue reused = VK_NULL_HANDLE;
    for (int round = 0; round < 3; ++round) {
        const Watchdog watchdog("a device that reuses a queue handle");
        Fixture fixture(reused);
        reused = fixture.context.queue;
        std::weak_ptr<QueueSubmitter> submitter;
        {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            Recorder recorder(fixture.context);
            fixture.Fill(recorder, 1);
            recorder.Submit();
            recorder.Sync();
            submitter = QueueSubmitter::Existing(reused);
            Expect(!submitter.expired(), "a recorder that submitted asynchronously has no submitter");
            if (round == 0) {
                {
                    std::lock_guard lock(fixture.device.mutex);
                    fixture.device.failSubmits = 1;
                }
                fixture.Fill(recorder, 2);
                recorder.Submit();
                Expect(!Reported([&] { recorder.Sync(); }).empty(), "a failed submit was not reported");
            }
            fixture.Fill(recorder, 3);
            recorder.Sync();
            const auto submissions = fixture.Submissions();
            Expect(!submissions.empty() && joined(submissions.back().recorded) == "[fill 3]", "a batch on a reused queue handle was not submitted through its own device");
        }
        Expect(submitter.expired() && QueueSubmitter::Existing(reused) == nullptr, "a submitter outlived the recorder that owned it");
    }
}

void DeferredEncodeIsDecidedPerBatch() {
    RuntimeSwitches::Assign(RuntimeSwitch::AsyncSubmit, true);
    Fixture fixture;
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Recorder recorder(fixture.context);
        RuntimeSwitches::Assign(RuntimeSwitch::DeferredEncode, false);
        fixture.Fill(recorder, 1);
        Expect(!CommandStream::IsHandle(recorder.Commands()), "a direct batch handed out a stream handle");
        RuntimeSwitches::Assign(RuntimeSwitch::DeferredEncode, true);
        fixture.Fill(recorder, 2);
        recorder.Submit();
        fixture.Fill(recorder, 3);
        Expect(CommandStream::IsHandle(recorder.Commands()), "a batch opened with the switch set was not deferred");
        RuntimeSwitches::Assign(RuntimeSwitch::DeferredEncode, false);
        fixture.Fill(recorder, 4);
        recorder.Submit();
        fixture.Fill(recorder, 5);
        recorder.Sync();
        const auto submissions = fixture.Submissions();
        Expect(submissions.size() == 3, "three batches made " + std::to_string(submissions.size()) + " submissions");
        if (submissions.size() == 3) {
            Expect(joined(submissions[0].recorded) == "[fill 1, fill 2]" && submissions[0].pool == fixture.context.pool && submissions[0].recordingThreads.count(fixture.main) == 1, "a batch opened with the switch unset was not recorded directly");
            Expect(joined(submissions[1].recorded) == "[fill 3, fill 4]" && submissions[1].pool != fixture.context.pool && submissions[1].recordingThreads.count(fixture.main) == 0, "a batch opened with the switch set was not encoded on an encoder");
            Expect(joined(submissions[2].recorded) == "[fill 5]" && submissions[2].pool == fixture.context.pool, "the batch after the switch went off was not recorded directly");
        }
    }
    std::lock_guard lock(fixture.device.mutex);
    Expect(fixture.device.allocated == fixture.device.freed, "mixed batches leaked " + std::to_string(fixture.device.allocated - fixture.device.freed) + " command buffers");
}

void AsyncSubmitOffEncodesInline() {
    RuntimeSwitches::Assign(RuntimeSwitch::AsyncSubmit, true);
    RuntimeSwitches::Assign(RuntimeSwitch::DeferredEncode, true);
    Fixture fixture;
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Recorder recorder(fixture.context);
        fixture.Fill(recorder, 1);
        RuntimeSwitches::Assign(RuntimeSwitch::AsyncSubmit, false);
        fixture.Fill(recorder, 2);
        recorder.Submit();
        const auto submissions = fixture.Submissions();
        Expect(submissions.size() == 1, "a deferred batch submitted after the submitter went off made " + std::to_string(submissions.size()) + " submissions");
        if (submissions.size() == 1) {
            Expect(joined(submissions[0].recorded) == "[fill 1, fill 2]", "a deferred batch encoded inline recorded " + joined(submissions[0].recorded));
            Expect(submissions[0].pool == fixture.context.pool && submissions[0].thread == fixture.main && submissions[0].recordingThreads.count(fixture.main) == 1, "a deferred batch submitted without the submitter was not encoded and submitted on the calling thread");
        }
        fixture.Fill(recorder, 3);
        Expect(!CommandStream::IsHandle(recorder.Commands()), "a batch opened without the submitter was deferred");
        recorder.Sync();
    }
    std::lock_guard lock(fixture.device.mutex);
    Expect(fixture.device.poolsCreated == 0, "inline encodes started encoder threads");
    Expect(fixture.device.allocated == fixture.device.freed, "inline encodes leaked " + std::to_string(fixture.device.allocated - fixture.device.freed) + " command buffers");
}

void DiscardedBatchIsNeverEncoded() {
    RuntimeSwitches::Assign(RuntimeSwitch::AsyncSubmit, true);
    RuntimeSwitches::Assign(RuntimeSwitch::DeferredEncode, true);
    const auto before = CommandEncoders::Totals();
    Fixture fixture;
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Recorder recorder(fixture.context);
        fixture.Fill(recorder, 1);
        Recorder::FailNextSubmitsForTest(1);
        bool failed = false;
        try {
            recorder.Submit();
        } catch (const std::exception&) {
            failed = true;
        }
        Expect(failed && !recorder.Recording(), "an injected submit failure did not discard the deferred batch");
        fixture.Fill(recorder, 2);
        recorder.Sync();
        const auto submissions = fixture.Submissions();
        Expect(submissions.size() == 1 && joined(submissions[0].recorded) == "[fill 2]", "the batch after a discarded deferred batch encoded " + (submissions.empty() ? std::string("nothing") : joined(submissions[0].recorded)));
    }
    const auto after = CommandEncoders::Totals();
    Expect(after.streams - before.streams == 1, "a discarded deferred batch was encoded");
}

void RetiredBatchesWaitForTheirSubmitToReturn() {
    RuntimeSwitches::Assign(RuntimeSwitch::AsyncSubmit, true);
    for (const bool deferred : {true, false}) {
        RuntimeSwitches::Assign(RuntimeSwitch::DeferredEncode, deferred);
        const std::string kind = deferred ? "deferred" : "direct";
        Fixture fixture;
        {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            Recorder recorder(fixture.context);
            {
                std::lock_guard lock(fixture.device.mutex);
                fixture.device.holdSubmitReturn = true;
            }
            fixture.Fill(recorder, 1);
            recorder.Submit();
            bool inSubmit = false;
            {
                std::unique_lock lock(fixture.device.mutex);
                inSubmit = fixture.device.changed.wait_for(lock, std::chrono::seconds(10), [&] { return !fixture.device.fencesInSubmit.empty(); });
            }
            std::atomic<bool> opened{false};
            std::thread opener([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                {
                    std::lock_guard lock(fixture.device.mutex);
                    fixture.device.holdSubmitReturn = false;
                    fixture.device.submitReturnOpen = true;
                    opened.store(true);
                }
                fixture.device.changed.notify_all();
            });
            const bool idle = recorder.Reap();
            const bool openedFirst = opened.load();
            opener.join();
            Expect(inSubmit, "the " + kind + " batch never reached vkQueueSubmit");
            Expect(idle, "a " + kind + " batch whose fence signaled was not retired");
            Expect(openedFirst, "a " + kind + " batch was retired, and its fence and command buffer reused, before its vkQueueSubmit returned");
            fixture.Fill(recorder, 2);
            recorder.Sync();
            const auto submissions = fixture.Submissions();
            Expect(submissions.size() == 2 && joined(submissions[1].recorded) == "[fill 2]", "the " + kind + " batch after a held submit was not submitted with its commands");
        }
    }
}

void StaleHandleIsRefusedAfterSubmit() {
    RuntimeSwitches::Assign(RuntimeSwitch::AsyncSubmit, true);
    RuntimeSwitches::Assign(RuntimeSwitch::DeferredEncode, true);
    Fixture fixture;
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Recorder recorder(fixture.context);
        fixture.Fill(recorder, 1);
        const auto stale = recorder.Commands();
        recorder.Submit();
        std::string refused;
        try {
            fixture.context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(stale, handleOf<VkBuffer>(0x100), 0, 4, 2);
        } catch (const std::exception& error) {
            refused = error.what();
        }
        Expect(refused.find("after its batch was closed") != std::string::npos, "a command recorded through a submitted batch's stream handle was not refused: '" + refused + "'");
        recorder.Sync();
        const auto submissions = fixture.Submissions();
        Expect(submissions.size() == 1 && joined(submissions[0].recorded) == "[fill 1]", "the refused command reached the submitted batch: " + (submissions.empty() ? std::string("nothing") : joined(submissions[0].recorded)));
    }
}

void RandomProgramsKeepProgramOrder() {
    RuntimeSwitches::Assign(RuntimeSwitch::AsyncSubmit, true);
    RuntimeSwitches::Assign(RuntimeSwitch::DeferredEncode, true);
    for (std::uint32_t seed = 1; seed <= 6; ++seed) {
        const Watchdog watchdog("a random program, seed " + std::to_string(seed));
        std::mt19937 random(seed);
        Fixture fixture;
        fixture.device.jitter = seed % 2 == 0;
        std::vector<std::vector<std::string>> expected(1);
        {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            Recorder recorder(fixture.context);
            const auto close = [&] {
                if (!expected.back().empty()) expected.emplace_back();
            };
            for (std::uint32_t step = 0; step < 600; ++step) {
                const auto value = static_cast<std::uint32_t>(random() % 251);
                auto& ops = expected.back();
                switch (random() % 14) {
                case 0:
                case 1:
                    fixture.Fill(recorder, value);
                    ops.push_back("fill " + std::to_string(value));
                    break;
                case 2:
                    fixture.Update(recorder, value);
                    ops.push_back("update " + std::to_string(value));
                    break;
                case 3:
                    fixture.context.Function<PFN_vkCmdDispatch>("vkCmdDispatch")(recorder.Commands(), value, 1, 1);
                    ops.push_back("dispatch " + std::to_string(value));
                    break;
                case 4: {
                    std::array<std::uint32_t, 8> values;
                    values.fill(value);
                    fixture.context.Function<PFN_vkCmdPushConstants>("vkCmdPushConstants")(recorder.Commands(), handleOf<VkPipelineLayout>(0x200), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(values), values.data());
                    values.fill(0xdeadbeefu);
                    ops.push_back("push " + std::to_string(value));
                    break;
                }
                case 5: {
                    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                    barrier.srcAccessMask = value;
                    fixture.context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(recorder.Commands(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
                    barrier.srcAccessMask = 0xdeadbeefu;
                    ops.push_back("barrier " + std::to_string(value));
                    break;
                }
                case 6:
                    fixture.context.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(recorder.Commands(), VK_PIPELINE_BIND_POINT_COMPUTE, handleOf<VkPipeline>(0x300 + value));
                    ops.push_back("bind pipeline");
                    break;
                case 7:
                    fixture.context.Function<PFN_vkCmdBeginQuery>("vkCmdBeginQuery")(recorder.Commands(), handleOf<VkQueryPool>(0x400), value, 0);
                    fixture.context.Function<PFN_vkCmdEndQuery>("vkCmdEndQuery")(recorder.Commands(), handleOf<VkQueryPool>(0x400), value);
                    ops.push_back("begin query");
                    ops.push_back("end query");
                    break;
                case 8:
                case 9:
                    recorder.Submit();
                    close();
                    break;
                case 10:
                    recorder.Sync();
                    close();
                    break;
                case 11:
                    recorder.Reap();
                    break;
                default:
                    fixture.Fill(recorder, value);
                    ops.push_back("fill " + std::to_string(value));
                    break;
                }
            }
            recorder.Sync();
        }
        std::vector<std::vector<std::string>> batches;
        for (const auto& ops : expected) {
            if (!ops.empty()) batches.push_back(ops);
        }
        const auto submissions = fixture.Submissions();
        Expect(submissions.size() == batches.size(), "seed " + std::to_string(seed) + ": " + std::to_string(batches.size()) + " batches made " + std::to_string(submissions.size()) + " submissions");
        for (std::size_t index = 0; index < std::min(submissions.size(), batches.size()); ++index) Expect(submissions[index].recorded == batches[index], "seed " + std::to_string(seed) + ": batch " + std::to_string(index) + " encoded " + joined(submissions[index].recorded) + ", the program recorded " + joined(batches[index]));
    }
}

void KeptOwnersOutliveTheirEncode() {
    RuntimeSwitches::Assign(RuntimeSwitch::AsyncSubmit, true);
    RuntimeSwitches::Assign(RuntimeSwitch::DeferredEncode, true);
    for (const bool failing : {false, true}) {
        Fixture fixture;
        {
            const Watchdog watchdog("a batch holding kept objects");
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            Recorder recorder(fixture.context);
            std::atomic<bool> destroyed{false};
            std::size_t submissionsAtDestruction = 99;
            std::shared_ptr<void> owner(new int(0), [&](int* value) {
                submissionsAtDestruction = fixture.Submissions().size();
                destroyed.store(true);
                delete value;
            });
            const std::weak_ptr<void> watched = owner;
            {
                std::lock_guard lock(fixture.device.mutex);
                fixture.device.gate = true;
                if (failing) fixture.device.encoderEndResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
            }
            fixture.Fill(recorder, 1);
            recorder.Keep(owner);
            owner.reset();
            recorder.Submit();
            Expect(!recorder.Reap() && !destroyed.load() && !watched.expired(), "a kept object was released while its batch was still being encoded");
            {
                std::lock_guard lock(fixture.device.mutex);
                fixture.device.gateOpen = true;
            }
            fixture.device.changed.notify_all();
            Reported([&] { recorder.Sync(); });
            recorder.FlushDeferredReleases();
            Expect(destroyed.load() && watched.expired(), "a kept object outlived its retired batch");
            Expect(failing || submissionsAtDestruction == 1, "a kept object was released before its batch was submitted");
        }
    }
}

void TeardownWaitsForQueuedEncodes() {
    RuntimeSwitches::Assign(RuntimeSwitch::AsyncSubmit, true);
    RuntimeSwitches::Assign(RuntimeSwitch::DeferredEncode, true);
    Fixture fixture;
    {
        const Watchdog watchdog("a recorder torn down with encodes queued");
        std::thread opener;
        {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            Recorder recorder(fixture.context);
            {
                std::lock_guard lock(fixture.device.mutex);
                fixture.device.gate = true;
            }
            for (std::uint32_t batch = 0; batch < 6; ++batch) {
                fixture.Fill(recorder, batch);
                recorder.Submit();
            }
            opener = std::thread([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                {
                    std::lock_guard lock(fixture.device.mutex);
                    fixture.device.gateOpen = true;
                }
                fixture.device.changed.notify_all();
            });
        }
        opener.join();
        const auto submissions = fixture.Submissions();
        Expect(submissions.size() == 6, "a recorder torn down with six queued encodes submitted " + std::to_string(submissions.size()));
        for (std::size_t index = 0; index < submissions.size(); ++index) Expect(joined(submissions[index].recorded) == "[fill " + std::to_string(index) + "]", "batch " + std::to_string(index) + " of a torn-down recorder encoded " + joined(submissions[index].recorded));
        std::lock_guard lock(fixture.device.mutex);
        Expect(fixture.device.poolsDestroyed == fixture.device.poolsCreated && fixture.device.allocated == fixture.device.freed, "tearing down with queued encodes leaked pools or command buffers");
    }
}

void VerifyEncodesInline() {
    RuntimeSwitches::Assign(RuntimeSwitch::AsyncSubmit, true);
    RuntimeSwitches::Assign(RuntimeSwitch::DeferredEncode, true);
    const auto before = CommandEncoders::Totals();
    Fixture fixture;
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Recorder recorder(fixture.context);
        fixture.Fill(recorder, 1);
        fixture.Update(recorder, 2);
        Expect(CommandStream::IsHandle(recorder.Commands()), "a verified deferred batch handed out a command buffer");
        recorder.Submit();
        recorder.Sync();
        const auto submissions = fixture.Submissions();
        Expect(submissions.size() == 1, "a verified deferred batch made " + std::to_string(submissions.size()) + " submissions");
        if (submissions.size() == 1) {
            Expect(joined(submissions[0].recorded) == "[fill 1, update 2]", "a verified deferred batch encoded " + joined(submissions[0].recorded));
            Expect(submissions[0].pool == fixture.context.pool && submissions[0].recordingThreads.count(fixture.main) == 1, "a verified deferred batch was not encoded on the recording thread");
            Expect(submissions[0].thread != fixture.main, "a verified deferred batch was not handed to the submitter");
        }
    }
    const auto after = CommandEncoders::Totals();
    Expect(after.inlineStreams - before.inlineStreams == 1 && after.streams == before.streams, "a verified deferred batch was not counted as an inline encode");
    std::lock_guard lock(fixture.device.mutex);
    Expect(fixture.device.poolsCreated == 0, "a verified deferred batch started encoder threads");
    Expect(fixture.device.allocated == fixture.device.freed, "verified batches leaked " + std::to_string(fixture.device.allocated - fixture.device.freed) + " command buffers");
}

}

int main(int argc, char** argv) {
    const bool verify = argc > 1 && std::string_view(argv[1]) == "verify";
    try {
        Expect(CommandEncoders::Inline() == verify, verify ? "APS5_VERIFY_DEFERRED_ENCODE is not seen" : "APS5_VERIFY_DEFERRED_ENCODE is set outside the verify run");
        if (verify) {
            VerifyEncodesInline();
        } else {
            DeferredBatchesEncodeOffTheRecordingThread();
            SubmitWaitsForTheEncodeAndDrainWaitsForBoth();
            FailedEncodeNeverCompletes();
            FailedSubmitReachesFinish();
            BatchesBehindAFailureAreNotSubmitted();
            SparseBindsShareTheQueueWithTheSubmitter();
            SubmittersAreScopedToTheirDevice();
            RandomProgramsKeepProgramOrder();
            KeptOwnersOutliveTheirEncode();
            TeardownWaitsForQueuedEncodes();
            DeferredEncodeIsDecidedPerBatch();
            AsyncSubmitOffEncodesInline();
            DiscardedBatchIsNeverEncoded();
            RetiredBatchesWaitForTheirSubmitToReturn();
            StaleHandleIsRefusedAfterSubmit();
        }
        RuntimeSwitches::Assign(RuntimeSwitch::AsyncSubmit, false);
        RuntimeSwitches::Assign(RuntimeSwitch::DeferredEncode, false);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d deferred encode checks failed\n", failures);
        return 1;
    }
    std::printf("recorder deferred encode tests passed\n");
    return 0;
}
