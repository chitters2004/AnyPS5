#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

struct DriverTestAccess {
    static void UseDevice(std::shared_ptr<VulkanDevice> device) { Driver::Get().device = std::move(device); }
    static void Stop() { Driver::Get().stop(); }
    static void StoreBufferFill(const std::shared_ptr<VulkanDevice>& device, std::uint64_t base, std::size_t bytes) {
        Driver::Get().storeBufferFill(0, base, bytes, {0u, 0u, 0u, 0u}, device, false, false);
    }
    static bool Precheck(const QueueState& queue) {
        Submission submission{};
        submission.queue = 0;
        const std::array<std::uint32_t, 1> packet{0xc0002d00u};
        Pm4::DrawParameters draw{};
        draw.indexCount = 3;
        draw.instanceCount = 1;
        draw.indexed = false;
        std::string rejected;
        bool traceIndirect = false;
        return Driver::Get().precheckDraw(queue, submission, 0, packet, draw, rejected, traceIndirect) == DrawVerdict::Drawn;
    }
};

}

namespace {

using namespace AgcDriver::GuestMemory;
using AgcDriver::DriverDetail::DriverTestAccess;
using AgcDriver::Graphics::RuntimeSwitch;
namespace RuntimeSwitches = AgcDriver::Graphics::RuntimeSwitches;

constexpr std::size_t Block = 65536;
constexpr std::uint32_t Side = 1024;
constexpr VkExtent2D Extent{Side, Side};

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint64_t GranulesOf(std::uint64_t address, std::uint64_t bytes) {
    return (address + bytes - 1u) / RangeGranuleBytes - address / RangeGranuleBytes + 1u;
}

std::byte* AllocateBlocks(std::size_t bytes) {
#ifdef _WIN32
    void* block = GuestArena::GuestArenaAllocate_nid_postfix(bytes, Block);
    if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
    Require(block != nullptr, "cannot allocate the guest blocks");
#else
    void* raw = mmap(nullptr, bytes + Block, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Require(raw != MAP_FAILED, "cannot map the guest blocks");
    const auto begin = reinterpret_cast<std::uintptr_t>(raw);
    const auto aligned = (begin + Block - 1) & ~static_cast<std::uintptr_t>(Block - 1);
    if (aligned != begin) munmap(raw, aligned - begin);
    if (aligned + bytes != begin + bytes + Block) munmap(reinterpret_cast<void*>(aligned + bytes), begin + Block - aligned);
    void* block = reinterpret_cast<void*>(aligned);
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
#endif
    std::memset(block, 0, bytes);
    GuestAllocations::Mutation().Add(block, bytes, true, true, true);
    return static_cast<std::byte*>(block);
}

struct Entries {
    std::uint64_t nonDraw = 0;
    std::uint64_t unscoped = 0;
};

Entries Take() {
    const auto counts = TakeRangeVersionCounts();
    return {counts.granules[static_cast<std::size_t>(RangeSource::NonDraw)], counts.unscoped[static_cast<std::size_t>(UnscopedSource::GpuMutex)]};
}

std::vector<std::string> failures;

template<typename TSite>
void RequireRanged(const char* site, std::uint64_t granules, TSite run) {
    RuntimeSwitches::Assign(RuntimeSwitch::NoRangedGpuEntries, false);
    Take();
    run();
    const auto ranged = Take();
    RuntimeSwitches::Assign(RuntimeSwitch::NoRangedGpuEntries, true);
    run();
    const auto unscoped = Take();
    RuntimeSwitches::Assign(RuntimeSwitch::NoRangedGpuEntries, false);
    const auto failed = [&](const std::string& what) {
        std::fprintf(stderr, "FAIL: %s %s\n", site, what.c_str());
        failures.push_back(site);
    };
    if (ranged.unscoped != 0) failed("still moved the unscoped version from its GPU mutex entry");
    else if (ranged.nonDraw != 2u * granules) failed("bumped " + std::to_string(ranged.nonDraw) + " granules instead of its " + std::to_string(granules) + " written granules before and after");
    else if (unscoped.unscoped != 1u || unscoped.nonDraw != 0u) failed("with APS5_NO_RANGED_GPU_ENTRIES did not move the unscoped version alone");
    else std::printf("%s: %llu granules ranged\n", site, static_cast<unsigned long long>(granules));
}

void HoldTests(std::uint64_t base) {
    const auto first = base + RangeGranuleBytes / 2u;
    const auto second = base + 4u * RangeGranuleBytes;
    const auto firstGranules = GranulesOf(first, RangeGranuleBytes);
    const auto secondGranules = GranulesOf(second, 2u * RangeGranuleBytes);
    const auto gapGranules = GranulesOf(second, RangeGranuleBytes);
    Take();
    {
        RangedGpuHold outer(true);
        RangedGpuWrites(first, RangeGranuleBytes);
        {
            std::lock_guard nested(GpuMutex());
            RangedGpuWrites(second, 2u * RangeGranuleBytes);
        }
        const auto during = Take();
        Require(during.unscoped == 0 && during.nonDraw == firstGranules + secondGranules, "a nested hold inside a ranged hold moved the unscoped version or missed its writes");
        GpuMutex().unlock();
        {
            RangedGpuHold gap(true);
            RangedGpuWrites(second, RangeGranuleBytes);
        }
        GpuMutex().lock();
        const auto relocked = Take();
        Require(relocked.unscoped == 1u && relocked.nonDraw == 2u * gapGranules, "a hold taken while a ranged hold was given up did not stay ranged, or the relock skipped the unscoped version");
        RangedGpuWrites(first, RangeGranuleBytes);
    }
    const auto released = Take();
    Require(released.unscoped == 0 && released.nonDraw == 3u * firstGranules + secondGranules, "a ranged hold did not bump its writes again on release, including those after its hold was given up");
    {
        std::lock_guard plain(GpuMutex());
        RangedGpuWrites(first, RangeGranuleBytes);
    }
    const auto plain = Take();
    Require(plain.unscoped == 1u && plain.nonDraw == 0u, "a plain hold took ranged writes");
    std::puts("ranged GPU holds passed");
}

AgcDriver::QueueState ExpandState(std::uint64_t source, std::uint64_t stencilSource, std::uint64_t destination, std::uint64_t stencilDestination, std::uint64_t htile) {
    AgcDriver::QueueState queue;
    queue.userConfig[0x242] = 4;
    queue.context = {
        {0x2d5, 0x2000}, {0x1b6, 0}, {0x1b8, 0}, {0x207, 0}, {0x203, 0x800}, {0x2dc, 0xaa00}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa}, {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240}, {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, (63u << 14u) | 3u}, {0x3b8, 0x9000000}, {0x1e0, 0}, {0xc, 0},
        {0x81, 0x80000000}, {0x90, 0x80000000}, {0x94, 0x80000000}, {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)}, {0x3, 0},
    };
    const auto low = [](std::uint64_t address) { return static_cast<std::uint32_t>(address >> 8u); };
    const auto high = [](std::uint64_t address) { return static_cast<std::uint32_t>((address >> 40u) & 0xffu); };
    const std::uint32_t extent = (Side << 16u) | Side;
    for (const auto [offset, value] : {std::pair{0x000u, 0x60u}, {0x002u, 0u}, {0x005u, low(htile)}, {0x007u, ((Side - 1u) << 16u) | (Side - 1u)}, {0x00au, 0u}, {0x00bu, 0u},
             {0x010u, 0x20000183u}, {0x011u, 0x20000181u}, {0x012u, low(source)}, {0x013u, low(stencilSource)}, {0x014u, low(destination)}, {0x015u, low(stencilDestination)},
             {0x01au, high(source)}, {0x01bu, high(stencilSource)}, {0x01cu, high(destination)}, {0x01du, high(stencilDestination)}, {0x01eu, high(htile)}, {0x200u, 0u}, {0x201u, 0u}, {0x202u, 0x00cc0000u},
             {0x8eu, 0xffffffffu}, {0x8fu, 0u}, {0x2f8u, 0x00108001u}, {0xdu, extent}, {0x82u, extent}, {0x91u, extent}, {0x95u, extent}}) {
        queue.context[offset] = value;
    }
    queue.context[0x10f] = std::bit_cast<std::uint32_t>(Side / 2.0f);
    queue.context[0x110] = std::bit_cast<std::uint32_t>(Side / 2.0f);
    queue.context[0x111] = std::bit_cast<std::uint32_t>(-(Side / 2.0f));
    queue.context[0x112] = std::bit_cast<std::uint32_t>(Side / 2.0f);
    return queue;
}

