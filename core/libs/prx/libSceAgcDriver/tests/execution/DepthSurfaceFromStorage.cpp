#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureSwizzleEquations.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "DepthFastClearHarness.hpp"
#include "VulkanTestDevice.hpp"
#ifdef APS5_AGC_ALLOCATION_COUNT
#include "../AllocationCounter.hpp"
#endif
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
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

using DepthFastClearHarness::Covered;
using DepthFastClearHarness::Height;
using DepthFastClearHarness::HtileBytes;
using DepthFastClearHarness::Require;
using DepthFastClearHarness::Width;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Format8Uint = 5;
constexpr std::uint32_t Format16Unorm = 7;
constexpr std::uint32_t Format32Float = 22;
constexpr std::uint32_t TileDepth64KB = 0x18;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Type2DArray = 13;
constexpr std::size_t PlaneBytes = 65536;
constexpr std::size_t BlockBytes = 86 * PlaneBytes;
constexpr std::uint32_t RowPasses = 15;

alignas(256) constexpr std::array<std::uint32_t, 9> StoreRowCode{
    0x7e280d00, 0x102828ff, 0x3cf80000, 0x7e3c0300, 0x7e3e0280, 0xf0201108, 0x0001141e, 0xbf810000, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 7> StoreStencilRowCode{
    0x7e280281, 0x7e3c0300, 0x7e3e0280, 0xf0201108, 0x0001141e, 0xbf810000, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 10> StoreRowLayerCode{
    0x7e280d00, 0x102828ff, 0x3cf80000, 0x7e3c0300, 0x7e3e0280, 0x7e400280, 0xf0201128, 0x0001141e, 0xbf810000, 0xbf810000,
};

class GuestBlock {
public:
    explicit GuestBlock(std::size_t bytes = BlockBytes, bool touched = true) : bytes(bytes) {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, 65536));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
#else
        constexpr std::uintptr_t alignment = 65536;
        void* mapped = mmap(nullptr, bytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + bytes != begin + bytes + alignment) munmap(reinterpret_cast<void*>(aligned + bytes), begin + alignment - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
        }
#endif
        Require(block != nullptr, "depth surface from storage: cannot allocate the guest block");
        if (touched) std::memset(block, 0, bytes);
        GuestAllocations::Mutation().Add(block, bytes, true, true, true);
        AgcDriver::GuestMemory::CollectWritesUncached(reinterpret_cast<std::uintptr_t>(block), bytes);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, bytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, bytes);
#else
        munmap(block, bytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, bytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Plane(std::size_t index) { return block + index * PlaneBytes; }

private:
    std::size_t bytes;
    std::uint8_t* block = nullptr;
};

std::uint64_t Address(const std::uint8_t* plane) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(plane));
}

std::array<std::uint32_t, 8> StorageView(std::uint64_t address, std::uint32_t format, std::uint32_t layers, std::uint32_t baseLevel = 0, std::uint32_t lastLevel = 0) {
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (format << 20u) | (((Width - 1u) & 3u) << 30u),
        ((Width - 1u) >> 2u) | ((Height - 1u) << 14u),
        0xfacu | (baseLevel << 12u) | (lastLevel << 16u) | (TileDepth64KB << 20u) | ((layers > 1u ? Type2DArray : Type2D) << 28u),
        layers - 1u, lastLevel << 4u, 0u, 0u,
    };
}

void Fill(std::uint8_t* plane, VkFormat format) {
    if (format == VK_FORMAT_D16_UNORM) {
        const std::uint16_t quarter = 0x4000u;
        for (std::size_t offset = 0; offset < PlaneBytes; offset += sizeof(quarter)) std::memcpy(plane + offset, &quarter, sizeof(quarter));
        return;
    }
    const float quarter = 0.25f;
    for (std::size_t offset = 0; offset < PlaneBytes; offset += sizeof(quarter)) std::memcpy(plane + offset, &quarter, sizeof(quarter));
}

