#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 256;
constexpr std::uint32_t Height = 128;
constexpr std::size_t ImageBytes = Width * Height * 4u;
constexpr std::size_t MaxImages = 64;
constexpr std::size_t BlockBytes = ImageBytes * MaxImages;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Initial = 0x3f800000u;
constexpr std::uint32_t First = 0x11111111u;
constexpr std::uint32_t Second = 0x33333333u;
constexpr std::size_t Rounds = 8;

alignas(256) constexpr std::array<std::uint32_t, 11> FirstCode{0x7e000280u, 0x7e020280u, 0x7e0402ffu, First, 0xf0200108u, 0x00010200u, 0x7e0202ffu, 0x00000064u, 0xf0200108u, 0x00010200u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 11> SecondCode{0x7e000280u, 0x7e020280u, 0x7e0402ffu, Second, 0xf0200108u, 0x00010200u, 0x7e0202ffu, 0x00000064u, 0xf0200108u, 0x00010200u, 0xbf810000u};

class GuestBlock {
public:
    explicit GuestBlock(bool watched) : watched(watched) {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, 65536));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
#else
        constexpr std::uintptr_t alignment = 65536;
        void* mapped = mmap(nullptr, BlockBytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + BlockBytes != begin + BlockBytes + alignment) munmap(reinterpret_cast<void*>(aligned + BlockBytes), begin + alignment - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            if (watched) GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
        }
#endif
        Require(block != nullptr, "label store sweep: cannot allocate the guest block");
        std::fill(Words(), Words() + BlockBytes / 4u, Initial);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
        AgcDriver::GuestMemory::CollectWritesUncached(Address(), BlockBytes);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, BlockBytes);
#else
        munmap(block, BlockBytes);
        if (watched) GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, BlockBytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint32_t* Words() const { return reinterpret_cast<std::uint32_t*>(block); }
    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block)); }

private:
    bool watched;
    std::uint8_t* block = nullptr;
};

ShaderRecompiler::RecompileResult Compile(std::span<const std::uint32_t> code, std::span<const std::uint32_t> userData, const ShaderRecompiler::SpirvTarget& target) {
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{64u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {64u, 0, userData, compute, std::nullopt, std::nullopt, memory},
        target,
        {0, 0, 0, 128}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

std::vector<std::uint32_t> LinearImageUserData(std::uint64_t base) {
    std::vector<std::uint32_t> userData(12, 0u);
    userData[4] = static_cast<std::uint32_t>(base >> 8u);
    userData[5] = static_cast<std::uint32_t>((base >> 40u) & 0xffu) | (Format32UInt << 20u) | (((Width - 1u) & 3u) << 30u);
    userData[6] = ((Width - 1u) >> 2u) | ((Height - 1u) << 14u);
    userData[7] = 0xfacu | (Type2D << 28u);
    return userData;
}

double Sweep(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::size_t images, const std::vector<ShaderRecompiler::RecompileResult>& first, const std::vector<ShaderRecompiler::RecompileResult>& second) {
    std::vector<double> micros;
    for (std::size_t round = 0; round < Rounds; ++round) {
        const auto& programs = round % 2 == 0 ? first : second;
        const auto* code = round % 2 == 0 ? FirstCode.data() : SecondCode.data();
        for (std::size_t index = 0; index < images; ++index) device.Dispatch(programs[index], 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(code));
        const auto started = std::chrono::steady_clock::now();
        AgcDriver::Graphics::StorageTexture::StoreReadbackBeforeLabel("readback before label", VK_NULL_HANDLE);
        micros.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count());
        device.WaitIdle();
        const auto expected = round % 2 == 0 ? First : Second;
        for (std::size_t index = 0; index < images; ++index) {
            const auto* words = guest.Words() + index * ImageBytes / 4u;
            Require(words[0] == expected && words[100u * Width] == expected, "label store sweep: image " + std::to_string(index) + " of " + std::to_string(images) + " was not stored by the sweep in round " + std::to_string(round));
        }
    }
    std::sort(micros.begin(), micros.end());
    return micros[micros.size() / 2];
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        GuestBlock guest(AgcDriver::GuestMemory::WriteWatched());
        std::vector<ShaderRecompiler::RecompileResult> first, second;
        for (std::size_t index = 0; index < MaxImages; ++index) {
            const auto userData = LinearImageUserData(guest.Address() + index * ImageBytes);
            first.push_back(Compile(FirstCode, userData, device->Target()));
            second.push_back(Compile(SecondCode, userData, device->Target()));
        }
        for (const std::size_t images : {std::size_t{1}, std::size_t{4}, std::size_t{16}, MaxImages}) {
            const auto median = Sweep(*device, guest, images, first, second);
            std::printf("label store sweep: %zu pending linear images, median %.1f us per sweep (%.1f us per image) over %zu rounds\n", images, median, median / static_cast<double>(images), Rounds);
        }
        std::puts("label store sweep tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
