#include "prx/libSceAgcDriver/Graphics/include/CommandEncoder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libSceAgcDriver/tests/AllocationFailure.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

namespace {

using AgcDriver::Graphics::CommandEncoder;
using AgcDriver::Graphics::DeviceFunctions;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::mutex logMutex;
std::map<VkCommandBuffer, std::vector<std::string>> recorded;

void Log(VkCommandBuffer commands, std::string text) {
    std::lock_guard lock(logMutex);
    recorded[commands].push_back(std::move(text));
}

template<typename T>
std::uint64_t Raw(T handle) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle));
}

template<typename T>
T Handle(std::uint64_t value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(static_cast<std::uintptr_t>(value));
    else return static_cast<T>(value);
}

VKAPI_ATTR void VKAPI_CALL FakeDraw(VkCommandBuffer commands, std::uint32_t vertices, std::uint32_t instances, std::uint32_t firstVertex, std::uint32_t firstInstance) {
    Log(commands, "draw " + std::to_string(vertices) + " " + std::to_string(instances) + " " + std::to_string(firstVertex) + " " + std::to_string(firstInstance));
}

VKAPI_ATTR void VKAPI_CALL FakeBindPipeline(VkCommandBuffer commands, VkPipelineBindPoint point, VkPipeline pipeline) {
    Log(commands, "pipeline " + std::to_string(point) + " " + std::to_string(Raw(pipeline)));
}

VKAPI_ATTR void VKAPI_CALL FakeSetViewport(VkCommandBuffer commands, std::uint32_t first, std::uint32_t count, const VkViewport* viewports) {
    std::string text = "viewport " + std::to_string(first);
    for (std::uint32_t index = 0; index < count; ++index) text += " " + std::to_string(viewports[index].x) + "," + std::to_string(viewports[index].width);
    Log(commands, text);
}

VKAPI_ATTR void VKAPI_CALL FakeSetColorWriteMask(VkCommandBuffer commands, std::uint32_t first, std::uint32_t count, const VkColorComponentFlags* masks) {
    std::string text = "mask " + std::to_string(first);
    for (std::uint32_t index = 0; index < count; ++index) text += " " + std::to_string(masks[index]);
    Log(commands, text);
}

VKAPI_ATTR void VKAPI_CALL FakePushConstants(VkCommandBuffer commands, VkPipelineLayout layout, VkShaderStageFlags stages, std::uint32_t offset, std::uint32_t size, const void* values) {
    std::string text = "push " + std::to_string(Raw(layout)) + " " + std::to_string(stages) + " " + std::to_string(offset);
    for (std::uint32_t index = 0; index < size; ++index) text += " " + std::to_string(static_cast<const std::uint8_t*>(values)[index]);
    Log(commands, text);
}

VKAPI_ATTR void VKAPI_CALL FakeBindDescriptorSets(VkCommandBuffer commands, VkPipelineBindPoint point, VkPipelineLayout layout, std::uint32_t first, std::uint32_t count, const VkDescriptorSet* sets, std::uint32_t offsets, const std::uint32_t* dynamicOffsets) {
    std::string text = "sets " + std::to_string(point) + " " + std::to_string(Raw(layout)) + " " + std::to_string(first);
    for (std::uint32_t index = 0; index < count; ++index) text += " " + std::to_string(Raw(sets[index]));
    for (std::uint32_t index = 0; index < offsets; ++index) text += " +" + std::to_string(dynamicOffsets[index]);
    Log(commands, text);
}

VKAPI_ATTR void VKAPI_CALL FakePipelineBarrier(VkCommandBuffer commands, VkPipelineStageFlags source, VkPipelineStageFlags destination, VkDependencyFlags, std::uint32_t memory, const VkMemoryBarrier* barriers, std::uint32_t, const VkBufferMemoryBarrier*, std::uint32_t, const VkImageMemoryBarrier*) {
    std::string text = "barrier " + std::to_string(source) + " " + std::to_string(destination);
    for (std::uint32_t index = 0; index < memory; ++index) text += " " + std::to_string(barriers[index].srcAccessMask) + (barriers[index].pNext != nullptr ? "+chain" : "");
    Log(commands, text);
}

VKAPI_ATTR void VKAPI_CALL FakeUpdateBuffer(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, const void* data) {
    std::string text = "update " + std::to_string(Raw(buffer)) + " " + std::to_string(offset);
    for (VkDeviceSize index = 0; index < size; ++index) text += " " + std::to_string(static_cast<const std::uint8_t*>(data)[index]);
    Log(commands, text);
}

VKAPI_ATTR void VKAPI_CALL FakeBeginRenderPass(VkCommandBuffer commands, const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
    std::string text = "begin " + std::to_string(Raw(info->renderPass)) + " " + std::to_string(contents);
    for (std::uint32_t index = 0; index < info->clearValueCount; ++index) text += " " + std::to_string(info->pClearValues[index].color.uint32[0]);
    Log(commands, text);
}

VKAPI_ATTR void VKAPI_CALL FakeEndRenderPass(VkCommandBuffer commands) {
    Log(commands, "end");
}

VKAPI_ATTR void VKAPI_CALL FakeCopyBuffer(VkCommandBuffer commands, VkBuffer source, VkBuffer destination, std::uint32_t count, const VkBufferCopy* regions) {
    std::string text = "copy " + std::to_string(Raw(source)) + " " + std::to_string(Raw(destination));
    for (std::uint32_t index = 0; index < count; ++index) text += " " + std::to_string(regions[index].srcOffset) + ">" + std::to_string(regions[index].dstOffset) + ":" + std::to_string(regions[index].size);
    Log(commands, text);
}

std::string ImageCopies(std::uint32_t count, const VkBufferImageCopy* regions) {
    std::string text;
    for (std::uint32_t index = 0; index < count; ++index) text += " " + std::to_string(regions[index].bufferOffset) + "@" + std::to_string(regions[index].imageExtent.width) + "x" + std::to_string(regions[index].imageExtent.height);
    return text;
}

VKAPI_ATTR void VKAPI_CALL FakeCopyBufferToImage(VkCommandBuffer commands, VkBuffer source, VkImage destination, VkImageLayout layout, std::uint32_t count, const VkBufferImageCopy* regions) {
    Log(commands, "to-image " + std::to_string(Raw(source)) + " " + std::to_string(Raw(destination)) + " " + std::to_string(layout) + ImageCopies(count, regions));
}

VKAPI_ATTR void VKAPI_CALL FakeCopyImageToBuffer(VkCommandBuffer commands, VkImage source, VkImageLayout layout, VkBuffer destination, std::uint32_t count, const VkBufferImageCopy* regions) {
    Log(commands, "to-buffer " + std::to_string(Raw(source)) + " " + std::to_string(layout) + " " + std::to_string(Raw(destination)) + ImageCopies(count, regions));
}

VKAPI_ATTR void VKAPI_CALL FakeClearColorImage(VkCommandBuffer commands, VkImage image, VkImageLayout layout, const VkClearColorValue* color, std::uint32_t count, const VkImageSubresourceRange* ranges) {
    std::string text = "clear " + std::to_string(Raw(image)) + " " + std::to_string(layout) + " " + std::to_string(color->uint32[0]) + "," + std::to_string(color->uint32[3]);
    for (std::uint32_t index = 0; index < count; ++index) text += " " + std::to_string(ranges[index].baseMipLevel) + "+" + std::to_string(ranges[index].levelCount);
    Log(commands, text);
}

VKAPI_ATTR void VKAPI_CALL FakeBeginQuery(VkCommandBuffer commands, VkQueryPool pool, std::uint32_t query, VkQueryControlFlags flags) {
    Log(commands, "begin-query " + std::to_string(Raw(pool)) + " " + std::to_string(query) + " " + std::to_string(flags));
}

VKAPI_ATTR void VKAPI_CALL FakeEndQuery(VkCommandBuffer commands, VkQueryPool pool, std::uint32_t query) {
    Log(commands, "end-query " + std::to_string(Raw(pool)) + " " + std::to_string(query));
}

DeviceFunctions FakeTable() {
    DeviceFunctions table{};
    table.cmdDraw = &FakeDraw;
    table.cmdBindPipeline = &FakeBindPipeline;
    table.cmdSetViewport = &FakeSetViewport;
    table.cmdSetColorWriteMask = &FakeSetColorWriteMask;
    table.cmdPushConstants = &FakePushConstants;
    table.cmdBindDescriptorSets = &FakeBindDescriptorSets;
    table.cmdPipelineBarrier = &FakePipelineBarrier;
    table.cmdUpdateBuffer = &FakeUpdateBuffer;
    table.cmdBeginRenderPass = &FakeBeginRenderPass;
    table.cmdEndRenderPass = &FakeEndRenderPass;
    table.cmdCopyBuffer = &FakeCopyBuffer;
    table.cmdCopyBufferToImage = &FakeCopyBufferToImage;
    table.cmdCopyImageToBuffer = &FakeCopyImageToBuffer;
    table.cmdClearColorImage = &FakeClearColorImage;
    table.cmdBeginQuery = &FakeBeginQuery;
    table.cmdEndQuery = &FakeEndQuery;
    return table;
}

struct Oracle {
    DeviceFunctions& table;
    std::map<VkCommandBuffer, std::vector<std::string>> expected;