void Store(AgcDriver::VulkanDevice& device, const std::array<std::uint32_t, 8>& view, std::span<const std::uint32_t> code) {
    std::vector<std::uint32_t> userData(16, 0u);
    std::copy(view.begin(), view.end(), userData.begin() + 4);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto program = ShaderRecompiler::Recompile(request);
    device.Dispatch(program, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

void StoreRow(AgcDriver::VulkanDevice& device, std::uint8_t* plane, VkFormat format, std::uint32_t layers, std::uint32_t level = 0) {
    const std::span<const std::uint32_t> code = layers > 1u ? std::span<const std::uint32_t>(StoreRowLayerCode) : std::span<const std::uint32_t>(StoreRowCode);
    Store(device, StorageView(Address(plane), format == VK_FORMAT_D16_UNORM ? Format16Unorm : Format32Float, layers, level, level), code);
}

void StoreStencilRow(AgcDriver::VulkanDevice& device, std::uint8_t* stencil) {
    Store(device, StorageView(Address(stencil), Format8Uint, 1), StoreStencilRowCode);
}

DepthFastClearHarness::Surface Plane(std::uint8_t* plane, VkFormat format, std::uint64_t htile = 0, std::uint32_t swizzle = 0) {
    return {Address(plane), 0, htile, false, format, 0, 0, swizzle};
}

std::uint32_t ZSwizzle() {
    return AgcDriver::Graphics::XorSwizzleMode(AgcDriver::Graphics::TextureTileMode::kZ64KBX);
}

std::vector<std::string> failures;
std::vector<std::string> skipped;

void RequireCount(std::uint32_t covered, std::uint32_t expected, const std::string& what) {
    if (covered != expected) failures.push_back(what + ": " + std::to_string(covered) + " pixels passed, expected " + std::to_string(expected));
}

void NewSurfaceCase(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* plane, VkFormat format, bool flushed, const std::string& what, std::uint32_t layers = 1, std::uint32_t swizzle = 0) {
    for (std::uint32_t layer = 0; layer < layers; ++layer) Fill(plane + layer * PlaneBytes, format);
    StoreRow(device, plane, format, layers);
    if (layers > 1u) {
        Require(AgcDriver::Graphics::StorageTexture::FindPending(Address(plane), PlaneBytes) != nullptr && AgcDriver::Graphics::StorageTexture::FindLive(Address(plane), PlaneBytes) == nullptr, what + ": the stored row is not pending in a storage image larger than the plane, or a cached image of the plane's size holds it");
    } else if (flushed) {
        AgcDriver::Graphics::StorageTexture::FlushPending(Address(plane), PlaneBytes, nullptr, "test");
        device.WaitIdle();
        Require(AgcDriver::Graphics::StorageTexture::FindPending(Address(plane), PlaneBytes) == nullptr && AgcDriver::Graphics::StorageTexture::FindLive(Address(plane), PlaneBytes) != nullptr, what + ": the stored row is not held by a flushed, live storage image");
    } else {
        Require(AgcDriver::Graphics::StorageTexture::FindPending(Address(plane), PlaneBytes) != nullptr, what + ": the stored row is not pending in a storage image");
    }
    const auto seeds = AgcDriver::Graphics::DepthSurfaceSeedCount();
    RequireCount(DepthFastClearHarness::Draw(device, shaders, Plane(plane, format, 0, swizzle), {}), RowPasses, what);
    RequireCount(static_cast<std::uint32_t>(AgcDriver::Graphics::DepthSurfaceSeedCount() - seeds), 0, what + ", surfaces seeded from guest memory");
}

constexpr VkExtent2D PatternExtent{192, 136};
constexpr std::uint32_t PatternTexels = PatternExtent.width * PatternExtent.height;
alignas(256) std::array<std::byte, PatternTexels * 4> PatternPixels{};

std::uint64_t EquationOffset(const AgcDriver::Graphics::TextureSwizzleEquation& equation, std::uint32_t x, std::uint32_t y) {
    std::uint64_t offset = 0;
    for (std::uint32_t bit = 0; bit < 16; ++bit) {
        const auto mask = equation.bits[bit];
        const auto selected = (x & (mask & 0xfffu)) ^ ((y << 12) & (mask & 0xfff000u));
        offset |= static_cast<std::uint64_t>(std::popcount(selected) & 1u) << bit;
    }
    return offset;
}

auto DescribePlane(VkExtent2D extent) {
    AgcDriver::Graphics::GuestTextureResource resource{};
    resource.width = extent.width;
    resource.height = extent.height;
    resource.mipCount = 1;
    resource.allocatedMipCount = 1;
    resource.tileMode = AgcDriver::Graphics::TextureTileMode::kZ64KBX;
    resource.dimension = AgcDriver::Graphics::TextureDimension::k2D;
    resource.format = Format32Float;
    return AgcDriver::Graphics::DescribeSurface(resource);
}

std::uint64_t TiledOffset(const decltype(DescribePlane({}))& geometry, std::uint32_t x, std::uint32_t y, std::uint32_t pipeBankXor) {
    static const auto layout = AgcDriver::Graphics::ThinBlockLayout(AgcDriver::Graphics::TextureTileMode::kZ64KBX, 4u);
    static const auto* equation = AgcDriver::Graphics::FindTextureSwizzleEquation(ZSwizzle(), 4u);
    Require(equation != nullptr && geometry.guestBytes <= 4u * PlaneBytes, "the SW_64KB_Z_X equation is missing or the plane does not fit four slots");
    const auto& mip = geometry.mips.front();
    return mip.tiledOffset + (static_cast<std::uint64_t>(y / layout[2]) * mip.blocksPerRow + x / layout[1]) * layout[0] + (EquationOffset(*equation, x, y) ^ pipeBankXor);
}

std::uint64_t TexelOffset(std::uint32_t x, std::uint32_t y, std::uint32_t pipeBankXor = 0) {
    static const auto geometry = DescribePlane(PatternExtent);
    return TiledOffset(geometry, x, y, pipeBankXor);
}

float PatternDepth(std::uint32_t x, std::uint32_t y, bool inverted) {
    const bool distant = (((x >> 3u) ^ (y >> 2u) ^ (x >> 6u) ^ ((y * 5u) >> 5u)) & 1u) != 0u;
    return distant != inverted ? 0.75f : 0.25f;
}

void WritePattern(std::uint8_t* plane, bool inverted, std::uint32_t pipeBankXor = 0) {
    for (std::uint32_t y = 0; y < PatternExtent.height; ++y) {
        for (std::uint32_t x = 0; x < PatternExtent.width; ++x) {
            const float depth = PatternDepth(x, y, inverted);
            std::memcpy(plane + TexelOffset(x, y, pipeBankXor), &depth, sizeof(depth));
        }
    }
}

void FillDepth(std::uint8_t* texel, float depth) {
    std::memcpy(texel, &depth, sizeof(depth));
}

void FillPlane(std::uint8_t* plane, float depth) {
    for (std::size_t offset = 0; offset < PlaneBytes; offset += sizeof(depth)) std::memcpy(plane + offset, &depth, sizeof(depth));
}

void RequirePattern(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* plane, bool inverted, const std::string& what, std::uint32_t pipeBankXor = 0) {
    auto surface = Plane(plane, VK_FORMAT_D32_SFLOAT, 0, ZSwizzle());
    surface.pipeBankXor = pipeBankXor;
    static_cast<void>(DepthFastClearHarness::Draw(device, shaders, surface, {}, PatternExtent, PatternPixels));
    std::uint32_t wrong = 0;
    for (std::uint32_t y = 0; y < PatternExtent.height; ++y) {
        for (std::uint32_t x = 0; x < PatternExtent.width; ++x) {
            const bool passed = std::to_integer<std::uint8_t>(PatternPixels[(static_cast<std::size_t>(y) * PatternExtent.width + x) * 4u]) == 255u;
            if (passed != (PatternDepth(x, y, inverted) > 0.5f)) ++wrong;
        }
    }
    if (wrong != 0) failures.push_back(what + ": " + std::to_string(wrong) + " of " + std::to_string(PatternTexels) + " pixels disagree with the plane's depth pattern");
}

std::uint32_t SeedsSince(std::uint64_t seeds) {
    return static_cast<std::uint32_t>(AgcDriver::Graphics::DepthSurfaceSeedCount() - seeds);
}

void GuestPlanePatternCase(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* plane) {
    const auto base = Address(plane);
    const auto bytes = static_cast<std::size_t>(4u * PlaneBytes);
    const bool watched = AgcDriver::GuestMemory::WriteWatched() && AgcDriver::GuestMemory::Watched(base, bytes);
    WritePattern(plane, false);
    const auto seeds = AgcDriver::Graphics::DepthSurfaceSeedCount();
    RequirePattern(device, shaders, plane, false, "a new 192x136 D32 depth surface over a plane only the CPU wrote, tested LESS at 0.5");
    RequireCount(SeedsSince(seeds), 1, "a new 192x136 D32 depth surface over a plane only the CPU wrote, surfaces seeded from guest memory");
    if (!watched) std::puts("guest plane seed: the plane is not write-watched here, so CPU writes after the bind are not checked");
    if (watched && !AgcDriver::GuestMemory::Watched(base, bytes)) failures.push_back("seeding a depth surface from its guest plane took the plane out of write tracking");
    if (watched) {
        WritePattern(plane, true);
        RequirePattern(device, shaders, plane, true, "the seeded depth surface after the CPU inverted its plane");
        WritePattern(plane, false);
        RequirePattern(device, shaders, plane, false, "the seeded depth surface after the CPU restored its plane");
    }
    RequireCount(DepthFastClearHarness::Draw(device, shaders, Plane(plane, VK_FORMAT_D32_SFLOAT, 0, ZSwizzle()), {.depthWrite = true, .depthCompare = VK_COMPARE_OP_ALWAYS}, PatternExtent, PatternPixels), PatternTexels, "the seeded depth surface writing depth 0.5");
    static_cast<void>(AgcDriver::Graphics::FlushDepthPlanes(base, bytes));
    device.WaitIdle();
    std::uint32_t unstored = 0;
    for (std::uint32_t y = 0; y < PatternExtent.height; ++y) {
        for (std::uint32_t x = 0; x < PatternExtent.width; ++x) {
            float depth = 0.0f;
            std::memcpy(&depth, plane + TexelOffset(x, y), sizeof(depth));
            if (depth != 0.5f) ++unstored;
        }
    }
    if (unstored != 0) failures.push_back("the seeded depth surface's write of 0.5, flushed to its plane: " + std::to_string(unstored) + " texels do not hold it");
    if (watched) {
        WritePattern(plane, true);
        RequirePattern(device, shaders, plane, true, "the seeded depth surface after the CPU wrote over the depth the GPU flushed to its plane");
    }
    RequireCount(SeedsSince(seeds), 1, "a seeded depth surface bound again, surfaces seeded from guest memory");
}

std::uint8_t* racedPlane = nullptr;

void WriteWhileSeeding(AgcDriver::Graphics::DepthPublicationStage stage) {
    if (stage == AgcDriver::Graphics::DepthPublicationStage::GuestPlaneSeeded && racedPlane != nullptr) FillPlane(racedPlane, 0.75f);
}

void SeedRaceCase(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* plane) {
    if (!AgcDriver::GuestMemory::WriteWatched() || !AgcDriver::GuestMemory::Watched(Address(plane), PlaneBytes)) {
        std::puts("guest plane seed: the plane is not write-watched here, so a CPU write during the seed is not checked");
        return;
    }
    FillPlane(plane, 0.25f);
    racedPlane = plane;
    AgcDriver::Graphics::SetDepthPublicationHookForTest(WriteWhileSeeding);
    const auto seeds = AgcDriver::Graphics::DepthSurfaceSeedCount();
    static_cast<void>(DepthFastClearHarness::Draw(device, shaders, Plane(plane, VK_FORMAT_D32_SFLOAT, 0, ZSwizzle()), {}));
    AgcDriver::Graphics::SetDepthPublicationHookForTest(nullptr);
    racedPlane = nullptr;
    RequireCount(SeedsSince(seeds), 1, "a depth surface whose plane the CPU wrote while it was seeded, surfaces seeded from guest memory");
    RequireCount(DepthFastClearHarness::Draw(device, shaders, Plane(plane, VK_FORMAT_D32_SFLOAT, 0, ZSwizzle()), {}), Covered, "a depth surface seeded from 0.25 while the CPU wrote 0.75 over its plane, bound again and tested LESS at 0.5");
}

std::uint8_t* collectedPlane = nullptr;

void CollectWhileStamping(AgcDriver::Graphics::DepthPublicationStage stage) {
    if (stage != AgcDriver::Graphics::DepthPublicationStage::UnitsCollected || collectedPlane == nullptr) return;
    FillPlane(collectedPlane, 0.75f);
    static_cast<void>(AgcDriver::GuestMemory::CollectWritesUncached(Address(collectedPlane), PlaneBytes));
    collectedPlane = nullptr;
}

void PartialWatchCollectCase(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* plane) {
    const auto base = Address(plane);
    if (!AgcDriver::GuestMemory::WriteWatched() || !AgcDriver::GuestMemory::Watched(base, 4u * PlaneBytes)) {
        std::puts("partial watch collect: the plane is not write-watched here, so a write collected during a page scan is not checked");
        return;
    }
    for (std::uint32_t block = 0; block < 4u; ++block) FillPlane(plane + block * PlaneBytes, 0.25f);
    AgcDriver::GuestMemory::Unwatch(base + 3u * PlaneBytes, PlaneBytes);
    Require(!AgcDriver::GuestMemory::Watched(base, 4u * PlaneBytes) && AgcDriver::GuestMemory::Watched(base, PlaneBytes), "partial watch collect: the plane's last block is still watched or its first is not");
    const auto surface = Plane(plane, VK_FORMAT_D32_SFLOAT, 0, ZSwizzle());
    RequireCount(DepthFastClearHarness::Draw(device, shaders, surface, {}, PatternExtent, PatternPixels), 0, "a new 192x136 D32 depth surface over a 0.25 plane whose last block is unwatched, tested LESS at 0.5");
    collectedPlane = plane;
    AgcDriver::Graphics::SetDepthPublicationHookForTest(CollectWhileStamping);
    static_cast<void>(DepthFastClearHarness::Draw(device, shaders, surface, {}, PatternExtent, PatternPixels));
    AgcDriver::Graphics::SetDepthPublicationHookForTest(nullptr);
    Require(collectedPlane == nullptr, "partial watch collect: the bind did not scan the plane page by page");
    std::uint32_t firstBlock = 0;
    for (std::uint32_t y = 0; y < PatternExtent.height; ++y) {
        for (std::uint32_t x = 0; x < PatternExtent.width; ++x) firstBlock += TexelOffset(x, y) < PlaneBytes ? 1u : 0u;
    }
    RequireCount(DepthFastClearHarness::Draw(device, shaders, surface, {}, PatternExtent, PatternPixels), firstBlock, "a depth surface over a partially watched plane whose first block another collect took 0.75 from while a bind scanned it page by page, bound again and tested LESS at 0.5");
}

void IncompatibleStorageCase(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* plane) {
    StoreStencilRow(device, plane);
    AgcDriver::Graphics::StorageTexture::FlushPending(Address(plane), PlaneBytes, nullptr, "test");
    device.WaitIdle();
    Require(AgcDriver::Graphics::StorageTexture::FindLive(Address(plane), PlaneBytes) != nullptr, "the R8 row over the depth plane is not held by a live storage image of the plane's size");
    FillPlane(plane, 0.25f);
    const auto seeds = AgcDriver::Graphics::DepthSurfaceSeedCount();
    RequireCount(DepthFastClearHarness::Draw(device, shaders, Plane(plane, VK_FORMAT_D32_SFLOAT, 0, ZSwizzle()), {}), 0, "a new D32 depth surface under a live R8 storage image of its plane, after the CPU wrote 0.25 there, tested LESS at 0.5");
    RequireCount(SeedsSince(seeds), 1, "a new D32 depth surface under a live R8 storage image of its plane, surfaces seeded from guest memory");
}

std::optional<AgcDriver::Graphics::DepthPublicationStage> faultStage;

void FaultWhileSeeding(AgcDriver::Graphics::DepthPublicationStage stage) {
    if (!faultStage || stage != *faultStage) return;
    faultStage.reset();
    throw std::runtime_error("injected seed fault");
}

void SeedFaultCase(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* plane, AgcDriver::Graphics::DepthPublicationStage stage, const std::string& what) {
    WritePattern(plane, false);
    faultStage = stage;
    AgcDriver::Graphics::SetDepthPublicationHookForTest(FaultWhileSeeding);
    bool threw = false;
    try {
        static_cast<void>(DepthFastClearHarness::Draw(device, shaders, Plane(plane, VK_FORMAT_D32_SFLOAT, 0, ZSwizzle()), {}, PatternExtent, PatternPixels));
    } catch (const std::runtime_error& error) {
        threw = std::string(error.what()).find("injected seed fault") != std::string::npos;
    }
    AgcDriver::Graphics::SetDepthPublicationHookForTest(nullptr);
    faultStage.reset();
    if (!threw) failures.push_back(what + ": the injected fault did not leave the bind");
    device.WaitIdle();
    const auto seeds = AgcDriver::Graphics::DepthSurfaceSeedCount();
    RequirePattern(device, shaders, plane, false, what + ", bound again and tested LESS at 0.5");
    RequireCount(SeedsSince(seeds), 1, what + ", bound again, surfaces seeded from guest memory");
}

#ifdef APS5_AGC_ALLOCATION_COUNT
void FailAllocationsWhileSeeding(AgcDriver::Graphics::DepthPublicationStage stage) {
    if (stage == AgcDriver::Graphics::DepthPublicationStage::SeedBaselineTaken) FailAllocationsAfter_nid_no_patch(0);
}

std::size_t allocationsAfterConstruction = 0;

void FailAllocationsAfterConstruction(AgcDriver::Graphics::DepthPublicationStage stage) {
    if (stage == AgcDriver::Graphics::DepthPublicationStage::CandidateConstructed) FailAllocationsAfter_nid_no_patch(allocationsAfterConstruction);
}
#endif

void AllocationFailureCase(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* plane) {
#ifdef APS5_AGC_ALLOCATION_COUNT
    const std::string what = "a depth surface seed whose allocations failed from its baseline on, through its cleanup";
    WritePattern(plane, false);
    AgcDriver::Graphics::SetDepthPublicationHookForTest(FailAllocationsWhileSeeding);
    bool threw = false;
    try {
        static_cast<void>(DepthFastClearHarness::Draw(device, shaders, Plane(plane, VK_FORMAT_D32_SFLOAT, 0, ZSwizzle()), {}, PatternExtent, PatternPixels));
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    StopAllocationFailures_nid_no_patch();
    AgcDriver::Graphics::SetDepthPublicationHookForTest(nullptr);
    if (!threw) failures.push_back(what + ": the failed allocation did not leave the bind");
    device.WaitIdle();
    const auto seeds = AgcDriver::Graphics::DepthSurfaceSeedCount();
    RequirePattern(device, shaders, plane, false, what + ", bound again and tested LESS at 0.5");
    RequireCount(SeedsSince(seeds), 1, what + ", bound again, surfaces seeded from guest memory");
#else
    static_cast<void>(device);
    static_cast<void>(shaders);
    static_cast<void>(plane);
    skipped.push_back("allocation failure during a depth surface seed: the driver's allocator is wrapped only in MinGW test builds");
#endif
}

void MultisampleAbortCase(AgcDriver::VulkanDevice& device, std::uint8_t* plane) {
    const std::string what = "a new 4-sample D32 depth surface over uneven samples, imported by a draw, whose allocations failed from the n-th after it was constructed";
#ifdef APS5_AGC_ALLOCATION_COUNT
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    if (!context.sampleRateShading || !context.storageImageMultisample || !context.storageImageWriteWithoutFormat) {
        skipped.push_back(what + ": the device cannot import depth samples by a draw");
        return;
    }
    constexpr VkExtent2D extent{64, 32};
    AgcDriver::Graphics::DepthTarget target{Address(plane), 0, extent, VK_FORMAT_D32_SFLOAT, 1.0f, 0, 0, false, 4};
    target.depthSwizzle = ZSwizzle();
    const auto bytes = static_cast<std::size_t>(AgcDriver::Graphics::MultisampledDepthBytes_nid_no_patch(extent, 4u, 4u));
    Require(bytes <= PlaneBytes, "the 4-sample D32 depth plane does not fit one plane");
    if (!AgcDriver::GuestMemory::Watched(target.address, bytes)) {
        skipped.push_back(what + ": the plane is not write-watched here, so its samples are not imported");
        return;
    }
    for (std::size_t offset = 0; offset < bytes; offset += sizeof(float)) {
        const float depth = 0.25f + static_cast<float>(offset / sizeof(float) % 4u) / 8.0f;
        std::memcpy(plane + offset, &depth, sizeof(depth));
    }
    device.WaitIdle();
    AgcDriver::Graphics::SetDepthPublicationHookForTest(FailAllocationsAfterConstruction);
    std::size_t aborted = 0;
    bool bound = false;
    bool recorded = true;
    for (allocationsAfterConstruction = 0; allocationsAfterConstruction < 64u && !bound && recorded; ++allocationsAfterConstruction) {
        {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            auto* recorder = AgcDriver::Graphics::Recorder::Active();
            recorded = recorder != nullptr && recorder->Device() == context.device;
            if (!recorded) break;
            try {
                bound = AgcDriver::Graphics::DepthSurfaceView(context, target) != VK_NULL_HANDLE;
            } catch (const std::bad_alloc&) {
                ++aborted;
            }
            StopAllocationFailures_nid_no_patch();
            recorder->Submit();
        }
        device.WaitIdle();
    }
    AgcDriver::Graphics::SetDepthPublicationHookForTest(nullptr);
    if (!recorded) {
        skipped.push_back(what + ": no active recorder holds the import draw");
        return;
    }
    if (aborted == 0u) failures.push_back(what + ": no failed allocation left the bind");
    if (!bound) failures.push_back(what + ": the bind did not succeed once allocations stopped failing");
    std::printf("%s: %zu failure points aborted the bind before it succeeded\n", what.c_str(), aborted);
#else
    static_cast<void>(device);
    static_cast<void>(plane);
    skipped.push_back(what + ": the driver's allocator is wrapped only in MinGW test builds");
#endif
}

void PipeBankXorCase(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* plane) {
    constexpr std::uint32_t pipeBankXor = 0xcf00u;
    const auto base = Address(plane);
    const auto bytes = static_cast<std::size_t>(4u * PlaneBytes);
    const bool watched = AgcDriver::GuestMemory::WriteWatched() && AgcDriver::GuestMemory::Watched(base, bytes);
    WritePattern(plane, false, pipeBankXor);
    const auto seeds = AgcDriver::Graphics::DepthSurfaceSeedCount();
    RequirePattern(device, shaders, plane, false, "a new 192x136 D32 depth surface with pipe/bank XOR 0xcf00 over a plane only the CPU wrote, tested LESS at 0.5", pipeBankXor);
    RequireCount(SeedsSince(seeds), 1, "a new 192x136 D32 depth surface with pipe/bank XOR 0xcf00, surfaces seeded from guest memory");
    if (!watched) return;
    WritePattern(plane, true, pipeBankXor);
    RequirePattern(device, shaders, plane, true, "the seeded depth surface with pipe/bank XOR 0xcf00 after the CPU inverted its plane", pipeBankXor);
}

AgcDriver::Graphics::DepthTarget Target(const DepthFastClearHarness::Surface& surface, VkExtent2D extent = {Width, Height}) {
    AgcDriver::Graphics::DepthTarget target{surface.depth, surface.stencil, extent, surface.format, 1.0f, surface.clearStencil, surface.htile, surface.htileStencil};
    target.firstSlice = surface.slice;
    target.depthSwizzle = surface.swizzle;
    target.pipeBankXor = surface.pipeBankXor;
    return target;
}

std::vector<std::uint32_t> ReadDepthImage(AgcDriver::VulkanDevice& device, const AgcDriver::Graphics::DepthTarget& target) {
    device.WaitIdle();
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    const auto image = AgcDriver::Graphics::DepthSurfaceImageForTest(context.device, target);
    Require(image != VK_NULL_HANDLE, "no live depth surface owns the target's depth plane to read back");
    const auto extent = target.extent;
    const auto bytes = static_cast<std::size_t>(extent.width) * extent.height * 4u;
    AgcDriver::Graphics::Buffer readback(context, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    std::fill(readback.Bytes().begin(), readback.Bytes().end(), std::byte{0xa5});
    {
        AgcDriver::Graphics::CommandBatch batch(context);
        const auto commands = batch.Handle();
        const auto barrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
        VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        before.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        region.imageExtent = {extent.width, extent.height, 1};
        context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(commands, image, VK_IMAGE_LAYOUT_GENERAL, readback.Handle(), 1, &region);
        VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
        batch.SubmitAndWait();
    }
    std::vector<std::uint32_t> words(bytes / 4u);
    std::memcpy(words.data(), readback.Bytes().data(), bytes);
    return words;
}

void RawDepthBitsCase(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* plane, std::uint32_t bits, const std::string& what) {
    for (std::size_t offset = 0; offset < PlaneBytes; offset += sizeof(bits)) std::memcpy(plane + offset, &bits, sizeof(bits));
    const auto seeds = AgcDriver::Graphics::DepthSurfaceSeedCount();
    RequireCount(DepthFastClearHarness::Draw(device, shaders, Plane(plane, VK_FORMAT_D32_SFLOAT, 0, ZSwizzle()), {}), 0, what + ", tested LESS at 0.5");
    RequireCount(SeedsSince(seeds), 1, what + ", surfaces seeded from guest memory");
    const auto texels = ReadDepthImage(device, Target(Plane(plane, VK_FORMAT_D32_SFLOAT, 0, ZSwizzle())));
    const auto changed = std::count_if(texels.begin(), texels.end(), [&](std::uint32_t texel) { return texel != bits; });
    if (changed != 0) {
        const auto first = *std::find_if(texels.begin(), texels.end(), [&](std::uint32_t texel) { return texel != bits; });
        char text[64];
        std::snprintf(text, sizeof(text), "0x%08x became 0x%08x", bits, first);
        failures.push_back(what + ", read back from the depth image: " + std::to_string(changed) + " of " + std::to_string(Covered) + " texels changed, " + text);
    }
}

void OutOfRangePlaneCase(AgcDriver::VulkanDevice& device, std::uint8_t* plane, bool unrestricted, const std::string& what) {
    constexpr std::uint32_t minusOne = 0xbf800000u;
    constexpr std::uint32_t clearOne = 0x3f800000u;
    for (std::size_t offset = 0; offset < PlaneBytes; offset += sizeof(minusOne)) std::memcpy(plane + offset, &minusOne, sizeof(minusOne));
    auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    context.depthRangeUnrestricted = unrestricted;
    AgcDriver::Graphics::DepthTarget target{Address(plane), 0, {Width, Height}, VK_FORMAT_D32_SFLOAT, 1.0f, 0};
    target.depthSwizzle = ZSwizzle();
    const auto seeds = AgcDriver::Graphics::DepthSurfaceSeedCount();
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        AgcDriver::Graphics::DepthSurfaceView(context, target);
    }
    RequireCount(SeedsSince(seeds), unrestricted ? 1 : 0, what + ", surfaces seeded from guest memory");
    const auto texels = ReadDepthImage(device, target);
    const auto expected = unrestricted ? minusOne : clearOne;
    const auto changed = std::count_if(texels.begin(), texels.end(), [&](std::uint32_t texel) { return texel != expected; });
    if (changed != 0) failures.push_back(what + ", read back from the depth image: " + std::to_string(changed) + " of " + std::to_string(texels.size()) + " texels are not " + (unrestricted ? "-1.0" : "the clear value 1.0"));
}

std::uint8_t* invertedPlane = nullptr;
AgcDriver::Graphics::DepthPublicationStage invertAt{};

void InvertWhileSeeding(AgcDriver::Graphics::DepthPublicationStage stage) {
    if (invertedPlane == nullptr || stage != invertAt) return;
    WritePattern(invertedPlane, true);
    invertedPlane = nullptr;
}

void SeedCopyRaceCase(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* plane, AgcDriver::Graphics::DepthPublicationStage stage, const std::string& what) {
    if (!AgcDriver::GuestMemory::WriteWatched() || !AgcDriver::GuestMemory::Watched(Address(plane), 4u * PlaneBytes)) {
        std::puts("guest plane seed: the plane is not write-watched here, so a CPU write during the seed's copy is not checked");
        return;
    }
    WritePattern(plane, false);
    invertedPlane = plane;
    invertAt = stage;
    AgcDriver::Graphics::SetDepthPublicationHookForTest(InvertWhileSeeding);
    const auto seeds = AgcDriver::Graphics::DepthSurfaceSeedCount();
    static_cast<void>(DepthFastClearHarness::Draw(device, shaders, Plane(plane, VK_FORMAT_D32_SFLOAT, 0, ZSwizzle()), {}, PatternExtent, PatternPixels));
    AgcDriver::Graphics::SetDepthPublicationHookForTest(nullptr);
    if (invertedPlane != nullptr) failures.push_back(what + ": the seed did not reach the stage that writes the plane");
    invertedPlane = nullptr;
    RequireCount(SeedsSince(seeds), 1, what + ", surfaces seeded from guest memory");
    RequirePattern(device, shaders, plane, true, what + ", bound again and tested LESS at 0.5");
}

void MismatchedOwnerCase(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* plane) {
    for (std::uint32_t block = 0; block < 4u; ++block) FillPlane(plane + block * PlaneBytes, 0.25f);
    RequireCount(DepthFastClearHarness::Draw(device, shaders, Plane(plane, VK_FORMAT_D32_SFLOAT, 0, ZSwizzle()), {}), 0, "a 64x32 D32 depth surface over a 0.25 plane, tested LESS at 0.5");
    const auto seeds = AgcDriver::Graphics::DepthSurfaceSeedCount();
    RequireCount(DepthFastClearHarness::Draw(device, shaders, Plane(plane, VK_FORMAT_D32_SFLOAT, 0, ZSwizzle()), {}, PatternExtent, PatternPixels), 0, "a new 192x136 D32 depth surface at the address of a live 64x32 one, over a 0.25 plane, tested LESS at 0.5");
    RequireCount(SeedsSince(seeds), 1, "a new 192x136 D32 depth surface at the address of a live 64x32 one, surfaces seeded from guest memory");
}

void SeedFaultOwnerCase(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* ownerDepth, std::uint8_t* stencil, std::uint8_t* faultedDepth) {
    constexpr VkStencilOpState writeOne{VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xffu, 0xffu, 1u};
    constexpr VkStencilOpState equalOne{VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0xffu, 0u, 1u};
    const DepthFastClearHarness::Surface owner{Address(ownerDepth), Address(stencil), 0, false, VK_FORMAT_D32_SFLOAT_S8_UINT, 0, 0, ZSwizzle()};
    const DepthFastClearHarness::Surface faulted{Address(faultedDepth), Address(stencil), 0, false, VK_FORMAT_D32_SFLOAT_S8_UINT, 0, 0, ZSwizzle()};
    RequireCount(DepthFastClearHarness::Draw(device, shaders, owner, {.depthTest = false, .stencilTest = true, .stencil = writeOne}), Covered, "a D32S8 depth surface writing stencil 1");
    faultStage = AgcDriver::Graphics::DepthPublicationStage::SeedRecorded;
    AgcDriver::Graphics::SetDepthPublicationHookForTest(FaultWhileSeeding);
    bool threw = false;
    try {
        static_cast<void>(DepthFastClearHarness::Draw(device, shaders, faulted, {.depthTest = false, .stencilTest = true, .stencil = equalOne}));
    } catch (const std::runtime_error& error) {
        threw = std::string(error.what()).find("injected seed fault") != std::string::npos;
    }
    AgcDriver::Graphics::SetDepthPublicationHookForTest(nullptr);
    faultStage.reset();
    if (!threw) failures.push_back("a D32S8 depth surface over another surface's stencil plane whose seed threw: the injected fault did not leave the bind");
    device.WaitIdle();
    RequireCount(DepthFastClearHarness::Draw(device, shaders, faulted, {.depthTest = false, .stencilTest = true, .stencil = equalOne}), Covered, "a D32S8 depth surface over another surface's stencil plane whose seed threw, bound again and tested EQUAL to the owner's stencil 1");
}

std::uint64_t DepthPlaneFills() {
    return AgcDriver::Graphics::DirectDepthCopyCount() + AgcDriver::Graphics::DepthUploadCount() + AgcDriver::Graphics::DepthTransferPassCount();
}

void DemandZeroPlaneCase(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* plane) {
    if (!AgcDriver::GuestMemory::WriteWatched() || !AgcDriver::GuestMemory::Watched(Address(plane), PlaneBytes)) {
        std::puts("demand-zero seed: the plane is not write-watched here, so a sync after the seed is not checked");
        return;
    }
    const auto seeds = AgcDriver::Graphics::DepthSurfaceSeedCount();
    const auto fills = DepthPlaneFills();
    RequireCount(DepthFastClearHarness::Draw(device, shaders, Plane(plane, VK_FORMAT_D32_SFLOAT, 0, ZSwizzle()), {}), 0, "a new D32 depth surface over a plane nothing has touched, tested LESS at 0.5");
    RequireCount(SeedsSince(seeds), 1, "a new D32 depth surface over a plane nothing has touched, surfaces seeded from guest memory");
    if (const auto refilled = DepthPlaneFills() - fills; refilled != 0u) failures.push_back("a new D32 depth surface over a plane nothing has touched: the same bind filled the seeded surface again " + std::to_string(refilled) + " times, expected 0");
}

void PendingChainCase(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* plane) {
    Fill(plane, VK_FORMAT_D32_SFLOAT);
    StoreRow(device, plane, VK_FORMAT_D32_SFLOAT, 1, 1);
    AgcDriver::Graphics::StorageTexture::FlushPending(Address(plane), PlaneBytes, nullptr, "test");
    device.WaitIdle();
    Require(AgcDriver::Graphics::StorageTexture::FindLive(Address(plane), PlaneBytes) != nullptr, "the two-level chain's storage image is not held after its first write was flushed");
    static_cast<void>(DepthFastClearHarness::Draw(device, shaders, Plane(plane, VK_FORMAT_D32_SFLOAT), {}));
    Require(AgcDriver::Graphics::DepthSurfaceAt(Address(plane)), "the drawn depth surface is not live");
    StoreRow(device, plane, VK_FORMAT_D32_SFLOAT, 1, 1);
    Require(AgcDriver::Graphics::StorageTexture::FindPending(Address(plane), 1) != nullptr, "level 1 of a two-level chain over the depth plane is not pending in a storage image");
    const auto words = StorageView(Address(plane), Format32Float, 1, 0, 1);
    const auto resource = AgcDriver::Graphics::DecodeTextureResource(words);
    std::shared_ptr<AgcDriver::Graphics::Texture> texture;
    try {
        texture = AgcDriver::Graphics::DepthSurfaceTexture(AgcDriver::VulkanDeviceTestAccess::ContextOf(device), words, resource, {});
    } catch (const std::runtime_error& error) {
        Require(false, std::string("a two-level view of a depth plane that a pending storage write covers was refused: ") + error.what());
    }
    Require(texture == nullptr, "a two-level view of a depth plane that a pending storage write covers was served from the depth plane");
    Require(!AgcDriver::Graphics::DepthSurfaceAt(Address(plane)), "a pending storage write over the depth plane, then a two-level view: the depth surface stayed live");
}

struct ChainAfterRetire {
    std::vector<std::uint8_t> level0;
    std::vector<std::uint8_t> level1;
    bool disjoint = false;
};

ChainAfterRetire RetireOverPendingChain(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* plane, bool depthWrite) {
    Fill(plane, VK_FORMAT_D32_SFLOAT);
    StoreRow(device, plane, VK_FORMAT_D32_SFLOAT, 1, 1);
    AgcDriver::Graphics::StorageTexture::FlushPending(Address(plane), PlaneBytes, nullptr, "test");
    device.WaitIdle();
    static_cast<void>(DepthFastClearHarness::Draw(device, shaders, Plane(plane, VK_FORMAT_D32_SFLOAT), {.depthTest = true, .depthWrite = depthWrite, .depthCompare = VK_COMPARE_OP_ALWAYS}));
    Require(AgcDriver::Graphics::DepthSurfaceAt(Address(plane)), "the drawn depth surface of the retire case is not live");
    StoreRow(device, plane, VK_FORMAT_D32_SFLOAT, 1, 1);
    const auto words = StorageView(Address(plane), Format32Float, 1, 0, 1);
    const auto resource = AgcDriver::Graphics::DecodeTextureResource(words);
    const auto texture = AgcDriver::Graphics::DepthSurfaceTexture(AgcDriver::VulkanDeviceTestAccess::ContextOf(device), words, resource, {});
    Require(texture == nullptr && !AgcDriver::Graphics::DepthSurfaceAt(Address(plane)), "a two-level view over a drawn depth plane under a pending storage write did not retire the surface");
    AgcDriver::Graphics::StorageTexture::FlushPending(Address(plane), PlaneBytes, nullptr, "test");
    device.WaitIdle();
    AgcDriver::GuestMemory::FlushGpuWrites(Address(plane), PlaneBytes);
    const auto geometry = AgcDriver::Graphics::DescribeSurface(resource);
    const auto& first = geometry.mips.at(0);
    const auto& second = geometry.mips.at(1);
    ChainAfterRetire result;
    result.level0.assign(plane + first.tiledOffset, plane + first.tiledOffset + first.tiledSize);
    result.level1.assign(plane + second.tiledOffset, plane + second.tiledOffset + second.tiledSize);
    result.disjoint = first.tiledOffset + first.tiledSize <= second.tiledOffset || second.tiledOffset + second.tiledSize <= first.tiledOffset;
    return result;
}

void RetireStoresDepthCase(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* control, std::uint8_t* drawn) {
    const auto kept = RetireOverPendingChain(device, shaders, control, false);
    const auto stored = RetireOverPendingChain(device, shaders, drawn, true);
    Require(stored.level1 == kept.level1, "a retire that stored depth drawn only on the GPU changed level 1 of the chain a pending storage write produced");
    if (!stored.disjoint) return;
    std::size_t half = 0;
    for (std::size_t offset = 0; offset + sizeof(float) <= stored.level0.size(); offset += sizeof(float)) {
        float value = 0.0f;
        std::memcpy(&value, stored.level0.data() + offset, sizeof(value));
        half += value == 0.5f;
    }
    Require(half == stored.level0.size() / sizeof(float), "the retire left " + std::to_string(stored.level0.size() / sizeof(float) - half) + " of the drawn depth plane's texels without the depth drawn on the GPU");
}

void RetiredSurfaceCase(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* plane, std::uint8_t* metadata, bool clearFirst, const std::string& what) {
    Fill(plane, VK_FORMAT_D32_SFLOAT);
    std::memset(metadata, 0xff, DepthFastClearHarness::HtileBytes);
    const auto htile = Address(metadata);
    static_cast<void>(DepthFastClearHarness::Draw(device, shaders, Plane(plane, VK_FORMAT_D32_SFLOAT, htile), {}));
    AgcDriver::Graphics::RetireDepthSurfaces(AgcDriver::VulkanDeviceTestAccess::ContextOf(device).device, Address(plane), PlaneBytes);
    Require(!AgcDriver::Graphics::DepthSurfaceAt(Address(plane)), what + ": the depth surface was not retired");
    if (clearFirst) AgcDriver::Graphics::NoteHtileDepthClear(htile, DepthFastClearHarness::HtileBytes);
    StoreRow(device, plane, VK_FORMAT_D32_SFLOAT, 1);
    Require(AgcDriver::Graphics::StorageTexture::FindPending(Address(plane), PlaneBytes) != nullptr, what + ": the stored row is not pending in a storage image");
    if (!clearFirst) AgcDriver::Graphics::NoteHtileDepthClear(htile, DepthFastClearHarness::HtileBytes);
    RequireCount(DepthFastClearHarness::Draw(device, shaders, Plane(plane, VK_FORMAT_D32_SFLOAT, htile), {}), clearFirst ? RowPasses : Covered, what);
}

std::uint32_t TexelsNot(const std::vector<std::uint32_t>& texels, std::uint32_t bits) {
    return static_cast<std::uint32_t>(std::count_if(texels.begin(), texels.end(), [&](std::uint32_t texel) { return texel != bits; }));
}

bool BindThrowsInjected(const AgcDriver::Graphics::Context& context, const AgcDriver::Graphics::DepthTarget& target, AgcDriver::Graphics::DepthPublicationStage stage) {
    faultStage = stage;
    AgcDriver::Graphics::SetDepthPublicationHookForTest(FaultWhileSeeding);
    bool threw = false;
    try {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        AgcDriver::Graphics::DepthSurfaceView(context, target);
    } catch (const std::runtime_error& error) {
        threw = std::string(error.what()).find("injected seed fault") != std::string::npos;
    }
    AgcDriver::Graphics::SetDepthPublicationHookForTest(nullptr);
    faultStage.reset();
    return threw;
}

void OwedClearCase(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* depth, std::uint8_t* ownerStencil, std::uint8_t* boundStencil, std::uint8_t* metadata) {
    FillPlane(depth, 0.25f);
    std::memset(metadata, 0xff, DepthFastClearHarness::HtileBytes);
    const DepthFastClearHarness::Surface owner{Address(depth), Address(ownerStencil), Address(metadata), false, VK_FORMAT_D32_SFLOAT_S8_UINT, 0, 0, ZSwizzle()};
    const DepthFastClearHarness::Surface bound{Address(depth), Address(boundStencil), 0, false, VK_FORMAT_D32_SFLOAT_S8_UINT, 0, 0, ZSwizzle()};
    static_cast<void>(DepthFastClearHarness::Draw(device, shaders, owner, {}));
    AgcDriver::Graphics::NoteHtileDepthClear(Address(metadata), DepthFastClearHarness::HtileBytes);
    auto target = Target(bound);
    target.clearDepth = 0.75f;
    const auto seeds = AgcDriver::Graphics::DepthSurfaceSeedCount();
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        AgcDriver::Graphics::DepthSurfaceView(AgcDriver::VulkanDeviceTestAccess::ContextOf(device), target);
    }
    const std::string what = "a new D32S8 depth surface with DB_DEPTH_CLEAR 0.75 over a 0.25 plane whose owner with HTILE (DB_DEPTH_CLEAR 1.0) owes a depth clear, bound with another stencil plane and no HTILE";
    RequireCount(SeedsSince(seeds), 0, what + ", surfaces seeded from guest memory");
    const auto texels = ReadDepthImage(device, target);
    if (const auto wrong = TexelsNot(texels, 0x3f400000u); wrong != 0u) failures.push_back(what + ", read back from the depth image: " + std::to_string(wrong) + " of " + std::to_string(texels.size()) + " texels are not its clear value 0.75");
    RequireCount(DepthFastClearHarness::Draw(device, shaders, bound, {}), Covered, what + ", drawn and tested LESS at 0.5");
}

void DisplacedOwnerCase(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* plane, std::uint8_t* unrelated) {
    const DepthFastClearHarness::Surface owner{Address(plane), 0, 0, false, VK_FORMAT_D16_UNORM};
    RequireCount(DepthFastClearHarness::Draw(device, shaders, owner, {.depthTest = true, .depthWrite = true, .depthCompare = VK_COMPARE_OP_ALWAYS}), Covered, "a D16 depth surface writing depth 0.5 on the GPU");
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    if (!BindThrowsInjected(context, Target(Plane(plane, VK_FORMAT_D32_SFLOAT, 0, ZSwizzle())), AgcDriver::Graphics::DepthPublicationStage::SeedBaselineTaken)) failures.push_back("a new D32 depth surface at a D16 surface's plane whose seed threw: the injected fault did not leave the bind");
    static_cast<void>(DepthFastClearHarness::Draw(device, shaders, Plane(unrelated, VK_FORMAT_D32_SFLOAT), {}));
    RequireCount(DepthFastClearHarness::Draw(device, shaders, owner, {}), 0, "a D16 depth surface that wrote 0.5, bound again after a new D32 surface at its plane failed to seed and another surface was created, tested LESS at 0.5");
}

void PaddingOutOfRangeCase(AgcDriver::VulkanDevice& device, std::uint8_t* plane) {
    constexpr std::uint32_t pipeBankXor = 0xcf00u;
    constexpr float quarter = 0.25f;
    for (std::uint32_t block = 0; block < 4u; ++block) FillPlane(plane + block * PlaneBytes, -1.0f);
    for (std::uint32_t y = 0; y < PatternExtent.height; ++y) {
        for (std::uint32_t x = 0; x < PatternExtent.width; ++x) std::memcpy(plane + TexelOffset(x, y, pipeBankXor), &quarter, sizeof(quarter));
    }
    auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    context.depthRangeUnrestricted = false;
    auto target = Target(Plane(plane, VK_FORMAT_D32_SFLOAT, 0, ZSwizzle()), PatternExtent);
    target.pipeBankXor = pipeBankXor;
    const auto seeds = AgcDriver::Graphics::DepthSurfaceSeedCount();
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        AgcDriver::Graphics::DepthSurfaceView(context, target);
    }
    const std::string what = "a new 192x136 D32 depth surface with pipe/bank XOR 0xcf00 whose texels hold 0.25 and whose block padding holds -1.0, on a device without VK_EXT_depth_range_unrestricted";
    RequireCount(SeedsSince(seeds), 1, what + ", surfaces seeded from guest memory");
    const auto texels = ReadDepthImage(device, target);
    if (const auto wrong = TexelsNot(texels, 0x3e800000u); wrong != 0u) failures.push_back(what + ", read back from the depth image: " + std::to_string(wrong) + " of " + std::to_string(texels.size()) + " texels are not 0.25");
}

void OverlappedOwnerCase(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* plane) {
    for (std::uint32_t block = 0; block < 4u; ++block) FillPlane(plane + block * PlaneBytes, 0.75f);
    RequireCount(DepthFastClearHarness::Draw(device, shaders, Plane(plane, VK_FORMAT_D32_SFLOAT, 0, ZSwizzle()), {.depthTest = true, .depthWrite = true, .depthCompare = VK_COMPARE_OP_ALWAYS, .minDepth = 0.25f, .maxDepth = 0.25f}, PatternExtent, PatternPixels), PatternTexels, "a 192x136 D32 depth surface writing depth 0.25 on the GPU over a 0.75 plane");
    const auto seeds = AgcDriver::Graphics::DepthSurfaceSeedCount();
    const std::string what = "a new 64x32 D32 depth surface over the second 64 KiB block of a 192x136 surface that wrote 0.25 there on the GPU after the CPU wrote 0.75";
    RequireCount(DepthFastClearHarness::Draw(device, shaders, Plane(plane + PlaneBytes, VK_FORMAT_D32_SFLOAT, 0, ZSwizzle()), {}), 0, what + ", tested LESS at 0.5");
    RequireCount(SeedsSince(seeds), 1, what + ", surfaces seeded from guest memory");
}

void DisplacedMirrorOwnerCase(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* plane) {
    for (std::uint32_t block = 0; block < 4u; ++block) FillPlane(plane + block * PlaneBytes, 0.75f);
    RequireCount(DepthFastClearHarness::Draw(device, shaders, Plane(plane, VK_FORMAT_D32_SFLOAT, 0, ZSwizzle()), {.depthTest = true, .depthWrite = true, .depthCompare = VK_COMPARE_OP_ALWAYS, .minDepth = 0.25f, .maxDepth = 0.25f}), Covered, "a 64x32 D32 depth surface writing depth 0.25 on the GPU over a 0.75 plane");
    const auto target = Target(Plane(plane, VK_FORMAT_D32_SFLOAT, 0, ZSwizzle()), PatternExtent);
    const auto seeds = AgcDriver::Graphics::DepthSurfaceSeedCount();
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        AgcDriver::Graphics::DepthSurfaceView(AgcDriver::VulkanDeviceTestAccess::ContextOf(device), target);
    }
    const std::string what = "a new 192x136 D32 depth surface at the plane of a 64x32 surface that wrote 0.25 there on the GPU after the CPU wrote 0.75";
    RequireCount(SeedsSince(seeds), 1, what + ", surfaces seeded from guest memory");
    const auto texels = ReadDepthImage(device, target);
    std::uint32_t wrong = 0;
    for (std::uint32_t y = 0; y < PatternExtent.height; ++y) {
        for (std::uint32_t x = 0; x < PatternExtent.width; ++x) wrong += texels[static_cast<std::size_t>(y) * PatternExtent.width + x] != (x < Width && y < Height ? 0x3e800000u : 0x3f400000u);
    }
    if (wrong != 0u) failures.push_back(what + ", read back from the depth image: " + std::to_string(wrong) + " of " + std::to_string(texels.size()) + " texels hold neither the old surface's 0.25 inside its extent nor the CPU's 0.75 outside it");
}

