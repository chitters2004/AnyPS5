#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/MultisampleTarget.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

using namespace AgcDriver::Graphics;

constexpr std::size_t BlockBytes = 65536;
constexpr std::size_t Blocks = 2;
constexpr std::size_t AllocationBytes = Blocks * BlockBytes;
constexpr VkExtent2D DepthExtent{5u, 3u};
constexpr std::uint32_t Samples = 4u;
constexpr std::uint32_t ColorSide = 64u;

class GuestBlock {
public:
    explicit GuestBlock(std::size_t bytes = AllocationBytes) : size((bytes + 4095u) & ~std::size_t{4095}) {
#ifdef _WIN32
        data = GuestArena::GuestArenaAllocate_nid_postfix(size, BlockBytes);
        if (data != nullptr) GuestArena::GuestArenaCommit_nid_postfix(data, size, PAGE_READWRITE, size);
#else
        void* mapped = mmap(nullptr, size + BlockBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + BlockBytes - 1) & ~(static_cast<std::uintptr_t>(BlockBytes) - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + size != begin + size + BlockBytes) munmap(reinterpret_cast<void*>(aligned + size), begin + BlockBytes - aligned);
            data = reinterpret_cast<void*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(data, size);
        }
#endif
        Require(data != nullptr, "cannot allocate tracked guard memory");
        std::memset(data, 0, size);
        GuestAllocations::Mutation().Add(data, size, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(data);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(data, size);
        GuestArena::GuestArenaRelease_nid_postfix(data, size);
#else
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(data, size);
        munmap(data, size);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;
    void Fill(int value) const { std::memset(data, value, size); }
    std::uint64_t Block(std::size_t index) const { return reinterpret_cast<std::uintptr_t>(data) + index * BlockBytes; }

private:
    void* data = nullptr;
    std::size_t size;
};

bool MemoEnabled() {
    return std::getenv("APS5_NO_GUEST_BUFFER_MEMO") == nullptr;
}

class Fixture {
public:
    Fixture(AgcDriver::VulkanDevice& device, std::uint64_t bufferAddress = 0u) : device(device), context(AgcDriver::VulkanDeviceTestAccess::ContextOf(device)) {
        const std::uint64_t address = bufferAddress != 0u ? bufferAddress : block.Block(0);
        ShaderRecompiler::DescriptorBinding binding{ShaderRecompiler::DescriptorKind::StorageBuffer, ShaderRecompiler::DescriptorRole::GuestBuffers, 0, 0, 1, {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) | (16u << 16u), 4u, 0x20077facu}};
        binding.bufferWritten = {false};
        binding.bufferAtomic = {false};
        binding.bufferRead = {true};
        program.bindings.push_back(binding);
        shader = CompiledShader{ShaderRecompiler::ShaderStage::Compute, &program, 0};
        resources = std::make_unique<ShaderResources>(context, shader);
    }

    ~Fixture() {
        resources.reset();
        device.WaitIdle();
        ClearCachedTextures(context.device);
        ClearDepthSurfaces(context.device);
        ClearMultisampleTargets(context.device);
    }

    bool Skipped() {
        const auto before = GuestBufferGuardChecks();
        resources->RequireGuestBuffers();
        const auto after = GuestBufferGuardChecks();
        Require(after.checks == before.checks + 1u, "the guest buffer was not checked once");
        Require(after.skipped == before.skipped || after.skipped == before.skipped + 1u, "a guest buffer skip count moved by more than one");
        return after.skipped == before.skipped + 1u;
    }

    template<typename TCall>
    std::string Refusal(TCall&& call) {
        const auto before = GuestBufferGuardChecks();
        std::string refusal;
        try {
            call();
        } catch (const std::runtime_error& error) {
            refusal = error.what();
        }
        Require(GuestBufferGuardChecks().skipped == before.skipped, "a guest buffer overlapping a guard skipped its checks");
        return refusal;
    }

    void WarmAndSkip(const char* when) {
        Skipped();
        Require(Skipped() == MemoEnabled(), std::string("a clean guest buffer did not skip its second check ") + when);
    }

    void ExpectChecked(const char* what) {
        Require(!Skipped(), what);
        Require(Skipped() == MemoEnabled(), std::string("the guest buffer did not skip again after a clean check: ") + what);
    }

    ColorTarget Color(std::uint64_t address) const {
        ColorTarget target{};
        target.address = address;
        target.surfaceAddress = address;
        target.extent = {ColorSide, ColorSide};
        target.surfaceExtent = target.extent;
        target.format = VK_FORMAT_R8G8B8A8_UNORM;
        target.tileMode = ColorTileMode::RenderTarget;
        target.elementBytes = 4;
        target.bytes = ColorSide * ColorSide * 4u;
        target.samples = Samples;
        return target;
    }

    void AddColor(std::uint64_t address) {
        Require(MultisampleTargetView(context, Color(address)) != VK_NULL_HANDLE, "the multisample color target has no view");
        device.SubmitRecorded(true);
        device.WaitIdle();
    }