    void call(std::mt19937& random, VkCommandBuffer commands) {
        const auto value = static_cast<std::uint32_t>(random() % 251);
        switch (random() % 10) {
        case 0:
            table.cmdDraw(commands, value, 1, value / 2, 0);
            FakeExpect(commands, "draw " + std::to_string(value) + " 1 " + std::to_string(value / 2) + " 0");
            break;
        case 1:
            table.cmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, Handle<VkPipeline>(value + 1));
            FakeExpect(commands, "pipeline 0 " + std::to_string(value + 1));
            break;
        case 2: {
            std::vector<VkViewport> viewports(1 + value % 3);
            std::string text = "viewport 0";
            for (std::size_t index = 0; index < viewports.size(); ++index) {
                viewports[index] = VkViewport{static_cast<float>(value + index), 0.0f, static_cast<float>(index), 1.0f, 0.0f, 1.0f};
                text += " " + std::to_string(viewports[index].x) + "," + std::to_string(viewports[index].width);
            }
            table.cmdSetViewport(commands, 0, static_cast<std::uint32_t>(viewports.size()), viewports.data());
            std::fill(viewports.begin(), viewports.end(), VkViewport{});
            FakeExpect(commands, text);
            break;
        }
        case 3: {
            std::vector<std::uint8_t> bytes(4 * (1 + value % 8));
            std::string text = "push 7 1 0";
            for (std::size_t index = 0; index < bytes.size(); ++index) {
                bytes[index] = static_cast<std::uint8_t>(value + index);
                text += " " + std::to_string(bytes[index]);
            }
            table.cmdPushConstants(commands, Handle<VkPipelineLayout>(7), VK_SHADER_STAGE_VERTEX_BIT, 0, static_cast<std::uint32_t>(bytes.size()), bytes.data());
            std::fill(bytes.begin(), bytes.end(), 0);
            FakeExpect(commands, text);
            break;
        }
        case 4: {
            VkDescriptorSet sets[2]{Handle<VkDescriptorSet>(value + 3), Handle<VkDescriptorSet>(value + 4)};
            std::uint32_t offsets[1]{value * 16};
            const std::uint32_t count = 1 + value % 2;
            const std::uint32_t dynamic = value % 2;
            std::string text = "sets 0 9 1";
            for (std::uint32_t index = 0; index < count; ++index) text += " " + std::to_string(Raw(sets[index]));
            for (std::uint32_t index = 0; index < dynamic; ++index) text += " +" + std::to_string(offsets[index]);
            table.cmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, Handle<VkPipelineLayout>(9), 1, count, sets, dynamic, offsets);
            sets[0] = VK_NULL_HANDLE;
            offsets[0] = 0;
            FakeExpect(commands, text);
            break;
        }
        case 5: {
            VkBaseInStructure link{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr};
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, value % 5 == 0 ? &link : nullptr, value, value + 1};
            table.cmdPipelineBarrier(commands, 1, 2, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            FakeExpect(commands, "barrier 1 2 " + std::to_string(value) + (value % 5 == 0 ? "+chain" : ""));
            break;
        }
        case 6: {
            std::uint8_t bytes[4]{static_cast<std::uint8_t>(value), 1, 2, 3};
            table.cmdUpdateBuffer(commands, Handle<VkBuffer>(5), value * 4, 4, bytes);
            FakeExpect(commands, "update 5 " + std::to_string(value * 4) + " " + std::to_string(bytes[0]) + " 1 2 3");
            break;
        }
        case 7: {
            VkClearValue clears[2]{};
            clears[0].color.uint32[0] = value;
            clears[1].color.uint32[0] = value + 1;
            VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            info.renderPass = Handle<VkRenderPass>(value + 11);
            info.clearValueCount = value % 3;
            info.pClearValues = clears;
            std::string text = "begin " + std::to_string(value + 11) + " 0";
            for (std::uint32_t index = 0; index < info.clearValueCount; ++index) text += " " + std::to_string(clears[index].color.uint32[0]);
            table.cmdBeginRenderPass(commands, &info, VK_SUBPASS_CONTENTS_INLINE);
            clears[0].color.uint32[0] = 0;
            FakeExpect(commands, text);
            break;
        }
        case 8: {
            VkColorComponentFlags masks[3]{value & 0xfu, (value >> 1) & 0xfu, (value >> 2) & 0xfu};
            const std::uint32_t first = value % 2;
            const std::uint32_t count = 1 + value % 3;
            std::string text = "mask " + std::to_string(first);
            for (std::uint32_t index = 0; index < count; ++index) text += " " + std::to_string(masks[index]);
            table.cmdSetColorWriteMask(commands, first, count, masks);
            std::fill(std::begin(masks), std::end(masks), 0u);
            FakeExpect(commands, text);
            break;
        }
        default:
            table.cmdEndRenderPass(commands);
            FakeExpect(commands, "end");
            break;
        }
    }

    void FakeExpect(VkCommandBuffer commands, std::string text) {
        expected[commands].push_back(std::move(text));
    }
};

void Compare(const std::map<VkCommandBuffer, std::vector<std::string>>& expected, const char* scenario) {
    std::lock_guard lock(logMutex);
    for (const auto& [commands, sequence] : expected) {
        const auto& actual = recorded[commands];
        Require(actual.size() == sequence.size(), std::string(scenario) + ": command buffer " + std::to_string(Raw(commands)) + " recorded " + std::to_string(actual.size()) + " commands, inline records " + std::to_string(sequence.size()));
        for (std::size_t index = 0; index < sequence.size(); ++index) Require(actual[index] == sequence[index], std::string(scenario) + ": command " + std::to_string(index) + " on command buffer " + std::to_string(Raw(commands)) + " is '" + actual[index] + "', inline records '" + sequence[index] + "'");
    }
}

std::map<VkCommandBuffer, CommandEncoder::Recording>& Recordings() {
    static std::map<VkCommandBuffer, CommandEncoder::Recording> value;
    return value;
}

CommandEncoder::Recording& RecordingOf(VkCommandBuffer commands) {
    return Recordings()[commands];
}

void Reset() {
    std::lock_guard lock(logMutex);
    recorded.clear();
    Recordings().clear();
}

std::size_t Recorded(VkCommandBuffer commands) {
    std::lock_guard lock(logMutex);
    return recorded[commands].size();
}

std::unique_ptr<CommandEncoder> Attached(VkDevice device, DeviceFunctions& table) {
    auto encoder = CommandEncoder::Attach(device, table);
    Require(encoder != nullptr, "no command encoder slot was free");
    return encoder;
}

void RandomProgramRecordsWhatInlineRecords(std::uint32_t seed, bool delays) {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x40);
    std::mt19937 random(seed);
    if (delays) {
        CommandEncoder::SetDelayForTesting([seed]() mutable {
            thread_local std::mt19937 delay(seed * 7 + 1);
            const auto pause = delay() % 4;
            if (pause == 1) std::this_thread::yield();
            if (pause == 2) std::this_thread::sleep_for(std::chrono::microseconds(delay() % 200));
        });
    }
    {
        const auto attached = Attached(device, table);
        auto& encoder = *attached;
        Oracle oracle{table, {}};
        const VkCommandBuffer buffers[2]{Handle<VkCommandBuffer>(0x100), Handle<VkCommandBuffer>(0x200)};
        std::uint32_t depth = 0;
        VkCommandBuffer open = VK_NULL_HANDLE;
        for (std::uint32_t step = 0; step < 20000; ++step) {
            const auto choice = random() % 20;
            if (choice == 0 && depth < 3) {
                if (depth == 0) open = buffers[random() % 2];
                encoder.Open(open, RecordingOf(open));
                ++depth;
            } else if (choice == 1 && depth != 0) {
                encoder.Close();
                --depth;
            } else if (choice == 2) {
                encoder.Quiesce();
            } else if (choice == 3) {
                AgcDriver::Graphics::BeforeCommandLookup(device);
            } else if (choice == 4) {
                encoder.Quiesce();
                const auto commands = buffers[random() % 2];
                encoder.Real().cmdDraw(commands, 1000 + step, 1, 0, 0);
                oracle.FakeExpect(commands, "draw " + std::to_string(1000 + step) + " 1 0 0");
            } else {
                oracle.call(random, buffers[random() % 2]);
            }
        }
        while (depth != 0) {
            encoder.Close();
            --depth;
        }
        encoder.Quiesce();
        Compare(oracle.expected, delays ? "random program with encoder delays" : "random program");
        const auto counts = encoder.Counts();
        Require(counts.commands != 0, "the encoder recorded no command: the windows deferred nothing");
    }
    CommandEncoder::SetDelayForTesting({});
    Require(table.cmdDraw == &FakeDraw, "the device table kept an encoder entry after the encoder was destroyed");
}

// A command the encoder does not interpose runs on the producing thread at once, ahead of everything
// queued before it: a state it sets lands on the wrong draws and races the encoder's own recording.
// Every command of the device table must therefore be replaced, so the next command added to the
// table without an encoder entry fails here instead of in a draw. The three listed members are the
// table's non-command entry points.
void EveryCommandOfTheTableIsInterposed() {
    static_assert(std::is_standard_layout_v<DeviceFunctions> && sizeof(DeviceFunctions) % sizeof(void*) == 0, "the table must be a plain array of entry points");
    constexpr std::size_t entries = sizeof(DeviceFunctions) / sizeof(void*);
    const std::size_t notCommands[]{offsetof(DeviceFunctions, updateDescriptorSets) / sizeof(void*), offsetof(DeviceFunctions, allocateDescriptorSets) / sizeof(void*), offsetof(DeviceFunctions, getFenceStatus) / sizeof(void*)};
    DeviceFunctions table{};
    void* before[entries];
    for (std::size_t index = 0; index < entries; ++index) before[index] = reinterpret_cast<void*>(&FakeDraw);
    std::memcpy(&table, before, sizeof(table));
    {
        const auto attached = Attached(Handle<VkDevice>(0x4f), table);
        void* after[entries];
        std::memcpy(after, &table, sizeof(table));
        for (std::size_t index = 0; index < entries; ++index) {
            const bool command = std::find(std::begin(notCommands), std::end(notCommands), index) == std::end(notCommands);
            Require(!command || after[index] != before[index], "entry " + std::to_string(index) + " of the device table is a command the encoder does not interpose; add it to install() in CommandEncoder.cpp, or list it as a non-command here");
            Require(command || after[index] == before[index], "entry " + std::to_string(index) + " of the device table is not a command but the encoder replaced it");
        }
    }
    Require(std::memcmp(&table, before, sizeof(table)) == 0, "destroying the encoder did not restore the device table");
}