void CurrentOwnerImageCase(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* depth, std::uint8_t* firstStencil, std::uint8_t* secondStencil) {
    const DepthFastClearHarness::Surface first{Address(depth), Address(firstStencil), 0, false, VK_FORMAT_D32_SFLOAT_S8_UINT};
    const DepthFastClearHarness::Surface second{Address(depth), Address(secondStencil), 0, false, VK_FORMAT_D32_SFLOAT_S8_UINT};
    RequireCount(DepthFastClearHarness::Draw(device, shaders, first, {.depthTest = true, .depthWrite = true, .depthCompare = VK_COMPARE_OP_ALWAYS}), Covered, "a D32S8 depth surface writing depth 0.5");
    RequireCount(DepthFastClearHarness::Draw(device, shaders, second, {.depthTest = true, .depthWrite = true, .depthCompare = VK_COMPARE_OP_ALWAYS, .minDepth = 0.75f, .maxDepth = 0.75f}), Covered, "a second D32S8 depth surface at the same depth plane with another stencil plane, writing depth 0.75");
    const auto texels = ReadDepthImage(device, Target(second));
    if (const auto wrong = TexelsNot(texels, 0x3f400000u); wrong != 0u) failures.push_back("the depth image read back for the second of two D32S8 surfaces sharing a depth plane: " + std::to_string(wrong) + " of " + std::to_string(texels.size()) + " texels are not its depth 0.75");
}