AgcDriver::QueueState ColorState(std::uint64_t address) {
    AgcDriver::QueueState queue;
    queue.userConfig[0x242] = 4;
    const std::uint32_t extent = (Side << 16u) | Side;
    queue.context = {
        {0x0, 0}, {0x2d5, 0x2000}, {0x1b6, 0}, {0x1b8, 0}, {0x207, 0}, {0x200, 0}, {0x203, 0x800}, {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa}, {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240}, {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0020}, {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, ((Side - 1u) << 14u) | (Side - 1u)}, {0x3b8, 0x9000000}, {0x1e0, 0}, {0xc, 0}, {0xd, extent},
        {0x81, 0x80000000}, {0x82, extent}, {0x90, 0x80000000}, {0x91, extent}, {0x94, 0x80000000}, {0x95, extent},
        {0x318, static_cast<std::uint32_t>(address >> 8u)}, {0x390, static_cast<std::uint32_t>(address >> 40u)}, {0x323, 0}, {0x324, 0},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0}, {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)}, {0x3, 0},
    };
    queue.context[0x10f] = std::bit_cast<std::uint32_t>(Side / 2.0f);
    queue.context[0x110] = std::bit_cast<std::uint32_t>(Side / 2.0f);
    queue.context[0x111] = std::bit_cast<std::uint32_t>(-(Side / 2.0f));
    queue.context[0x112] = std::bit_cast<std::uint32_t>(Side / 2.0f);
    return queue;
}

