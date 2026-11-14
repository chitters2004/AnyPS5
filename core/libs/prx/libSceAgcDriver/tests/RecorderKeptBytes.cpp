#include <limits>
#include <new>
#include <stdexcept>
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <optional>
#include <tuple>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace AgcDriver::Graphics;

namespace {

thread_local int failAllocationIn = -1;
std::atomic<std::uint64_t> injectedFailures{0};

}

void* operator new(std::size_t bytes) {
    if (failAllocationIn >= 0 && failAllocationIn-- == 0) {
        injectedFailures.fetch_add(1, std::memory_order_relaxed);
        throw std::bad_alloc();
    }
    if (void* memory = std::malloc(bytes != 0 ? bytes : 1)) return memory;
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {

int failures = 0;

void Expect(bool condition, const std::string& what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
}

constexpr std::size_t Budget = Recorder::KeptBytesBudget;

struct MockDevice {
    std::uintptr_t next = 1;
    std::map<VkFence, bool> signaled;
    std::uint64_t submits = 0;
    std::uint64_t fenceWaits = 0;
    std::uint64_t allocatedCommands = 0;
    std::uint64_t freedCommands = 0;
    VkResult endResult = VK_SUCCESS;
    VkResult submitResult = VK_SUCCESS;
    bool lostSubmissionPending = false;
    std::uint64_t idleWaits = 0;
    VkResult idleResult = VK_ERROR_DEVICE_LOST;
};

MockDevice mock;

VKAPI_ATTR VkResult VKAPI_CALL mockAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer* commands) {
    *commands = reinterpret_cast<VkCommandBuffer>(mock.next++);
    ++mock.allocatedCommands;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockFreeCommandBuffers(VkDevice, VkCommandPool, std::uint32_t count, const VkCommandBuffer*) {
    Expect(!mock.lostSubmissionPending, "command buffers were freed before the lost submission finished");
    mock.freedCommands += count;
}

VKAPI_ATTR VkResult VKAPI_CALL mockCreateFence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* fence) {
    *fence = reinterpret_cast<VkFence>(mock.next++);
    mock.signaled[*fence] = false;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockDestroyFence(VkDevice, VkFence fence, const VkAllocationCallbacks*) {
    Expect(!mock.lostSubmissionPending, "a fence was destroyed before the lost submission finished");
    mock.signaled.erase(fence);
}

VKAPI_ATTR VkResult VKAPI_CALL mockGetFenceStatus(VkDevice, VkFence fence) {
    return mock.signaled.at(fence) ? VK_SUCCESS : VK_NOT_READY;
}

VKAPI_ATTR VkResult VKAPI_CALL mockWaitForFences(VkDevice, std::uint32_t count, const VkFence* fences, VkBool32, std::uint64_t) {
    for (std::uint32_t i = 0; i < count; ++i) mock.signaled.at(fences[i]) = true;
    ++mock.fenceWaits;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockResetFences(VkDevice, std::uint32_t count, const VkFence* fences) {
    Expect(!mock.lostSubmissionPending, "a fence was reset before the lost submission finished");
    for (std::uint32_t i = 0; i < count; ++i) mock.signaled.at(fences[i]) = false;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockEndCommandBuffer(VkCommandBuffer) {
    return mock.endResult;
}

VKAPI_ATTR VkResult VKAPI_CALL mockQueueSubmit(VkQueue, std::uint32_t, const VkSubmitInfo*, VkFence) {
    ++mock.submits;
    if (mock.submitResult == VK_ERROR_DEVICE_LOST) mock.lostSubmissionPending = true;
    return mock.submitResult;
}

VKAPI_ATTR VkResult VKAPI_CALL mockDeviceWaitIdle(VkDevice) {
    ++mock.idleWaits;
    mock.lostSubmissionPending = false;
    return mock.idleResult;
}

VKAPI_ATTR void VKAPI_CALL mockCmdUpdateBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, const void*) {}
VKAPI_ATTR void VKAPI_CALL mockCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, std::uint32_t, const VkMemoryBarrier*, std::uint32_t, const VkBufferMemoryBarrier*, std::uint32_t, const VkImageMemoryBarrier*) {}
VKAPI_ATTR void VKAPI_CALL mockCmdBeginQuery(VkCommandBuffer, VkQueryPool, std::uint32_t, VkQueryControlFlags) {}
VKAPI_ATTR void VKAPI_CALL mockCmdEndQuery(VkCommandBuffer, VkQueryPool, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL mockCmdResetQueryPool(VkCommandBuffer, VkQueryPool, std::uint32_t, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL mockCmdCopyQueryPoolResults(VkCommandBuffer, VkQueryPool, std::uint32_t, std::uint32_t, VkBuffer, VkDeviceSize, VkDeviceSize, VkQueryResultFlags) {}
VKAPI_ATTR void VKAPI_CALL mockCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}
VKAPI_ATTR void VKAPI_CALL mockCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, std::uint32_t, std::uint32_t, const void*) {}
VKAPI_ATTR void VKAPI_CALL mockCmdDispatch(VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t) {}

PFN_vkVoidFunction VKAPI_CALL mockProc(VkDevice, const char* name) {
    static const std::map<std::string_view, PFN_vkVoidFunction> table{
        {"vkAllocateCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(mockAllocateCommandBuffers)},
        {"vkFreeCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(mockFreeCommandBuffers)},
        {"vkCreateFence", reinterpret_cast<PFN_vkVoidFunction>(mockCreateFence)},
        {"vkDestroyFence", reinterpret_cast<PFN_vkVoidFunction>(mockDestroyFence)},
        {"vkGetFenceStatus", reinterpret_cast<PFN_vkVoidFunction>(mockGetFenceStatus)},
        {"vkWaitForFences", reinterpret_cast<PFN_vkVoidFunction>(mockWaitForFences)},
        {"vkResetFences", reinterpret_cast<PFN_vkVoidFunction>(mockResetFences)},
        {"vkBeginCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockBeginCommandBuffer)},
        {"vkEndCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockEndCommandBuffer)},
        {"vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(mockQueueSubmit)},
        {"vkDeviceWaitIdle", reinterpret_cast<PFN_vkVoidFunction>(mockDeviceWaitIdle)},
        {"vkCmdUpdateBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockCmdUpdateBuffer)},
        {"vkCmdPipelineBarrier", reinterpret_cast<PFN_vkVoidFunction>(mockCmdPipelineBarrier)},
        {"vkCmdBeginQuery", reinterpret_cast<PFN_vkVoidFunction>(mockCmdBeginQuery)},
        {"vkCmdEndQuery", reinterpret_cast<PFN_vkVoidFunction>(mockCmdEndQuery)},
        {"vkCmdResetQueryPool", reinterpret_cast<PFN_vkVoidFunction>(mockCmdResetQueryPool)},
        {"vkCmdCopyQueryPoolResults", reinterpret_cast<PFN_vkVoidFunction>(mockCmdCopyQueryPoolResults)},
        {"vkCmdBindPipeline", reinterpret_cast<PFN_vkVoidFunction>(mockCmdBindPipeline)},
        {"vkCmdPushConstants", reinterpret_cast<PFN_vkVoidFunction>(mockCmdPushConstants)},
        {"vkCmdDispatch", reinterpret_cast<PFN_vkVoidFunction>(mockCmdDispatch)},
    };
    const auto it = table.find(name);
    return it == table.end() ? nullptr : it->second;
}

Context mockContext() {
    Context context{};
    context.device = reinterpret_cast<VkDevice>(mock.next++);
    context.queue = reinterpret_cast<VkQueue>(mock.next++);
    context.deviceProc = mockProc;
    return context;
}

void OpenBatchUnderBudget() {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Recorder recorder(mockContext());
    recorder.Keep(std::make_shared<int>(0));
    recorder.Keep(std::make_shared<int>(1), Budget - 1);
    recorder.BoundKeptBytes();
    Expect(recorder.Recording() && recorder.Submissions() == 0 && mock.submits == 0, "a batch keeping one byte less than the budget was submitted");
    recorder.Keep(std::make_shared<int>(2), 1);
    recorder.BoundKeptBytes();
    Expect(!recorder.Recording() && recorder.Submissions() == 1 && mock.submits == 1, "the open batch was not submitted once its kept bytes reached the budget");
    Expect(recorder.InFlightKeptBytes() == Budget, "the submitted batch counts " + std::to_string(recorder.InFlightKeptBytes()) + " kept bytes in flight, not the budget");
    Expect(mock.fenceWaits == 0, "the recorder waited for a batch with one budget in flight");
    recorder.Sync();
    Expect(recorder.InFlightKeptBytes() == 0, "a synced recorder still counts " + std::to_string(recorder.InFlightKeptBytes()) + " kept bytes in flight");
}

void InFlightWithinTwiceTheBudget() {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Recorder recorder(mockContext());
    std::vector<std::weak_ptr<int>> batches;
    for (int batch = 0; batch < 6; ++batch) {
        auto object = std::make_shared<int>(batch);
        batches.push_back(object);
        recorder.Keep(std::move(object), Budget / 2);
        recorder.Keep(std::make_shared<int>(batch), Budget / 2);
        recorder.BoundKeptBytes();
        Expect(recorder.InFlightKeptBytes() <= 2 * Budget, "batch " + std::to_string(batch) + " left " + std::to_string(recorder.InFlightKeptBytes()) + " kept bytes in flight, more than twice the budget");
    }
    Expect(recorder.Submissions() == 6, "six budget batches made " + std::to_string(recorder.Submissions()) + " submissions");
    Expect(recorder.InFlightBatches() == 2 && recorder.InFlightKeptBytes() == 2 * Budget, "the newest two batches are not the ones in flight");
    Expect(mock.fenceWaits == 4, "the recorder waited for " + std::to_string(mock.fenceWaits) + " batches, not the four oldest");
    for (std::size_t batch = 0; batch < batches.size(); ++batch) {
        const bool released = batches[batch].expired();
        Expect(released == (batch < 4), "batch " + std::to_string(batch) + (released ? " released its kept objects while in flight" : " still holds its kept objects under the GPU mutex after the wait for it"));
    }
}

void SyncedWithinTwiceTheBudget() {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Recorder recorder(mockContext());
    std::vector<std::weak_ptr<int>> batches;
    const auto alive = [&] { return std::count_if(batches.begin(), batches.end(), [](const auto& batch) { return !batch.expired(); }); };
    for (int batch = 0; batch < 6; ++batch) {
        auto object = std::make_shared<int>(batch);
        batches.push_back(object);
        recorder.Keep(std::move(object), Budget);
        recorder.BoundKeptBytes();
        recorder.Sync();
        Expect(alive() <= 2, "after batch " + std::to_string(batch) + ", " + std::to_string(alive()) + " synced budget batches still hold their kept objects under the GPU mutex");
    }
    Expect(recorder.Submissions() == 6 && recorder.InFlightKeptBytes() == 0, "six synced budget batches made " + std::to_string(recorder.Submissions()) + " submissions and left " + std::to_string(recorder.InFlightKeptBytes()) + " kept bytes in flight");
    Expect(!batches[4].expired() && !batches[5].expired(), "the newest two synced batches released their kept objects before the unlock");
}

void CountFollowsEveryPath() {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Recorder recorder(mockContext());
    recorder.Keep(std::make_shared<int>(0), 3 * Budget);
    recorder.BoundKeptBytes();
    Expect(recorder.Submissions() == 1 && recorder.InFlightBatches() == 0 && recorder.InFlightKeptBytes() == 0, "a batch keeping three budgets was not submitted, waited for and released");
    recorder.Keep(std::make_shared<int>(1), Budget);
    recorder.Submit();
    Expect(recorder.InFlightKeptBytes() == Budget, "a plain Submit did not count the batch's kept bytes in flight");
    for (auto& [fence, signaled] : mock.signaled) signaled = true;
    Expect(recorder.Reap() && recorder.InFlightKeptBytes() == 0, "a reaped batch still counts " + std::to_string(recorder.InFlightKeptBytes()) + " kept bytes in flight");
}

void SharedOwnerCountsOncePerBatch() {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Recorder recorder(mockContext());
    auto shared = std::make_shared<int>(0);
    for (int keep = 0; keep < 3; ++keep) {
        recorder.Keep(shared);
        recorder.KeepBytes(shared.get(), Budget / 2);
        recorder.BoundKeptBytes();
    }
    Expect(recorder.Recording() && recorder.OpenKeptBytes() == Budget / 2 && mock.submits == 0, "three keeps of one owner's half budget counted " + std::to_string(recorder.OpenKeptBytes()) + " kept bytes, not half the budget once");
    auto other = std::make_shared<int>(1);
    recorder.Keep(other);
    recorder.KeepBytes(other.get(), Budget / 2);
    recorder.BoundKeptBytes();
    Expect(!recorder.Recording() && recorder.Submissions() == 1 && recorder.InFlightKeptBytes() == Budget, "a second owner's half budget did not complete the batch's budget");
    recorder.Keep(shared);
    recorder.KeepBytes(shared.get(), Budget / 2);
    Expect(recorder.OpenKeptBytes() == Budget / 2, "the next batch did not count an owner the submitted batch counted");
    recorder.Sync();
}

using Kind = Recorder::ReadKind;

struct NotedRead {
    std::uint64_t begin;
    std::uint64_t end;
    Kind kind;
    std::shared_ptr<const Recorder::ReadList> list;
};

std::optional<Kind> FirstNotedReader(const std::vector<NotedRead>& notes, std::uint64_t address, std::uint64_t end, std::optional<Kind> only = std::nullopt) {
    for (const auto& note : notes) {
        if (only.has_value() && note.kind != *only) continue;
        if (note.list == nullptr) {
            if (address < note.end && note.begin < end) return note.kind;
            continue;
        }
        for (const auto& [begin, finish] : *note.list) {
            if (address < finish && begin < end) return note.kind;
        }
    }
    return std::nullopt;
}

struct ReadSequence {
    std::uint32_t state = 0x2545f491u;
    std::uint32_t Next() {
        state = state * 1664525u + 1013904223u;
        return state >> 8u;
    }
};

void RepeatedReadsAnswerLikeEveryNote() {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Recorder recorder(mockContext());
    constexpr std::uint64_t base = 0x7f0000100000ull;
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 6> ranges{{{0x000, 0x100}, {0x080, 0x200}, {0x000, 0x100}, {0x300, 0x340}, {0x100, 0x180}, {0x1c0, 0x400}}};
    const std::array<Kind, 3> kinds{Kind::DispatchElement, Kind::AddressBased, Kind::CopySource};
    ReadSequence random;
    std::vector<NotedRead> notes;
    std::vector<std::shared_ptr<const Recorder::ReadList>> lists;
    std::vector<std::tuple<std::uint64_t, std::uint64_t, Kind>> seen;
    std::uint64_t expectedRepeats = 0;
    const auto remember = [&](std::uint64_t begin, std::uint64_t end, Kind kind) {
        notes.push_back({begin, end, kind, nullptr});
        if (std::find(seen.begin(), seen.end(), std::make_tuple(begin, end, kind)) != seen.end()) ++expectedRepeats;
        else seen.emplace_back(begin, end, kind);
    };
    const auto pick = [&] {
        const auto& range = ranges[random.Next() % ranges.size()];
        return std::make_tuple(base + range.first, base + range.second, kinds[random.Next() % kinds.size()]);
    };
    const auto before = Recorder::ReadCounts();
    std::uint64_t notedCalls = 0;
    for (int operation = 0; operation < 600; ++operation) {
        const auto choice = random.Next() % 10;
        if (choice < 7) {
            const auto [begin, end, kind] = pick();
            recorder.NotePendingRead(begin, static_cast<std::size_t>(end - begin), kind);
            remember(begin, end, kind);
            ++notedCalls;
        } else if (choice < 9) {
            const auto kind = kinds[random.Next() % kinds.size()];
            std::vector<std::pair<std::uint64_t, std::uint64_t>> span;
            const auto count = 1 + random.Next() % 3;
            for (std::uint32_t index = 0; index < count; ++index) {
                const auto [begin, end, ignored] = pick();
                static_cast<void>(ignored);
                span.emplace_back(begin, end);
            }
            span.emplace_back(base + 0x500, base + 0x500);
            recorder.NotePendingReads(span, kind);
            for (const auto& [begin, end] : span) {
                if (end > begin) remember(begin, end, kind);
            }
            notedCalls += span.size();
        } else {
            const auto kind = kinds[random.Next() % kinds.size()];
            std::shared_ptr<const Recorder::ReadList> list;
            if (!lists.empty() && random.Next() % 2 == 0) {
                list = lists[random.Next() % lists.size()];
            } else {
                const auto start = base + 0x40 * (random.Next() % 16);
                list = std::make_shared<Recorder::ReadList>(Recorder::ReadList{{start, start + 0x20}, {start + 0x400, start + 0x440}});
                lists.push_back(list);
            }
            recorder.NotePendingReadList(list, kind);
            const bool listed = std::any_of(notes.begin(), notes.end(), [&](const NotedRead& note) { return note.list == list && note.kind == kind; });
            if (!listed) notes.push_back({0, 0, kind, list});
            notedCalls += 2;
        }
    }
    const auto after = Recorder::ReadCounts();
    Expect(after.noted - before.noted == notedCalls, "the noted counter counted " + std::to_string(after.noted - before.noted) + " ranges, not every one of the " + std::to_string(notedCalls) + " noted");
    const auto repeats = Recorder::FlipReadCheck() || RuntimeSwitches::Set(RuntimeSwitch::NoEffectsDedupe) ? 0 : expectedRepeats;
    Expect(after.repeated - before.repeated == repeats, "the batch skipped " + std::to_string(after.repeated - before.repeated) + " repeated reads, not the " + std::to_string(repeats) + " (range, kind) repeats it was given");
    std::size_t mismatches = 0;
    for (std::uint64_t offset = 0; offset < 0x900; offset += 0x10) {
        for (const std::uint64_t bytes : {1ull, 0x20ull, 0x90ull, 0x300ull}) {
            const auto address = base + offset;
            const auto expected = FirstNotedReader(notes, address, address + bytes);
            const auto described = recorder.DescribePendingRead(address, static_cast<std::size_t>(bytes));
            const bool leased = FirstNotedReader(notes, address, address + bytes, Kind::AddressBased).has_value();
            if (recorder.PendingReadOverlaps(address, static_cast<std::size_t>(bytes)) != expected.has_value()) ++mismatches;
            if (described.has_value() != expected.has_value() || (described.has_value() && (described->kind != *expected || !described->open))) ++mismatches;
            if (recorder.PendingLeaseSerial(address, static_cast<std::size_t>(bytes)) != (leased ? recorder.OpenSerial() : 0)) ++mismatches;
            if (recorder.PendingWriterSerial(address, static_cast<std::size_t>(bytes)) != (leased ? recorder.OpenSerial() : 0)) ++mismatches;
        }
    }
    Expect(mismatches == 0, std::to_string(mismatches) + " read queries answered differently from the reader every noted read names");
    recorder.Submit();
    const auto submitted = recorder.Submissions();
    const auto repeatedBefore = Recorder::ReadCounts().repeated;
    recorder.NotePendingRead(base, 0x100, Kind::CopySource);
    Expect(Recorder::ReadCounts().repeated == repeatedBefore, "a range the submitted batch noted was skipped as a repeat in the next batch");
    const auto next = recorder.DescribePendingRead(base, 4);
    Expect(next.has_value() && next->open && next->kind == Kind::CopySource && next->serial == submitted + 1, "the next batch does not report the read it noted again");
    recorder.NotePendingRead(base, 0x100, Kind::AddressBased);
    Expect(recorder.PendingLeaseSerial(base, 4) == recorder.OpenSerial(), "the next batch dropped a range noted again with another reader kind");
    Expect(!lists.empty(), "the read sequence noted no list: the list checks prove nothing");
    if (!lists.empty()) {
        const auto& list = lists.front();
        recorder.NotePendingReadList(list, Kind::DispatchElement);
        const auto listed = recorder.DescribePendingRead(list->back().first, 4);
        Expect(listed.has_value() && listed->open && listed->kind == Kind::DispatchElement && listed->serial == submitted + 1, "the next batch does not report a list the submitted batch had noted");
    }
    recorder.Sync();
    Expect(recorder.Idle() && !recorder.PendingReadOverlaps(base, 0x1000, false), "repeated reads outlived their batch");
}

void DedupeSwitchFlipsMidBatch() {
    if (Recorder::FlipReadCheck()) {
        std::printf("APS5_FLIP_READ_CHECK keeps every note: the mid-batch switch flip check is skipped\n");
        return;
    }
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Recorder recorder(mockContext());
    const bool environment = RuntimeSwitches::Set(RuntimeSwitch::NoEffectsDedupe);
    constexpr std::uint64_t base = 0x7f0000300000ull;
    const auto repeated = [] { return Recorder::ReadCounts().repeated; };
    RuntimeSwitches::Assign(RuntimeSwitch::NoEffectsDedupe, false);
    recorder.NotePendingRead(base, 0x100, Kind::CopySource);
    auto before = repeated();
    recorder.NotePendingRead(base, 0x100, Kind::CopySource);
    Expect(repeated() == before + 1, "a repeated read with the dedupe on was not skipped");
    RuntimeSwitches::Assign(RuntimeSwitch::NoEffectsDedupe, true);
    before = repeated();
    recorder.NotePendingRead(base, 0x100, Kind::CopySource);
    recorder.NotePendingRead(base + 0x200, 0x40, Kind::AddressBased);
    recorder.NotePendingRead(base + 0x200, 0x40, Kind::AddressBased);
    const auto list = std::make_shared<Recorder::ReadList>(Recorder::ReadList{{base + 0x400, base + 0x420}});
    recorder.NotePendingReadList(list, Kind::DispatchElement);
    recorder.NotePendingReadList(list, Kind::DispatchElement);
    Expect(repeated() == before, "APS5_NO_EFFECTS_DEDUPE set at run time still skipped " + std::to_string(repeated() - before) + " repeated reads");
    RuntimeSwitches::Assign(RuntimeSwitch::NoEffectsDedupe, false);
    before = repeated();
    recorder.NotePendingRead(base, 0x100, Kind::CopySource);
    recorder.NotePendingRead(base + 0x200, 0x40, Kind::AddressBased);
    recorder.NotePendingRead(base + 0x200, 0x40, Kind::AddressBased);
    recorder.NotePendingReadList(list, Kind::DispatchElement);
    Expect(repeated() == before + 2, "after the switch went back off, the batch skipped " + std::to_string(repeated() - before) + " repeated reads, not the 2 noted with the dedupe on");
    const auto first = recorder.DescribePendingRead(base, 4);
    const auto second = recorder.DescribePendingRead(base + 0x200, 4);
    const auto listed = recorder.DescribePendingRead(base + 0x400, 4);
    Expect(first.has_value() && first->open && first->kind == Kind::CopySource, "a read noted before the flips is no longer reported");
    Expect(second.has_value() && second->open && second->kind == Kind::AddressBased && recorder.PendingLeaseSerial(base + 0x200, 4) == recorder.OpenSerial(), "a read noted while the dedupe was off is not reported");
    Expect(listed.has_value() && listed->open && listed->kind == Kind::DispatchElement, "a list noted while the dedupe was off is not reported");
    RuntimeSwitches::Assign(RuntimeSwitch::NoEffectsDedupe, environment);
    recorder.Sync();
}

void ReadSurvivesFailedNote() {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Recorder recorder(mockContext());
    constexpr std::uint64_t base = 0x7f0000200000ull;
    for (std::uint64_t index = 0; index < 8; ++index) recorder.NotePendingRead(base + index * 0x100, 0x10, Kind::CopySource);
    const auto injected = injectedFailures.load(std::memory_order_relaxed);
    bool threw = false;
    failAllocationIn = 0;
    try {
        recorder.NotePendingRead(base + 0x800, 0x10, Kind::CopySource);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    failAllocationIn = -1;
    if (injectedFailures.load(std::memory_order_relaxed) == injected) {
        std::printf("allocation failure not injected into the driver: the failed read note check is skipped\n");
        recorder.Sync();
        return;
    }
    Expect(threw, "the injected allocation failure did not reach the read note");
    recorder.NotePendingRead(base + 0x800, 0x10, Kind::CopySource);
    Expect(recorder.PendingReadOverlaps(base + 0x800, 4), "a read whose first note failed to allocate was lost when noted again");
    recorder.Sync();
}

template<typename Condition>
bool EventuallyTrue(Condition&& condition) {
    for (int attempt = 0; attempt < 5000 && !condition(); ++attempt) {
        for (const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1); std::chrono::steady_clock::now() < until;) std::this_thread::yield();
    }
    return condition();
}

void FailedSubmission(VkResult endResult, VkResult submitResult, VkResult idleResult = VK_ERROR_DEVICE_LOST) {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    alignas(8) std::array<std::byte, 128> memory{};
    const auto address = reinterpret_cast<std::uint64_t>(memory.data());
    bool completed = false;
    bool earlierCompleted = false;
    bool earlierWrittenBack = false;
    {
        Recorder recorder(mockContext());
        recorder.Activate();
        recorder.Keep(std::make_shared<int>(0), 17);
        recorder.NoteLabel(address, std::span<const std::byte>(memory).first(4), 1, 0);
        recorder.OnComplete([&] { earlierWrittenBack = true; });
        Expect(recorder.AfterRecordedWork([&] { earlierCompleted = true; }), "the earlier batch did not accept its completion");
        recorder.Submit();
        auto kept = std::shared_ptr<int>(new int(1), [](int* object) {
            Expect(!mock.lostSubmissionPending, "kept resources were destroyed before the lost submission finished");
            delete object;
        });
        std::weak_ptr<int> released = kept;
        recorder.Keep(std::move(kept), Budget);
        recorder.NoteLabel(address + 8, std::span<const std::byte>(memory).first(4), 2, 0);
        recorder.NoteLabel(address + 16, std::span<const std::byte>(memory).first(Recorder::LabelTableBytes + 4), 3, 0);
        recorder.AfterCompletions(address + 96, std::span<const std::byte>(memory).first(4), 4, 0, false);
        recorder.OnComplete([&] { completed = true; });
        Expect(recorder.AfterRecordedWork([&] { completed = true; }), "the failed batch did not accept its completion");
        mock.endResult = endResult;
        mock.submitResult = submitResult;
        mock.idleResult = idleResult;
        bool failed = false;
        try {
            recorder.Submit();
        } catch (const std::exception& error) {
            const auto operation = endResult != VK_SUCCESS ? "vkEndCommandBuffer recorder" : "vkQueueSubmit recorder";
            failed = std::string_view(error.what()).find(operation) != std::string_view::npos;
        }
        Expect(failed, "submission did not report its Vulkan failure");
        Expect(released.expired() && !completed, "a failed batch retained objects or ran its completions");
        Expect(!recorder.Recording() && recorder.Submissions() == 1 && recorder.InFlightBatches() == 1 && recorder.InFlightKeptBytes() == 17,
            "the failed batch changed the earlier in-flight submission");
        Expect(recorder.PendingLabels() == 1 && recorder.PendingLabelIn(address, 4), "a failed batch retained labels or removed the earlier batch's label");
        Expect(!recorder.PendingLabelIn(address + 8, 4) && !recorder.PendingLabelIn(address + 16, Recorder::LabelTableBytes + 4),
            "a failed batch retained a narrow or wide label");
        Expect(!recorder.CompletionLabelIn(address + 96, 4) && Recorder::PendingCompletionLabels() == 1 && Recorder::PendingWriteBackCompletions() == 1,
            "a failed batch retained pending completions or removed earlier ones");
        Expect(!Recorder::PendingLabelSince().has_value(), "a failed batch left its label flush deadline pending");
        Expect(Recorder::SnapshotWriteOverlaps(address, 4) && !Recorder::SnapshotWriteOverlaps(address + 8, 100),
            "a failed batch left stale writes in the pending snapshot or removed an earlier write");
        Expect(mock.fenceWaits == 0, "a failed submission waited for an unsignaled fence");
        Expect(mock.idleWaits == (submitResult == VK_ERROR_DEVICE_LOST ? 1u : 0u), "a failed submission did not settle device loss before releasing its resources");
        mock.lostSubmissionPending = false;
        mock.endResult = mock.submitResult = VK_SUCCESS;
        recorder.Sync();
        Expect(recorder.Idle() && !completed && earlierCompleted && earlierWrittenBack,
            "sync ran the failed batch's completions or lost the earlier batch's completions");
        Expect(Recorder::PendingCompletionLabels() == 0 && Recorder::PendingWriteBackCompletions() == 0,
            "sync left stale completion counts after submission failure");
    }
    Expect(mock.signaled.empty() && mock.allocatedCommands == mock.freedCommands, "a failed submission leaked its fence or command buffer");
}

}

int AssertsWithOpenBatch() {
    mock = MockDevice{};
    Recorder recorder(mockContext());
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        recorder.Keep(std::make_shared<int>(0));
    }
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    recorder.NotePendingRead(0x10000, 16, Recorder::ReadKind::CopySource);
    std::fprintf(stderr, "FAIL: a read noted without the GPU mutex into an open batch did not assert under APS5_ASSERT_GPU_LOCK\n");
    std::_Exit(1);
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--assert-open-batch") return AssertsWithOpenBatch();
    try {
        OpenBatchUnderBudget();
        InFlightWithinTwiceTheBudget();
        SyncedWithinTwiceTheBudget();
        CountFollowsEveryPath();
        SharedOwnerCountsOncePerBatch();
        RepeatedReadsAnswerLikeEveryNote();
        DedupeSwitchFlipsMidBatch();
        ReadSurvivesFailedNote();
        FailedSubmission(VK_ERROR_OUT_OF_HOST_MEMORY, VK_SUCCESS);
        FailedSubmission(VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_SUCCESS);
        FailedSubmission(VK_SUCCESS, VK_ERROR_OUT_OF_HOST_MEMORY);
        FailedSubmission(VK_SUCCESS, VK_ERROR_OUT_OF_DEVICE_MEMORY);
        FailedSubmission(VK_SUCCESS, VK_ERROR_DEVICE_LOST);
        FailedSubmission(VK_SUCCESS, VK_ERROR_DEVICE_LOST, VK_SUCCESS);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d recorder kept bytes checks failed\n", failures);
        return 1;
    }
    std::printf("recorder kept bytes tests passed\n");
    return 0;
}