struct HeldBatchResult {
    bool checked = false;
    std::string unchecked;
    bool recorded = false;
    bool discarded = false;
    bool waited = false;
    std::uint64_t released = 0;
};

HeldBatchResult DiscardBehindHeldBatch(AgcDriver::VulkanDevice& device, const std::function<bool(const AgcDriver::Graphics::Context&)>& record, std::uint8_t* unrelated) {
    using namespace std::chrono_literals;
    HeldBatchResult result;
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    PFN_vkSignalSemaphore signal = nullptr;
    try {
        signal = context.Function<PFN_vkSignalSemaphore>("vkSignalSemaphore");
    } catch (const std::runtime_error&) {
        try {
            signal = context.Function<PFN_vkSignalSemaphoreKHR>("vkSignalSemaphoreKHR");
        } catch (const std::runtime_error&) {
        }
    }
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    const VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
    VkSemaphore hold = VK_NULL_HANDLE;
    if (signal == nullptr || context.Function<PFN_vkCreateSemaphore>("vkCreateSemaphore")(context.device, &info, nullptr, &hold) != VK_SUCCESS) {
        result.unchecked = "the device has no timeline semaphores to hold a batch on the GPU";
        return result;
    }
    std::atomic<bool> released{false};
    const auto release = [&] {
        if (released.exchange(true)) return false;
        VkSemaphoreSignalInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        signalInfo.semaphore = hold;
        signalInfo.value = 1;
        static_cast<void>(signal(context.device, &signalInfo));
        return true;
    };
    bool timedOut = false;
    std::jthread watchdog([&](std::stop_token stop) {
        for (int tick = 0; tick < 200 && !stop.stop_requested(); ++tick) std::this_thread::sleep_for(100ms);
        if (!stop.stop_requested() && release()) timedOut = true;
    });
    device.WaitIdle();
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        auto* recorder = AgcDriver::Graphics::Recorder::Active();
        if (recorder != nullptr && recorder->Device() == context.device) {
            result.checked = true;
            result.recorded = record(context);
            recorder->HoldNextSubmit(hold, 1);
            recorder->Submit();
            const auto releasedBefore = AgcDriver::Graphics::DepthSurfacesReleased();
            AgcDriver::Graphics::DepthSurfaceView(context, Target(Plane(unrelated, VK_FORMAT_D16_UNORM)));
            result.released = AgcDriver::Graphics::DepthSurfacesReleased() - releasedBefore;
            AgcDriver::Graphics::Recorder::FailNextSubmitsForTest(1);
            try {
                recorder->Submit();
            } catch (const std::runtime_error& error) {
                result.discarded = std::string(error.what()).find("injected failure") != std::string::npos;
            }
        } else {
            result.unchecked = "no active recorder";
        }
    }
    static_cast<void>(release());
    watchdog.request_stop();
    watchdog.join();
    result.waited = timedOut;
    device.WaitIdle();
    context.Function<PFN_vkDestroySemaphore>("vkDestroySemaphore")(context.device, hold, nullptr);
    return result;
}