void OtherThreadsRecordDirectlyWhileAWindowIsOpen() {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x41);
    const auto attached = Attached(device, table);
    auto& encoder = *attached;
    const auto mine = Handle<VkCommandBuffer>(0x300);
    const auto theirs = Handle<VkCommandBuffer>(0x400);
    std::map<VkCommandBuffer, std::vector<std::string>> expected;
    std::thread other([&] {
        for (std::uint32_t index = 0; index < 5000; ++index) table.cmdDraw(theirs, index, 2, 0, 0);
    });
    encoder.Open(mine, RecordingOf(mine));
    for (std::uint32_t index = 0; index < 5000; ++index) {
        table.cmdDraw(mine, index, 1, 0, 0);
        expected[mine].push_back("draw " + std::to_string(index) + " 1 0 0");
        expected[theirs].push_back("draw " + std::to_string(index) + " 2 0 0");
    }
    encoder.Close();
    other.join();
    encoder.Quiesce();
    Compare(expected, "another thread's command buffer");
}

void HookDuringUnencodedOpsWaits() {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x42);
    const auto attached = Attached(device, table);
    auto& encoder = *attached;
    const auto commands = Handle<VkCommandBuffer>(0x500);
    std::atomic<bool> release{false};
    CommandEncoder::SetDelayForTesting([&] {
        while (!release.load()) std::this_thread::yield();
    });
    encoder.Open(commands, RecordingOf(commands));
    table.cmdDraw(commands, 1, 1, 0, 0);
    encoder.Close();
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        release.store(true);
    });
    table.cmdDraw(commands, 2, 1, 0, 0);
    releaser.join();
    CommandEncoder::SetDelayForTesting({});
    encoder.Quiesce();
    Compare({{commands, {"draw 1 1 0 0", "draw 2 1 0 0"}}}, "a direct call after a closed window");
}

void NestedWindowKeepsOrder() {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x43);
    const auto attached = Attached(device, table);
    auto& encoder = *attached;
    const auto outer = Handle<VkCommandBuffer>(0x600);
    const auto inner = Handle<VkCommandBuffer>(0x700);
    encoder.Open(outer, RecordingOf(outer));
    table.cmdDraw(outer, 1, 1, 0, 0);
    encoder.Open(outer, RecordingOf(outer));
    table.cmdDraw(outer, 2, 1, 0, 0);
    encoder.Close();
    table.cmdDraw(outer, 3, 1, 0, 0);
    encoder.Open(inner, RecordingOf(inner));
    table.cmdDraw(inner, 4, 1, 0, 0);
    table.cmdDraw(outer, 5, 1, 0, 0);
    encoder.Close();
    table.cmdDraw(outer, 6, 1, 0, 0);
    encoder.Close();
    table.cmdDraw(outer, 7, 1, 0, 0);
    encoder.Quiesce();
    Compare({{outer, {"draw 1 1 0 0", "draw 2 1 0 0", "draw 3 1 0 0", "draw 5 1 0 0", "draw 6 1 0 0", "draw 7 1 0 0"}}, {inner, {"draw 4 1 0 0"}}}, "nested windows");
}

void LargeRecordsSpanChunks() {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x44);
    const auto attached = Attached(device, table);
    auto& encoder = *attached;
    const auto commands = Handle<VkCommandBuffer>(0x800);
    std::map<VkCommandBuffer, std::vector<std::string>> expected;
    encoder.Open(commands, RecordingOf(commands));
    for (std::uint32_t round = 0; round < 64; ++round) {
        std::vector<std::uint8_t> bytes(4096 + round);
        std::string text = "push 1 1 0";
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            bytes[index] = static_cast<std::uint8_t>(round + index);
            text += " " + std::to_string(bytes[index]);
        }
        table.cmdPushConstants(commands, Handle<VkPipelineLayout>(1), VK_SHADER_STAGE_VERTEX_BIT, 0, static_cast<std::uint32_t>(bytes.size()), bytes.data());
        expected[commands].push_back(text);
    }
    std::vector<std::uint8_t> huge(256 * 1024, 7);
    table.cmdPushConstants(commands, Handle<VkPipelineLayout>(1), VK_SHADER_STAGE_VERTEX_BIT, 0, static_cast<std::uint32_t>(huge.size()), huge.data());
    std::string text = "push 1 1 0";
    for (const auto byte : huge) text += " " + std::to_string(byte);
    expected[commands].push_back(text);
    encoder.Close();
    encoder.Quiesce();
    Compare(expected, "records larger than a chunk");
}

void RecordEveryPacked(DeviceFunctions& table, VkCommandBuffer commands, std::uint32_t round, std::vector<std::string>& expected) {
    VkBufferCopy copies[2]{{round, 16 + round, 4}, {32, 48, 8 + round}};
    table.cmdCopyBuffer(commands, Handle<VkBuffer>(1), Handle<VkBuffer>(2), 2, copies);
    expected.push_back("copy 1 2 " + std::to_string(round) + ">" + std::to_string(16 + round) + ":4 32>48:" + std::to_string(8 + round));
    std::uint8_t bytes[3]{static_cast<std::uint8_t>(round), 9, 8};
    table.cmdUpdateBuffer(commands, Handle<VkBuffer>(3), 4 * round, 3, bytes);
    expected.push_back("update 3 " + std::to_string(4 * round) + " " + std::to_string(round) + " 9 8");
    VkBufferImageCopy regions[1]{};
    regions[0].bufferOffset = 256 * round;
    regions[0].imageExtent = {8 + round, 4, 1};
    table.cmdCopyBufferToImage(commands, Handle<VkBuffer>(4), Handle<VkImage>(5), VK_IMAGE_LAYOUT_GENERAL, 1, regions);
    expected.push_back("to-image 4 5 1 " + std::to_string(256 * round) + "@" + std::to_string(8 + round) + "x4");
    table.cmdCopyImageToBuffer(commands, Handle<VkImage>(5), VK_IMAGE_LAYOUT_GENERAL, Handle<VkBuffer>(6), 1, regions);
    expected.push_back("to-buffer 5 1 6 " + std::to_string(256 * round) + "@" + std::to_string(8 + round) + "x4");
    VkClearColorValue color{};
    color.uint32[0] = round;
    color.uint32[3] = 7;
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, round, 1, 0, 1};
    table.cmdClearColorImage(commands, Handle<VkImage>(7), VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
    expected.push_back("clear 7 1 " + std::to_string(round) + ",7 " + std::to_string(round) + "+1");
    table.cmdBeginQuery(commands, Handle<VkQueryPool>(8), round, 1);
    expected.push_back("begin-query 8 " + std::to_string(round) + " 1");
    table.cmdEndQuery(commands, Handle<VkQueryPool>(8), round);
    expected.push_back("end-query 8 " + std::to_string(round));
    copies[0] = {};
    bytes[0] = 0;
    regions[0] = {};
    color = {};
}

void TransfersAndQueriesInAWindowNeverWait() {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x46);
    const auto attached = Attached(device, table);
    auto& encoder = *attached;
    const auto commands = Handle<VkCommandBuffer>(0xa00);
    CommandEncoder::SetDelayForTesting([] { std::this_thread::sleep_for(std::chrono::milliseconds(5)); });
    std::vector<std::string> expected;
    for (std::uint32_t round = 0; round < 4; ++round) {
        encoder.Open(commands, RecordingOf(commands));
        table.cmdDraw(commands, round, 1, 0, 0);
        expected.push_back("draw " + std::to_string(round) + " 1 0 0");
        RecordEveryPacked(table, commands, round, expected);
        Require(encoder.Counts().waits == 0, "a transfer or query command in a window waited for the encoder");
        encoder.Close();
    }
    encoder.Quiesce();
    CommandEncoder::SetDelayForTesting({});
    Compare({{commands, expected}}, "transfers and queries in a window");
}

void UnwindingSettlesQueuedRecording() {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x45);
    const auto attached = Attached(device, table);
    auto& encoder = *attached;
    const auto commands = Handle<VkCommandBuffer>(0x900);
    CommandEncoder::SetDelayForTesting([] { std::this_thread::sleep_for(std::chrono::milliseconds(30)); });
    std::size_t recordedAtCatch = 0;
    try {
        encoder.Open(commands, RecordingOf(commands));
        const CommandEncoder::UnwindQuiesce guard(attached.get());
        table.cmdDraw(commands, 1, 1, 0, 0);
        table.cmdDraw(commands, 2, 1, 0, 0);
        encoder.Close();
        throw std::runtime_error("injected failure after the window closed");
    } catch (const std::runtime_error&) {
        std::lock_guard lock(logMutex);
        recordedAtCatch = recorded[commands].size();
    }
    CommandEncoder::SetDelayForTesting({});
    Require(recordedAtCatch == 2, "unwinding released the owners of a queued draw while the encoder had recorded " + std::to_string(recordedAtCatch) + " of 2 commands");
    encoder.Quiesce();
}

struct RecordedAtDestruction {
    VkCommandBuffer commands;
    std::size_t& seen;
    ~RecordedAtDestruction() {
        std::lock_guard lock(logMutex);
        seen = recorded[commands].size();
    }
};

