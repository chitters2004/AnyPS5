#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
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
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <mutex>
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

constexpr std::size_t BlockBytes = 1u << 20u;
constexpr std::uint32_t Initial = 0x3f800000u;
constexpr std::uint32_t Stored = 0x11111111u;
constexpr std::uint32_t Filled = 0x33333333u;
constexpr std::size_t ShadowUnitBytes = 0x10000u;

class GuestBlock {
public:
    GuestBlock() {
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
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
        }
#endif
        Require(block != nullptr, "latched teardown: cannot allocate the guest block");
        Require(AgcDriver::GuestMemory::Watched(Address(), BlockBytes), "latched teardown: the guest block is not write-watched");
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
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, BlockBytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint32_t* Words() const { return reinterpret_cast<std::uint32_t*>(block); }
    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block)); }

private:
    std::uint8_t* block = nullptr;
};

bool SeedShadowUnit(AgcDriver::VulkanDevice& device, std::uint64_t unit) {
    using namespace AgcDriver::Graphics;
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    auto* recorder = Recorder::Active();
    const auto* import = HostImportFor(context, unit, ShadowUnitBytes);
    if (recorder == nullptr || import == nullptr) return false;
    auto destination = ShadowDestinationFor(context, *import, unit, unit + ShadowUnitBytes);
    if (!destination.has_value()) return false;
    auto pattern = std::make_shared<Buffer>(context, ShadowUnitBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto* words = reinterpret_cast<std::uint32_t*>(pattern->Bytes().data());
    std::fill(words, words + ShadowUnitBytes / 4u, Stored);
    const auto commands = recorder->Commands();
    recorder->Keep(pattern);
    recorder->Keep(destination->slab);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkBufferCopy piece{0, SlabOffset(*import, *destination->slab, unit), ShadowUnitBytes};
    context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer")(commands, pattern->Handle(), destination->buffer, 1, &piece);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    recorder->MarkCovered(VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    const ShadowedRange range{unit, unit + ShadowUnitBytes, destination->slab};
    MarkShadowed(*import, std::span(&range, 1), AgcDriver::GuestMemory::TrackerGeneration());
    return true;
}

bool Shadowed(std::uint64_t unit) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    return AgcDriver::Graphics::AnyShadowedOverlaps(unit, ShadowUnitBytes);
}

}

int main() {
    try {
        if (!AgcDriver::GuestMemory::WriteWatched() || !AgcDriver::Graphics::UnitShadowEnabled()) {
            std::puts("guest memory has no write watch or unit shadows are off: latched teardown not tested");
            return VulkanTestSkipped;
        }
        if (std::getenv("APS5_WRITE_WATCH_IMPORTS") == nullptr) {
#ifdef _WIN32
            _putenv_s("APS5_WRITE_WATCH_IMPORTS", "watch");
#else
            setenv("APS5_WRITE_WATCH_IMPORTS", "watch", 1);
#endif
        }
        GuestBlock guest;
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto unit = guest.Address() + ShadowUnitBytes;
        if (!SeedShadowUnit(*device, unit)) {
            std::puts("no watched host import or shadow slab for the guest block: latched teardown not tested");
            return VulkanTestSkipped;
        }
        device->WaitIdle();
        const auto* words = guest.Words() + ShadowUnitBytes / 4u;
        Require(Shadowed(unit) && std::count(words, words + ShadowUnitBytes / 4u, Stored) == 0, "latched teardown: the shadowed units were published before the failure");
        const std::array<std::uint32_t, 4> pattern{Filled, Filled, Filled, Filled};
        Require(device->FillBuffer(guest.Address() + BlockBytes / 2u, 4096u, pattern), "latched teardown: the fill was not recorded on the GPU");
        std::string failure;
        AgcDriver::Graphics::Recorder::FailNextFenceWaitsForTest(1);
        try {
            device->WaitIdle();
        } catch (const std::exception& error) {
            failure = error.what();
        }
        AgcDriver::Graphics::Recorder::FailNextFenceWaitsForTest(0);
        Require(failure.find("vkWaitForFences") != std::string::npos, "latched teardown: the injected fence failure did not fail the drain ('" + failure + "')");
        {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            const auto* recorder = AgcDriver::Graphics::Recorder::Active();
            Require(recorder != nullptr && recorder->Latched(), "latched teardown: the fence failure did not latch the device's recorder");
        }
        std::string refused;
        {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            try {
                static_cast<void>(AgcDriver::Graphics::PublishShadow(unit, ShadowUnitBytes, AgcDriver::Graphics::PublishScope::Whole, AgcDriver::Graphics::PublishReason::Hook));
            } catch (const std::exception& error) {
                refused = error.what();
            }
        }
        Require(refused == failure && Shadowed(unit), "latched teardown: a host access published or dropped a shadowed unit after the failure ('" + refused + "')");
        device.reset();
        Require(std::count(words, words + ShadowUnitBytes / 4u, Stored) == 0, "latched teardown: the teardown published shadowed GPU results after the failure");
        std::puts("latched teardown tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
