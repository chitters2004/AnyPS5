#include "prx/libSceAgcDriver/Graphics/include/VkEntryProfile.hpp"

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace AgcDriver {

namespace {

struct ThreadVkTally {
    std::uint64_t nanoseconds[VkEntryKindCount];
    std::uint64_t calls[VkEntryKindCount];
};

ThreadVkTally& threadVkTally() {
    thread_local ThreadVkTally tally{};
    return tally;
}

std::atomic<std::uint64_t> unwrappedSlots{0};

template<VkEntryKind Kind>
struct VkEntryScope {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~VkEntryScope() {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        auto& tally = threadVkTally();
        tally.nanoseconds[Kind] += static_cast<std::uint64_t>(elapsed);
        ++tally.calls[Kind];
    }
};

template<auto Member, VkEntryKind Kind, typename TFunction>
struct TimedEntry;

template<auto Member, VkEntryKind Kind, typename TResult, typename... TArguments>
struct TimedEntry<Member, Kind, TResult(VKAPI_PTR*)(TArguments...)> {
    using Function = TResult(VKAPI_PTR*)(TArguments...);
    static inline std::atomic<Function> real{nullptr};
    static inline std::uint64_t tables = 0;
    static TResult VKAPI_CALL Call(TArguments... arguments) {
        VkEntryScope<Kind> scope;
        return real.load(std::memory_order_relaxed)(arguments...);
    }
};

std::mutex& slotMutex() {
    static std::mutex mutex;
    return mutex;
}

template<auto Member, VkEntryKind Kind, typename TFunction>
TFunction timedEntry(TFunction function) {
    using Entry = TimedEntry<Member, Kind, TFunction>;
    if (function == nullptr || function == &Entry::Call) return function;
    std::lock_guard lock(slotMutex());
    const auto bound = Entry::real.load(std::memory_order_relaxed);
    if (bound != nullptr && bound != function) {
        unwrappedSlots.fetch_add(1, std::memory_order_relaxed);
        return function;
    }
    Entry::real.store(function, std::memory_order_relaxed);
    ++Entry::tables;
    return &Entry::Call;
}

template<auto Member, VkEntryKind Kind, typename TFunction>
TFunction untimedEntry(TFunction function) {
    using Entry = TimedEntry<Member, Kind, TFunction>;
    if (function != &Entry::Call) return function;
    std::lock_guard lock(slotMutex());
    const auto bound = Entry::real.load(std::memory_order_relaxed);
    if (--Entry::tables == 0) Entry::real.store(nullptr, std::memory_order_relaxed);
    return bound;
}

template<auto Member, VkEntryKind Kind>
void wrapEntry(Graphics::DeviceFunctions& functions) {
    functions.*Member = timedEntry<Member, Kind>(functions.*Member);
}

template<auto Member, VkEntryKind Kind>
void unwrapEntry(Graphics::DeviceFunctions& functions) {
    functions.*Member = untimedEntry<Member, Kind>(functions.*Member);
}

}

bool VkEntryProfiled() {
    static const bool profiled = std::getenv("APS5_PROFILE_VK_ENTRY") != nullptr;
    return profiled;
}

VkEntryTally ThreadVkEntryTally() {
    const auto& tally = threadVkTally();
    VkEntryTally taken;
    for (std::size_t kind = 0; kind < VkEntryKindCount; ++kind) {
        taken.nanoseconds[kind] = tally.nanoseconds[kind];
        taken.calls[kind] = tally.calls[kind];
    }
    return taken;
}

std::uint64_t VkEntryUnwrappedSlots() {
    return unwrappedSlots.load(std::memory_order_relaxed);
}

void VkEntryShape::Note(std::span<const double, Phases> phaseUs, std::span<const VkEntryTally, Phases> phases) {
    std::lock_guard lock(mutex);
    ++counts.draws;
    for (std::size_t phase = 0; phase < Phases; ++phase) {
        counts.phaseUs[phase] += phaseUs[phase];
        auto& into = counts.phases[phase];
        const auto& from = phases[phase];
        for (std::size_t kind = 0; kind < VkEntryKindCount; ++kind) {
            into.nanoseconds[kind] += from.nanoseconds[kind];
            into.calls[kind] += from.calls[kind];
        }
        into.lookups += from.lookups;
    }
}

VkEntryShape::Report VkEntryShape::Take() {
    std::lock_guard lock(mutex);
    auto taken = counts;
    counts = Report{};
    return taken;
}

std::string VkEntryShape::Line(const Report& report, std::span<const char* const, Phases> phaseNames, std::uint64_t unwrappedSlots) {
    std::string text;
    char item[256];
    const auto draws = report.draws != 0 ? static_cast<double>(report.draws) : 1.0;
    double totalUs = 0;
    double driverUs = 0;
    for (std::size_t phase = 0; phase < Phases; ++phase) {
        totalUs += report.phaseUs[phase];
        for (const auto nanoseconds : report.phases[phase].nanoseconds) driverUs += static_cast<double>(nanoseconds) / 1000.0;
    }
    std::snprintf(item, sizeof(item), "[vk-entry] 10 s: %llu draws, %.2f us/draw = vk %.2f + own %.2f; unwrapped slots %llu;", static_cast<unsigned long long>(report.draws), totalUs / draws, driverUs / draws, (totalUs - driverUs) / draws, static_cast<unsigned long long>(unwrappedSlots));
    text += item;
    for (std::size_t phase = 0; phase < Phases; ++phase) {
        const auto& tally = report.phases[phase];
        const auto us = report.phaseUs[phase];
        std::uint64_t calls = 0;
        double phaseDriverUs = 0;
        for (std::size_t kind = 0; kind < VkEntryKindCount; ++kind) {
            calls += tally.calls[kind];
            phaseDriverUs += static_cast<double>(tally.nanoseconds[kind]) / 1000.0;
        }
        if (us <= 0 && calls == 0 && tally.lookups == 0) continue;
        std::snprintf(item, sizeof(item), " %s %.2f = own %.2f", phaseNames[phase], us / draws, (us - phaseDriverUs) / draws);
        text += item;
        for (std::size_t kind = 0; kind < VkEntryKindCount; ++kind) {
            if (tally.calls[kind] == 0) continue;
            std::snprintf(item, sizeof(item), " + %s %.2f (%.2f calls)", VkEntryKindNames[kind], static_cast<double>(tally.nanoseconds[kind]) / 1000.0 / draws, static_cast<double>(tally.calls[kind]) / draws);
            text += item;
        }
        if (tally.lookups != 0) {
            std::snprintf(item, sizeof(item), ", %.2f lookups", static_cast<double>(tally.lookups) / draws);
            text += item;
        }
        text += ';';
    }
    return text;
}

namespace Graphics {

void WrapDeviceFunctions(DeviceFunctions& functions) {
    if (!VkEntryProfiled()) return;
    wrapEntry<&DeviceFunctions::cmdPipelineBarrier, VkEntryCommands>(functions);
    wrapEntry<&DeviceFunctions::cmdCopyBuffer, VkEntryCommands>(functions);
    wrapEntry<&DeviceFunctions::cmdUpdateBuffer, VkEntryCommands>(functions);
    wrapEntry<&DeviceFunctions::cmdFillBuffer, VkEntryCommands>(functions);
    functions.cmdBindPipeline = timedEntry<&DeviceFunctions::cmdBindPipeline, VkEntryCommands>(functions.cmdBindPipeline.function);
    wrapEntry<&DeviceFunctions::cmdBindDescriptorSets, VkEntryCommands>(functions);
    wrapEntry<&DeviceFunctions::cmdPushConstants, VkEntryCommands>(functions);
    wrapEntry<&DeviceFunctions::cmdDispatch, VkEntryCommands>(functions);
    wrapEntry<&DeviceFunctions::cmdDispatchIndirect, VkEntryCommands>(functions);
    wrapEntry<&DeviceFunctions::cmdBeginRenderPass, VkEntryCommands>(functions);
    wrapEntry<&DeviceFunctions::cmdEndRenderPass, VkEntryCommands>(functions);
    wrapEntry<&DeviceFunctions::cmdBeginRendering, VkEntryCommands>(functions);
    wrapEntry<&DeviceFunctions::cmdEndRendering, VkEntryCommands>(functions);
    functions.cmdSetViewport = timedEntry<&DeviceFunctions::cmdSetViewport, VkEntryCommands>(functions.cmdSetViewport.function);
    functions.cmdSetScissor = timedEntry<&DeviceFunctions::cmdSetScissor, VkEntryCommands>(functions.cmdSetScissor.function);
    functions.cmdSetColorWriteMask = timedEntry<&DeviceFunctions::cmdSetColorWriteMask, VkEntryCommands>(functions.cmdSetColorWriteMask.function);
    functions.cmdSetDepthBounds = timedEntry<&DeviceFunctions::cmdSetDepthBounds, VkEntryCommands>(functions.cmdSetDepthBounds.function);
    functions.cmdSetDepthBias = timedEntry<&DeviceFunctions::cmdSetDepthBias, VkEntryCommands>(functions.cmdSetDepthBias.function);
    functions.cmdSetCullMode = timedEntry<&DeviceFunctions::cmdSetCullMode, VkEntryCommands>(functions.cmdSetCullMode.function);
    functions.cmdSetFrontFace = timedEntry<&DeviceFunctions::cmdSetFrontFace, VkEntryCommands>(functions.cmdSetFrontFace.function);
    functions.cmdSetDepthTestEnable = timedEntry<&DeviceFunctions::cmdSetDepthTestEnable, VkEntryCommands>(functions.cmdSetDepthTestEnable.function);
    functions.cmdSetDepthWriteEnable = timedEntry<&DeviceFunctions::cmdSetDepthWriteEnable, VkEntryCommands>(functions.cmdSetDepthWriteEnable.function);
    functions.cmdSetDepthCompareOp = timedEntry<&DeviceFunctions::cmdSetDepthCompareOp, VkEntryCommands>(functions.cmdSetDepthCompareOp.function);
    functions.cmdSetDepthBoundsTestEnable = timedEntry<&DeviceFunctions::cmdSetDepthBoundsTestEnable, VkEntryCommands>(functions.cmdSetDepthBoundsTestEnable.function);
    functions.cmdSetStencilTestEnable = timedEntry<&DeviceFunctions::cmdSetStencilTestEnable, VkEntryCommands>(functions.cmdSetStencilTestEnable.function);
    functions.cmdSetStencilOp = timedEntry<&DeviceFunctions::cmdSetStencilOp, VkEntryCommands>(functions.cmdSetStencilOp.function);
    functions.cmdSetStencilCompareMask = timedEntry<&DeviceFunctions::cmdSetStencilCompareMask, VkEntryCommands>(functions.cmdSetStencilCompareMask.function);
    functions.cmdSetStencilWriteMask = timedEntry<&DeviceFunctions::cmdSetStencilWriteMask, VkEntryCommands>(functions.cmdSetStencilWriteMask.function);
    functions.cmdSetStencilReference = timedEntry<&DeviceFunctions::cmdSetStencilReference, VkEntryCommands>(functions.cmdSetStencilReference.function);
    wrapEntry<&DeviceFunctions::cmdBindVertexBuffers, VkEntryCommands>(functions);
    wrapEntry<&DeviceFunctions::cmdBindIndexBuffer, VkEntryCommands>(functions);
    wrapEntry<&DeviceFunctions::cmdDraw, VkEntryCommands>(functions);
    wrapEntry<&DeviceFunctions::cmdDrawIndexed, VkEntryCommands>(functions);
    wrapEntry<&DeviceFunctions::cmdDrawIndirect, VkEntryCommands>(functions);
    wrapEntry<&DeviceFunctions::cmdDrawIndexedIndirect, VkEntryCommands>(functions);
    wrapEntry<&DeviceFunctions::cmdCopyBufferToImage, VkEntryCommands>(functions);
    wrapEntry<&DeviceFunctions::cmdCopyImageToBuffer, VkEntryCommands>(functions);
    wrapEntry<&DeviceFunctions::cmdClearColorImage, VkEntryCommands>(functions);
    wrapEntry<&DeviceFunctions::updateDescriptorSets, VkEntryDescriptors>(functions);
    wrapEntry<&DeviceFunctions::allocateDescriptorSets, VkEntryDescriptors>(functions);
    wrapEntry<&DeviceFunctions::getFenceStatus, VkEntryOther>(functions);
    wrapEntry<&DeviceFunctions::createBuffer, VkEntryOther>(functions);
    wrapEntry<&DeviceFunctions::destroyBuffer, VkEntryOther>(functions);
    wrapEntry<&DeviceFunctions::getBufferMemoryRequirements, VkEntryOther>(functions);
    wrapEntry<&DeviceFunctions::bindBufferMemory, VkEntryOther>(functions);
    wrapEntry<&DeviceFunctions::allocateMemory, VkEntryOther>(functions);
    wrapEntry<&DeviceFunctions::freeMemory, VkEntryOther>(functions);
    wrapEntry<&DeviceFunctions::mapMemory, VkEntryOther>(functions);
    wrapEntry<&DeviceFunctions::unmapMemory, VkEntryOther>(functions);
}

void UnwrapDeviceFunctions(DeviceFunctions& functions) {
    unwrapEntry<&DeviceFunctions::cmdPipelineBarrier, VkEntryCommands>(functions);
    unwrapEntry<&DeviceFunctions::cmdCopyBuffer, VkEntryCommands>(functions);
    unwrapEntry<&DeviceFunctions::cmdUpdateBuffer, VkEntryCommands>(functions);
    unwrapEntry<&DeviceFunctions::cmdFillBuffer, VkEntryCommands>(functions);
    functions.cmdBindPipeline = untimedEntry<&DeviceFunctions::cmdBindPipeline, VkEntryCommands>(functions.cmdBindPipeline.function);
    unwrapEntry<&DeviceFunctions::cmdBindDescriptorSets, VkEntryCommands>(functions);
    unwrapEntry<&DeviceFunctions::cmdPushConstants, VkEntryCommands>(functions);
    unwrapEntry<&DeviceFunctions::cmdDispatch, VkEntryCommands>(functions);
    unwrapEntry<&DeviceFunctions::cmdDispatchIndirect, VkEntryCommands>(functions);
    unwrapEntry<&DeviceFunctions::cmdBeginRenderPass, VkEntryCommands>(functions);
    unwrapEntry<&DeviceFunctions::cmdEndRenderPass, VkEntryCommands>(functions);
    unwrapEntry<&DeviceFunctions::cmdBeginRendering, VkEntryCommands>(functions);
    unwrapEntry<&DeviceFunctions::cmdEndRendering, VkEntryCommands>(functions);
    functions.cmdSetViewport = untimedEntry<&DeviceFunctions::cmdSetViewport, VkEntryCommands>(functions.cmdSetViewport.function);
    functions.cmdSetScissor = untimedEntry<&DeviceFunctions::cmdSetScissor, VkEntryCommands>(functions.cmdSetScissor.function);
    functions.cmdSetColorWriteMask = untimedEntry<&DeviceFunctions::cmdSetColorWriteMask, VkEntryCommands>(functions.cmdSetColorWriteMask.function);
    functions.cmdSetDepthBounds = untimedEntry<&DeviceFunctions::cmdSetDepthBounds, VkEntryCommands>(functions.cmdSetDepthBounds.function);
    functions.cmdSetDepthBias = untimedEntry<&DeviceFunctions::cmdSetDepthBias, VkEntryCommands>(functions.cmdSetDepthBias.function);
    functions.cmdSetCullMode = untimedEntry<&DeviceFunctions::cmdSetCullMode, VkEntryCommands>(functions.cmdSetCullMode.function);
    functions.cmdSetFrontFace = untimedEntry<&DeviceFunctions::cmdSetFrontFace, VkEntryCommands>(functions.cmdSetFrontFace.function);
    functions.cmdSetDepthTestEnable = untimedEntry<&DeviceFunctions::cmdSetDepthTestEnable, VkEntryCommands>(functions.cmdSetDepthTestEnable.function);
    functions.cmdSetDepthWriteEnable = untimedEntry<&DeviceFunctions::cmdSetDepthWriteEnable, VkEntryCommands>(functions.cmdSetDepthWriteEnable.function);
    functions.cmdSetDepthCompareOp = untimedEntry<&DeviceFunctions::cmdSetDepthCompareOp, VkEntryCommands>(functions.cmdSetDepthCompareOp.function);
    functions.cmdSetDepthBoundsTestEnable = untimedEntry<&DeviceFunctions::cmdSetDepthBoundsTestEnable, VkEntryCommands>(functions.cmdSetDepthBoundsTestEnable.function);
    functions.cmdSetStencilTestEnable = untimedEntry<&DeviceFunctions::cmdSetStencilTestEnable, VkEntryCommands>(functions.cmdSetStencilTestEnable.function);
    functions.cmdSetStencilOp = untimedEntry<&DeviceFunctions::cmdSetStencilOp, VkEntryCommands>(functions.cmdSetStencilOp.function);
    functions.cmdSetStencilCompareMask = untimedEntry<&DeviceFunctions::cmdSetStencilCompareMask, VkEntryCommands>(functions.cmdSetStencilCompareMask.function);
    functions.cmdSetStencilWriteMask = untimedEntry<&DeviceFunctions::cmdSetStencilWriteMask, VkEntryCommands>(functions.cmdSetStencilWriteMask.function);
    functions.cmdSetStencilReference = untimedEntry<&DeviceFunctions::cmdSetStencilReference, VkEntryCommands>(functions.cmdSetStencilReference.function);
    unwrapEntry<&DeviceFunctions::cmdBindVertexBuffers, VkEntryCommands>(functions);
    unwrapEntry<&DeviceFunctions::cmdBindIndexBuffer, VkEntryCommands>(functions);
    unwrapEntry<&DeviceFunctions::cmdDraw, VkEntryCommands>(functions);
    unwrapEntry<&DeviceFunctions::cmdDrawIndexed, VkEntryCommands>(functions);
    unwrapEntry<&DeviceFunctions::cmdDrawIndirect, VkEntryCommands>(functions);
    unwrapEntry<&DeviceFunctions::cmdDrawIndexedIndirect, VkEntryCommands>(functions);
    unwrapEntry<&DeviceFunctions::cmdCopyBufferToImage, VkEntryCommands>(functions);
    unwrapEntry<&DeviceFunctions::cmdCopyImageToBuffer, VkEntryCommands>(functions);
    unwrapEntry<&DeviceFunctions::cmdClearColorImage, VkEntryCommands>(functions);
    unwrapEntry<&DeviceFunctions::updateDescriptorSets, VkEntryDescriptors>(functions);
    unwrapEntry<&DeviceFunctions::allocateDescriptorSets, VkEntryDescriptors>(functions);
    unwrapEntry<&DeviceFunctions::getFenceStatus, VkEntryOther>(functions);
    unwrapEntry<&DeviceFunctions::createBuffer, VkEntryOther>(functions);
    unwrapEntry<&DeviceFunctions::destroyBuffer, VkEntryOther>(functions);
    unwrapEntry<&DeviceFunctions::getBufferMemoryRequirements, VkEntryOther>(functions);
    unwrapEntry<&DeviceFunctions::bindBufferMemory, VkEntryOther>(functions);
    unwrapEntry<&DeviceFunctions::allocateMemory, VkEntryOther>(functions);
    unwrapEntry<&DeviceFunctions::freeMemory, VkEntryOther>(functions);
    unwrapEntry<&DeviceFunctions::mapMemory, VkEntryOther>(functions);
    unwrapEntry<&DeviceFunctions::unmapMemory, VkEntryOther>(functions);
}

}

}
