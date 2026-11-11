#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
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
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::size_t BlockBytes = 1u << 20u;
constexpr std::uint32_t Width = 256;
constexpr std::uint32_t Height = 128;
constexpr std::size_t ImageBytes = Width * Height * 4u;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Initial = 0x3f800000u;
constexpr std::uint32_t Stored = 0x11111111u;
constexpr std::uint32_t Overwritten = 0x22222222u;
constexpr std::uint32_t Restored = 0x33333333u;
constexpr std::size_t OverwrittenDword = 0x100u / 4u;
constexpr std::size_t FarDword = 100u * Width;
constexpr std::uint32_t Filled = 0x33333333u;
constexpr std::size_t FillBytes = 4096u;
constexpr std::size_t FillPast = 0x10000u;
constexpr std::size_t ShadowUnitBytes = 0x10000u;
constexpr std::uint32_t Standard256B = 1u;
constexpr std::uint32_t LabelValue = 0x5eedf00du;
constexpr std::size_t LabelPast = 0x100u;

alignas(256) constexpr std::array<std::uint32_t, 11> StoreImageCode{0x7e000280u, 0x7e020280u, 0x7e0402ffu, Stored, 0xf0200108u, 0x00010200u, 0x7e0202ffu, 0x00000064u, 0xf0200108u, 0x00010200u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 11> RestoreImageCode{0x7e000280u, 0x7e020280u, 0x7e0402ffu, Restored, 0xf0200108u, 0x00010200u, 0x7e0202ffu, 0x00000064u, 0xf0200108u, 0x00010200u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 7> StoreGlobalCode{0x7e000208u, 0x7e020209u, 0x7e0402ffu, Overwritten, 0xdc708100u, 0x007d0200u, 0xbf810000u};

class GuestBlock {
public:
    explicit GuestBlock(bool watched) : watched(watched) {
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
            if (watched) GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
        }
#endif
        Require(block != nullptr, "storage flush ordering: cannot allocate the guest block");
        Require(!watched || AgcDriver::GuestMemory::Watched(Address(), BlockBytes), "storage flush ordering: the guest block is not write-watched");
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
        if (watched) GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, BlockBytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint32_t* Words() const { return reinterpret_cast<std::uint32_t*>(block); }
    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block)); }

private:
    bool watched;
    std::uint8_t* block = nullptr;
};

ShaderRecompiler::RecompileResult Compile(std::span<const std::uint32_t> code, std::span<const std::uint32_t> userData, const ShaderRecompiler::SpirvTarget& target) {
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{64u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {64u, 0, userData, compute, std::nullopt, std::nullopt, memory},
        target,
        {0, 0, 0, 128}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

std::vector<std::uint32_t> ImageUserData(std::uint64_t base, std::uint32_t tileMode = 0u) {
    std::vector<std::uint32_t> userData(12, 0u);
    userData[4] = static_cast<std::uint32_t>(base >> 8u);
    userData[5] = static_cast<std::uint32_t>((base >> 40u) & 0xffu) | (Format32UInt << 20u) | (((Width - 1u) & 3u) << 30u);
    userData[6] = ((Width - 1u) >> 2u) | ((Height - 1u) << 14u);
    userData[7] = 0xfacu | (tileMode << 20u) | (Type2D << 28u);
    return userData;
}

std::vector<std::uint32_t> GlobalUserData(std::uint64_t base) {
    std::vector<std::uint32_t> userData(10, 0u);
    userData[8] = static_cast<std::uint32_t>(base);
    userData[9] = static_cast<std::uint32_t>(base >> 32u);
    return userData;
}

bool AddressBasedStorePending(std::uint64_t image, bool open) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto* recorder = AgcDriver::Graphics::Recorder::Active();
    const auto read = recorder != nullptr ? recorder->DescribePendingRead(image + ImageBytes, 4u) : std::nullopt;
    return read.has_value() && read->kind == AgcDriver::Graphics::Recorder::ReadKind::AddressBased && read->open == open;
}

bool OpenBatchWrites(std::uint64_t address, std::size_t bytes) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto* recorder = AgcDriver::Graphics::Recorder::Active();
    return recorder != nullptr && recorder->OpenWriteOverlaps(address, bytes);
}

std::uint64_t StoreImage(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::size_t offset, std::uint32_t tileMode = 0u) {
    const auto image = guest.Address() + offset;
    const auto storeImage = Compile(StoreImageCode, ImageUserData(image, tileMode), device.Target());
    device.Dispatch(storeImage, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(StoreImageCode.data()));
    return image;
}