void RequireHeldBatch(const HeldBatchResult& result, const std::string& what) {
    if (!result.checked) {
        skipped.push_back(what + ": " + result.unchecked);
        return;
    }
    if (!result.discarded) failures.push_back(what + ": the batch after the held one was not discarded by the injected submit failure");
    if (result.waited) failures.push_back(what + ": creating another surface waited for the held batch");
}

void PendingBatchFaultCase(AgcDriver::VulkanDevice& device, std::uint8_t* plane, std::uint8_t* unrelated, bool uneven) {
    FillPlane(plane, 0.25f);
    if (uneven) {
        for (std::size_t offset = 0; offset < PlaneBytes; offset += 2u * sizeof(float)) FillDepth(plane + offset, 0.75f);
    }
    const auto result = DiscardBehindHeldBatch(device, [&](const AgcDriver::Graphics::Context& context) { return BindThrowsInjected(context, Target(Plane(plane, VK_FORMAT_D32_SFLOAT, 0, ZSwizzle())), AgcDriver::Graphics::DepthPublicationStage::SeedRecorded); }, unrelated);
    const std::string what = std::string(uneven ? "an uneven" : "a uniform") + " depth surface seed that threw after recording into a batch then held on the GPU";
    if (result.checked && !result.recorded) failures.push_back(what + ": the injected fault did not leave the bind");
    RequireHeldBatch(result, what);
}