void OwnersOutliveTheUnwindDrain() {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x46);
    const auto attached = Attached(device, table);
    auto& encoder = *attached;
    const auto commands = Handle<VkCommandBuffer>(0xa00);
    CommandEncoder::SetDelayForTesting([] { std::this_thread::sleep_for(std::chrono::milliseconds(30)); });
    std::size_t recordedAtRelease = 0;
    try {
        encoder.Open(commands, RecordingOf(commands));
        RecordedAtDestruction owner{commands, recordedAtRelease};
        const CommandEncoder::UnwindQuiesce guard(attached.get());
        table.cmdDraw(commands, 1, 1, 0, 0);
        table.cmdDraw(commands, 2, 1, 0, 0);
        encoder.Close();
        throw std::runtime_error("injected failure after the window closed");
    } catch (const std::runtime_error&) {
    }
    CommandEncoder::SetDelayForTesting({});
    Require(recordedAtRelease == 2, "an owner of a queued draw was released while the encoder had recorded " + std::to_string(recordedAtRelease) + " of 2 commands");
    encoder.Quiesce();
}

void ForeignThreadCannotProduceIntoAnOpenWindow() {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x47);
    const auto attached = Attached(device, table);
    auto& encoder = *attached;
    const auto commands = Handle<VkCommandBuffer>(0xb00);
    encoder.Open(commands, RecordingOf(commands));
    table.cmdDraw(commands, 1, 1, 0, 0);
    std::atomic<bool> refused{false};
    std::thread other([&] {
        try {
            table.cmdDraw(commands, 9, 1, 0, 0);
        } catch (const std::logic_error&) {
            refused.store(true);
        }
    });
    other.join();
    Require(refused.load(), "a call on a command buffer with an open window from another thread was not refused");
    table.cmdDraw(commands, 2, 1, 0, 0);
    encoder.Close();
    std::thread later([&] { table.cmdDraw(commands, 3, 1, 0, 0); });
    later.join();
    encoder.Quiesce();
    Compare({{commands, {"draw 1 1 0 0", "draw 2 1 0 0", "draw 3 1 0 0"}}}, "a foreign producer");
}

void ForeignLookupWhileAWindowIsOpenProceeds() {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x48);
    const auto attached = Attached(device, table);
    auto& encoder = *attached;
    const auto commands = Handle<VkCommandBuffer>(0xc00);
    const auto own = Handle<VkCommandBuffer>(0xc80);
    encoder.Open(commands, RecordingOf(commands));
    table.cmdDraw(commands, 1, 1, 0, 0);
    std::atomic<bool> refused{false};
    std::thread other([&] {
        try {
            AgcDriver::Graphics::BeforeCommandLookup(device);
            table.cmdDraw(own, 7, 1, 0, 0);
        } catch (const std::logic_error&) {
            refused.store(true);
        }
    });
    other.join();
    Require(!refused.load(), "a command lookup from another thread for its own command buffer was refused while a window was open");
    table.cmdDraw(commands, 2, 1, 0, 0);
    encoder.Close();
    encoder.Quiesce();
    Compare({{commands, {"draw 1 1 0 0", "draw 2 1 0 0"}}, {own, {"draw 7 1 0 0"}}}, "a foreign lookup");
}

void RawCallAfterLookupKeepsOrder() {
    Reset();
    auto table = FakeTable();
    const auto raw = table;
    const auto device = Handle<VkDevice>(0x49);
    const auto attached = Attached(device, table);
    auto& encoder = *attached;
    const auto commands = Handle<VkCommandBuffer>(0xd00);
    CommandEncoder::SetDelayForTesting([] { std::this_thread::sleep_for(std::chrono::milliseconds(5)); });
    encoder.Open(commands, RecordingOf(commands));
    table.cmdDraw(commands, 1, 1, 0, 0);
    table.cmdDraw(commands, 2, 1, 0, 0);
    AgcDriver::Graphics::BeforeCommandLookup(device);
    raw.cmdDraw(commands, 3, 1, 0, 0);
    table.cmdDraw(commands, 4, 1, 0, 0);
    encoder.Close();
    encoder.Quiesce();
    CommandEncoder::SetDelayForTesting({});
    Compare({{commands, {"draw 1 1 0 0", "draw 2 1 0 0", "draw 3 1 0 0", "draw 4 1 0 0"}}}, "a raw call after a lookup inside a window");
}

}

bool Reports(const std::function<void()>& settle) {
    try {
        settle();
        return false;
    } catch (const std::runtime_error& error) {
        Require(std::string(error.what()) == "injected replay failure", std::string("the encoder reported '") + error.what() + "' for a failed replay");
        return true;
    }
}

bool Refuses(const std::function<void()>& finish) {
    try {
        finish();
        return false;
    } catch (const std::runtime_error& error) {
        return std::string(error.what()) != "injected replay failure";
    }
}

void FailedReplayStaysWithItsRecording() {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x4a);
    const auto failing = Handle<VkCommandBuffer>(0xe00);
    const auto healthy = Handle<VkCommandBuffer>(0xe80);
    {
        const auto attached = Attached(device, table);
        auto& encoder = *attached;
        std::atomic<int> failures{1};
        CommandEncoder::SetDelayForTesting([&] {
            if (failures.fetch_sub(1) > 0) throw std::runtime_error("injected replay failure");
        });
        std::thread producer([&] {
            encoder.Open(failing, RecordingOf(failing));
            table.cmdDraw(failing, 1, 1, 0, 0);
            encoder.Close();
            encoder.Quiesce();
        });
        producer.join();
        std::thread other([&] {
            encoder.Settle();
            AgcDriver::Graphics::BeforeCommandLookup(device);
        });
        other.join();
        encoder.Open(healthy, RecordingOf(healthy));
        table.cmdDraw(healthy, 2, 1, 0, 0);
        encoder.Close();
        encoder.Open(failing, RecordingOf(failing));
        table.cmdDraw(failing, 3, 1, 0, 0);
        encoder.Close();
        encoder.Quiesce();
        CommandEncoder::SetDelayForTesting({});
    }
    Require(RecordingOf(failing).Failed() && !RecordingOf(healthy).Failed(), "a replay failure was not kept with the recording it happened in");
    Require(Reports([&] { RecordingOf(failing).Report(); }), "a failed replay recorded by a thread that has exited was not reported to the next settle of its recording");
    Require(!Reports([&] { RecordingOf(failing).Report(); }), "a failed replay was reported twice");
    Require(Refuses([&] { RecordingOf(failing).Finish(); }), "finishing a recording whose reported replay failure was not refused, or reported it again");
    Require(!Reports([&] { RecordingOf(healthy).Finish(); }), "finishing a healthy recording reported another recording's failed replay");
    Require(Recorded(failing) == 0, "the encoder recorded " + std::to_string(Recorded(failing)) + " commands into a recording after a replay into it failed");
    Compare({{healthy, {"draw 2 1 0 0"}}}, "another recording after a failed replay");
}

void FinishCarriesAnUnreportedFailure() {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x4b);
    const auto failing = Handle<VkCommandBuffer>(0xf00);
    const auto attached = Attached(device, table);
    auto& encoder = *attached;
    std::atomic<int> failures{1};
    CommandEncoder::SetDelayForTesting([&] {
        if (failures.fetch_sub(1) > 0) throw std::runtime_error("injected replay failure");
    });
    encoder.Open(failing, RecordingOf(failing));
    table.cmdDraw(failing, 1, 1, 0, 0);
    encoder.Close();
    encoder.Quiesce();
    CommandEncoder::SetDelayForTesting({});
    std::atomic<bool> carried{false};
    std::thread submitter([&] { carried.store(Reports([&] { RecordingOf(failing).Finish(); })); });
    submitter.join();
    Require(carried.load(), "finishing a recording on another thread did not carry its unreported replay failure");
    Require(!Reports([&] { RecordingOf(failing).Report(); }), "a failed replay carried by finishing its recording was reported again");
}

void DirectCallsSkipAFailedRecording() {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x4e);
    const auto failing = Handle<VkCommandBuffer>(0x1200);
    const auto healthy = Handle<VkCommandBuffer>(0x1280);
    const auto attached = Attached(device, table);
    auto& encoder = *attached;
    std::atomic<int> failures{1};
    CommandEncoder::SetDelayForTesting([&] {
        if (failures.fetch_sub(1) > 0) throw std::runtime_error("injected replay failure");
    });
    const std::uint8_t bytes[] = {7};
    const VkMemoryBarrier chainedLink{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    const VkMemoryBarrier chained{VK_STRUCTURE_TYPE_MEMORY_BARRIER, &chainedLink, 1, 2};
    encoder.Open(failing, RecordingOf(failing));
    table.cmdDraw(failing, 1, 1, 0, 0);
    table.cmdUpdateBuffer(failing, Handle<VkBuffer>(0x30), 0, 1, bytes);
    table.cmdPipelineBarrier(failing, 1, 2, 0, 1, &chained, 0, nullptr, 0, nullptr);
    encoder.Close();
    encoder.Open(healthy, RecordingOf(healthy));
    table.cmdUpdateBuffer(healthy, Handle<VkBuffer>(0x31), 0, 1, bytes);
    encoder.Close();
    encoder.Quiesce();
    CommandEncoder::SetDelayForTesting({});
    Require(Recorded(failing) == 0, "a window recorded " + std::to_string(Recorded(failing)) + " direct calls into a recording after a replay into it failed");
    Require(Reports([&] { RecordingOf(failing).Report(); }), "a failed replay followed by direct calls was not reported");
    Compare({{healthy, {"update 49 0 7"}}}, "a direct call into another recording after a failed replay");
}

void AppendFailureKeepsTheWindow() {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x4f);
    const auto commands = Handle<VkCommandBuffer>(0x1300);
    const auto attached = Attached(device, table);
    auto& encoder = *attached;
    encoder.Open(commands, RecordingOf(commands));
    table.cmdDraw(commands, 1, 1, 0, 0);
    bool refused = false;
    try {
        static_cast<void>(encoder.Append([](const std::byte*, VkCommandBuffer, const DeviceFunctions&) {}, static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max() / 2)));
    } catch (const std::exception&) {
        refused = true;
    }
    table.cmdDraw(commands, 2, 1, 0, 0);
    encoder.Close();
    encoder.Quiesce();
    Require(refused, "a record too large to allocate was appended");
    Compare({{commands, {"draw 1 1 0 0", "draw 2 1 0 0"}}}, "a window after a record failed to allocate");
}