void Fill(AgcDriver::VulkanDevice& device, std::uint64_t address, std::size_t bytes, const std::string& what) {
    const std::array<std::uint32_t, 4> pattern{Filled, Filled, Filled, Filled};
    Require(device.FillBuffer(address, bytes, pattern), what + "the fill was not recorded on the GPU");
    Require(OpenBatchWrites(address, bytes), what + "the fill is not a write of the open batch");
}

void RunWriterOutsideLayers(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::size_t offset) {
    const std::string what = "storage flush ordering, writer outside the image's pending layers: ";
    const auto* words = guest.Words() + offset / 4u;
    const auto image = StoreImage(device, guest, offset);
    const auto fill = image + ImageBytes + FillPast;
    Fill(device, fill, FillBytes, what);
    Require(AgcDriver::Graphics::StorageTexture::FlushPending(image, static_cast<std::size_t>(fill + FillBytes - image), nullptr, "test"), what + "the image store left no results pending");
    Require(OpenBatchWrites(fill, FillBytes), what + "the flush submitted and waited for a writer its stores cannot overwrite");
    device.WaitIdle();
    Require(words[FarDword] == Stored, what + "the pending image's texel was not stored");
    Require(words[(ImageBytes + FillPast) / 4u] == Filled, what + "the fill's bytes did not land");
}

void RunWriterInsideLayer(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::size_t offset) {
    const std::string what = "storage flush ordering, writer inside the image's pending layer: ";
    const auto* words = guest.Words() + offset / 4u;
    const auto image = StoreImage(device, guest, offset);
    const auto fill = image + OverwrittenDword * 4u;
    Fill(device, fill, 16u, what);
    Require(AgcDriver::Graphics::StorageTexture::FlushPending(image, ImageBytes, nullptr, "test"), what + "the image store left no results pending");
    device.WaitIdle();
    Require(words[OverwrittenDword] == Filled, what + "the pending image's store put its older texels over a newer fill");
    Require(words[FarDword] == Stored, what + "the pending image's texel away from the newer fill was not stored");
}

void RunShadowedUnitUnderWriter(AgcDriver::VulkanDevice& device, const GuestBlock& guest) {
    const std::string what = "storage flush ordering, shadowed unit under a writer outside the flushed image's layers: ";
    if (!AgcDriver::Graphics::UnitShadowEnabled()) {
        std::puts("unit shadows are off: the shadowed-unit case is skipped");
        return;
    }
    const auto* shadowedWords = guest.Words();
    const auto* flushedWords = guest.Words() + (BlockBytes / 2u) / 4u;
    const auto shadowedImage = StoreImage(device, guest, 0, Standard256B);
    Require(AgcDriver::Graphics::StorageTexture::FlushPending(shadowedImage, ImageBytes, nullptr, "test", AgcDriver::Graphics::PublishScope::None), what + "the first image's store left no results pending");
    if (!AgcDriver::Graphics::AnyShadowedOverlaps(shadowedImage, ShadowUnitBytes) || !AgcDriver::Graphics::AnyShadowedOverlaps(shadowedImage + ShadowUnitBytes, ShadowUnitBytes)) {
        std::puts("the write-back did not retile both units into the unit shadow: the shadowed-unit case is skipped");
        return;
    }
    Fill(device, shadowedImage, ShadowUnitBytes, what);
    const auto flushedImage = StoreImage(device, guest, BlockBytes / 2u);
    Require(AgcDriver::Graphics::StorageTexture::FlushPending(shadowedImage, static_cast<std::size_t>(flushedImage + ImageBytes - shadowedImage), nullptr, "test"), what + "the second image's store left no results pending");
    Require(!OpenBatchWrites(shadowedImage, ShadowUnitBytes), what + "the flush did not wait for the writer over the shadowed unit it publishes");
    device.WaitIdle();
    Require(shadowedWords[0] == Filled, what + "the publish put the shadowed unit's older texels over a newer fill");
    Require(shadowedWords[ShadowUnitBytes / 4u - 1u] == Filled, what + "the fill's last dword did not land");
    Require(std::count(shadowedWords + ShadowUnitBytes / 4u, shadowedWords + ImageBytes / 4u, Stored) == 1, what + "the shadowed unit the fill did not touch was not published");
    Require(flushedWords[FarDword] == Stored, what + "the flushed image's texel was not stored");
}

