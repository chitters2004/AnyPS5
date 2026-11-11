#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
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
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;

constexpr std::uint32_t Width = 128;
constexpr std::uint32_t Height = 128;
constexpr std::uint32_t FormatRgba16Float = 71;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Standard64KB = 0x09;
constexpr std::size_t SurfaceBytes = 0x20000;
constexpr std::size_t AllocationBytes = std::size_t{Width} * Height * 8u;

alignas(256) constexpr std::array<std::uint32_t, 11> WriteCode{0xd7460004u, 0x0401060eu, 0xd7460005u, 0x0405060fu, 0x7e100d04u, 0x7e120d05u, 0x7e140c0du, 0x7e1602f2u, 0xf0200f08u, 0x00000804u, 0xbf810000u};

std::array<std::uint32_t, 8> TextureDescriptor(const void* texels) {
    const auto address = reinterpret_cast<std::uintptr_t>(texels);
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (FormatRgba16Float << 20u) | (((Width - 1u) & 3u) << 30u),
        ((Width - 1u) >> 2u) | ((Height - 1u) << 14u),
        0xfacu | (Standard64KB << 20u) | (Type2D << 28u),
        0u, 0u, 0u, 0u,
    };
}

void Dispatch(std::uint8_t* texels) {
    std::vector<std::uint32_t> data;
    const auto texture = TextureDescriptor(texels);
    data.insert(data.end(), texture.begin(), texture.end());
    data.resize(14, 0u);
    data[13] = 7u;
    std::vector<std::uint32_t> words;
    const auto registers = [&](std::uint32_t first, std::span<const std::uint32_t> values) {
        words.push_back(0xc0007600u | (static_cast<std::uint32_t>(values.size()) << 16u));
        words.push_back(first);
        words.insert(words.end(), values.begin(), values.end());
    };
    const std::array<std::uint32_t, 3> shape{8, 8, 1};
    registers(0x207, shape);
    const auto code = reinterpret_cast<std::uintptr_t>(WriteCode.data());
    const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(code >> 8u), static_cast<std::uint32_t>(code >> 40u)};
    registers(0x20c, program);
    const std::array<std::uint32_t, 1> resource{(static_cast<std::uint32_t>(data.size()) << 1u) | (7u << 7u) | (1u << 11u)};
    registers(0x213, resource);
    registers(0x240, data);
    words.insert(words.end(), {0xc0031500u, Width / 8u, Height / 8u, 1u, 0x41u});
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    AgcDriver::Submit(&packet, 0);
    AgcDriverWaitIdle_nid_postfix();
}

class GuestSurface {
public:
    GuestSurface() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(SurfaceBytes, Alignment));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, SurfaceBytes, PAGE_READWRITE, SurfaceBytes);
#else
        void* mapped = mmap(nullptr, SurfaceBytes + Alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + Alignment - 1) & ~(static_cast<std::uintptr_t>(Alignment) - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + SurfaceBytes != begin + SurfaceBytes + Alignment) munmap(reinterpret_cast<void*>(aligned + SurfaceBytes), begin + Alignment - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            if (AgcDriver::GuestMemory::WriteWatched()) GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, SurfaceBytes);
        }
#endif
        Require(block != nullptr, "storage write-back run: cannot allocate a guest surface");
        std::memset(block, 0, SurfaceBytes);
        GuestAllocations::Mutation mutation;
        mutation.Add(block, SurfaceBytes, true, true, true);
    }

    ~GuestSurface() {
        {
            GuestAllocations::Mutation mutation;
            mutation.Remove(block);
        }
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, SurfaceBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, SurfaceBytes);
#else
        munmap(block, SurfaceBytes);
        if (AgcDriver::GuestMemory::WriteWatched()) GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, SurfaceBytes);
#endif
    }

    GuestSurface(const GuestSurface&) = delete;
    GuestSurface& operator=(const GuestSurface&) = delete;

    std::uint8_t* Data() { return block; }

private:
    static constexpr std::size_t Alignment = 65536;
    std::uint8_t* block = nullptr;
};

struct Stored {
    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> second;
    std::uint64_t joins = 0;
};

Stored WriteBackBoth(bool runs) {
    AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::NoWriteBackRuns, !runs);
    GuestSurface first;
    GuestSurface second;
    Dispatch(first.Data());
    Dispatch(second.Data());
    const auto joins = AgcDriver::Graphics::Recorder::WriteBackJoins();
    AgcDriver::Graphics::StorageTexture::FlushAllPending("test");
    AgcDriverWaitIdle_nid_postfix();
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(first.Data()), SurfaceBytes);
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(second.Data()), SurfaceBytes);
    Stored stored;
    stored.first.assign(first.Data(), first.Data() + SurfaceBytes);
    stored.second.assign(second.Data(), second.Data() + SurfaceBytes);
    stored.joins = AgcDriver::Graphics::Recorder::WriteBackJoins() - joins;
    return stored;
}

}

int main() {
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        const auto alone = WriteBackBoth(false);
        Require(alone.joins == 0, "storage write-back run: a write-back joined a run with APS5_NO_WRITE_BACK_RUNS set");
        const auto texels = [](const std::vector<std::uint8_t>& bytes) {
            std::size_t count = 0;
            for (std::size_t at = 0; at < AllocationBytes; at += 8) count += !std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin() + static_cast<std::ptrdiff_t>(at + 8), [](std::uint8_t byte) { return byte == 0; });
            return count;
        };
        Require(texels(alone.first) != 0 && texels(alone.second) != 0, "storage write-back run: the surfaces written back one at a time received no texels");
        const auto joined = WriteBackBoth(true);
        if (joined.joins == 0) {
            std::puts("storage write-back run: no write-back here takes the direct single-pass path; skipped");
            AgcDriverShutdown_nid_postfix();
            return VulkanTestSkipped;
        }
        const auto compare = [](const std::vector<std::uint8_t>& expected, const std::vector<std::uint8_t>& stored, const char* which) {
            const auto differs = std::mismatch(expected.begin(), expected.end(), stored.begin());
            Require(differs.first == expected.end(), std::string("storage write-back run: byte ") + std::to_string(differs.first - expected.begin()) + " of the " + which + " surface differs from its write-back outside a run");
        };
        compare(alone.first, joined.first, "first");
        compare(alone.second, joined.second, "second");
        AgcDriverShutdown_nid_postfix();
        std::printf("storage write-back run tests passed (%llu joined)\n", static_cast<unsigned long long>(joined.joins));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