void DevicesPastTheSlotsRecordInline() {
    Reset();
    std::vector<DeviceFunctions> tables(CommandEncoder::Slots + 1, FakeTable());
    std::vector<std::unique_ptr<CommandEncoder>> encoders;
    for (std::size_t index = 0; index < tables.size(); ++index) encoders.push_back(CommandEncoder::Attach(Handle<VkDevice>(0x60 + index), tables[index]));
    Require(encoders.back() == nullptr, "a device past the encoder slots got an encoder");
    Require(std::all_of(encoders.begin(), encoders.end() - 1, [](const auto& encoder) { return encoder != nullptr; }), "a device within the encoder slots got no encoder");
    Require(tables.back().cmdDraw == &FakeDraw, "a device without an encoder had its table patched");
    encoders.front().reset();
    Require(CommandEncoder::Attach(Handle<VkDevice>(0x70), tables.back()) != nullptr, "a slot freed by a destroyed encoder was not reused");
    Require(tables.back().cmdDraw == &FakeDraw, "the reused slot's encoder left its table patched");
}

void PostingNeverAllocates() {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x4c);
    const auto commands = Handle<VkCommandBuffer>(0x1000);
    const auto attached = Attached(device, table);
    auto& encoder = *attached;
    std::atomic<bool> release{false};
    CommandEncoder::SetDelayForTesting([&] {
        while (!release.load()) std::this_thread::yield();
    });
    for (std::uint32_t index = 0; index < 200; ++index) {
        encoder.Open(commands, RecordingOf(commands));
        table.cmdDraw(commands, index, 1, 0, 0);
        bool allocated = false;
        AgcDriver::Tests::SetAllocationFailureCountdown(0);
        try {
            encoder.Close();
        } catch (const std::bad_alloc&) {
            allocated = true;
        }
        AgcDriver::Tests::SetAllocationFailureCountdown(-1);
        Require(!allocated, "posting chunk " + std::to_string(index) + " allocated memory");
    }
    encoder.Open(commands, RecordingOf(commands));
    table.cmdDraw(commands, 200, 1, 0, 0);
    release.store(true);
    AgcDriver::Tests::SetAllocationFailureCountdown(0);
    encoder.Quiesce();
    AgcDriver::Tests::SetAllocationFailureCountdown(-1);
    encoder.Close();
    CommandEncoder::SetDelayForTesting({});
    Require(Recorded(commands) == 201, "the encoder recorded " + std::to_string(Recorded(commands)) + " of 201 commands posted without allocating");
}

void ReplayFailureNeedsNoAllocation() {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x4d);
    const auto failing = Handle<VkCommandBuffer>(0x1100);
    const auto healthy = Handle<VkCommandBuffer>(0x1180);
    const auto attached = Attached(device, table);
    auto& encoder = *attached;
    const std::runtime_error injected("injected replay failure");
    std::atomic<int> calls{0};
    CommandEncoder::SetDelayForTesting([&] {
        if (calls.fetch_add(1) == 0) {
            AgcDriver::Tests::SetAllocationFailureCountdown(0);
            throw injected;
        }
        AgcDriver::Tests::SetAllocationFailureCountdown(-1);
    });
    encoder.Open(failing, RecordingOf(failing));
    table.cmdDraw(failing, 1, 1, 0, 0);
    encoder.Close();
    encoder.Quiesce();
    encoder.Open(healthy, RecordingOf(healthy));
    table.cmdDraw(healthy, 2, 1, 0, 0);
    encoder.Close();
    encoder.Quiesce();
    CommandEncoder::SetDelayForTesting({});
    Require(Reports([&] { RecordingOf(failing).Finish(); }), "a failed replay was not kept after its failure was stored without allocating");
    Compare({{healthy, {"draw 2 1 0 0"}}}, "a replay after a failure stored without allocating");
}

void PackedRecordsSkipAFailedRecording() {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x50);
    const auto failing = Handle<VkCommandBuffer>(0x1400);
    const auto healthy = Handle<VkCommandBuffer>(0x1480);
    const auto attached = Attached(device, table);
    auto& encoder = *attached;
    std::atomic<int> failures{1};
    CommandEncoder::SetDelayForTesting([&] {
        if (failures.fetch_sub(1) > 0) throw std::runtime_error("injected replay failure");
    });
    encoder.Open(failing, RecordingOf(failing));
    table.cmdDraw(failing, 1, 1, 0, 0);
    encoder.Close();
    encoder.Quiesce();
    Require(RecordingOf(failing).Failed(), "the injected replay failure was not stored in its recording");
    std::vector<std::string> expected;
    std::vector<std::string> dropped;
    for (std::uint32_t round = 0; round < 3; ++round) {
        encoder.Open(failing, RecordingOf(failing));
        RecordEveryPacked(table, failing, round, dropped);
        encoder.Open(healthy, RecordingOf(healthy));
        RecordEveryPacked(table, healthy, round, expected);
        encoder.Close();
        RecordEveryPacked(table, failing, round + 8, dropped);
        encoder.Close();
    }
    encoder.Quiesce();
    CommandEncoder::SetDelayForTesting({});
    Require(Recorded(failing) == 0, "the encoder recorded " + std::to_string(Recorded(failing)) + " of " + std::to_string(dropped.size()) + " packed transfer and query commands into a recording after a replay into it failed");
    Require(Reports([&] { RecordingOf(failing).Report(); }), "a failed replay followed by packed records was not reported");
    Require(!RecordingOf(healthy).Failed(), "another recording's failed replay marked a healthy recording failed");
    Compare({{healthy, expected}}, "packed records in another recording after a failed replay");
}

void QueriesKeepOrderWithTheirRecording() {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x51);
    const auto first = Handle<VkCommandBuffer>(0x1500);
    const auto second = Handle<VkCommandBuffer>(0x1580);
    CommandEncoder::SetDelayForTesting([] {
        thread_local std::mt19937 delay(17);
        const auto pause = delay() % 4;
        if (pause == 1) std::this_thread::yield();
        if (pause == 2) std::this_thread::sleep_for(std::chrono::microseconds(delay() % 300));
    });
    std::map<VkCommandBuffer, std::vector<std::string>> expected;
    {
        const auto attached = Attached(device, table);
        auto& encoder = *attached;
        const auto query = [&](VkCommandBuffer commands, std::uint32_t slot) {
            table.cmdBeginQuery(commands, Handle<VkQueryPool>(9), slot, 0);
            expected[commands].push_back("begin-query 9 " + std::to_string(slot) + " 0");
            table.cmdDraw(commands, slot, 1, 0, 0);
            expected[commands].push_back("draw " + std::to_string(slot) + " 1 0 0");
            table.cmdEndQuery(commands, Handle<VkQueryPool>(9), slot);
            expected[commands].push_back("end-query 9 " + std::to_string(slot));
        };
        for (std::uint32_t round = 0; round < 200; ++round) {
            const auto outer = round % 2 == 0 ? first : second;
            const auto inner = round % 2 == 0 ? second : first;
            encoder.Open(outer, RecordingOf(outer));
            table.cmdBeginQuery(outer, Handle<VkQueryPool>(8), round, 1);
            expected[outer].push_back("begin-query 8 " + std::to_string(round) + " 1");
            table.cmdDraw(outer, 1000 + round, 1, 0, 0);
            expected[outer].push_back("draw " + std::to_string(1000 + round) + " 1 0 0");
            encoder.Open(inner, RecordingOf(inner));
            query(inner, 2 * round);
            encoder.Close();
            table.cmdEndQuery(outer, Handle<VkQueryPool>(8), round);
            expected[outer].push_back("end-query 8 " + std::to_string(round));
            encoder.Close();
            query(outer, 2 * round + 1);
        }
        encoder.Quiesce();
        Require(!RecordingOf(first).Failed() && !RecordingOf(second).Failed(), "a recording with queries was marked failed");
    }
    CommandEncoder::SetDelayForTesting({});
    Compare(expected, "queries around nested windows of two recordings");
}

class EncoderThreadClock {
public:
    EncoderThreadClock() = default;
    EncoderThreadClock(const EncoderThreadClock&) = delete;
    EncoderThreadClock& operator=(const EncoderThreadClock&) = delete;
    ~EncoderThreadClock() {
#ifdef _WIN32
        if (handle != nullptr) CloseHandle(handle);
#endif
    }
    void Capture() {
        if (captured.load(std::memory_order_acquire)) return;
#ifdef _WIN32
        handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
        if (handle == nullptr) return;
#else
        if (pthread_getcpuclockid(pthread_self(), &clock) != 0) return;
#endif
        captured.store(true, std::memory_order_release);
    }
    bool Captured() const { return captured.load(std::memory_order_acquire); }
    std::chrono::nanoseconds Used() const {
#ifdef _WIN32
        FILETIME created{};
        FILETIME exited{};
        FILETIME kernel{};
        FILETIME user{};
        Require(GetThreadTimes(handle, &created, &exited, &kernel, &user) != 0, "the encoder thread's CPU time could not be read");
        const auto ticks = [](FILETIME time) { return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32u) | time.dwLowDateTime; };
        return std::chrono::nanoseconds((ticks(kernel) + ticks(user)) * 100u);
#else
        timespec at{};
        Require(clock_gettime(clock, &at) == 0, "the encoder thread's CPU time could not be read");
        return std::chrono::seconds(at.tv_sec) + std::chrono::nanoseconds(at.tv_nsec);
#endif
    }

private:
#ifdef _WIN32
    HANDLE handle = nullptr;
#else
    clockid_t clock{};
#endif
    std::atomic<bool> captured{false};
};