void Run(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::size_t offset, bool submit, const char* name) {
    const auto* words = guest.Words() + offset / 4u;
    const auto image = guest.Address() + offset;
    const auto target = device.Target();
    const auto storeImage = Compile(StoreImageCode, ImageUserData(image), target);
    const auto storeGlobal = Compile(StoreGlobalCode, GlobalUserData(image), target);
    device.Dispatch(storeImage, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(StoreImageCode.data()));
    device.Dispatch(storeGlobal, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(StoreGlobalCode.data()));
    if (submit) {
        std::uint64_t serial = 0;
        {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            serial = device.SubmitAndEpoch();
        }
        device.WaitRecorded(serial);
    }
    const std::string what = std::string("storage flush ordering, ") + name + ": ";
    Require(AddressBasedStorePending(image, !submit), what + "the address-based store was not pending in the expected batch at the flush");
    Require(AgcDriver::Graphics::StorageTexture::FlushPending(image, ImageBytes, nullptr, "test"), what + "the image store left no results pending");
    device.WaitIdle();
    Require(words[FarDword] == Stored, what + "the pending image's texel away from the newer write was not stored");
    Require(words[OverwrittenDword] == Overwritten, what + "the pending image's store put its older texels over a newer address-based store");
}

std::uint64_t OpenSerial() {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto* recorder = AgcDriver::Graphics::Recorder::Active();
    Require(recorder != nullptr, "storage flush ordering: no active recorder");
    return recorder->OpenSerial();
}

void RunStoredAgain(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::size_t offset, std::size_t images, bool unrelated) {
    const auto first = guest.Address() + offset;
    const auto target = device.Target();
    std::vector<ShaderRecompiler::RecompileResult> stores, restores;
    for (std::size_t index = 0; index < images; ++index) {
        stores.push_back(Compile(StoreImageCode, ImageUserData(first + index * ImageBytes), target));
        restores.push_back(Compile(RestoreImageCode, ImageUserData(first + index * ImageBytes), target));
    }
    for (const auto& store : stores) device.Dispatch(store, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(StoreImageCode.data()));
    const std::string subject = "storage flush ordering, " + std::to_string(images) + (images == 1 ? " image" : " images") + (unrelated ? " under another write stamped at completion" : "");
    Require(AgcDriver::Graphics::StorageTexture::FlushPending(first, ImageBytes * images, nullptr, "test"), subject + " stored again in the open batch: the first store left no results pending");
    const auto& context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    const bool imported = AgcDriver::Graphics::ExistingHostImportFor(context, first, ImageBytes * images) != nullptr;
    const bool compared = imported && AgcDriver::Graphics::ComparedHostImportCovers(context, first, ImageBytes * images);
    const bool skips = imported && !unrelated;
    const std::string what = subject + (imported ? compared ? " over a compared host import" : "" : " without a host import") + " stored again in the open batch: ";
    for (const auto& restore : restores) device.Dispatch(restore, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(RestoreImageCode.data()));
    if (unrelated) {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        const std::array unstamped{std::pair{first, first + ImageBytes}};
        AgcDriver::Graphics::Recorder::Active()->NotePendingWrites(unstamped);
    }
    const auto open = OpenSerial();
    Require(AgcDriver::Graphics::StorageTexture::FlushPending(first, ImageBytes * images, nullptr, "test"), what + "the second store left no results pending");
    if (!compared || unrelated) {
        if (skips) Require(OpenSerial() == open, what + "the second store waited for the GPU to finish an earlier store stamped when it was recorded");
        else if (unrelated) Require(OpenSerial() != open, what + "the second store did not wait for a write that is not the image's own and stamps guest memory only at completion");
        else Require(OpenSerial() != open, what + "the second store did not wait for the earlier store, which only an image with a host import may skip");
    }
    device.WaitIdle();
    for (std::size_t index = 0; index < images; ++index) {
        const auto* words = guest.Words() + (offset + index * ImageBytes) / 4u;
        Require(words[0] == Restored && words[FarDword] == Restored, what + "the newer texels did not land over the earlier store");
    }
}

