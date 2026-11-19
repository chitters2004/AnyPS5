#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::StorageTexture;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Side = 128;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t Format32Float = 22;
constexpr std::uint32_t Format8UNorm = 1;
constexpr std::uint32_t TileR64KBX = 0x1b;
constexpr std::uint32_t Type2D = 9;
constexpr std::size_t SurfaceBytes = 65536;
constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t Initial = 0x5a5a5a5au;

alignas(256) constexpr std::array<std::uint32_t, 5> StoreCode{
    0x7e020280, 0x7e04020c, 0xf0201108, 0x00010200, 0xbf810000,
};

std::uint64_t AddressOf(const void* data) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
}

class GuestBlock {
public:
    explicit GuestBlock(bool watched) {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, 65536));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
        static_cast<void>(watched);
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
        Require(block != nullptr, "format alias census: cannot allocate the guest block");
        Require(!watched || AgcDriver::GuestMemory::Watched(AddressOf(block), BlockBytes), "format alias census: the guest block is not write-watched");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, BlockBytes);
#else
        munmap(block, BlockBytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, BlockBytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Data() { return block; }

private:
    std::uint8_t* block = nullptr;
};

std::array<std::uint32_t, 8> TextureDescriptor(const std::uint8_t* texels, std::uint32_t type, std::uint32_t format = Format32UInt) {
    const auto address = AddressOf(texels);
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (format << 20u) | (((Side - 1u) & 3u) << 30u),
        ((Side - 1u) >> 2u) | ((Side - 1u) << 14u),
        0xfacu | (TileR64KBX << 20u) | (type << 28u),
        0u,
        0u,
        0u,
        0u,
    };
}