void PendingCopySourceCase(AgcDriver::VulkanDevice& device, std::uint8_t* depth, std::uint8_t* stencil, std::uint8_t* otherDepth, std::uint8_t* otherStencil, std::uint8_t* unrelated) {
    const auto source = Target({Address(depth), Address(stencil), 0, false, VK_FORMAT_D32_SFLOAT_S8_UINT});
    const auto depthTaker = Target({Address(depth), Address(otherStencil), 0, false, VK_FORMAT_D32_SFLOAT_S8_UINT});
    const auto stencilTaker = Target({Address(otherDepth), Address(stencil), 0, false, VK_FORMAT_D32_SFLOAT_S8_UINT});
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
        AgcDriver::Graphics::DepthSurfaceView(context, source);
        AgcDriver::Graphics::DepthSurfaceView(context, depthTaker);
    }
    const auto result = DiscardBehindHeldBatch(device, [&](const AgcDriver::Graphics::Context& context) {
        AgcDriver::Graphics::DepthSurfaceView(context, stencilTaker);
        return true;
    }, unrelated);
    const std::string what = "a D32S8 depth surface whose last plane was copied into a batch then held on the GPU";
    if (result.checked && result.released == 0u) failures.push_back(what + ": the surface it was copied from was not released into the next batch");
    RequireHeldBatch(result, what);
}
}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto shaders = DepthFastClearHarness::Compile(*device);
        GuestBlock block;
        GuestBlock untouched(PlaneBytes, false);
        NewSurfaceCase(*device, shaders, block.Plane(0), VK_FORMAT_D32_SFLOAT, false, "a new D32 depth surface over a pending R32F storage write");
        NewSurfaceCase(*device, shaders, block.Plane(1), VK_FORMAT_D16_UNORM, false, "a new D16 depth surface over a pending R16 unorm storage write");
        NewSurfaceCase(*device, shaders, block.Plane(2), VK_FORMAT_D32_SFLOAT, true, "a new D32 depth surface over a flushed R32F storage write");
        NewSurfaceCase(*device, shaders, block.Plane(7), VK_FORMAT_D32_SFLOAT, false, "a new D32 depth surface over a pending R32F storage write through a two-layer 2D array view", 2);
        NewSurfaceCase(*device, shaders, block.Plane(27), VK_FORMAT_D32_SFLOAT, true, "a new mirrored D32 depth surface over a flushed R32F storage write", 1, ZSwizzle());
        GuestPlanePatternCase(*device, shaders, block.Plane(23));
        SeedRaceCase(*device, shaders, block.Plane(28));
        IncompatibleStorageCase(*device, shaders, block.Plane(29));
        PartialWatchCollectCase(*device, shaders, block.Plane(30));
        DemandZeroPlaneCase(*device, shaders, untouched.Plane(0));
        SeedFaultCase(*device, shaders, block.Plane(34), AgcDriver::Graphics::DepthPublicationStage::SeedBaselineTaken, "a depth surface seed that threw after taking its baseline");
        SeedFaultCase(*device, shaders, block.Plane(38), AgcDriver::Graphics::DepthPublicationStage::GuestPlaneSeeded, "a depth surface seed that threw after reading its plane");
        SeedFaultCase(*device, shaders, block.Plane(42), AgcDriver::Graphics::DepthPublicationStage::SeedRecorded, "a depth surface seed that threw after recording its copy");
        AllocationFailureCase(*device, shaders, block.Plane(79));
        PipeBankXorCase(*device, shaders, block.Plane(46));
        RawDepthBitsCase(*device, shaders, block.Plane(50), 0x80000000u, "a new D32 depth surface over a plane of -0.0");
        RawDepthBitsCase(*device, shaders, block.Plane(51), 0x00000001u, "a new D32 depth surface over a plane of the smallest denormal");
        OutOfRangePlaneCase(*device, block.Plane(64), false, "a new D32 depth surface over a plane of -1.0 on a device without VK_EXT_depth_range_unrestricted");
        if (AgcDriver::VulkanDeviceTestAccess::ContextOf(*device).depthRangeUnrestricted) OutOfRangePlaneCase(*device, block.Plane(65), true, "a new D32 depth surface over a plane of -1.0 on a device with VK_EXT_depth_range_unrestricted");
        MismatchedOwnerCase(*device, shaders, block.Plane(52));
        SeedFaultOwnerCase(*device, shaders, block.Plane(56), block.Plane(57), block.Plane(58));
        SeedCopyRaceCase(*device, shaders, block.Plane(59), AgcDriver::Graphics::DepthPublicationStage::GuestPlaneSeeded, "a depth surface whose plane the CPU inverted after the seed read it");
        SeedCopyRaceCase(*device, shaders, block.Plane(63), AgcDriver::Graphics::DepthPublicationStage::SeedRecorded, "a depth surface whose plane the CPU inverted after the seed recorded its copy");
        OwedClearCase(*device, shaders, block.Plane(24), block.Plane(25), block.Plane(26), block.Plane(31));
        DisplacedOwnerCase(*device, shaders, block.Plane(35), block.Plane(36));
        PaddingOutOfRangeCase(*device, block.Plane(71));
        OverlappedOwnerCase(*device, shaders, block.Plane(67));
        DisplacedMirrorOwnerCase(*device, shaders, block.Plane(75));
        CurrentOwnerImageCase(*device, shaders, block.Plane(39), block.Plane(40), block.Plane(41));

        auto* cleared = block.Plane(3);
        const auto htile = Address(block.Plane(4));
        Fill(cleared, VK_FORMAT_D32_SFLOAT);
        StoreRow(*device, cleared, VK_FORMAT_D32_SFLOAT, 1);
        AgcDriver::Graphics::NoteHtileDepthClear(htile, DepthFastClearHarness::HtileBytes);
        RequireCount(DepthFastClearHarness::Draw(*device, shaders, Plane(cleared, VK_FORMAT_D32_SFLOAT, htile), {}), Covered, "a new D32 depth surface whose HTILE was cleared after a storage write");

        auto* stenciled = block.Plane(5);
        const DepthFastClearHarness::Surface depthStencil{Address(stenciled), Address(block.Plane(6)), 0, false, VK_FORMAT_D32_SFLOAT_S8_UINT};
        constexpr VkStencilOpState equalZero{VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0xffu, 0u, 0u};
        Fill(stenciled, VK_FORMAT_D32_SFLOAT);
        StoreRow(*device, stenciled, VK_FORMAT_D32_SFLOAT, 1);
        RequireCount(DepthFastClearHarness::Draw(*device, shaders, depthStencil, {}), RowPasses, "a new D32S8 depth surface over a pending R32F storage write");
        RequireCount(DepthFastClearHarness::Draw(*device, shaders, depthStencil, {.depthTest = false, .stencilTest = true, .stencil = equalZero}), Covered, "the stencil plane of a new D32S8 depth surface that took a storage write, tested EQUAL to its clear value 0");
        PendingChainCase(*device, shaders, block.Plane(8));
        RetireStoresDepthCase(*device, shaders, block.Plane(9), block.Plane(10));
        RetiredSurfaceCase(*device, shaders, block.Plane(19), block.Plane(20), true, "a retired D32 surface with HTILE, a depth clear noted on it, then a storage write over its whole plane, bound again");
        RetiredSurfaceCase(*device, shaders, block.Plane(21), block.Plane(22), false, "a retired D32 surface with HTILE, a storage write over its whole plane, then a depth clear noted on it, bound again");

        constexpr VkStencilOpState equalOne{VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0xffu, 0u, 1u};
        const DepthFastClearHarness::Surface fresh{Address(block.Plane(11)), Address(block.Plane(12)), 0, false, VK_FORMAT_D32_SFLOAT_S8_UINT};
        StoreStencilRow(*device, block.Plane(12));
        Require(AgcDriver::Graphics::StorageTexture::FindPending(Address(block.Plane(12)), Width * Height) != nullptr, "the stencil row is not pending in an R8_UINT storage image");
        RequireCount(DepthFastClearHarness::Draw(*device, shaders, fresh, {.depthTest = false, .stencilTest = true, .stencil = equalOne}), Threads, "a new D32S8 depth surface over a pending R8_UINT storage write of its stencil plane, tested EQUAL to the stored 1");
        const DepthFastClearHarness::Surface drawn{Address(block.Plane(13)), Address(block.Plane(14)), 0, false, VK_FORMAT_D32_SFLOAT_S8_UINT};
        RequireCount(DepthFastClearHarness::Draw(*device, shaders, drawn, {.depthTest = false, .stencilTest = true, .stencil = equalZero}), Covered, "a D32S8 depth surface drawn before its stencil plane is written, tested EQUAL to its clear value 0");
        StoreStencilRow(*device, block.Plane(14));
        RequireCount(DepthFastClearHarness::Draw(*device, shaders, drawn, {.depthTest = false, .stencilTest = true, .stencil = equalOne}), Threads, "a D32S8 depth surface drawn again after an R8_UINT storage write of its stencil plane, tested EQUAL to the stored 1");
        RequireCount(DepthFastClearHarness::Draw(*device, shaders, drawn, {.depthTest = false, .stencilTest = true, .stencil = equalOne}), Threads, "a D32S8 depth surface drawn a third time with no storage write in between, tested EQUAL to the stored 1");

        const auto swizzle = AgcDriver::Graphics::XorSwizzleMode(AgcDriver::Graphics::TextureTileMode::kZ64KBX);
        const DepthFastClearHarness::Surface synced{Address(block.Plane(15)), Address(block.Plane(16)), 0, false, VK_FORMAT_D32_SFLOAT_S8_UINT, 0, 0, swizzle};
        RequireCount(DepthFastClearHarness::Draw(*device, shaders, synced, {.depthTest = false, .stencilTest = true, .stencil = equalZero}), Covered, "a mirrored D32S8 depth surface before its stencil plane changes, tested EQUAL to 0");
        std::memset(block.Plane(16), 3, PlaneBytes);
        StoreStencilRow(*device, block.Plane(16));
        RequireCount(DepthFastClearHarness::Draw(*device, shaders, synced, {.depthTest = false, .stencilTest = true, .stencil = equalOne}), Threads, "a mirrored D32S8 depth surface whose stencil plane memory changed and then took a pending R8_UINT storage write, tested EQUAL to 1 on the next bind");

        constexpr VkStencilOpState writeOne{VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xffu, 0xffu, 1u};
        constexpr VkStencilOpState writeTwo{VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xffu, 0xffu, 2u};
        constexpr VkStencilOpState equalTwo{VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0xffu, 0u, 2u};
        const DepthFastClearHarness::Surface stored{Address(block.Plane(17)), Address(block.Plane(18)), 0, false, VK_FORMAT_D32_SFLOAT_S8_UINT, 0, 0, swizzle};
        RequireCount(DepthFastClearHarness::Draw(*device, shaders, stored, {.depthTest = false, .stencilTest = true, .stencil = writeOne}), Covered, "a mirrored D32S8 depth surface writing stencil 1");
        static_cast<void>(AgcDriver::Graphics::FlushDepthPlanes(Address(block.Plane(18)), PlaneBytes));
        RequireCount(DepthFastClearHarness::Draw(*device, shaders, stored, {.depthTest = false, .stencilTest = true, .stencil = writeTwo}), Covered, "a mirrored D32S8 depth surface writing stencil 2 after its stencil plane was stored through a read");
        RequireCount(DepthFastClearHarness::Draw(*device, shaders, stored, {.depthTest = false, .stencilTest = true, .stencil = equalTwo}), Covered, "a mirrored D32S8 depth surface whose stencil plane was stored through a read and drawn again, tested EQUAL to 2 on the next bind");
        PendingBatchFaultCase(*device, block.Plane(32), block.Plane(33), false);
        PendingBatchFaultCase(*device, block.Plane(84), block.Plane(85), true);
        MultisampleAbortCase(*device, block.Plane(83));
        PendingCopySourceCase(*device, block.Plane(37), block.Plane(43), block.Plane(44), block.Plane(45), block.Plane(47));

        if (!failures.empty()) {
            std::string text;
            for (const auto& failure : failures) text += failure + "\n";
            throw std::runtime_error(text);
        }
        for (const auto& reason : skipped) std::printf("not checked: %s\n", reason.c_str());
        std::printf("depth surface from storage tests passed, %zu cases not checked\n", skipped.size());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