void RunStoredAloneAfterJoint(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::size_t offset) {
    const auto first = guest.Address() + offset;
    const auto target = device.Target();
    const std::array stores{Compile(StoreImageCode, ImageUserData(first), target), Compile(StoreImageCode, ImageUserData(first + ImageBytes), target)};
    const auto restore = Compile(RestoreImageCode, ImageUserData(first), target);
    for (const auto& store : stores) device.Dispatch(store, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(StoreImageCode.data()));
    const std::string what = "storage flush ordering, 1 image stored alone after a joint store of 2 images in the open batch: ";
    Require(AgcDriver::Graphics::StorageTexture::FlushPending(first, ImageBytes * 2u, nullptr, "test"), what + "the joint store left no results pending");
    const auto& context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    const bool imported = AgcDriver::Graphics::ExistingHostImportFor(context, first, ImageBytes * 2u) != nullptr;
    const bool compared = imported && AgcDriver::Graphics::ComparedHostImportCovers(context, first, ImageBytes * 2u);
    device.Dispatch(restore, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(RestoreImageCode.data()));
    const auto open = OpenSerial();
    Require(AgcDriver::Graphics::StorageTexture::FlushPending(first, ImageBytes, nullptr, "test"), what + "the second store left no results pending");
    if (!compared && imported) Require(OpenSerial() == open, what + "the second store waited for the GPU to finish the joint store, which was stamped when it was recorded");
    if (!imported) Require(OpenSerial() != open, what + "the second store did not wait for the joint store without a host import");
    device.WaitIdle();
    const auto* words = guest.Words() + offset / 4u;
    Require(words[0] == Restored && words[FarDword] == Restored, what + "the newer texels did not land over the joint store");
}

std::uint64_t NoteOf(std::uint64_t address, std::size_t bytes) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto* recorder = AgcDriver::Graphics::Recorder::Active();
    const auto note = recorder != nullptr ? recorder->LastWriteNote(address, bytes) : 0;
    Require(note != 0, "storage flush ordering: the writer's note is not the open batch's last write");
    return note;
}

bool StillPending(std::uint64_t note, std::uint64_t address, std::size_t bytes) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto* recorder = AgcDriver::Graphics::Recorder::Active();
    if (recorder == nullptr) return false;
    const auto writes = recorder->PendingWritesOver(address, bytes);
    return std::any_of(writes.begin(), writes.end(), [&](const AgcDriver::Graphics::Recorder::PendingWrite& write) { return write.note == note; });
}

bool Verifying() {
    static const bool verifying = std::getenv("APS5_LABEL_STORE_VERIFY") != nullptr;
    return verifying;
}

bool StampsVisible(const AgcDriver::VulkanDevice& device, std::uint64_t address, std::size_t bytes) {
    const auto& context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    return AgcDriver::GuestMemory::Watched(address, bytes) && AgcDriver::Graphics::ExistingHostImportFor(context, address, bytes) != nullptr && !AgcDriver::Graphics::ComparedHostImportCovers(context, address, bytes);
}

void RunStampedWriterLater(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::size_t offset) {
    const std::string what = "storage flush ordering, a writer stamped when recorded after the image's results: ";
    const auto* words = guest.Words() + offset / 4u;
    const auto image = StoreImage(device, guest, offset);
    const auto fill = image + OverwrittenDword * 4u;
    Fill(device, fill, 16u, what);
    const auto note = NoteOf(fill, 16u);
    Require(AgcDriver::Graphics::StorageTexture::FlushPending(image, ImageBytes, nullptr, "test"), what + "the image store left no results pending");
    if (!Verifying() && StampsVisible(device, image, ImageBytes)) Require(StillPending(note, fill, 16u), what + "the store waited for the GPU to finish a fill whose stamp it already sees");
    device.WaitIdle();
    Require(words[OverwrittenDword] == Filled, what + "the image's older texels landed over the newer fill");
    Require(words[FarDword] == Stored, what + "the image's texel in a block the fill did not touch was not stored");
}

void RunReaderGenerationAfterSkippedWait(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::size_t offset) {
    const std::string what = "storage flush ordering, a reader's collect generation taken before a writer the flush did not wait for: ";
    const auto image = StoreImage(device, guest, offset);
    const auto fill = image + OverwrittenDword * 4u;
    const auto seen = AgcDriver::GuestMemory::CollectWrites(fill, 16u);
    if (seen == 0 || !StampsVisible(device, image, ImageBytes)) {
        device.WaitIdle();
        std::puts("the image's memory is compared, not write-watched: the reader generation case is skipped");
        return;
    }
    Fill(device, fill, 16u, what);
    const auto note = NoteOf(fill, 16u);
    Require(AgcDriver::Graphics::StorageTexture::FlushPending(image, ImageBytes, nullptr, "test"), what + "the image store left no results pending");
    if (!Verifying()) Require(StillPending(note, fill, 16u), what + "the store waited for the GPU to finish a fill whose stamp it already sees");
    device.WaitIdle();
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        const auto* recorder = AgcDriver::Graphics::Recorder::Active();
        Require(recorder == nullptr || !recorder->PendingWriteOverlaps(fill, 16u), what + "the fill is still pending after the device went idle");
    }
    AgcDriver::GuestMemory::CollectWritesUncached(fill, 16u);
    Require(!AgcDriver::GuestMemory::UnchangedSince(fill, 16u, seen), what + "the range reads as unchanged since a generation taken before the fill, so a reader would keep bytes from before it");
    Require(guest.Words()[offset / 4u + OverwrittenDword] == Filled, what + "the fill's bytes did not land");
}

