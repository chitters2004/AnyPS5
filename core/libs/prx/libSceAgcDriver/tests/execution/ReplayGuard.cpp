#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ReplayScope.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
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
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using AgcDriver::Graphics::Context;
using AgcDriver::Graphics::DescribeSurface;
using AgcDriver::Graphics::GuestTextureResource;
using AgcDriver::Graphics::Recorder;
using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::StorageTexture;
using AgcDriver::Graphics::TextureDetiler;
using AgcDriver::Graphics::TextureDimension;
using AgcDriver::Graphics::TextureTileMode;

constexpr std::size_t UnitBytes = 65536;

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
        Require(block != nullptr, "replay guard: cannot allocate the guest block");
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

struct Fixture {
    explicit Fixture(AgcDriver::VulkanDevice& device) : context(device.graphicsContext()), detiler(context), block(UnitBytes) {
        context.detiler = &detiler;
        GuestTextureResource resource{};
        resource.width = 64;
        resource.height = 64;
        resource.mipCount = 1;
        resource.tileMode = TextureTileMode::kR64KBX;
        resource.dimension = TextureDimension::k2D;
        resource.format = 22;
        resource.dstSelX = 4;
        resource.dstSelY = 5;
        resource.dstSelZ = 6;
        resource.dstSelW = 7;
        resource.baseAddress = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block.Data()));
        Require(DescribeSurface(resource).guestBytes <= UnitBytes, "replay guard: the texture fixture does not fit its guest block");
        texture = StorageTexture::Create(context, detiler, resource, 0);
        address = resource.baseAddress;
    }

    Context context;
    TextureDetiler detiler;
    GuestBlock block;
    std::shared_ptr<StorageTexture> texture;
    std::uint64_t address = 0;
};

using Site = std::function<void(Fixture&)>;

const std::map<std::string, Site>& Sites() {
    static const std::map<std::string, Site> sites = {
        {"GuestMemory::Read", [](Fixture& fixture) { std::byte bytes[4]; AgcDriver::GuestMemory::Read(fixture.address, bytes); }},
        {"GuestMemory::ReadCommitted", [](Fixture& fixture) { std::byte bytes[4]; AgcDriver::GuestMemory::ReadCommitted(fixture.address, bytes); }},
        {"GuestMemory::ReadCommittedUnsynced", [](Fixture& fixture) { std::byte bytes[4]; AgcDriver::GuestMemory::ReadCommittedUnsynced(fixture.address, bytes); }},
        {"GuestMemory::Write", [](Fixture& fixture) { const std::byte bytes[4]{}; AgcDriver::GuestMemory::Write(fixture.address, bytes); }},
        {"GuestMemory::CollectWrites", [](Fixture& fixture) { AgcDriver::GuestMemory::CollectWrites(fixture.address, 4); }},
        {"GuestMemory::CollectWritesUncached", [](Fixture& fixture) { AgcDriver::GuestMemory::CollectWritesUncached(fixture.address, 4); }},
        {"GuestMemory::FlushGpuWrites", [](Fixture& fixture) { AgcDriver::GuestMemory::FlushGpuWrites(fixture.address, 4); }},
        {"Recorder::Active", [](Fixture&) { static_cast<void>(Recorder::Active()); }},
        {"HostImportFor", [](Fixture& fixture) { static_cast<void>(AgcDriver::Graphics::HostImportFor(fixture.context, fixture.address, 4)); }},
        {"ExistingHostImportFor", [](Fixture& fixture) { static_cast<void>(AgcDriver::Graphics::ExistingHostImportFor(fixture.context, fixture.address, 4)); }},
        {"StorageTexture::MarkDirty", [](Fixture& fixture) { fixture.texture->MarkDirty(); }},
        {"StorageTexture::MarkSliceDirty", [](Fixture& fixture) { fixture.texture->MarkSliceDirty(0); }},
        {"StorageTexture::MarkDrawn", [](Fixture& fixture) { static_cast<void>(fixture.texture->MarkDrawn(0, 1, VkRect2D{{0, 0}, {64, 64}})); }},
        {"StorageTexture::MarkWholeWritten", [](Fixture& fixture) { fixture.texture->MarkWholeWritten(); }},
        {"StorageTexture::MarkUnitsWritten", [](Fixture& fixture) { fixture.texture->MarkUnitsWritten(std::vector<bool>{true}); }},
        {"StorageTexture::Flush", [](Fixture& fixture) { fixture.texture->Flush("replay guard test"); }},
        {"StorageTexture::WriteBack", [](Fixture& fixture) { fixture.texture->WriteBack(); }},
        {"StorageTexture::Detach", [](Fixture& fixture) { fixture.texture->Detach(); }},
    };
    return sites;
}

void ScopeMarksOnlyItsThread() {
    Require(!AgcDriver::Replay::Replaying(), "replay guard: a thread outside every replay scope reads as replaying");
    {
        const AgcDriver::Replay::Scope outer;
        Require(AgcDriver::Replay::Replaying(), "replay guard: a replay scope did not mark its thread");
        bool other = true;
        std::thread([&] { other = AgcDriver::Replay::Replaying(); }).join();
        Require(!other, "replay guard: a replay scope marked another thread");
        {
            const AgcDriver::Replay::Scope inner;
            Require(AgcDriver::Replay::Replaying(), "replay guard: a nested replay scope did not mark its thread");
        }
        Require(AgcDriver::Replay::Replaying(), "replay guard: leaving a nested replay scope unmarked the outer one");
    }
    Require(!AgcDriver::Replay::Replaying(), "replay guard: leaving the replay scope left its thread marked");
}

int CallInReplay(AgcDriver::VulkanDevice& device, const std::string& name) {
    const auto site = Sites().find(name);
    Require(site != Sites().end(), "replay guard: no guarded site is named " + name);
    Fixture fixture(device);
    std::signal(SIGABRT, [](int) { std::_Exit(0); });
    {
        const AgcDriver::Replay::Scope replay;
        site->second(fixture);
    }
    std::signal(SIGABRT, SIG_DFL);
    device.WaitIdle();
    if (!AgcDriver::Replay::Guarded()) {
        std::cout << "the replay guard is off: " << name << " ran inside a replay scope\n";
        return 0;
    }
    std::cerr << name << " returned inside a replay scope instead of stopping the process\n";
    return 1;
}

}

int main(int argc, char** argv) {
    try {
        ScopeMarksOnlyItsThread();
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (argc > 2 && std::string(argv[1]) == "--site") return CallInReplay(*device, argv[2]);
        for (const auto& [name, site] : Sites()) {
            Fixture fixture(*device);
            site(fixture);
            device->WaitIdle();
        }
        std::printf("replay guard tests passed (%zu guarded sites run outside a replay scope)\n", Sites().size());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
