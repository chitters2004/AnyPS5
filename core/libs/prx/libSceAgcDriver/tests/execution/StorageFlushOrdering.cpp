#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
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
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <vector>

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
constexpr std::size_t OverwrittenDword = 0x100u / 4u;
constexpr std::size_t FarDword = 100u * Width;

alignas(256) constexpr std::array<std::uint32_t, 11> StoreImageCode{0x7e000280u, 0x7e020280u, 0x7e0402ffu, Stored, 0xf0200108u, 0x00010200u, 0x7e0202ffu, 0x00000064u, 0xf0200108u, 0x00010200u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 7> StoreGlobalCode{0x7e000208u, 0x7e020209u, 0x7e0402ffu, Overwritten, 0xdc708100u, 0x007d0200u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 7> StoreBaselineCode{0x7e000208u, 0x7e020209u, 0x7e0402ffu, Initial, 0xdc708100u, 0x007d0200u, 0xbf810000u};

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

std::vector<std::uint32_t> ImageUserData(std::uint64_t base) {
    std::vector<std::uint32_t> userData(12, 0u);
    userData[4] = static_cast<std::uint32_t>(base >> 8u);
    userData[5] = static_cast<std::uint32_t>((base >> 40u) & 0xffu) | (Format32UInt << 20u) | (((Width - 1u) & 3u) << 30u);
    userData[6] = ((Width - 1u) >> 2u) | ((Height - 1u) << 14u);
    userData[7] = 0xfacu | (Type2D << 28u);
    return userData;
}

std::vector<std::uint32_t> GlobalUserData(std::uint64_t base) {
    std::vector<std::uint32_t> userData(10, 0u);
    userData[8] = static_cast<std::uint32_t>(base);
    userData[9] = static_cast<std::uint32_t>(base >> 32u);
    return userData;
}

bool AddressBasedStorePending(std::uint64_t image, bool open, std::size_t bytes = ImageBytes) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto* recorder = AgcDriver::Graphics::Recorder::Active();
    const auto read = recorder != nullptr ? recorder->DescribePendingRead(image + bytes, 4u) : std::nullopt;
    return read.has_value() && read->kind == AgcDriver::Graphics::Recorder::ReadKind::AddressBased && read->open == open;
}

constexpr std::uint32_t TiledSide = 128;
constexpr std::size_t TiledBytes = TiledSide * TiledSide * 4u;
constexpr std::uint32_t TileStandard64KB = 9;

std::vector<std::uint32_t> TiledUserData(std::uint64_t base) {
    std::vector<std::uint32_t> userData(12, 0u);
    userData[4] = static_cast<std::uint32_t>(base >> 8u);
    userData[5] = static_cast<std::uint32_t>((base >> 40u) & 0xffu) | (Format32UInt << 20u) | (((TiledSide - 1u) & 3u) << 30u);
    userData[6] = ((TiledSide - 1u) >> 2u) | ((TiledSide - 1u) << 14u);
    userData[7] = 0xfacu | (TileStandard64KB << 20u) | (Type2D << 28u);
    return userData;
}

void RunTiled(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::size_t offset, const char* name, bool baseline) {
    const auto* words = guest.Words() + offset / 4u;
    const auto image = guest.Address() + offset;
    const auto target = device.Target();
    const auto& globalCode = baseline ? StoreBaselineCode : StoreGlobalCode;
    const auto storeImage = Compile(StoreImageCode, TiledUserData(image), target);
    const auto storeGlobal = Compile(globalCode, GlobalUserData(image), target);
    device.Dispatch(storeImage, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(StoreImageCode.data()));
    device.Dispatch(storeGlobal, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(globalCode.data()));
    const std::string what = std::string("storage flush ordering, tiled ") + name + ": ";
    Require(AddressBasedStorePending(image, true, TiledBytes), what + "the address-based store was not pending in the open batch at the flush");
    std::shared_ptr<AgcDriver::Graphics::StorageTexture> texture;
    std::uint64_t before = 0;
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        texture = AgcDriver::Graphics::StorageTexture::FindLive(image, TiledBytes);
        if (texture != nullptr) before = texture->Generation();
    }
    Require(texture != nullptr, what + "the image has no storage texture");
    Require(AgcDriver::Graphics::StorageTexture::FlushPending(image, TiledBytes, nullptr, "readback before label"), what + "the image store left no results pending");
    Require(AddressBasedStorePending(image, true, TiledBytes), what + "the label store waited for the address-based store instead of comparing on the GPU");
    device.WaitIdle();
    const auto stored = std::count(words, words + TiledBytes / 4u, Stored);
    if (baseline) {
        Require(stored == 2 && words[OverwrittenDword] == Initial, what + "the image's two texels were not stored over a write of the same bytes");
    } else {
        Require(stored == 0 && words[OverwrittenDword] == Overwritten, what + "the image's store put its older texels over a newer address-based store");
    }
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const bool synced = AgcDriver::GuestMemory::Watched(image, TiledBytes) ? texture->Generation() > before : texture->GuestSnapshotValid();
    if (baseline) Require(synced, what + "the image stored whole on the GPU was left stale at completion");
    else Require(!synced, what + "the image whose block the GPU kept was put back in sync at completion");
}