void RunStampedWriterEarlier(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::size_t offset) {
    const std::string what = "storage flush ordering, a writer stamped when recorded before the image's results: ";
    const auto* words = guest.Words() + offset / 4u;
    const auto image = guest.Address() + offset;
    Fill(device, image, 16u, what);
    const auto note = NoteOf(image, 16u);
    StoreImage(device, guest, offset);
    if (!StillPending(note, image, 16u)) {
        std::puts("the image's first use waited for the earlier fill: the earlier stamped writer case is skipped");
        device.WaitIdle();
        return;
    }
    Require(AgcDriver::Graphics::StorageTexture::FlushPending(image, ImageBytes, nullptr, "test"), what + "the image store left no results pending");
    if (!Verifying() && StampsVisible(device, image, ImageBytes)) Require(StillPending(note, image, 16u), what + "the store waited for the GPU to finish a fill whose stamp it already sees");
    device.WaitIdle();
    Require(words[0] == Stored, what + "the earlier fill kept its block over the image's newer texel");
    Require(words[FarDword] == Stored, what + "the image's texel away from the fill was not stored");
}

void RunStampedWriterRewritesBaseline(AgcDriver::VulkanDevice& device, const GuestBlock& guest) {
    const std::string what = "storage flush ordering, a writer stamped when recorded that rewrites the image's baseline value after its results: ";
    const auto* words = guest.Words();
    const auto image = StoreImage(device, guest, 0);
    const std::array<std::uint32_t, 4> baseline{Initial, Initial, Initial, Initial};
    Require(device.FillBuffer(image, 16u, baseline), what + "the fill was not recorded on the GPU");
    Require(OpenBatchWrites(image, 16u), what + "the fill is not a write of the open batch");
    if (!StampsVisible(device, image, ImageBytes)) {
        device.WaitIdle();
        std::puts("the image's memory is compared, not write-watched: the baseline rewrite case is skipped");
        return;
    }
    Require(AgcDriver::Graphics::StorageTexture::FlushPending(image, ImageBytes, nullptr, "test"), what + "the image store left no results pending");
    device.WaitIdle();
    Require(words[0] == Initial, what + "the image's older texel landed over the newer fill because the fill's bytes equal the image's baseline");
    Require(words[FarDword] == Stored, what + "the image's texel in a block the fill did not touch was not stored");
}

void RunCompletionWriterWaits(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::size_t offset) {
    const std::string what = "storage flush ordering, a write stamped only at completion over the image's results: ";
    const auto image = StoreImage(device, guest, offset);
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        const std::array unstamped{std::pair{image + OverwrittenDword * 4u, image + OverwrittenDword * 4u + 16u}};
        AgcDriver::Graphics::Recorder::Active()->NotePendingWrites(unstamped);
    }
    const auto note = NoteOf(image + OverwrittenDword * 4u, 16u);
    Require(AgcDriver::Graphics::StorageTexture::FlushPending(image, ImageBytes, nullptr, "test"), what + "the image store left no results pending");
    Require(!StillPending(note, image + OverwrittenDword * 4u, 16u), what + "the store did not wait for a writer whose stamp lands only when its batch completes");
    device.WaitIdle();
    Require(guest.Words()[offset / 4u + FarDword] == Stored, what + "the image's texel was not stored");
}

