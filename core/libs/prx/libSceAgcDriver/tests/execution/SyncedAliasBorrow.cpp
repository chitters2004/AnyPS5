#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
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
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Buffer;
using AgcDriver::Graphics::CommandBatch;
using AgcDriver::Graphics::Context;
using AgcDriver::Graphics::DescribeSurface;
using AgcDriver::Graphics::GuestTextureResource;
using AgcDriver::Graphics::Recorder;
using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::StorageTexture;
using AgcDriver::Graphics::TextureDetiler;
using AgcDriver::Graphics::TextureDimension;
using AgcDriver::Graphics::TextureTileMode;

constexpr std::uint32_t Width = 256;
constexpr std::uint32_t Height = 256;
constexpr std::uint32_t Format32Float = 22;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::size_t UnitBytes = 65536;

std::uint64_t AddressOf(const void* data) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
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
        Require(block != nullptr, "synced alias borrow: cannot allocate the guest block");
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

GuestTextureResource Surface(std::uint64_t address, std::uint32_t format) {
    GuestTextureResource resource{};
    resource.baseAddress = address;
    resource.width = Width;
    resource.height = Height;
    resource.mipCount = 1;
    resource.tileMode = TextureTileMode::kStandard64KB;
    resource.dimension = TextureDimension::k2D;
    resource.format = format;
    resource.dstSelX = 4;
    resource.dstSelY = 5;
    resource.dstSelZ = 6;
    resource.dstSelW = 7;
    return resource;
}

void Settle() {
    if (auto* recorder = Recorder::Active()) recorder->Sync();
}