void RenderControlTests(const std::shared_ptr<AgcDriver::VulkanDevice>& device, std::byte* blocks) {
    constexpr std::uint32_t Samples = 4u;
    const VkExtent2D extent{Side, Side / 4u};
    const auto depthBytes = AgcDriver::Graphics::MultisampledDepthBytes_nid_no_patch(extent, 4u, Samples);
    const auto htileBytes = AgcDriver::Graphics::HtileMetadataBytes(extent);
    const auto address = reinterpret_cast<std::uint64_t>(blocks);
    const auto htile = address + (depthBytes + Block - 1u) / Block * Block;
    AgcDriver::Graphics::DepthTarget target{address, 0u, extent, VK_FORMAT_D32_SFLOAT, 0.0f, 0u, htile, false, Samples};
    target.depthSwizzle = 24u;
    target.iterate256 = target.htilePipeAligned = true;
    target.htileDepthOnly = true;
    const auto context = device->graphicsContext();
    AgcDriver::Graphics::Recorder recorder(context);
    recorder.Activate();
    struct Cleanup {
        AgcDriver::Graphics::Recorder& recorder;
        AgcDriver::Graphics::Context context;
        ~Cleanup() {
            recorder.Sync();
            AgcDriver::Graphics::ClearCachedTextures(context.device);
            AgcDriver::Graphics::ClearDepthSurfaces(context.device);
        }
    } cleanup{recorder, context};
    Require(AgcDriver::Graphics::DepthSurfaceView(context, target) != VK_NULL_HANDLE, "the multisample depth surface was not created");
    RequireRanged("NoteDepthRenderControl", GranulesOf(address, depthBytes) + GranulesOf(htile, htileBytes), [&] { AgcDriver::Graphics::NoteDepthRenderControl({address, Samples, 0x40u}); });
    RequireRanged("NoteDepthRenderControl without a surface", 0u, [&] { AgcDriver::Graphics::NoteDepthRenderControl({address + Block, Samples, 0x40u}); });
}

}