void RunPollerAfterLabel(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::size_t offset) {
    const std::string what = "storage flush ordering, a guest thread polling a label after a linear image and a newer fill: ";
    const auto* words = guest.Words() + offset / 4u;
    const auto image = guest.Address() + offset;
    device.Dispatch(Compile(RestoreImageCode, ImageUserData(image), device.Target()), 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(RestoreImageCode.data()));
    const auto fill = image + OverwrittenDword * 4u;
    Fill(device, fill, 16u, what);
    const auto note = NoteOf(fill, 16u);
    auto* label = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(image + ImageBytes + LabelPast));
    std::atomic_ref<std::uint32_t>(*label).store(0, std::memory_order_release);
    std::atomic<bool> seen{false};
    std::array<std::uint32_t, 2> read{};
    std::thread poller([&] {
        const auto start = std::chrono::steady_clock::now();
        while (std::atomic_ref<std::uint32_t>(*label).load(std::memory_order_acquire) != LabelValue) {
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(20)) return;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        const volatile std::uint32_t* texels = words;
        read = {texels[FarDword], texels[OverwrittenDword]};
        seen.store(true, std::memory_order_release);
    });
    const std::uint32_t value = LabelValue;
    const auto stored = device.WriteLabelOnGpu(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(label)), std::as_bytes(std::span(&value, 1)), 1, 0, false);
    const bool waited = !StillPending(note, fill, 16u);
    if (stored != 0) {
        device.WaitIdle();
        std::atomic_ref<std::uint32_t>(*label).store(LabelValue, std::memory_order_release);
        poller.join();
        std::printf("the label was not recorded on the GPU (reason %d): the label poller case is skipped\n", stored);
        return;
    }
    device.SubmitRecorded(false);
    poller.join();
    Require(seen.load(std::memory_order_acquire), what + "the label never landed");
    if (!Verifying() && StampsVisible(device, image, ImageBytes)) Require(!waited, what + "the label's readback store waited for the GPU to finish a fill whose stamp it already sees");
    Require(read[0] == Restored, what + "the image's texel was not in guest memory when the label was");
    Require(read[1] == Filled, what + "the image's older texels landed over the newer fill before the label");
    device.WaitIdle();
}

void RunStamped(AgcDriver::VulkanDevice& device, bool watched) {
    {
        GuestBlock guest(watched);
        RunStampedWriterLater(device, guest, 0);
    }
    {
        GuestBlock guest(watched);
        RunStampedWriterEarlier(device, guest, 0);
    }
    {
        GuestBlock guest(watched);
        RunReaderGenerationAfterSkippedWait(device, guest, 0);
    }
    {
        GuestBlock guest(watched);
        RunStampedWriterRewritesBaseline(device, guest);
    }
    {
        GuestBlock guest(watched);
        RunCompletionWriterWaits(device, guest, 0);
    }
    {
        GuestBlock guest(watched);
        RunPollerAfterLabel(device, guest, 0);
    }
}

}

int main(int argc, char** argv) {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const bool watched = AgcDriver::GuestMemory::WriteWatched();
        GuestBlock guest(watched);
        if (argc > 1 && std::string(argv[1]) == "--stamped") {
            RunStamped(*device, watched);
            std::puts("storage flush ordering stamped writer tests passed");
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "--stored-again") {
            RunStoredAgain(*device, guest, BlockBytes / 4u, 1, false);
            RunStoredAgain(*device, guest, BlockBytes / 8u * 5u, 2, false);
            RunStoredAgain(*device, guest, BlockBytes / 8u * 3u, 1, true);
            RunStoredAloneAfterJoint(*device, guest, BlockBytes / 8u * 5u);
            std::puts("storage flush ordering stored-again tests passed");
            return 0;
        }
        if (!watched) std::puts("guest memory has no write watch: the writer-in-flight case is skipped");
        else if (!device->CanWaitUnlocked()) std::puts("the recorder cannot wait for the GPU without running completions: the writer-in-flight case is skipped");
        else Run(*device, guest, 0, true, "writer in flight");
        Run(*device, guest, BlockBytes / 2u, false, "writer in the open batch");
        RunStoredAgain(*device, guest, BlockBytes / 4u, 1, false);
        RunStoredAgain(*device, guest, BlockBytes / 8u * 5u, 2, false);
        RunStoredAgain(*device, guest, BlockBytes / 8u * 3u, 1, true);
        RunStoredAloneAfterJoint(*device, guest, BlockBytes / 8u * 5u);
        RunWriterOutsideLayers(*device, guest, BlockBytes / 4u);
        RunWriterInsideLayer(*device, guest, 3u * BlockBytes / 4u);
        RunStamped(*device, watched);
        GuestBlock shadowed(watched);
        RunShadowedUnitUnderWriter(*device, shadowed);
        std::puts("storage flush ordering tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
