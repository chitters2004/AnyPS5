#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
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
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Context;
using AgcDriver::Graphics::DescribeSurface;
using AgcDriver::Graphics::GuestTextureResource;
using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::StorageTexture;
using AgcDriver::Graphics::TextureDetiler;
using AgcDriver::Graphics::TextureDimension;
using AgcDriver::Graphics::TextureTileMode;

constexpr std::uint32_t Side = 64;
constexpr std::uint32_t MipCount = 2;
constexpr std::uint32_t Format32Float = 22;
constexpr std::size_t UnitBytes = 65536;

std::vector<std::string> failures;

void Fail(std::string failure) {
    failures.push_back(std::move(failure));
}

PFN_vkGetDeviceProcAddr originalProc = nullptr;
PFN_vkCreateImageView originalCreateView = nullptr;
PFN_vkDestroyImageView originalDestroyView = nullptr;
std::atomic<long> liveViews{0};
std::atomic<bool> failNextView{false};

VKAPI_ATTR VkResult VKAPI_CALL CreateView(VkDevice device, const VkImageViewCreateInfo* info, const VkAllocationCallbacks* callbacks, VkImageView* view) {
    if (failNextView.exchange(false)) return VK_ERROR_OUT_OF_HOST_MEMORY;
    const auto result = originalCreateView(device, info, callbacks, view);
    if (result == VK_SUCCESS) liveViews.fetch_add(1);
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyView(VkDevice device, VkImageView view, const VkAllocationCallbacks* callbacks) {
    if (view != VK_NULL_HANDLE) liveViews.fetch_sub(1);
    originalDestroyView(device, view, callbacks);
}

PFN_vkVoidFunction VKAPI_CALL Resolve(VkDevice device, const char* name) {
    if (std::strcmp(name, "vkCreateImageView") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&CreateView);
    if (std::strcmp(name, "vkDestroyImageView") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&DestroyView);
    return originalProc(device, name);
}

class GuestBlock {
public:
    explicit GuestBlock(std::size_t bytes) : bytes(bytes) {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, UnitBytes));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
#else
        void* mapped = mmap(nullptr, bytes + UnitBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + UnitBytes - 1) & ~(static_cast<std::uintptr_t>(UnitBytes) - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + bytes != begin + bytes + UnitBytes) munmap(reinterpret_cast<void*>(aligned + bytes), begin + UnitBytes - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            if (AgcDriver::GuestMemory::WriteWatched()) GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
        }
#endif
        Require(block != nullptr, "storage view cache: cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, bytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, bytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, bytes);
#else
        munmap(block, bytes);
        if (AgcDriver::GuestMemory::WriteWatched()) GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, bytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Data() { return block; }

private:
    std::uint8_t* block = nullptr;
    std::size_t bytes = 0;
};

struct ViewKind {
    const char* name;
    bool arrayed;
    std::function<VkImageView(StorageTexture&)> get;
};

struct Fixture {
    const Context& context;
    TextureDetiler& detiler;
    GuestTextureResource resource;
    GuestTextureResource arrayed;
    std::shared_ptr<StorageTexture> Make(bool layers) const { return StorageTexture::Create(context, detiler, layers ? arrayed : resource, 0); }
};

long ViewsSince(long base) {
    return liveViews.load() - base;
}

void RetryAfterFailure(const ViewKind& kind, StorageTexture& texture, long base, const std::string& at) {
    if (ViewsSince(base) != 0) Fail(at + ": " + std::to_string(ViewsSince(base)) + " image views outlived the failed request");
    VkImageView view = VK_NULL_HANDLE;
    try {
        view = kind.get(texture);
    } catch (const std::exception& error) {
        Fail(at + ": the retry threw: " + error.what());
        return;
    }
    if (view == VK_NULL_HANDLE) Fail(at + ": the retry returned no view");
    if (kind.get(texture) != view) Fail(at + ": the cache did not keep the retried view");
    if (ViewsSince(base) != 1) Fail(at + ": the texture owns " + std::to_string(ViewsSince(base)) + " image views after the retry instead of 1");
}

void Exercise(const Fixture& fixture, const ViewKind& kind) {
    {
        const auto probe = fixture.Make(kind.arrayed);
        try {
            kind.get(*probe);
        } catch (const std::exception& error) {
            std::printf("%s: skipped, the view is not available on this device: %s\n", kind.name, error.what());
            return;
        }
    }
    {
        const auto texture = fixture.Make(kind.arrayed);
        const auto base = liveViews.load();
        failNextView = true;
        bool threw = false;
        try {
            kind.get(*texture);
        } catch (const std::exception&) {
            threw = true;
        }
        const bool reached = !failNextView.exchange(false);
        if (!reached || !threw) Fail(std::string(kind.name) + ": the failing vkCreateImageView was not reached or not reported");
        RetryAfterFailure(kind, *texture, base, std::string(kind.name) + ", failing vkCreateImageView");
    }
    std::size_t cases = 0;
#ifdef APS5_AGC_ALLOCATION_COUNT
    for (std::size_t failAfter = 0;; ++failAfter) {
        Require(failAfter < 4096, std::string(kind.name) + ": the allocation sweep did not end");
        const auto texture = fixture.Make(kind.arrayed);
        const auto base = liveViews.load();
        FailAllocationsAfter_nid_no_patch(failAfter);
        bool threw = false;
        try {
            kind.get(*texture);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        const bool failed = !AllocationFailurePending_nid_no_patch();
        StopAllocationFailures_nid_no_patch();
        if (!failed) break;
        ++cases;
        const auto at = std::string(kind.name) + ", failing allocation " + std::to_string(failAfter);
        if (!threw) {
            Fail(at + ": the request did not report the failure");
            continue;
        }
        RetryAfterFailure(kind, *texture, base, at);
    }
    if (cases == 0) Fail(std::string(kind.name) + ": the request made no allocation to fail");
#endif
    std::printf("%s: a failed vkCreateImageView and %zu failed allocations each left no view behind, and the retry cached one\n", kind.name, cases);
}

void Run(AgcDriver::VulkanDevice& device) {
    GuestTextureResource resource{};
    resource.width = Side;
    resource.height = Side;
    resource.mipCount = MipCount;
    resource.tileMode = TextureTileMode::kStandard64KB;
    resource.dimension = TextureDimension::k2D;
    resource.format = Format32Float;
    resource.dstSelX = 4;
    resource.dstSelY = 5;
    resource.dstSelZ = 6;
    resource.dstSelW = 7;
    auto arrayed = resource;
    arrayed.dimension = TextureDimension::k2DArray;
    arrayed.depthOrLastArray = 1;
    const auto guestBytes = std::max(DescribeSurface(resource).guestBytes, DescribeSurface(arrayed).guestBytes);
    GuestBlock block(static_cast<std::size_t>((guestBytes + UnitBytes - 1) / UnitBytes * UnitBytes));
    resource.baseAddress = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block.Data()));
    arrayed.baseAddress = resource.baseAddress;
    std::memset(block.Data(), 0, static_cast<std::size_t>(guestBytes));

    auto context = device.graphicsContext();
    TextureDetiler detiler(context);
    originalProc = context.deviceProc;
    originalCreateView = context.Function<PFN_vkCreateImageView>("vkCreateImageView");
    originalDestroyView = context.Function<PFN_vkDestroyImageView>("vkDestroyImageView");
    context.functions = nullptr;
    context.deviceProc = &Resolve;
    context.detiler = &detiler;
    const Fixture fixture{context, detiler, resource, arrayed};
    const std::vector<ViewKind> kinds{
        {"a view of another mip", false, [](StorageTexture& texture) { return texture.View(1); }},
        {"a first-layer view of a 2D array", true, [](StorageTexture& texture) { return texture.FirstLayerView(0); }},
        {"an atomic view", false, [](StorageTexture& texture) { return texture.AtomicView(0, false); }},
        {"an attachment view", false, [](StorageTexture& texture) { return texture.AttachmentView(VK_FORMAT_R32_SFLOAT); }},
    };
    for (const auto& kind : kinds) Exercise(fixture, kind);
    device.WaitIdle();
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device);
        for (const auto& failure : failures) std::cerr << failure << '\n';
        return failures.empty() ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
