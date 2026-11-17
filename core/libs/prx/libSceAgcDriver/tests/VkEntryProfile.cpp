#include "prx/libSceAgcDriver/Graphics/include/VkEntryProfile.hpp"

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace AgcDriver::Graphics {

std::uint64_t& DeviceProcLookups() {
    static std::uint64_t count = 0;
    return count;
}

}

namespace {

using AgcDriver::ThreadVkEntryTally;
using AgcDriver::VkEntryCommands;
using AgcDriver::VkEntryDescriptors;
using AgcDriver::VkEntryOther;
using AgcDriver::VkEntryProfiled;
using AgcDriver::VkEntryShape;
using AgcDriver::VkEntryTally;
using AgcDriver::VkEntryUnwrappedSlots;
using AgcDriver::Graphics::Context;
using AgcDriver::Graphics::DeviceFunctions;
using AgcDriver::Graphics::UnwrapDeviceFunctions;
using AgcDriver::Graphics::WrapDeviceFunctions;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr auto FakeWork = std::chrono::microseconds(200);

void spin() {
    const auto until = std::chrono::steady_clock::now() + FakeWork;
    while (std::chrono::steady_clock::now() < until) {
    }
}

struct Seen {
    std::uint32_t drawVertices = 0;
    std::uint32_t drawInstances = 0;
    std::uint32_t descriptorWrites = 0;
    VkPipeline boundPipeline = VK_NULL_HANDLE;
    VkDeviceSize mappedBytes = 0;
    int otherDraws = 0;
};

Seen seen;

void VKAPI_CALL fakeDraw(VkCommandBuffer, std::uint32_t vertices, std::uint32_t instances, std::uint32_t, std::uint32_t) {
    seen.drawVertices = vertices;
    seen.drawInstances = instances;
    spin();
}

void VKAPI_CALL otherDraw(VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {
    ++seen.otherDraws;
}

void VKAPI_CALL fakeUpdateDescriptorSets(VkDevice, std::uint32_t writes, const VkWriteDescriptorSet*, std::uint32_t, const VkCopyDescriptorSet*) {
    seen.descriptorWrites = writes;
    spin();
}

void VKAPI_CALL fakeBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline pipeline) {
    seen.boundPipeline = pipeline;
}

VkResult VKAPI_CALL fakeMapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize bytes, VkMemoryMapFlags, void** mapped) {
    seen.mappedBytes = bytes;
    *mapped = nullptr;
    return VK_ERROR_MEMORY_MAP_FAILED;
}

VkEntryTally since(const VkEntryTally& before) {
    auto after = ThreadVkEntryTally();
    for (std::size_t kind = 0; kind < AgcDriver::VkEntryKindCount; ++kind) {
        after.nanoseconds[kind] -= before.nanoseconds[kind];
        after.calls[kind] -= before.calls[kind];
    }
    return after;
}

DeviceFunctions fakeTable() {
    DeviceFunctions functions;
    functions.cmdDraw = fakeDraw;
    functions.updateDescriptorSets = fakeUpdateDescriptorSets;
    functions.cmdBindPipeline = fakeBindPipeline;
    functions.mapMemory = fakeMapMemory;
    return functions;
}

void callTable(const DeviceFunctions& functions) {
    Context context{};
    context.functions = &functions;
    functions.cmdDraw(VK_NULL_HANDLE, 3, 2, 0, 0);
    functions.updateDescriptorSets(VK_NULL_HANDLE, 5, nullptr, 0, nullptr);
    context.BindComputePipeline(VK_NULL_HANDLE, reinterpret_cast<VkPipeline>(static_cast<std::uintptr_t>(0x40)));
    void* mapped = reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));
    Require(functions.mapMemory(VK_NULL_HANDLE, VK_NULL_HANDLE, 0, 4096, 0, &mapped) == VK_ERROR_MEMORY_MAP_FAILED && mapped == nullptr, "the map call did not return the driver's result and output");
    Require(seen.drawVertices == 3 && seen.drawInstances == 2 && seen.descriptorWrites == 5 && seen.mappedBytes == 4096 && seen.boundPipeline == reinterpret_cast<VkPipeline>(static_cast<std::uintptr_t>(0x40)), "a table entry did not reach the driver function with its arguments");
}

void tableIsLeftAloneWhenOff() {
    auto functions = fakeTable();
    WrapDeviceFunctions(functions);
    Require(functions.cmdDraw == fakeDraw && functions.updateDescriptorSets == fakeUpdateDescriptorSets && functions.mapMemory == fakeMapMemory, "the table was wrapped without APS5_PROFILE_VK_ENTRY");
    const auto before = ThreadVkEntryTally();
    callTable(functions);
    const auto counted = since(before);
    Require(counted.calls[VkEntryCommands] == 0 && counted.calls[VkEntryDescriptors] == 0 && counted.calls[VkEntryOther] == 0, "calls were counted without APS5_PROFILE_VK_ENTRY");
    std::cout << "vk entry off: table left as filled, 0 calls counted\n";
}