void Dispatch(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, const std::vector<std::uint32_t>& userData) {
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

void Store(AgcDriver::VulkanDevice& device, const std::uint8_t* texels, std::uint32_t value) {
    std::vector<std::uint32_t> userData(16, 0u);
    const auto texture = TextureDescriptor(texels, Type2D);
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    userData[12] = value;
    Dispatch(device, StoreCode, userData);
}

std::shared_ptr<StorageTexture> FloatImage(const AgcDriver::Graphics::Context& context, const std::uint8_t* texels, std::uint32_t format = Format32Float) {
    const auto words = TextureDescriptor(texels, Type2D, format);
    return AgcDriver::Graphics::CachedStorageSurface(context, AgcDriver::Graphics::DecodeTextureResource(words));
}

void RequireCensus(const StorageTexture::FormatAliasCensus& base, std::uint64_t uploads, std::uint64_t afterWriteBack, const char* what) {
    const auto now = StorageTexture::FormatAliasTotals();
    const auto counted = now.uploads - base.uploads;
    const auto paired = now.afterWriteBack - base.afterWriteBack;
    const auto bytes = now.uploadBytes - base.uploadBytes;
    Require(counted == uploads && paired == afterWriteBack && bytes == uploads * SurfaceBytes, std::string(what) + ": " + std::to_string(counted) + " aliased uploads (" + std::to_string(bytes) + " bytes), " + std::to_string(paired) + " after a write-back; expected " + std::to_string(uploads) + " and " + std::to_string(afterWriteBack));
}

void FullExtentRule() {
    using AgcDriver::Graphics::ColorTarget;
    using AgcDriver::Graphics::FullExtentDraw;
    using AgcDriver::Graphics::State;
    const auto full = [] {
        State state{};
        state.blends.resize(1);
        state.blends[0].colorWriteMask = 0xfu;
        state.colorRop = 0xccu;
        state.scissor = {{0, 0}, {64, 32}};
        state.viewport = {0.0f, 0.0f, 64.0f, 32.0f, 0.0f, 1.0f};
        return state;
    };
    ColorTarget color{};
    color.extent = {64, 32};
    color.format = VK_FORMAT_R8G8B8A8_UNORM;
    int cases = 0;
    const auto expect = [&](bool expected, const State& state, const ColorTarget& target, const char* what) {
        Require(FullExtentDraw(state, target) == expected, std::string("full-extent draw rule: ") + what + (expected ? " is not a full-extent draw" : " is a full-extent draw"));
        ++cases;
    };
    expect(true, full(), color, "the full state");
    auto state = full();
    state.viewport = {0.0f, 32.0f, 64.0f, -32.0f, 0.0f, 1.0f};
    expect(true, state, color, "a flipped viewport over the target");
    state = full();
    state.blends[0].colorWriteMask = 0x1u;
    auto narrowColor = color;
    narrowColor.format = VK_FORMAT_R32_UINT;
    expect(true, state, narrowColor, "a red-only mask on a one-channel target");
    expect(false, state, color, "a red-only mask on a four-channel target");
    state = full();
    state.blends[0].blendEnable = VK_TRUE;
    expect(false, state, color, "blending");
    state = full();
    state.scissor = {{0, 0}, {64, 31}};
    expect(false, state, color, "a scissor short of the target");
    state = full();
    state.scissor = {{1, 0}, {64, 32}};
    expect(false, state, color, "a scissor offset into the target");
    state = full();
    state.viewport = {0.0f, 0.0f, 63.0f, 32.0f, 0.0f, 1.0f};
    expect(false, state, color, "a viewport short of the target");
    state = full();
    state.viewport = {0.0f, 1.0f, 64.0f, 32.0f, 0.0f, 1.0f};
    expect(false, state, color, "a viewport offset into the target");
    state = full();
    state.depthTest = true;
    expect(false, state, color, "a depth test");
    state = full();
    state.stencilTest = true;
    expect(false, state, color, "a stencil test");
    state = full();
    state.alphaToCoverage = true;
    expect(false, state, color, "alpha to coverage");
    state = full();
    state.colorRop = 0x88u;
    expect(false, state, color, "a logic operation");
    state = full();
    state.rasterizerDiscard = true;
    expect(false, state, color, "rasterizer discard");
    auto partial = color;
    partial.partialWriteMask = 0x3u;
    expect(false, full(), partial, "a partial packed write");
    std::printf("full-extent draw rule: %d cases\n", cases);
}

void Run(AgcDriver::VulkanDevice& device, std::uint8_t* texels, bool counted) {
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    const auto surface = AddressOf(texels);
    const auto base = StorageTexture::FormatAliasTotals();
    const std::uint64_t on = counted ? 1u : 0u;
    std::fill_n(reinterpret_cast<std::uint32_t*>(texels), SurfaceBytes / 4u, Initial);
    AgcDriver::GuestMemory::CollectWrites(surface, SurfaceBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Store(device, texels, 0x11111111u);
    const auto written = StorageTexture::FindPending(surface, SurfaceBytes);
    Require(written != nullptr, "the 32-bit integer store left no pending image");
    RequireCensus(base, 0, 0, "the first image over the surface");
    StorageTexture::ReportFormatAliases();
    StorageTexture::FlushPending(surface, SurfaceBytes, nullptr, "test");
    device.WaitIdle();
    const auto sampled = FloatImage(context, texels);
    Require(sampled != nullptr && sampled.get() != written.get(), "the float lookup did not make a second image over the surface");
    RequireCensus(base, on, on, "the float image made after the integer image's write-back");
    Store(device, texels, 0x22222222u);
    Require(StorageTexture::FindPending(surface, SurfaceBytes) == written, "the second store did not reuse the integer image");
    StorageTexture::FlushPending(surface, SurfaceBytes, nullptr, "test");
    device.WaitIdle();
    Require(FloatImage(context, texels) == sampled, "the float lookup made a third image");
    RequireCensus(base, 2 * on, 2 * on, "the float image refreshed after the second write-back");
    std::fill_n(reinterpret_cast<std::uint32_t*>(texels), SurfaceBytes / 4u, 0x33333333u);
    AgcDriver::GuestMemory::CollectWrites(surface, SurfaceBytes);
    Require(FloatImage(context, texels) == sampled, "the float lookup made a third image after the CPU write");
    RequireCensus(base, 3 * on, 2 * on, "the float image refreshed after a CPU write");
    const char* refusal = nullptr;
    const std::array<std::uint32_t, 4> zero{};
    Require(sampled->FillClear(std::span<const std::uint32_t, 4>(zero), StorageTexture::WholeImage, refusal), std::string("the float image refused a whole-image clear: ") + (refusal != nullptr ? refusal : "no reason"));
    const auto narrow = FloatImage(context, texels, Format8UNorm);
    Require(narrow != nullptr && narrow != sampled && narrow != written, "the 8-bit lookup did not make a third image over the surface");
    Require(StorageTexture::FormatAliasTotals().uploads - base.uploads == 4 * on, "the 8-bit image's upload was not counted as aliased");
    StorageTexture::ReportFormatAliases();
    AgcDriver::Graphics::ClearCachedTextures(device.Device());
}

void ContentRun(AgcDriver::VulkanDevice& device, std::uint8_t* texels, std::uint8_t* copy, std::uint8_t* unlike, std::uint8_t* unwritten) {
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    const auto surface = AddressOf(texels);
    const auto elsewhere = AddressOf(copy);
    std::fill_n(reinterpret_cast<std::uint32_t*>(texels), SurfaceBytes / 4u, Initial);
    AgcDriver::GuestMemory::CollectWritesUncached(surface, SurfaceBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Store(device, texels, 0x44444444u);
    StorageTexture::FlushPending(surface, SurfaceBytes, nullptr, "test");
    device.WaitIdle();
    std::copy_n(texels, SurfaceBytes, copy);
    AgcDriver::GuestMemory::CollectWritesUncached(elsewhere, SurfaceBytes);
    const auto copied = FloatImage(context, copy, Format32UInt);
    Require(copied != nullptr, "the copy's lookup made no image");
    std::fill_n(reinterpret_cast<std::uint32_t*>(unlike), SurfaceBytes / 4u, 0x55555555u);
    AgcDriver::GuestMemory::CollectWritesUncached(AddressOf(unlike), SurfaceBytes);
    Require(FloatImage(context, unlike, Format32UInt) != nullptr, "the unlike surface's lookup made no image");
    std::copy_n(unlike, SurfaceBytes, unwritten);
    AgcDriver::GuestMemory::CollectWritesUncached(AddressOf(unwritten), SurfaceBytes);
    Require(FloatImage(context, unwritten, Format32UInt) != nullptr, "the surface matching an image that never wrote back made no image");
    AgcDriver::GuestMemory::MarkWritten(elsewhere, SurfaceBytes);
    Require(FloatImage(context, copy, Format32UInt) == copied, "the copy's second lookup made another image");
    StorageTexture::ReportFormatAliases();
    AgcDriver::Graphics::ClearCachedTextures(device.Device());
}

}

int main() {
    try {
        const bool watched = AgcDriver::GuestMemory::WriteWatched();
        GuestBlock block(watched);
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const bool counted = std::getenv("APS5_PROFILE_DRAW") != nullptr;
        FullExtentRule();
        Run(*device, block.Data(), counted);
        if (counted && std::getenv("APS5_PROFILE_UPLOAD_CONTENT") != nullptr) {
            GuestBlock writer(watched);
            GuestBlock copy(watched);
            GuestBlock unlike(watched);
            GuestBlock unwritten(watched);
            ContentRun(*device, writer.Data(), copy.Data(), unlike.Data(), unwritten.Data());
        }
        std::printf("storage format alias census tests passed (%s)\n", counted ? "counted" : "not counted");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