int main() {
    try {
        Require(ScopedProofsProfiled(), "the ranged GPU entry tests need APS5_PROFILE_SCOPED_PROOFS");
        const auto opened = OpenVulkanTestDevice();
        if (!opened) return VulkanTestSkipped;
        const std::shared_ptr<AgcDriver::VulkanDevice> device(opened.get(), [](AgcDriver::VulkanDevice*) {});
        DriverTestAccess::UseDevice(device);
        const auto planeBytes = AgcDriver::Graphics::DepthSliceBytes(Extent, 4u);
        const auto stencilBytes = AgcDriver::Graphics::DepthSliceBytes(Extent, 1u);
        const auto htileBytes = AgcDriver::Graphics::HtileMetadataBytes(Extent);
        auto* blocks = AllocateBlocks(2u * planeBytes + 2u * stencilBytes + 2u * Block + 8u * RangeGranuleBytes);
        const auto base = reinterpret_cast<std::uint64_t>(blocks);
        HoldTests(base);
        const auto fill = base + Block;
        const auto fillBytes = 3u * RangeGranuleBytes;
        RequireRanged("NoteDepthMetadataFill", GranulesOf(fill, fillBytes), [&] { AgcDriver::Graphics::NoteDepthMetadataFill(fill, fillBytes, 0u); });
        RequireRanged("storeBufferFill", GranulesOf(fill, fillBytes), [&] { DriverTestAccess::StoreBufferFill(device, fill, fillBytes); });
        device->WaitIdle();
        const auto source = base;
        const auto destination = source + planeBytes;
        const auto stencilSource = destination + planeBytes;
        const auto stencilDestination = stencilSource + stencilBytes;
        const auto htile = stencilDestination + stencilBytes;
        const auto expandGranules = GranulesOf(destination, planeBytes) + GranulesOf(stencilDestination, stencilBytes) + GranulesOf(source, planeBytes) + GranulesOf(stencilSource, stencilBytes) + GranulesOf(htile, htileBytes);
        const auto queue = ExpandState(source, stencilSource, destination, stencilDestination, htile);
        RequireRanged("precheckDraw depth expand", expandGranules, [&] { Require(DriverTestAccess::Precheck(queue), "the DB expand draw was not run"); });
        device->WaitIdle();
        const auto colorQueue = ColorState(base);
        const auto colorPass = AgcDriver::Graphics::DecodeColorMetadataPass(colorQueue);
        Require(colorPass.has_value() && colorPass->mode == AgcDriver::Graphics::ColorMetadataPass::Mode::EliminateFastClear && colorPass->targets.size() == 1u, "the fast-clear eliminate did not decode");
        std::uint64_t colorGranules = 0;
        for (const auto& [begin, end] : AgcDriver::Graphics::PipelinedColorMetadataPassWrites(*colorPass)) colorGranules += GranulesOf(begin, end - begin);
        Require(colorGranules >= 2u, "the fast-clear eliminate writes less than two granules");
        RequireRanged("precheckDraw color metadata", colorGranules, [&] { Require(DriverTestAccess::Precheck(colorQueue), "the fast-clear eliminate draw was not run"); });
        device->WaitIdle();
        RenderControlTests(device, AllocateBlocks(8u * RangeGranuleBytes));
        DriverTestAccess::UseDevice(nullptr);
        DriverTestAccess::Stop();
        if (!failures.empty()) return 1;
        std::puts("ranged GPU entries passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
