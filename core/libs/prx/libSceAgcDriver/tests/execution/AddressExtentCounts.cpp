#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::AddressExtentTotalForTests;
using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::ShaderReadMemory;
using AgcDriver::Graphics::ShaderReadPath;
using AgcDriver::Graphics::ShaderReadTotal;
using AgcDriver::Graphics::ShaderReadTotalForTests;
using AgcDriver::Graphics::ShaderReadUse;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 64;
constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t InputDwords = 1024;
constexpr std::uint32_t OutputBase = 0x8000;

alignas(256) constexpr std::array<std::uint32_t, 33> Code{
    0x34040082, 0x34060085, 0x4a0606ff, OutputBase,
    0xdc308010, 0x04000002, 0xdc308014, 0x05000002, 0xdc308018, 0x06000002,
    0xdc308403, 0x07000002,
    0xf4000100, 0xfa000203, 0xf4000140, 0xfa0002f6,
    0xbf8c3f70, 0xbf8cc07f, 0x7e100204, 0x7e120205,
    0xdc708000, 0x00000403, 0xdc708004, 0x00000503, 0xdc708008, 0x00000603, 0xdc70800c, 0x00000703,
    0xdc708010, 0x00000803, 0xdc708014, 0x00000903, 0xbf810000};

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "address extent counts: cannot allocate the guest block");
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

    std::uint8_t* Data() { return block; }

private:
    std::uint8_t* block = nullptr;
};

ShaderRecompiler::RecompileResult Recompile(std::uint32_t waveSize, std::span<const std::uint32_t> userData, std::span<const std::byte> guest, const ShaderRecompiler::SpirvTarget& target) {
    const std::span<const std::uint32_t> code(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 2> regions{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}, {reinterpret_cast<std::uintptr_t>(guest.data()), guest}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {waveSize, 0, userData, compute, std::nullopt, std::nullopt, regions},
        target,
        {0, 0, 0, 128}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

struct Totals {
    ShaderReadTotal bound;
    ShaderReadTotal extent;
};

Totals Now() {
    Totals totals;
    for (std::uint8_t memory = 0; memory < static_cast<std::uint8_t>(ShaderReadMemory::Count); ++memory) {
        const auto bound = ShaderReadTotalForTests(ShaderReadUse::Dispatch, ShaderReadPath::Address, static_cast<ShaderReadMemory>(memory));
        const auto extent = AddressExtentTotalForTests(ShaderReadUse::Dispatch, static_cast<ShaderReadMemory>(memory));
        totals.bound = {totals.bound.bytes + bound.bytes, totals.bound.bindings + bound.bindings};
        totals.extent = {totals.extent.bytes + extent.bytes, totals.extent.bindings + extent.bindings};
    }
    return totals;
}

Totals Since(const Totals& before) {
    const auto now = Now();
    return {{now.bound.bytes - before.bound.bytes, now.bound.bindings - before.bound.bindings}, {now.extent.bytes - before.extent.bytes, now.extent.bindings - before.extent.bindings}};
}

std::string Text(const ShaderReadTotal& total) {
    return std::to_string(total.bytes) + " bytes in " + std::to_string(total.bindings) + " ranges";
}

void SetEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    Require(_putenv_s(name, value) == 0, std::string("address extent counts: cannot set ") + name);
#else
    Require(setenv(name, value, 1) == 0, std::string("address extent counts: cannot set ") + name);
#endif
}

}

int main() {
    try {
        SetEnvironment("APS5_PROFILE_DRAW", "1");
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Require(AgcDriver::Graphics::ShaderReadsCounted(), "address extent counts: APS5_PROFILE_DRAW does not count shader reads");
        GuestBlock guest;
        std::vector<std::uint32_t> memory(BlockBytes / 4u, 0u);
        for (std::uint32_t dword = 0; dword < InputDwords; ++dword) memory[dword] = 0x61000000u + dword;
        std::memcpy(guest.Data(), memory.data(), BlockBytes);
        const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(guest.Data()));
        const std::vector<std::uint32_t> userData{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u)};
        const auto shader = Recompile(64, userData, std::span<const std::byte>(reinterpret_cast<const std::byte*>(guest.Data()), InputDwords * sizeof(std::uint32_t)), device->Target());
        const auto dispatch = [&] {
            const auto before = Now();
            device->Dispatch(shader, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(Code.data()));
            device->WaitIdle();
            return Since(before);
        };
        AgcDriver::Graphics::NoteHostImportFrame();
        const auto first = dispatch();
        if (first.bound.bindings == 0) {
            std::puts("skipped, the dispatch bound no address space on this device");
            return VulkanTestSkipped;
        }
        Require(first.extent.bindings != 0 && first.extent.bytes != 0, "address extent counts: the first build of a frame bound " + Text(first.bound) + " through addresses but counted no extent");
        Require(first.extent.bytes <= first.bound.bytes && first.extent.bindings <= first.bound.bindings, "address extent counts: the first build counted an extent of " + Text(first.extent) + ", more than the " + Text(first.bound) + " it bound");
        const auto second = dispatch();
        Require(second.bound.bytes == first.bound.bytes && second.bound.bindings == first.bound.bindings, "address extent counts: a second build of the same dispatch bound " + Text(second.bound) + ", the first " + Text(first.bound));
        Require(second.extent.bytes == 0 && second.extent.bindings == 0, "address extent counts: a second build in the same frame counted " + Text(second.extent) + " of extent again");
        AgcDriver::Graphics::NoteHostImportFrame();
        const auto next = dispatch();
        Require(next.extent.bytes == first.extent.bytes && next.extent.bindings == first.extent.bindings, "address extent counts: the same build in the next frame counted " + Text(next.extent) + ", the first frame " + Text(first.extent));
        std::printf("address extent counts: one build bound %s through addresses; its extent of %s was counted once a frame\n", Text(first.bound).c_str(), Text(first.extent).c_str());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