void tableIsTimedWhenOn() {
    auto functions = fakeTable();
    WrapDeviceFunctions(functions);
    Require(functions.cmdDraw != fakeDraw && functions.updateDescriptorSets != fakeUpdateDescriptorSets && functions.mapMemory != fakeMapMemory, "APS5_PROFILE_VK_ENTRY left a filled entry unwrapped");
    Require(functions.cmdCopyBuffer == nullptr && functions.cmdDrawIndexed == nullptr, "an empty entry was given a wrapper");
    const auto wrappedDraw = functions.cmdDraw;
    WrapDeviceFunctions(functions);
    Require(functions.cmdDraw == wrappedDraw, "wrapping a wrapped table wrapped it twice");
    const auto before = ThreadVkEntryTally();
    callTable(functions);
    const auto counted = since(before);
    const auto floor = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(FakeWork).count());
    Require(counted.calls[VkEntryCommands] == 2 && counted.calls[VkEntryDescriptors] == 1 && counted.calls[VkEntryOther] == 1, "the calls were not counted once each by kind: " + std::to_string(counted.calls[VkEntryCommands]) + " vkCmd, " + std::to_string(counted.calls[VkEntryDescriptors]) + " descriptors, " + std::to_string(counted.calls[VkEntryOther]) + " other");
    Require(counted.nanoseconds[VkEntryCommands] >= floor && counted.nanoseconds[VkEntryDescriptors] >= floor, "a driver call was timed shorter than it ran");
    std::cout << "vk entry on: vkCmd " << counted.calls[VkEntryCommands] << " calls " << counted.nanoseconds[VkEntryCommands] << " ns, descriptors " << counted.calls[VkEntryDescriptors] << " calls " << counted.nanoseconds[VkEntryDescriptors] << " ns, other " << counted.calls[VkEntryOther] << " calls\n";

    const auto unwrappedBefore = VkEntryUnwrappedSlots();
    auto second = fakeTable();
    WrapDeviceFunctions(second);
    Require(second.cmdDraw == wrappedDraw && VkEntryUnwrappedSlots() == unwrappedBefore, "a second table with the same driver functions was not wrapped");
    auto other = fakeTable();
    other.cmdDraw = otherDraw;
    WrapDeviceFunctions(other);
    Require(other.cmdDraw == otherDraw && VkEntryUnwrappedSlots() == unwrappedBefore + 1, "a table with a different driver function took the first table's wrapper or was not counted as unwrapped");
    const auto otherBefore = ThreadVkEntryTally();
    other.cmdDraw(VK_NULL_HANDLE, 1, 1, 0, 0);
    Require(seen.otherDraws == 1 && since(otherBefore).calls[VkEntryCommands] == 0, "the unwrapped entry was timed or did not reach its own driver");
    std::cout << "vk entry on: second device with other entry points left unwrapped (" << VkEntryUnwrappedSlots() - unwrappedBefore << " slot)\n";

    UnwrapDeviceFunctions(other);
    Require(other.cmdDraw == otherDraw, "releasing an untimed table changed its entry");
    UnwrapDeviceFunctions(functions);
    Require(functions.cmdDraw == fakeDraw && functions.updateDescriptorSets == fakeUpdateDescriptorSets && functions.mapMemory == fakeMapMemory, "releasing a table did not give back its driver functions");
    second.cmdDraw(VK_NULL_HANDLE, 7, 1, 0, 0);
    Require(seen.drawVertices == 7, "a table still sharing the slot lost its driver function when another table was released");
    UnwrapDeviceFunctions(second);
    Require(second.cmdDraw == fakeDraw, "releasing the last table did not give back its driver function");
    auto later = fakeTable();
    later.cmdDraw = otherDraw;
    const auto unwrappedLater = VkEntryUnwrappedSlots();
    WrapDeviceFunctions(later);
    Require(later.cmdDraw != otherDraw && VkEntryUnwrappedSlots() == unwrappedLater, "a device made after every earlier table was released was not wrapped");
    const auto laterBefore = ThreadVkEntryTally();
    later.cmdDraw(VK_NULL_HANDLE, 1, 1, 0, 0);
    Require(seen.otherDraws == 2 && since(laterBefore).calls[VkEntryCommands] == 1, "the later device's entry was not timed or did not reach its own driver");
    UnwrapDeviceFunctions(later);
    std::cout << "vk entry on: released slots taken by a later device with other entry points\n";
}

void lineSplitsEachPhase() {
    constexpr std::array<const char*, VkEntryShape::Phases> names{"validate", "vertex", "setup", "readTarget", "prepare", "lookup", "resources", "pipeline", "record", "keep", "sync", "writeBack", "describe"};
    VkEntryShape shape;
    std::array<double, VkEntryShape::Phases> us{};
    std::array<VkEntryTally, VkEntryShape::Phases> tallies{};
    us[8] = 6.0;
    tallies[8].nanoseconds[VkEntryCommands] = 1500;
    tallies[8].calls[VkEntryCommands] = 12;
    tallies[8].nanoseconds[VkEntryDescriptors] = 500;
    tallies[8].calls[VkEntryDescriptors] = 1;
    tallies[8].lookups = 2;
    us[5] = 3.0;
    shape.Note(us, tallies);
    tallies[8].lookups = 0;
    shape.Note(us, tallies);
    const auto report = shape.Take();
    Require(report.draws == 2 && shape.Take().draws == 0, "Take did not hand over and reset the window");
    const auto line = VkEntryShape::Line(report, names, 3);
    std::cout << line << '\n';
    const auto expect = [&](const std::string& part) { Require(line.find(part) != std::string::npos, "the line lacks '" + part + "': " + line); };
    expect("[vk-entry] 10 s: 2 draws, 9.00 us/draw = vk 2.00 + own 7.00; unwrapped slots 3;");
    expect(" lookup 3.00 = own 3.00;");
    expect(" record 6.00 = own 4.00 + vkCmd 1.50 (12.00 calls) + descriptors 0.50 (1.00 calls), 1.00 lookups;");
    Require(line.find(" validate") == std::string::npos, "a phase with no time was printed: " + line);
}

}

int main() {
    try {
        if (VkEntryProfiled()) tableIsTimedWhenOn();
        else tableIsLeftAloneWhenOff();
        lineSplitsEachPhase();
        std::cout << "vk entry profile tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