std::vector<std::uint32_t> RunLeaseTable(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::size_t offset, bool label, bool inside) {
    const auto* words = guest.Words() + offset / 4u;
    const auto image = guest.Address() + offset;
    const auto target = device.Target();
    const std::array<std::uint32_t, 4> pattern{0x33333333u, 0x33333333u, 0x33333333u, 0x33333333u};
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Require(device.FillBuffer(image, 4096u, pattern), "storage flush ordering, lease table: the GPU fill was not recorded");
    }
    std::uint64_t serial = 0;
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        serial = device.SubmitAndEpoch();
    }
    device.WaitRecorded(serial);
    const auto storeImage = Compile(StoreImageCode, ImageUserData(image), target);
    const auto storeGlobal = Compile(StoreGlobalCode, GlobalUserData(inside ? image : image + ImageBytes), target);
    device.Dispatch(storeImage, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(StoreImageCode.data()));
    device.Dispatch(storeGlobal, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(StoreGlobalCode.data()));
    const std::string what = std::string("storage flush ordering, lease table ") + (inside ? "inside" : "outside") + (label ? ", label: " : ", waited: ");
    Require(AgcDriver::Graphics::StorageTexture::FlushPending(image, ImageBytes, nullptr, label ? "readback before label" : "test"), what + "the image store left no results pending");
    if (label) Require(AddressBasedStorePending(image, true), what + "the label store waited for the address-based store instead of reading its table on the GPU");
    device.WaitIdle();
    return {words, words + (ImageBytes + 0x200u) / 4u};
}

void Run(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::size_t offset, bool submit, const char* name, bool gpuCompare, bool baseline = false) {
    const auto* words = guest.Words() + offset / 4u;
    const auto image = guest.Address() + offset;
    const auto target = device.Target();
    const auto storeImage = Compile(StoreImageCode, ImageUserData(image), target);
    const auto& globalCode = baseline ? StoreBaselineCode : StoreGlobalCode;
    const auto storeGlobal = Compile(globalCode, GlobalUserData(image), target);
    device.Dispatch(storeImage, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(StoreImageCode.data()));
    device.Dispatch(storeGlobal, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(globalCode.data()));
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
    std::shared_ptr<AgcDriver::Graphics::StorageTexture> texture;
    std::uint64_t before = 0;
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        texture = AgcDriver::Graphics::StorageTexture::FindLive(image, ImageBytes);
        if (texture != nullptr) before = texture->Generation();
    }
    Require(AgcDriver::Graphics::StorageTexture::FlushPending(image, ImageBytes, nullptr, gpuCompare ? "readback before label" : "test"), what + "the image store left no results pending");
    if (gpuCompare && !submit) Require(AddressBasedStorePending(image, true), what + "the label store waited for the address-based store instead of comparing on the GPU");
    device.WaitIdle();
    Require(words[FarDword] == Stored, what + "the pending image's texel away from the newer write was not stored");
    if (baseline) {
        Require(words[0] == Stored && words[OverwrittenDword] == Initial, what + "the pending image's results were not stored over a write of the same bytes");
    } else {
        Require(words[OverwrittenDword] == Overwritten, what + "the pending image's store put its older texels over a newer address-based store");
    }
    if (!gpuCompare || submit) return;
    Require(texture != nullptr, what + "the stored image is no longer live");
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const bool watched = AgcDriver::GuestMemory::Watched(image, ImageBytes);
    const bool synced = watched ? texture->Generation() > before : texture->GuestSnapshotValid();
    if (baseline) Require(synced, what + "the image stored whole on the GPU was left stale at completion");
    else Require(!synced, what + "the image whose block the GPU kept was put back in sync at completion");
}

}

int main(int argc, char** argv) {
    try {
        const bool gpuCompare = argc == 2 && std::string(argv[1]) == "--label-gpu-compare";
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const bool watched = AgcDriver::GuestMemory::WriteWatched();
        GuestBlock guest(watched);
        if (!watched) std::puts("guest memory has no write watch: the writer-in-flight case is skipped");
        else if (!device->CanWaitUnlocked()) std::puts("the recorder cannot wait for the GPU without running completions: the writer-in-flight case is skipped");
        else Run(*device, guest, 0, true, "writer in flight", gpuCompare);
        Run(*device, guest, BlockBytes / 2u, false, "writer in the open batch", gpuCompare);
        if (gpuCompare) {
            Run(*device, guest, BlockBytes / 4u, false, "writer of the baseline bytes in the open batch", gpuCompare, true);
            RunTiled(*device, guest, BlockBytes / 8u, "writer in the open batch", false);
            RunTiled(*device, guest, 3u * BlockBytes / 4u, "writer of the baseline bytes in the open batch", true);
            for (const bool inside : {true, false}) {
                GuestBlock waitedBlock(watched);
                GuestBlock labelBlock(watched);
                const auto waited = RunLeaseTable(*device, waitedBlock, 0, false, inside);
                const auto stored = RunLeaseTable(*device, labelBlock, 0, true, inside);
                for (std::size_t index = 0; index < waited.size(); ++index) {
                    if (waited[index] != stored[index]) {
                        std::fprintf(stderr, "lease table %s: word 0x%zx waited 0x%08x label 0x%08x\n", inside ? "inside" : "outside", index, waited[index], stored[index]);
                        break;
                    }
                }
                Require(waited == stored, std::string("storage flush ordering, lease table ") + (inside ? "inside" : "outside") + ": the GPU decision differs from the decision after the wait");
            }
        }
        std::puts("storage flush ordering tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
