#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "PipelineSpecialization.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <chrono>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using AgcDriver::Graphics::Recorder;
using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Lanes = 64;
constexpr std::uint32_t BufferBytes = Lanes * 4u;
constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t Iterations = 1u << 23u;

alignas(256) constexpr std::array<std::uint32_t, 13> SpinThenStore{0x34020082u, 0xbe9403ffu, Iterations, 0x7e060300u, 0xd5690003u, 0x00020703u, 0x4a060681u, 0x80948114u, 0xbf078014u, 0xbf85fffau, 0xe0701000u, 0x80000301u, 0xbf810000u};

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint32_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint32_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "store inside a written element: cannot allocate a guest block");
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

    std::uint32_t* Words() const { return block; }
    std::uint64_t Address(std::size_t offset = 0) const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block)) + offset; }

private:
    std::uint32_t* block = nullptr;
};

std::array<std::uint32_t, 4> Descriptor(std::uint64_t address, std::uint32_t bytes) {
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

ShaderRecompiler::RecompileResult Compile(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, const std::array<std::uint32_t, 4>& first, const std::array<std::uint32_t, 4>& second) {
    std::vector<std::uint32_t> userData(8, 0u);
    std::copy(first.begin(), first.end(), userData.begin());
    std::copy(second.begin(), second.end(), userData.begin() + 4);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {64, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

std::uint32_t Spun(std::uint32_t lane) {
    std::uint32_t value = lane;
    for (std::uint32_t iteration = 0; iteration < Iterations; ++iteration) value = value * value + 1u;
    return value;
}

void StoreDuringWork(AgcDriver::VulkanDevice& device, bool handover) {
    const GuestBlock block;
    const auto spin = Compile(device, SpinThenStore, Descriptor(block.Address(), BufferBytes * 2u), {});
    for (std::uint32_t word = 0; word < Lanes * 2u; ++word) block.Words()[word] = 100u + word;
    const auto before = Recorder::HandoverCounts();
    device.Dispatch(spin, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(SpinThenStore.data()));
    device.SubmitRecorded();
    const auto submitted = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) block.Words()[Lanes + lane] = 700u + lane;
    const auto stored = std::chrono::steady_clock::now();
    device.WaitIdle();
    const auto idle = std::chrono::steady_clock::now();
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
        const auto expected = Spun(lane);
        Require(block.Words()[lane] == expected, "store inside a written element: lane " + std::to_string(lane) + " of the shader's half is " + std::to_string(block.Words()[lane]) + ", expected " + std::to_string(expected));
    }
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
        const auto expected = 700u + lane;
        Require(block.Words()[Lanes + lane] == expected, "store inside a written element: word " + std::to_string(lane) + " of the half the shader leaves alone is " + std::to_string(block.Words()[Lanes + lane]) + " after the CPU stored " + std::to_string(expected) + " into it while the dispatch ran");
    }
    const auto after = Recorder::HandoverCounts();
    const auto milliseconds = [](auto from, auto to) { return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count(); };
    std::printf("store inside a written element: the CPU stored %lld ms after the submit and the work was idle %lld ms after the store; %llu copy-backs landed through the handover%s\n", static_cast<long long>(milliseconds(submitted, stored)), static_cast<long long>(milliseconds(stored, idle)), static_cast<unsigned long long>(after.landed - before.landed), handover ? "" : " (handover off)");
}

void SetEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    Require(_putenv_s(name, value) == 0, std::string("store inside a written element: cannot set ") + name);
#else
    Require(setenv(name, value, 1) == 0, std::string("store inside a written element: cannot set ") + name);
#endif
}

}

int main(int argc, char** argv) {
    try {
        const bool off = argc > 1 && std::string(argv[1]) == "--off";
        SetEnvironment("APS5_WRITTEN_SHADOW_MIN_KIB", "0");
        SetEnvironment("APS5_NO_CHANGED_COPY_BACK", "1");
        if (off) SetEnvironment("APS5_NO_STAGING_HANDOVER", "1");
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        StoreDuringWork(*device, !off);
        StoreDuringWork(*device, !off);
        device->WaitIdle();
        std::puts("store inside a written element tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
