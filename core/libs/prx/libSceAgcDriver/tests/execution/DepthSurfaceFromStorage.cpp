#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureSwizzleEquations.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "DepthFastClearHarness.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
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
constexpr std::size_t BlockBytes = 34 * PlaneBytes;
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

std::uint64_t TexelOffset(std::uint32_t x, std::uint32_t y) {
    static const auto geometry = [] {
        AgcDriver::Graphics::GuestTextureResource resource{};
        resource.width = PatternExtent.width;
        resource.height = PatternExtent.height;
        resource.mipCount = 1;
        resource.allocatedMipCount = 1;
        resource.tileMode = AgcDriver::Graphics::TextureTileMode::kZ64KBX;
        resource.dimension = AgcDriver::Graphics::TextureDimension::k2D;
        resource.format = Format32Float;
        return AgcDriver::Graphics::DescribeSurface(resource);
    }();
    static const auto layout = AgcDriver::Graphics::ThinBlockLayout(AgcDriver::Graphics::TextureTileMode::kZ64KBX, 4u);
    static const auto* equation = AgcDriver::Graphics::FindTextureSwizzleEquation(ZSwizzle(), 4u);
    Require(equation != nullptr && geometry.guestBytes <= 4u * PlaneBytes, "the SW_64KB_Z_X equation is missing or the 192x136 plane does not fit four slots");
    const auto& mip = geometry.mips.front();
    return mip.tiledOffset + (static_cast<std::uint64_t>(y / layout[2]) * mip.blocksPerRow + x / layout[1]) * layout[0] + EquationOffset(*equation, x, y);
}

float PatternDepth(std::uint32_t x, std::uint32_t y, bool inverted) {
    const bool distant = (((x >> 3u) ^ (y >> 2u) ^ (x >> 6u) ^ ((y * 5u) >> 5u)) & 1u) != 0u;
    return distant != inverted ? 0.75f : 0.25f;
}

void WritePattern(std::uint8_t* plane, bool inverted) {
    for (std::uint32_t y = 0; y < PatternExtent.height; ++y) {
        for (std::uint32_t x = 0; x < PatternExtent.width; ++x) {
            const float depth = PatternDepth(x, y, inverted);
            std::memcpy(plane + TexelOffset(x, y), &depth, sizeof(depth));
        }
    }
}

void FillPlane(std::uint8_t* plane, float depth) {
    for (std::size_t offset = 0; offset < PlaneBytes; offset += sizeof(depth)) std::memcpy(plane + offset, &depth, sizeof(depth));
}

void RequirePattern(AgcDriver::VulkanDevice& device, const DepthFastClearHarness::Shaders& shaders, std::uint8_t* plane, bool inverted, const std::string& what) {
    static_cast<void>(DepthFastClearHarness::Draw(device, shaders, Plane(plane, VK_FORMAT_D32_SFLOAT, 0, ZSwizzle()), {}, PatternExtent, PatternPixels));
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

        if (!failures.empty()) {
            std::string text;
            for (const auto& failure : failures) text += failure + "\n";
            throw std::runtime_error(text);
        }
        std::puts("depth surface from storage tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
