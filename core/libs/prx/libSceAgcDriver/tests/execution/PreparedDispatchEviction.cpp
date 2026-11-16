#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::StorageTexture;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Side = 512;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t TileR64KBX = 0x1b;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint64_t SurfaceBytes = std::uint64_t{Side} * Side * 4u;
constexpr std::uint64_t Stride = 0x10000;
constexpr std::uint32_t Entries = 4;
constexpr std::uint32_t Surfaces = Entries + 2;
constexpr std::uint32_t StoredOne = 0x3f800000u;
constexpr std::uint32_t StoredTwo = 0x40000000u;

alignas(256) constexpr std::array<std::uint32_t, 8> StoreOneCode{
    0x7e020280, 0x7e0402f2, 0x7e0602f2, 0x7e0802f2, 0x7e0a02f2, 0xf0201f08, 0x00010200, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 8> StoreTwoCode{
    0x7e020280, 0x7e0402f4, 0x7e0602f4, 0x7e0802f4, 0x7e0a02f4, 0xf0201f08, 0x00010200, 0xbf810000,
};

std::span<const std::uint32_t> Program(bool two) {
    return two ? std::span<const std::uint32_t>(StoreTwoCode) : std::span<const std::uint32_t>(StoreOneCode);
}

std::array<std::uint32_t, 8> TextureDescriptor(std::uint64_t address) {
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format32UInt << 20u) | (((Side - 1u) & 3u) << 30u),
        ((Side - 1u) >> 2u) | ((Side - 1u) << 14u),
        0xfacu | (TileR64KBX << 20u) | (Type2D << 28u),
        0u,
        0u,
        0u,
        0u,
    };
}

ShaderRecompiler::RecompileResult Compile(AgcDriver::VulkanDevice& device, std::uint64_t address, bool two) {
    std::vector<std::uint32_t> userData(16, 0u);
    const auto texture = TextureDescriptor(address);
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    const auto code = Program(two);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{32, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = true;
    return ShaderRecompiler::Recompile(request);
}

std::uint64_t ProgramAddress(bool two) {
    return reinterpret_cast<std::uintptr_t>(Program(two).data());
}

void Flush(AgcDriver::VulkanDevice& device, std::uint64_t address) {
    device.WaitIdle();
    StorageTexture::FlushPending(address, static_cast<std::size_t>(SurfaceBytes), nullptr, "test");
    device.WaitIdle();
}

void Store(AgcDriver::VulkanDevice& device, std::uint64_t address, bool two = false) {
    device.Dispatch(Compile(device, address, two), 1, 1, 1, {}, ProgramAddress(two));
    Flush(device, address);
}

std::size_t StoredWords(std::uint64_t address, std::uint32_t value) {
    std::vector<std::uint32_t> words(SurfaceBytes / 4u);
    AgcDriver::GuestMemory::Read(address, std::as_writable_bytes(std::span(words)));
    return static_cast<std::size_t>(std::ranges::count(words, value));
}

void SetVariable(const char* name, const char* value) {
#ifdef _WIN32
    Require(_putenv_s(name, value) == 0, std::string("cannot set ") + name);
#else
    Require(setenv(name, value, 1) == 0, std::string("cannot set ") + name);
#endif
}

}

int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "--storage-eviction";
        Require(mode == "--storage-eviction" || mode == "--resource-eviction", "prepared dispatch eviction: unknown mode " + mode);
        const bool storageEviction = mode == "--storage-eviction";
        SetVariable("APS5_STORAGE_CACHE_ENTRIES", storageEviction ? "4" : "0");
        SetVariable("APS5_TEXTURE_CACHE_MIB", "1024");
        SetVariable("APS5_PROFILE_DRAW", "1");
        if (!storageEviction) SetVariable("APS5_RESOURCE_CACHE_ENTRIES", "2");
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::vector<std::uint32_t> storage((SurfaceBytes + Surfaces * Stride + Stride) / 4u, 0u);
        auto* texels = reinterpret_cast<std::uint32_t*>((reinterpret_cast<std::uintptr_t>(storage.data()) + Stride - 1u) & ~std::uintptr_t{Stride - 1u});
        const auto base = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(texels));
        const auto blockBytes = static_cast<std::size_t>(SurfaceBytes + (Surfaces - 1u) * Stride);
        {
            GuestAllocations::Mutation mutation;
            mutation.Add(texels, blockBytes, true, true, true);
        }
        const auto release = [&] {
            AgcDriver::Graphics::ClearCachedTextures(device->Device());
            GuestAllocations::Mutation mutation;
            mutation.Remove(texels);
        };
        try {
            Store(*device, base, true);
            Store(*device, base, true);
            const auto first = StoredWords(base, StoredTwo);
            Require(first != 0, mode + ": the dispatch storing 2.0 stored nothing");
            Store(*device, base);
            Require(StoredWords(base, StoredOne) == first && StoredWords(base, StoredTwo) == 0, mode + ": the dispatch storing 1.0 did not replace every 2.0");
            const auto result = Compile(*device, base, true);
            auto prepared = device->PrepareDispatch(result, {}, 32, ProgramAddress(true));
            const auto phases = AgcDriver::VulkanDevice::PreparePhaseMs(*prepared);
            Require(phases[2] > 0.0 && phases[4] == 0.0, mode + ": the prepare did not find the cached build, so the dispatch would not be handed one");
            const std::weak_ptr<StorageTexture> held = StorageTexture::FindLive(base, SurfaceBytes);
            Require(!held.expired(), mode + ": surface 0 has no cached storage image before the eviction");
            if (storageEviction) {
                for (std::uint32_t surface = 1; surface < Surfaces; ++surface) Store(*device, base + surface * Stride);
                Require(StorageTexture::FindLive(base, SurfaceBytes) == nullptr, mode + ": surface 0's storage image was not evicted");
            } else {
                Store(*device, base + Stride, true);
                Store(*device, base + Stride);
                Require(StorageTexture::FindLive(base, SurfaceBytes) != nullptr, mode + ": surface 0's storage image left the cache");
            }
            device->Dispatch(result, 1, 1, 1, {}, ProgramAddress(true), std::move(prepared));
            Flush(*device, base);
            const auto after = StoredWords(base, StoredTwo);
            std::cout << mode << ": " << first << " words of 2.0 stored before, " << after << " by the prepared dispatch after the eviction (" << StoredWords(base, StoredOne) << " words of 1.0 left); the evicted image is " << (held.expired() ? "gone" : "still held") << '\n';
            Require(after == first, mode + ": the prepared dispatch after the eviction stored " + std::to_string(after) + " words of 2.0 in guest memory instead of " + std::to_string(first));
        } catch (...) {
            release();
            throw;
        }
        release();
        std::cout << "prepared dispatch eviction tests passed (" << mode << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "prepared dispatch eviction test failed: " << error.what() << '\n';
        return 1;
    }
}