void PostOne(CommandEncoder& encoder, DeviceFunctions& table, VkCommandBuffer commands, std::uint32_t index) {
    encoder.Open(commands, RecordingOf(commands));
    table.cmdDraw(commands, index, 1, 0, 0);
    encoder.Close();
}

void BusyFor(std::chrono::nanoseconds span) {
    const auto until = std::chrono::steady_clock::now() + span;
    while (std::chrono::steady_clock::now() < until) {}
}

void EncoderSleepsBetweenFrequentPosts() {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x52);
    const auto commands = Handle<VkCommandBuffer>(0x1500);
    EncoderThreadClock clock;
    CommandEncoder::SetDelayForTesting([&] { clock.Capture(); });
    std::uint32_t posts = 0;
    {
        const auto attached = Attached(device, table);
        auto& encoder = *attached;
        PostOne(encoder, table, commands, posts++);
        encoder.Quiesce();
        Require(clock.Captured(), "the encoder thread's clock was not captured");
        const auto used = clock.Used();
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(400)) {
            PostOne(encoder, table, commands, posts++);
            BusyFor(std::chrono::microseconds(30));
        }
        const auto wall = std::chrono::steady_clock::now() - start;
        const auto spent = clock.Used() - used;
        encoder.Quiesce();
        Require(spent * 2 < wall, "the encoder thread used " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(spent).count()) + " ms of CPU in " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(wall).count()) + " ms while a chunk was posted every 30 us");
    }
    CommandEncoder::SetDelayForTesting({});
    Require(Recorded(commands) == posts, "the encoder recorded " + std::to_string(Recorded(commands)) + " of " + std::to_string(posts) + " frequently posted commands");
}

void EncoderKeepsUpWithoutAWait() {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x53);
    const auto commands = Handle<VkCommandBuffer>(0x1600);
    constexpr std::uint32_t Posts = 64;
    constexpr std::uint32_t LongestLag = 16;
    {
        const auto attached = Attached(device, table);
        auto& encoder = *attached;
        PostOne(encoder, table, commands, 0);
        encoder.Quiesce();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        for (std::uint32_t index = 1; index <= Posts; ++index) {
            PostOne(encoder, table, commands, index);
            BusyFor(std::chrono::microseconds(30));
        }
        const auto start = std::chrono::steady_clock::now();
        while (Recorded(commands) <= Posts - LongestLag && std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Require(Recorded(commands) > Posts - LongestLag, "the parked encoder recorded " + std::to_string(Recorded(commands) - 1) + " of " + std::to_string(Posts) + " chunks posted after it parked without a wait, leaving at least " + std::to_string(LongestLag) + " behind");
        encoder.Quiesce();
    }
    Require(Recorded(commands) == Posts + 1, "the encoder recorded " + std::to_string(Recorded(commands)) + " of " + std::to_string(Posts + 1) + " commands after the final wait");
}

void ParkedEncoderWakesForEveryWait() {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x55);
    const auto commands = Handle<VkCommandBuffer>(0x1800);
    std::uint32_t posts = 0;
    {
        const auto attached = Attached(device, table);
        auto& encoder = *attached;
        const auto parkOne = [&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            PostOne(encoder, table, commands, posts++);
        };
        parkOne();
        encoder.Quiesce();
        Require(Recorded(commands) == posts, "an owner wait on a parked encoder returned before its chunk was recorded");
        parkOne();
        std::thread foreign([&] { AgcDriver::Graphics::BeforeCommandLookup(device); });
        foreign.join();
        Require(Recorded(commands) == posts, "a foreign lookup on a parked encoder returned before the posted chunk was recorded");
        parkOne();
    }
    Require(Recorded(commands) == posts, "destroying a parked encoder left " + std::to_string(posts - Recorded(commands)) + " posted chunks unrecorded");
}

void SparsePostsAndWaitsNeverLoseAWake() {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x54);
    const auto commands = Handle<VkCommandBuffer>(0x1700);
    std::atomic<std::uint64_t> progress{0};
    std::atomic<bool> finished{false};
    std::thread watchdog([&] {
        auto seen = progress.load();
        auto since = std::chrono::steady_clock::now();
        while (!finished.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const auto now = progress.load();
            if (now != seen) {
                seen = now;
                since = std::chrono::steady_clock::now();
            } else if (std::chrono::steady_clock::now() - since > std::chrono::seconds(20)) {
                std::cerr << "a wait on the encoder made no progress for 20 s after " << now << " steps: a wake-up was lost\n";
                std::_Exit(1);
            }
        }
    });
    std::vector<std::string> expected;
    std::string failure;
    {
        const auto attached = Attached(device, table);
        auto& encoder = *attached;
        std::atomic<bool> stop{false};
        std::thread foreign([&] {
            std::mt19937 random(11);
            while (!stop.load()) {
                AgcDriver::Graphics::BeforeCommandLookup(device);
                progress.fetch_add(1);
                if (random() % 2 == 0) std::this_thread::yield();
                else BusyFor(std::chrono::microseconds(random() % 50));
            }
        });
        std::mt19937 random(7);
        for (std::uint32_t index = 0; index < 6000; ++index) {
            PostOne(encoder, table, commands, index);
            expected.push_back("draw " + std::to_string(index) + " 1 0 0");
            const auto pause = random() % 4;
            if (pause == 1) std::this_thread::yield();
            if (pause == 2) BusyFor(std::chrono::microseconds(random() % 40));
            if (pause == 3) std::this_thread::sleep_for(std::chrono::microseconds(random() % 100));
            if (random() % 8 == 0) {
                encoder.Quiesce();
                if (Recorded(commands) != index + 1) {
                    failure = "a wait returned with " + std::to_string(Recorded(commands)) + " of " + std::to_string(index + 1) + " posted commands recorded";
                    break;
                }
            }
            progress.fetch_add(1);
        }
        encoder.Quiesce();
        stop.store(true);
        foreign.join();
    }
    finished.store(true);
    watchdog.join();
    Require(failure.empty(), failure);
    Compare({{commands, expected}}, "sparse posts with owner and foreign waits");
}

std::uint64_t PostsBy(const CommandEncoder::Statistics& counts, CommandEncoder::PostCause cause) {
    return counts.postsBy[static_cast<std::size_t>(cause)];
}

void ChunkPostingBatchesWindows() {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x56);
    const auto commands = Handle<VkCommandBuffer>(0x1900);
    constexpr std::uint32_t Windows = 200;
    std::vector<std::string> expected;
    {
        const auto attached = Attached(device, table);
        auto& encoder = *attached;
        for (std::uint32_t index = 0; index < Windows; ++index) {
            PostOne(encoder, table, commands, index);
            expected.push_back("draw " + std::to_string(index) + " 1 0 0");
        }
        const auto parked = encoder.Counts();
        Require(Recorded(commands) == 0 && parked.posts == 0, "a closed window was posted while posting by chunks: " + std::to_string(parked.posts) + " posts, " + std::to_string(Recorded(commands)) + " commands recorded");
        encoder.FinishBatch();
        const auto counts = encoder.Counts();
        Require(counts.posts == 1 && counts.windows == Windows && PostsBy(counts, CommandEncoder::PostCause::BatchEnd) == 1, "posting by chunks took " + std::to_string(counts.posts) + " posts for " + std::to_string(counts.windows) + " of " + std::to_string(Windows) + " windows instead of one at the batch end");
    }
    Compare({{commands, expected}}, "windows posted by chunks");
}

void ForeignFlushesKeepTheOwnersOrder() {
    Reset();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x57);
    const auto commands = Handle<VkCommandBuffer>(0x1a00);
    std::vector<std::string> expected;
    std::uint64_t foreignPosts = 0;
    {
        const auto attached = Attached(device, table);
        auto& encoder = *attached;
        std::atomic<bool> stop{false};
        std::thread foreign([&] {
            while (!stop.load(std::memory_order_relaxed)) AgcDriver::Graphics::BeforeCommandLookup(device);
        });
        std::mt19937 random(13);
        for (std::uint32_t index = 0; index < 20000; ++index) {
            PostOne(encoder, table, commands, index);
            expected.push_back("draw " + std::to_string(index) + " 1 0 0");
            if (random() % 64 == 0) encoder.Quiesce();
        }
        stop.store(true, std::memory_order_relaxed);
        foreign.join();
        encoder.Quiesce();
        foreignPosts = PostsBy(encoder.Counts(), CommandEncoder::PostCause::Foreign);
    }
    Compare({{commands, expected}}, "parked chunks flushed by a foreign thread");
    Require(foreignPosts != 0, "no foreign thread flushed a parked chunk, so the race was not exercised");
}

using SubmitState = CommandEncoder::SubmitRequest::State;
using Settled = std::vector<std::pair<std::uint64_t, SubmitState>>;

std::vector<std::uint64_t> submittedSerials;
std::atomic<VkResult> fakeSubmitResult{VK_SUCCESS};

