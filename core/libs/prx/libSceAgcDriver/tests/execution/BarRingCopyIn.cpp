#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace {

namespace Graphics = AgcDriver::Graphics;
using Graphics::Require;

struct Copied {
    VkBuffer source;
    VkDeviceSize sourceOffset;
    VkDeviceSize size;
    VkDeviceSize readbackOffset;
};

PFN_vkGetDeviceProcAddr originalProc = nullptr;
PFN_vkCmdCopyBuffer originalCopy = nullptr;
PFN_vkCmdPipelineBarrier originalBarrier = nullptr;
VkBuffer readback = VK_NULL_HANDLE;
std::vector<Copied> copies;
VkDeviceSize readbackUsed = 0;

VKAPI_ATTR void VKAPI_CALL Copy(VkCommandBuffer commands, VkBuffer source, VkBuffer target, std::uint32_t count, const VkBufferCopy* regions) {
    originalCopy(commands, source, target, count, regions);
    if (target == readback) return;
    VkMemoryBarrier copyBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    copyBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    copyBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    originalBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &copyBarrier, 0, nullptr, 0, nullptr);
    for (std::uint32_t i = 0; i < count; ++i) {
        const VkBufferCopy back{regions[i].dstOffset, readbackUsed, regions[i].size};
        originalCopy(commands, target, readback, 1, &back);
        copies.push_back({source, regions[i].srcOffset, regions[i].size, readbackUsed});
        readbackUsed += regions[i].size;
    }
}

PFN_vkVoidFunction VKAPI_CALL Resolve(VkDevice device, const char* name) {
    if (std::strcmp(name, "vkCmdCopyBuffer") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&Copy);
    return originalProc(device, name);
}

void Scenario(Graphics::Context context, bool ring) {
    constexpr std::size_t blockBytes = 65536 * 2;
    constexpr std::size_t firstOffset = 4, firstBytes = 36;
    constexpr std::size_t secondOffset = 65536 + 12, secondBytes = 52;
    auto block = std::unique_ptr<std::byte[], void (*)(std::byte*)>(static_cast<std::byte*>(::operator new(blockBytes, std::align_val_t{65536})), [](std::byte* value) { ::operator delete(value, std::align_val_t{65536}); });
    for (std::size_t i = 0; i < blockBytes; ++i) block[i] = static_cast<std::byte>((i * 7 + 3) & 0xff);
    GuestAllocations::Mutation().Add(block.get(), blockBytes, true, false, true);
    const auto unregister = [&](std::byte*) { GuestAllocations::Mutation().Remove(block.get()); };
    std::unique_ptr<std::byte, decltype(unregister)> registered(block.get(), unregister);
    Graphics::RuntimeSwitches::Assign(Graphics::RuntimeSwitch::NoBarRing, !ring);
    context.functions = nullptr;
    context.deviceProc = &Resolve;
    context.bufferPool = std::make_shared<Graphics::BufferPool>(context);
    auto ownedReadback = std::make_unique<Graphics::Buffer>(context, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    readback = ownedReadback->Handle();
    copies.clear();
    readbackUsed = 0;
    const auto before = Graphics::Recorder::RingCounts();
    {
        Graphics::Recorder recorder(context);
        recorder.Activate();
        {
            Graphics::GuestBufferMemory memory(context);
            memory.AddReadable(reinterpret_cast<std::uintptr_t>(block.get()) + firstOffset, firstBytes);
            memory.AddReadable(reinterpret_cast<std::uintptr_t>(block.get()) + secondOffset, secondBytes);
            memory.Upload(false);
        }
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        originalBarrier(recorder.Commands(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        recorder.Sync();
    }
    const auto after = Graphics::Recorder::RingCounts();
    ownedReadback->Invalidate();
    const auto bytes = ownedReadback->Bytes();
    std::size_t matched = 0;
    for (const auto& copy : copies) {
        const auto* guest = copy.size == firstBytes ? block.get() + firstOffset : copy.size == secondBytes ? block.get() + secondOffset : nullptr;
        if (guest == nullptr) continue;
        Require(std::equal(guest, guest + copy.size, bytes.begin() + static_cast<std::ptrdiff_t>(copy.readbackOffset)), std::string("the ") + (copy.size == firstBytes ? "first" : "second") + " input copy did not carry the guest bytes" + (ring ? " through the BAR ring" : ""));
        ++matched;
    }
    Require(matched == 2, "the fixture did not record one input copy for each readable range (" + std::to_string(matched) + " matched of " + std::to_string(copies.size()) + " copies)");
    const auto ringSlices = after.slices - before.slices;
    if (ring) {
        Require(ringSlices == 2 && after.bytes - before.bytes == firstBytes + secondBytes, "the two input copies did not go through the BAR ring");
        Require(copies[0].source == copies[1].source && copies[0].sourceOffset != copies[1].sourceOffset, "the two ring slices were not disjoint parts of one chunk");
    } else {
        Require(ringSlices == 0 && after.fallbacks[0] - before.fallbacks[0] == 2, "an input copy took the BAR ring with APS5_NO_BAR_RING set");
    }
    registered.reset();
    Graphics::ClearHostImports(context.device);
    ownedReadback.reset();
    readback = VK_NULL_HANDLE;
    context.bufferPool.reset();
    std::printf("BAR ring copy-in: ring=%d copies=%zu ring slices=%llu\n", ring, copies.size(), static_cast<unsigned long long>(ringSlices));
}

}

int main() {
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        auto context = device->graphicsContext();
        if (context.hostImportAlignment == 0 || context.hostImportAlignment > 65536) {
            Require(std::getenv("ANYPS5_REQUIRE_VULKAN") == nullptr, "the BAR ring copy-in test requires host imports with at most 64 KiB alignment");
            std::puts("skipped, suitable host imports are unavailable");
            return VulkanTestSkipped;
        }
        if (Graphics::SnapshotMemoryProperties(context.memory, false) != Graphics::MappedDeviceProperties) {
            std::puts("skipped, the device has no resizable BAR type");
            return VulkanTestSkipped;
        }
        Graphics::ClearHostImports(context.device);
        originalProc = context.deviceProc;
        originalCopy = context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
        originalBarrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
        Scenario(context, true);
        Scenario(context, false);
        Graphics::RuntimeSwitches::Assign(Graphics::RuntimeSwitch::NoBarRing, false);
        std::puts("BAR ring copy-in tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
