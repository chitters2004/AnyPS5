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
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::StorageTexture;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 256;
constexpr std::uint32_t Height = 128;
constexpr std::size_t ImageBytes = Width * Height * 4u;
constexpr std::size_t BlockBytes = ImageBytes;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Initial = 0x3f800000u;
constexpr std::uint32_t Stored = 0x11111111u;
constexpr std::uint32_t Filled = 0x55555555u;

alignas(256) constexpr std::array<std::uint32_t, 11> StoreCode{0x7e000280u, 0x7e020280u, 0x7e0402ffu, Stored, 0xf0200108u, 0x00010200u, 0x7e0202ffu, 0x00000064u, 0xf0200108u, 0x00010200u, 0xbf810000u};

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
        Require(block != nullptr, "label wait counters: cannot allocate the guest block");
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

constexpr std::size_t Label = 0;
constexpr std::size_t Other = 1;

std::string Describe(const StorageTexture::FlushWaitCounts& counts) {
    return "flushes " + std::to_string(counts.flushes) + ", ordered " + std::to_string(counts.ordered) + ", no writer " + std::to_string(counts.none) + ", stamped only " + std::to_string(counts.stampedOnly) + ", unstamped " + std::to_string(counts.unstamped);
}

void Expect(const std::string& what, const StorageTexture::FlushWaitCounts& counts, const StorageTexture::FlushWaitCounts& expected) {
    const bool same = counts.flushes == expected.flushes && counts.ordered == expected.ordered && counts.none == expected.none && counts.stampedOnly == expected.stampedOnly && counts.unstamped == expected.unstamped;
    Require(same, "label wait counters, " + what + ": counted " + Describe(counts) + ", expected " + Describe(expected));
}

class Cases {
public:
    Cases(AgcDriver::VulkanDevice& device, const GuestBlock& guest) : device(device), guest(guest), program(Compile(StoreCode, ImageUserData(guest.Address()), device.Target())) {}

    void Store() {
        device.Dispatch(program, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(StoreCode.data()));
    }

    void Settle() {
        device.WaitIdle();
        static_cast<void>(StorageTexture::TakeFlushWaitCounts());
    }

    void NoWriter() {
        Settle();
        Store();
        Require(StorageTexture::FlushPending(guest.Address(), ImageBytes, nullptr, "test"), "label wait counters, no writer: the image left no results pending");
        const auto counts = StorageTexture::TakeFlushWaitCounts();
        Expect("an image with no other writer", counts[Other], {1, 0, 1, 0, 0});
        Expect("an image with no other writer, label scope", counts[Label], {0, 0, 0, 0, 0});
    }

    void StampedFill() {
        Settle();
        Store();
        const std::array<std::uint32_t, 4> pattern{Filled, Filled, Filled, Filled};
        Require(device.FillBuffer(guest.Address(), 16u, pattern), "label wait counters, stamped fill: the fill was not recorded on the GPU");
        Require(StorageTexture::FlushPending(guest.Address(), ImageBytes, nullptr, "test"), "label wait counters, stamped fill: the image left no results pending");
        const auto counts = StorageTexture::TakeFlushWaitCounts();
        Expect("a GPU fill stamped when recorded", counts[Other], {1, 0, 0, 1, 0});
    }

    void UnstampedNote() {
        Settle();
        Store();
        {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            auto* recorder = AgcDriver::Graphics::Recorder::Active();
            Require(recorder != nullptr, "label wait counters, unstamped note: no active recorder");
            recorder->NotePendingWrite(guest.Address(), 16u);
        }
        Require(StorageTexture::FlushPending(guest.Address(), ImageBytes, nullptr, "test"), "label wait counters, unstamped note: the image left no results pending");
        const auto counts = StorageTexture::TakeFlushWaitCounts();
        Expect("a write note not stamped when recorded", counts[Other], {1, 0, 0, 0, 1});
    }

    void LabelStore() {
        Settle();
        Store();
        StorageTexture::StoreReadbackBeforeLabel("readback before label", VK_NULL_HANDLE);
        const auto counts = StorageTexture::TakeFlushWaitCounts();
        Require(counts[Label].flushes == 1 && counts[Label].ordered + counts[Label].none == 1 && counts[Label].stampedOnly + counts[Label].unstamped == 0, "label wait counters, label store: counted " + Describe(counts[Label]) + ", expected one flush that skipped or had no writer");
        Expect("a label store, other scope", counts[Other], {0, 0, 0, 0, 0});
        std::printf("label wait counters: label store counted %s\n", Describe(counts[Label]).c_str());
    }

private:
    AgcDriver::VulkanDevice& device;
    const GuestBlock& guest;
    ShaderRecompiler::RecompileResult program;
};

}

int main() {
    try {
#ifdef _WIN32
        _putenv_s("APS5_PROFILE_DRAW", "1");
#else
        setenv("APS5_PROFILE_DRAW", "1", 1);
#endif
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        GuestBlock guest(AgcDriver::GuestMemory::WriteWatched());
        Cases cases(*device, guest);
        cases.NoWriter();
        cases.StampedFill();
        cases.UnstampedNote();
        cases.LabelStore();
        device->WaitIdle();
        std::puts("label wait counters tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