    static DepthTarget Depth(const GuestBlock& depth, const GuestBlock& stencil, const GuestBlock& htile) {
        DepthTarget target{depth.Block(0), stencil.Block(0), DepthExtent, VK_FORMAT_D32_SFLOAT_S8_UINT, 0.0f, 0x5au, htile.Block(0), false, Samples};
        target.depthSwizzle = 24u;
        target.pipeBankXor = 0x8c00u;
        target.iterate256 = true;
        target.htilePipeAligned = true;
        target.htileDepthOnly = true;
        return target;
    }

    void AddDepth(const GuestBlock& depth, const GuestBlock& stencil, const GuestBlock& htile) {
        Require(DepthSurfaceView(context, Depth(depth, stencil, htile)) != VK_NULL_HANDLE, "the native depth target has no view");
    }

    AgcDriver::VulkanDevice& device;
    Context context;
    GuestBlock block;
    ShaderRecompiler::RecompileResult program;
    CompiledShader shader{};
    std::unique_ptr<ShaderResources> resources;
};

void CheckColorRegistry(AgcDriver::VulkanDevice& device) {
    std::puts("checking the multisample color registry");
    Fixture fixture(device);
    fixture.WarmAndSkip("with no guards");

    fixture.AddColor(fixture.block.Block(1));
    fixture.ExpectChecked("a disjoint multisample color target was added");

    fixture.AddColor(fixture.block.Block(0));
    const auto refusal = fixture.Refusal([&] { fixture.resources->RequireGuestBuffers(); });
    Require(refusal.find("multisampled color target") != std::string::npos || refusal.find("whose samples are only on the host") != std::string::npos, "a guest buffer over a new multisample color target was not refused: " + refusal);

    ClearMultisampleTargets(fixture.context.device);
    fixture.ExpectChecked("the multisample color targets were retired");
}

void CheckOneFragmentFmaskRegistry(AgcDriver::VulkanDevice& device) {
    std::puts("checking the one-fragment FMASK registry");
    Fixture fixture(device);
    fixture.WarmAndSkip("with no guards");
    auto target = fixture.Color(fixture.block.Block(1));
    target.coverageSamples = Samples;
    target.fmaskAddress = fixture.block.Block(0);
    GuardOneFragmentFmask(fixture.context, target);
    const auto refusal = fixture.Refusal([&] { fixture.resources->RequireGuestBuffers(); });
    Require(refusal.find("FMASK") != std::string::npos, "a guest buffer over a one-fragment FMASK was not refused: " + refusal);
}

void CheckDepthRegistry(AgcDriver::VulkanDevice& device) {
    std::puts("checking the resident depth registry");
    const auto depthBytes = static_cast<std::size_t>(MultisampledDepthBytes_nid_no_patch(DepthExtent, 4u, Samples));
    const auto stencilBytes = static_cast<std::size_t>(MultisampledDepthBytes_nid_no_patch(DepthExtent, 1u, Samples));
    GuestBlock depth(depthBytes), stencil(stencilBytes), htile(BlockBytes);
    GuestBlock otherDepth(depthBytes), otherStencil(stencilBytes), otherHtile(BlockBytes);
    stencil.Fill(0x5a);
    otherStencil.Fill(0x5a);
    Fixture fixture(device, depth.Block(0));
    Recorder recorder(fixture.context);
    recorder.Activate();
    struct Cleanup {
        Recorder& recorder;
        ~Cleanup() { recorder.Sync(); }
    } cleanup{recorder};
    fixture.WarmAndSkip("with no guards");

    fixture.AddDepth(otherDepth, otherStencil, otherHtile);
    Require(HasResidentDepthGuards_nid_no_patch(), "the disjoint native depth target registered no guard");
    Require(!ResidentDepthGuardOverlaps_nid_no_patch(depth.Block(0), 64u), "the disjoint native depth target guards the buffer");
    fixture.ExpectChecked("a disjoint native depth target was added");

    fixture.AddDepth(depth, stencil, htile);
    Require(ResidentDepthGuardOverlaps_nid_no_patch(depth.Block(0), 64u), "the overlapping native depth target registered no guard over the buffer");
    Require(!fixture.Skipped(), "a guest buffer over a new native depth guard skipped its checks");
    Require(!ResidentDepthGuardOverlaps_nid_no_patch(depth.Block(0), 64u), "the guest buffer check did not retire the guard of a depth target no draw wrote");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(*device);
        if (!AgcDriver::GuestMemory::WriteWatched() || (context.limits.framebufferColorSampleCounts & VK_SAMPLE_COUNT_4_BIT) == 0) {
            Require(std::getenv("ANYPS5_REQUIRE_VULKAN") == nullptr, "guest buffer guard memo tests require write tracking and 4x color targets");
            std::puts("skipped, guest buffer guard memo tests require write tracking and 4x color targets");
            return VulkanTestSkipped;
        }
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        CheckColorRegistry(*device);
        CheckOneFragmentFmaskRegistry(*device);
        CheckDepthRegistry(*device);
        std::puts("guest buffer guard memo tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