VKAPI_ATTR VkResult VKAPI_CALL FakeEndCommandBuffer(VkCommandBuffer commands) {
    Log(commands, "end");
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL FakeQueueSubmit(VkQueue, std::uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    const auto result = fakeSubmitResult.load();
    if (result != VK_SUCCESS || count != 1) return result;
    const auto& submit = submits[0];
    const auto* timeline = static_cast<const VkTimelineSemaphoreSubmitInfoKHR*>(submit.pNext);
    const auto serial = timeline != nullptr && timeline->signalSemaphoreValueCount == 1 ? timeline->pSignalSemaphoreValues[0] : 0;
    Log(submit.pCommandBuffers[0], "submit " + std::to_string(serial) + " fence " + std::to_string(Raw(fence)));
    std::lock_guard lock(logMutex);
    submittedSerials.push_back(serial);
    return VK_SUCCESS;
}

void NoteSettled(void* owner, const CommandEncoder::SubmitRequest& request, SubmitState state) noexcept {
    std::lock_guard lock(logMutex);
    static_cast<Settled*>(owner)->push_back({request.serial, state});
}

std::unique_ptr<CommandEncoder::SubmitRequest> Request(VkCommandBuffer commands, std::uint64_t serial, Settled& settled, AgcDriver::Mutex* queueMutex = nullptr) {
    auto request = std::make_unique<CommandEncoder::SubmitRequest>();
    request->commands = commands;
    request->queue = Handle<VkQueue>(0x77);
    request->fence = Handle<VkFence>(0x9000 + serial);
    request->timeline = Handle<VkSemaphore>(0x55);
    request->serial = serial;
    request->endCommandBuffer = &FakeEndCommandBuffer;
    request->queueSubmit = &FakeQueueSubmit;
    request->queueMutex = queueMutex;
    request->recording = &RecordingOf(commands);
    request->settled = &NoteSettled;
    request->owner = &settled;
    return request;
}

std::string SubmitLine(std::uint64_t serial) {
    return "submit " + std::to_string(serial) + " fence " + std::to_string(0x9000 + serial);
}

void ResetSubmits() {
    Reset();
    std::lock_guard lock(logMutex);
    submittedSerials.clear();
}

std::vector<std::uint64_t> Submitted() {
    std::lock_guard lock(logMutex);
    return submittedSerials;
}

void AsyncSubmitsFollowTheirChunks(std::uint32_t seed, bool delays) {
    ResetSubmits();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x58);
    if (delays) {
        CommandEncoder::SetDelayForTesting([seed]() mutable {
            thread_local std::mt19937 delay(seed * 11 + 3);
            if (delay() % 3 == 0) std::this_thread::sleep_for(std::chrono::microseconds(delay() % 100));
        });
    }
    constexpr std::uint64_t Batches = 300;
    std::map<VkCommandBuffer, std::vector<std::string>> expected;
    std::vector<std::unique_ptr<CommandEncoder::SubmitRequest>> requests;
    Settled settled;
    std::uint64_t counted = 0;
    {
        const auto attached = Attached(device, table);
        auto& encoder = *attached;
        std::atomic<bool> stop{false};
        std::thread foreign([&] {
            for (std::uint32_t round = 0; !stop.load(std::memory_order_relaxed); ++round) {
                AgcDriver::Graphics::BeforeCommandLookup(device);
                if (round % 8 == 0) AgcDriver::Graphics::SettleQueueSubmits(device, false);
            }
        });
        std::mt19937 random(seed);
        for (std::uint64_t batch = 1; batch <= Batches; ++batch) {
            const auto commands = Handle<VkCommandBuffer>(0x3000 + batch * 0x10);
            const auto windows = random() % 6;
            for (std::uint32_t window = 0; window < windows; ++window) {
                encoder.Open(commands, RecordingOf(commands));
                for (std::uint32_t draw = random() % 4 + 1; draw != 0; --draw) {
                    const auto vertices = static_cast<std::uint32_t>(batch * 100 + window * 10 + draw);
                    table.cmdDraw(commands, vertices, 1, 0, 0);
                    expected[commands].push_back("draw " + std::to_string(vertices) + " 1 0 0");
                }
                encoder.Close();
            }
            requests.push_back(Request(commands, batch, settled));
            encoder.PostSubmit(*requests.back());
            expected[commands].push_back("end");
            expected[commands].push_back(SubmitLine(batch));
            if (random() % 16 == 0) {
                encoder.SettleSubmits();
                for (const auto& request : requests) Require(request->Current() == SubmitState::Submitted, "a settle returned before submit " + std::to_string(request->serial) + " reached the queue");
            }
        }
        stop.store(true, std::memory_order_relaxed);
        foreign.join();
        encoder.SettleSubmits();
        counted = encoder.Counts().submits;
    }
    CommandEncoder::SetDelayForTesting({});
    Compare(expected, delays ? "async submits with encoder delays" : "async submits");
    std::vector<std::uint64_t> order(Batches);
    for (std::uint64_t index = 0; index < Batches; ++index) order[index] = index + 1;
    Require(Submitted() == order, "the encoder submitted the batches out of their posted order");
    Require(counted == Batches, "the encoder counted " + std::to_string(counted) + " async submits for " + std::to_string(Batches) + " batches");
    Require(settled.size() == Batches, "the owner heard of " + std::to_string(settled.size()) + " settled submits, not " + std::to_string(Batches));
    for (std::uint64_t index = 0; index < Batches; ++index) Require(settled[index] == std::make_pair(index + 1, SubmitState::Submitted), "the owner heard of submit " + std::to_string(settled[index].first) + " out of order or as not submitted");
}

enum class SubmitFailure { Injected, DeviceLost, Replay };

void AFailedSubmitIsSticky(SubmitFailure failure) {
    ResetSubmits();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x59);
    const bool deviceLost = failure == SubmitFailure::DeviceLost;
    std::vector<std::unique_ptr<CommandEncoder::SubmitRequest>> requests;
    Settled settled;
    std::uint64_t failures = 0;
    std::vector<VkCommandBuffer> buffers;
    std::atomic<bool> failReplay{false};
    CommandEncoder::SetDelayForTesting([&] {
        if (failReplay.exchange(false)) throw std::runtime_error("injected replay failure");
    });
    {
        const auto attached = Attached(device, table);
        auto& encoder = *attached;
        for (std::uint64_t batch = 1; batch <= 6; ++batch) {
            const auto commands = Handle<VkCommandBuffer>(0x4000 + batch * 0x10);
            buffers.push_back(commands);
            if (batch == 3) {
                encoder.SettleSubmits();
                if (failure == SubmitFailure::DeviceLost) fakeSubmitResult.store(VK_ERROR_DEVICE_LOST);
                if (failure == SubmitFailure::Replay) failReplay.store(true);
            }
            PostOne(encoder, table, commands, static_cast<std::uint32_t>(batch));
            requests.push_back(Request(commands, batch, settled));
            if (batch == 3 && failure == SubmitFailure::Injected) requests.back()->injectFailure = VK_ERROR_UNKNOWN;
            encoder.PostSubmit(*requests.back());
        }
        encoder.SettleSubmits();
        fakeSubmitResult.store(VK_SUCCESS);
        failures = encoder.Counts().submitFailures;
    }
    CommandEncoder::SetDelayForTesting({});
    const std::string scenario = deviceLost ? "a lost device" : failure == SubmitFailure::Replay ? "a replay failure" : "an injected failure";
    const SubmitState states[6]{SubmitState::Submitted, SubmitState::Submitted, SubmitState::Failed, SubmitState::NotSubmitted, SubmitState::NotSubmitted, SubmitState::NotSubmitted};
    for (std::size_t index = 0; index < 6; ++index) {
        Require(requests[index]->Current() == states[index], "after " + scenario + " on submit 3, submit " + std::to_string(index + 1) + " ended in state " + std::to_string(static_cast<int>(requests[index]->Current())));
        Require(settled[index] == std::make_pair(static_cast<std::uint64_t>(index + 1), states[index]), "after " + scenario + ", the owner heard of submit " + std::to_string(settled[index].first) + " in the wrong order or state");
    }
    if (deviceLost) Require(requests[2]->Result() == VK_ERROR_DEVICE_LOST, "a lost device was not kept as the failed submit's result");
    else {
        std::string kept;
        try {
            if (requests[2]->Error() != nullptr) std::rethrow_exception(requests[2]->Error());
        } catch (const std::runtime_error& error) {
            kept = error.what();
        }
        const std::string expected = failure == SubmitFailure::Replay ? "injected replay failure" : "vkQueueSubmit recorder: injected failure";
        Require(kept == expected, scenario + " was kept as '" + kept + "', not as the failed submit's error");
    }
    Require(Submitted() == std::vector<std::uint64_t>{1, 2}, "after " + scenario + ", a submit after the failed one reached the queue");
    if (failure == SubmitFailure::Replay) Require(Recorded(buffers[2]) == 0, "a batch whose replay failed was ended");
    for (std::size_t index = 3; index < 6; ++index) Require(Recorded(buffers[index]) == 1, "a batch after " + scenario + " was ended though it is never submitted");
    Require(failures == 4, "the encoder counted " + std::to_string(failures) + " failed submits, not the failed one and the three after it");
}

void SettleOnTheEncoderThreadIsRefused() {
    ResetSubmits();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x5a);
    const auto commands = Handle<VkCommandBuffer>(0x5000);
    AgcDriver::Mutex main;
    AgcDriver::Graphics::Context context{};
    context.device = device;
    context.mainQueueMutex = &main;
    Settled settled;
    std::atomic<bool> posted{false};
    std::atomic<bool> ran{false};
    std::atomic<bool> settleRefused{false};
    std::atomic<bool> accessRefused{false};
    CommandEncoder* running = nullptr;
    CommandEncoder::SetDelayForTesting([&] {
        if (ran.exchange(true)) return;
        while (!posted.load()) std::this_thread::yield();
        try {
            running->SettleSubmits();
        } catch (const std::logic_error&) {
            settleRefused.store(true);
        }
        try {
            const AgcDriver::Graphics::QueueAccess access(context);
        } catch (const std::logic_error&) {
            accessRefused.store(true);
        }
    });
    {
        const auto attached = Attached(device, table);
        running = attached.get();
        PostOne(*attached, table, commands, 1);
        const auto request = Request(commands, 1, settled, &main);
        attached->PostSubmit(*request);
        posted.store(true);
        attached->SettleSubmits();
        Require(request->Current() == SubmitState::Submitted, "the submit after a refused settle did not reach the queue");
    }
    CommandEncoder::SetDelayForTesting({});
    Require(ran.load() && settleRefused.load(), "the encoder thread settled its own submits instead of refusing");
    Require(accessRefused.load(), "the encoder thread took queue access while its own submit was posted");
}