std::vector<std::uint32_t> Texels(const Context& context, const StorageTexture& texture) {
    Settle();
    const auto bytes = static_cast<std::size_t>(Width) * Height * 4u;
    Buffer readback(context, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    {
        CommandBatch batch(context);
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = texture.Image();
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(batch.Handle(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {Width, Height, 1};
        context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(batch.Handle(), texture.Image(), VK_IMAGE_LAYOUT_GENERAL, readback.Handle(), 1, &region);
        AgcDriver::Graphics::RecordMemoryBarrier(context, batch.Handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        batch.SubmitAndWait();
    }
    readback.Invalidate();
    std::vector<std::uint32_t> texels(bytes / 4u);
    std::memcpy(texels.data(), readback.Bytes().data(), bytes);
    return texels;
}

struct Fixture {
    Context context;
    TextureDetiler& detiler;
    GuestTextureResource floats;
    GuestTextureResource uints;
    std::size_t bytes;
    std::uint8_t* guest;
};

void CheckMatchesGuest(const Fixture& fixture, const StorageTexture& texture, std::uint32_t format, const std::string& what) {
    const auto truth = StorageTexture::Create(fixture.context, fixture.detiler, format == Format32Float ? fixture.floats : fixture.uints, 0);
    const auto expected = Texels(fixture.context, *truth);
    const auto actual = Texels(fixture.context, texture);
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (expected[index] == actual[index]) continue;
        char text[160];
        std::snprintf(text, sizeof(text), "texel %zu holds 0x%08x where an upload of guest memory gives 0x%08x", index, actual[index], expected[index]);
        Require(false, "synced alias borrow: " + what + ": " + text);
    }
}

void Fill(StorageTexture& texture, std::uint32_t bits, const std::string& what) {
    const std::array<std::uint32_t, 4> pattern{bits, bits, bits, bits};
    const char* refusal = nullptr;
    Require(texture.FillClear(pattern, StorageTexture::WholeImage, refusal), "synced alias borrow: " + what + ": the GPU fill was refused (" + std::string(refusal != nullptr ? refusal : "no reason") + ")");
    Require(texture.Dirty(), "synced alias borrow: " + what + ": the GPU fill left no pending results");
}

void Store(Fixture& fixture, std::uint32_t bits, const std::string& what) {
    Require(StorageTexture::FlushPending(AddressOf(fixture.guest), fixture.bytes, nullptr, "test"), "synced alias borrow: " + what + ": the flush stored nothing");
    Settle();
    std::uint32_t word = 0;
    std::memcpy(&word, fixture.guest, sizeof(word));
    Require(word == bits, "synced alias borrow: " + what + ": a CPU read after the store sees 0x" + std::to_string(word) + ", not the GPU's result");
    AgcDriver::GuestMemory::BumpCollectEpoch();
}

StorageTexture::SyncedBorrowCounts Delta(const StorageTexture::SyncedBorrowCounts& before) {
    const auto after = StorageTexture::SyncedBorrows();
    return {after.borrows - before.borrows, after.units - before.units, after.bytes - before.bytes, after.rollBacks - before.rollBacks};
}

void Write(Fixture& fixture, std::size_t offset, std::size_t length, std::uint8_t value) {
    std::memset(fixture.guest + offset, value, length);
}

void Refresh(StorageTexture& texture, const std::string& what) {
    AgcDriver::GuestMemory::BumpCollectEpoch();
    Require(!texture.Refresh(), "synced alias borrow: " + what + ": the refresh found guest memory unchanged");
}

void ExpectUnits(const StorageTexture::SyncedBorrowCounts& before, std::uint64_t units, const std::string& what) {
    const auto delta = Delta(before);
    Require(delta.units == units, "synced alias borrow: " + what + ": took " + std::to_string(delta.units) + " units from an in-sync alias image, expected " + std::to_string(units));
}

std::shared_ptr<StorageTexture> Cached(const Fixture& fixture, std::uint32_t format) {
    auto texture = StorageTexture::Create(fixture.context, fixture.detiler, format == Format32Float ? fixture.floats : fixture.uints, 0);
    texture->SetCached(true);
    return texture;
}

void GpuStores(Fixture& fixture, std::uint32_t units) {
    const auto floats = Cached(fixture, Format32Float);
    const auto uints = Cached(fixture, Format32UInt);
    Fill(*floats, 0x3f800000u, "a GPU write of the float image");
    Store(fixture, 0x3f800000u, "the float image's store");
    auto before = StorageTexture::SyncedBorrows();
    Refresh(*uints, "the uint image after the float image's store");
    CheckMatchesGuest(fixture, *uints, Format32UInt, "after the float image's store");
    ExpectUnits(before, units, "the uint image after the float image's store");
    Fill(*uints, 0x12345678u, "a GPU write of the uint image");
    Store(fixture, 0x12345678u, "the uint image's store");
    before = StorageTexture::SyncedBorrows();
    Refresh(*floats, "the float image after the uint image's store");
    CheckMatchesGuest(fixture, *floats, Format32Float, "after the uint image's store");
    ExpectUnits(before, units, "the float image after the uint image's store");
    floats->SetCached(false);
    uints->SetCached(false);
    std::puts("synced alias borrow: GPU results stored by one alias are taken on the device by the other, and a CPU read sees them");
}

int Run(AgcDriver::VulkanDevice& device) {
    const auto bytes = static_cast<std::size_t>(DescribeSurface(Surface(0, Format32Float)).guestBytes);
    GuestBlock block(bytes);
    const auto address = AddressOf(block.Data());
    std::memset(block.Data(), 0, bytes);
    AgcDriver::GuestMemory::CollectWrites(address, bytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    auto context = device.graphicsContext();
    TextureDetiler detiler(context);
    context.detiler = &detiler;
    std::unique_lock gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto* import = AgcDriver::Graphics::HostImportFor(context, address, bytes);
    if (import == nullptr || !AgcDriver::GuestMemory::Watched(address, bytes)) {
        std::printf("synced alias borrow: the device keeps no write-watched host import of the surface; skipped\n");
        return VulkanTestSkipped;
    }
    Fixture fixture{context, detiler, Surface(address, Format32Float), Surface(address, Format32UInt), bytes, block.Data()};
    auto floats = Cached(fixture, Format32Float);
    auto uints = Cached(fixture, Format32UInt);
    const auto units = floats->TrackedUnits();
    Require(units >= 2 && uints->TrackedUnits() == units, "synced alias borrow: the surface has fewer than two block units");

    Write(fixture, 0, bytes, 0x11);
    Refresh(*floats, "the float image after a CPU store");
    auto before = StorageTexture::SyncedBorrows();
    Refresh(*uints, "the uint image after a CPU store the float image already read");
    CheckMatchesGuest(fixture, *uints, Format32UInt, "after a CPU store the float image already read");
    ExpectUnits(before, units, "the uint image after a CPU store the float image already read");
    std::puts("synced alias borrow: an alias image took units the other image already holds on the device instead of re-uploading guest memory");

    Write(fixture, 0, bytes, 0x22);
    Refresh(*floats, "the float image after a second CPU store");
    Write(fixture, UnitBytes / 2, 256, 0x33);
    before = StorageTexture::SyncedBorrows();
    Refresh(*uints, "the uint image after a CPU store into unit 0 the float image has not read");
    CheckMatchesGuest(fixture, *uints, Format32UInt, "after a CPU store into unit 0 the float image has not read");
    ExpectUnits(before, units - 1u, "the uint image after a CPU store into unit 0 the float image has not read");
    std::puts("synced alias borrow: a CPU store the lender has not read wins its unit");

    Write(fixture, 0, bytes, 0x44);
    Refresh(*uints, "the uint image after a third CPU store");
    before = StorageTexture::SyncedBorrows();
    Refresh(*floats, "the float image after a CPU store the uint image already read");
    CheckMatchesGuest(fixture, *floats, Format32Float, "after a CPU store the uint image already read");
    ExpectUnits(before, units, "the float image after a CPU store the uint image already read");
    std::puts("synced alias borrow: either alias lends to the other");

    Write(fixture, 0, bytes, 0x55);
    Refresh(*floats, "the float image before the range is forgotten");
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(static_cast<std::uintptr_t>(address), bytes);
    before = StorageTexture::SyncedBorrows();
    Refresh(*uints, "the uint image after the range was forgotten");
    CheckMatchesGuest(fixture, *uints, Format32UInt, "after the range was forgotten");
    ExpectUnits(before, 0, "the uint image after the range was forgotten (unmapped or re-registered) since the float image read it");
    floats->SetCached(false);
    uints->SetCached(false);
    std::puts("synced alias borrow: an image over memory forgotten since it read it lends nothing");

    const auto fresh = Cached(fixture, Format32UInt);
    const auto lender = Cached(fixture, Format32Float);
    Write(fixture, 0, bytes, 0x66);
    Refresh(*lender, "the lender after a CPU store");
    Settle();
    {
        Recorder recorder(context);
        recorder.Activate();
        before = StorageTexture::SyncedBorrows();
        Refresh(*fresh, "the recorded refresh");
        ExpectUnits(before, units, "the recorded refresh");
        Recorder::FailNextSubmitsForTest(1u);
        bool discarded = false;
        try {
            recorder.Submit();
        } catch (const std::exception& error) {
            discarded = std::string(error.what()).find("injected failure") != std::string::npos;
        }
        Recorder::FailNextSubmitsForTest(0u);
        Require(discarded, "synced alias borrow: the batch holding the borrow was not discarded");
        Require(Delta(before).rollBacks == 1, "synced alias borrow: the discarded borrow was not rolled back");
        Refresh(*fresh, "the refresh after the discarded borrow");
        recorder.Sync();
        CheckMatchesGuest(fixture, *fresh, Format32UInt, "after the discarded borrow and a second refresh");
    }
    fresh->SetCached(false);
    lender->SetCached(false);
    std::puts("synced alias borrow: a discarded borrow gives its units back to guest memory");

    const char* forced = std::getenv("APS5_WRITE_WATCH_IMPORTS");
    if (forced != nullptr && std::strcmp(forced, "watch") == 0) {
        std::puts("synced alias borrow: GPU stores into a write-watch forced on by APS5_WRITE_WATCH_IMPORTS would fault on this device; the GPU-store cases run where imports stay watched on their own");
    } else {
        GpuStores(fixture, units);
    }
    std::puts("synced alias borrow tests passed");
    return 0;
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        return Run(*device);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