void TeardownLeavesNoSubmitPending() {
    ResetSubmits();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x5b);
    std::vector<std::unique_ptr<CommandEncoder::SubmitRequest>> requests;
    Settled settled;
    CommandEncoder::SetDelayForTesting([] { std::this_thread::sleep_for(std::chrono::milliseconds(2)); });
    {
        const auto attached = Attached(device, table);
        for (std::uint64_t batch = 1; batch <= 8; ++batch) {
            const auto commands = Handle<VkCommandBuffer>(0x6000 + batch * 0x10);
            PostOne(*attached, table, commands, static_cast<std::uint32_t>(batch));
            requests.push_back(Request(commands, batch, settled));
            attached->PostSubmit(*requests.back());
        }
    }
    CommandEncoder::SetDelayForTesting({});
    std::size_t notSubmitted = 0;
    for (const auto& request : requests) {
        const auto state = request->Current();
        Require(state == SubmitState::Submitted || state == SubmitState::NotSubmitted, "a submit posted before the teardown ended in state " + std::to_string(static_cast<int>(state)));
        if (state == SubmitState::NotSubmitted) ++notSubmitted;
        else Require(notSubmitted == 0, "a submit was made at teardown after an earlier one was refused");
    }
    Require(notSubmitted != 0, "every submit posted before the teardown was made, so the teardown was not exercised");
    Require(settled.size() == requests.size(), "the owner heard of " + std::to_string(settled.size()) + " of the " + std::to_string(requests.size()) + " submits posted before the teardown");
    Require(Submitted().size() == requests.size() - notSubmitted, "a submit refused at teardown reached the queue");
}

void QueueAccessSettlesPostedSubmits() {
    ResetSubmits();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x5c);
    AgcDriver::Mutex main;
    AgcDriver::Graphics::Context context{};
    context.device = device;
    context.mainQueueMutex = &main;
    Settled settled;
    CommandEncoder::SetDelayForTesting([] { std::this_thread::sleep_for(std::chrono::milliseconds(5)); });
    {
        const auto attached = Attached(device, table);
        auto& encoder = *attached;
        const auto first = Handle<VkCommandBuffer>(0x7000);
        PostOne(encoder, table, first, 1);
        const auto presented = Request(first, 1, settled, &main);
        encoder.PostSubmit(*presented);
        {
            const AgcDriver::Graphics::QueueAccess access(context, AgcDriver::Graphics::QueueAccess::Queues::Main, AgcDriver::Graphics::QueueAccess::Settle::Present);
            Require(presented->Current() == SubmitState::Submitted, "queue access took the main queue before a posted submit reached it");
        }
        const auto second = Handle<VkCommandBuffer>(0x7100);
        PostOne(encoder, table, second, 2);
        const auto other = Request(second, 2, settled, &main);
        encoder.PostSubmit(*other);
        {
            const AgcDriver::Graphics::QueueAccess access(context, AgcDriver::Graphics::QueueAccess::Queues::All);
            Require(other->Current() == SubmitState::Submitted, "access to every queue was taken before a posted submit reached the main queue");
        }
        const auto counts = encoder.Counts();
        Require(counts.presentSettleWaits == 1 && counts.settleWaits == 1, "the settles were counted as " + std::to_string(counts.presentSettleWaits) + " present and " + std::to_string(counts.settleWaits) + " other waits, not one of each");
        CommandEncoder::SetDelayForTesting({});
        const auto third = Handle<VkCommandBuffer>(0x7200);
        PostOne(encoder, table, third, 3);
        const auto held = Request(third, 3, settled, &main);
        {
            std::unique_lock hold(main);
            encoder.PostSubmit(*held);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            Require(held->Current() == SubmitState::Pending && Submitted().size() == 2, "the encoder submitted while another thread held the main queue");
        }
        Require(held->Wait() == SubmitState::Submitted && Submitted().size() == 3, "a submit held back by the main queue never reached it");
        const auto idle = encoder.Counts();
        {
            const AgcDriver::Graphics::QueueAccess access(context);
        }
        Require(encoder.Counts().settleWaits == idle.settleWaits, "queue access waited with no submit posted");
    }
    CommandEncoder::SetDelayForTesting({});
}

void NextBatchDoesNotWaitForTheSubmit() {
    ResetSubmits();
    auto table = FakeTable();
    const auto device = Handle<VkDevice>(0x5d);
    Settled settled;
    std::atomic<bool> gate{false};
    std::atomic<bool> held{false};
    CommandEncoder::SetDelayForTesting([&] {
        if (held.exchange(true)) return;
        while (!gate.load()) std::this_thread::yield();
    });
    std::chrono::steady_clock::duration opened{};
    SubmitState stateAfterOpen = SubmitState::Submitted;
    {
        const auto attached = Attached(device, table);
        auto& encoder = *attached;
        const auto first = Handle<VkCommandBuffer>(0x8000);
        const auto second = Handle<VkCommandBuffer>(0x8100);
        PostOne(encoder, table, first, 1);
        const auto request = Request(first, 1, settled);
        encoder.PostSubmit(*request);
        std::thread release([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            gate.store(true);
        });
        const auto start = std::chrono::steady_clock::now();
        PostOne(encoder, table, second, 2);
        opened = std::chrono::steady_clock::now() - start;
        stateAfterOpen = request->Current();
        release.join();
        encoder.SettleSubmits();
        encoder.Quiesce();
    }
    CommandEncoder::SetDelayForTesting({});
    Require(stateAfterOpen == SubmitState::Pending && opened < std::chrono::milliseconds(250), "the next batch's window waited " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(opened).count()) + " ms for the previous batch's posted submit");
    Compare({{Handle<VkCommandBuffer>(0x8000), {"draw 1 1 0 0", "end", SubmitLine(1)}}, {Handle<VkCommandBuffer>(0x8100), {"draw 2 1 0 0"}}}, "a window on the next batch while the previous submit is posted");
}

void RunAsyncSubmit() {
    NextBatchDoesNotWaitForTheSubmit();
    for (std::uint32_t seed = 1; seed <= 4; ++seed) AsyncSubmitsFollowTheirChunks(seed, seed % 2 == 0);
    AFailedSubmitIsSticky(SubmitFailure::Injected);
    AFailedSubmitIsSticky(SubmitFailure::DeviceLost);
    AFailedSubmitIsSticky(SubmitFailure::Replay);
    SettleOnTheEncoderThreadIsRefused();
    TeardownLeavesNoSubmitPending();
    QueueAccessSettlesPostedSubmits();
}

void QueueAccessLocksTheQueuesItCovers() {
    Require(AgcDriver::Graphics::QueueAccessEnabled(), "APS5_QUEUE_ACCESS did not enable queue access");
    AgcDriver::Mutex main;
    std::mutex sparse;
    std::mutex settle;
    std::mutex present;
    AgcDriver::Graphics::Context context{};
    context.mainQueueMutex = &main;
    context.sparseQueueMutex = &sparse;
    context.settleQueueMutex = &settle;
    context.presentQueueMutex = &present;
    const auto held = [&](auto& mutex) {
        std::atomic<bool> locked{false};
        std::thread probe([&] {
            if (mutex.try_lock()) {
                mutex.unlock();
                return;
            }
            locked.store(true);
        });
        probe.join();
        return locked.load();
    };
    {
        const AgcDriver::Graphics::QueueAccess access(context);
        Require(held(main) && !held(sparse) && !held(settle) && !held(present), "main queue access did not hold exactly the main queue");
    }
    {
        const AgcDriver::Graphics::QueueAccess access(context, AgcDriver::Graphics::QueueAccess::Queues::All);
        Require(held(main) && held(sparse) && held(settle) && held(present), "access to every queue left a queue unlocked");
    }
    Require(!held(main) && !held(sparse) && !held(settle) && !held(present), "queue access kept a queue locked after it ended");
}

void RunAll(bool chunks) {
    AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::EncoderPostChunks, chunks);
    for (std::uint32_t seed = 1; seed <= 6; ++seed) RandomProgramRecordsWhatInlineRecords(seed, seed % 2 == 0);
    OtherThreadsRecordDirectlyWhileAWindowIsOpen();
    HookDuringUnencodedOpsWaits();
    EveryCommandOfTheTableIsInterposed();
    NestedWindowKeepsOrder();
    LargeRecordsSpanChunks();
    TransfersAndQueriesInAWindowNeverWait();
    PackedRecordsSkipAFailedRecording();
    QueriesKeepOrderWithTheirRecording();
    UnwindingSettlesQueuedRecording();
    OwnersOutliveTheUnwindDrain();
    ForeignThreadCannotProduceIntoAnOpenWindow();
    ForeignLookupWhileAWindowIsOpenProceeds();
    RawCallAfterLookupKeepsOrder();
    FailedReplayStaysWithItsRecording();
    FinishCarriesAnUnreportedFailure();
    DirectCallsSkipAFailedRecording();
    AppendFailureKeepsTheWindow();
    DevicesPastTheSlotsRecordInline();
    PostingNeverAllocates();
    ReplayFailureNeedsNoAllocation();
    EncoderSleepsBetweenFrequentPosts();
    if (!chunks) EncoderKeepsUpWithoutAWait();
    ParkedEncoderWakesForEveryWait();
    SparsePostsAndWaitsNeverLoseAWake();
    if (chunks) {
    ChunkPostingBatchesWindows();
    ForeignFlushesKeepTheOwnersOrder();
    }
    RunAsyncSubmit();
}

int main() {
    try {
#ifdef _WIN32
        _putenv_s("APS5_QUEUE_ACCESS", "1");
#else
        setenv("APS5_QUEUE_ACCESS", "1", 1);
#endif
        QueueAccessLocksTheQueuesItCovers();
        RunAll(false);
        RunAll(true);
        std::cout << "command encoder tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
