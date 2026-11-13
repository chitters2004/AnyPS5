#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
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
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::ColorTargetLayout;
using AgcDriver::Graphics::ColorTileMode;
using AgcDriver::Graphics::DrawInputCounters;
using AgcDriver::Graphics::DrawInputOutcome;
using AgcDriver::Graphics::DrawInputStage;
using AgcDriver::Graphics::Recorder;
using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::size_t BufferBytes = 65536;
constexpr std::uint32_t Width = 256;
constexpr std::uint32_t Height = 128;

alignas(256) constexpr std::array<std::uint32_t, 7> VertexCode{
    0xbe822100, 0x34020a84, 0xe0781000, 0x80010801, 0xf80008cf, 0x0b0a0908, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 4> PixelCode{
    0x7e0e02f2, 0xf800180f, 0x07070707, 0xbf810000,
};

std::uint64_t AddressOf(const void* data) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
}

std::array<std::uint32_t, 4> VertexBuffer(const void* data, std::uint32_t stride, std::uint32_t count) {
    const auto address = AddressOf(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), count, (0x0104dfacu & ~0xfffu) | 0xfacu | (3u << 28u)};
}

std::array<std::uint32_t, 4> RawBuffer(const void* data, std::uint32_t bytes) {
    const auto address = AddressOf(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

class GuestBuffer {
public:
    explicit GuestBuffer(bool registered, std::size_t bytes = BufferBytes) : registered(registered), bytes(bytes), watched(registered && AgcDriver::GuestMemory::WriteWatched()) {
#ifdef _WIN32
        if (watched) {
            block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, 65536));
            if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
        } else {
            block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        }
#else
        if (watched) {
            constexpr std::uintptr_t alignment = 65536;
            void* mapped = mmap(nullptr, bytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            Require(mapped != MAP_FAILED, "draw input counters: cannot map a guest buffer");
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + bytes != begin + bytes + alignment) munmap(reinterpret_cast<void*>(aligned + bytes), begin + alignment - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
        } else {
            block = static_cast<std::uint8_t*>(std::aligned_alloc(65536, bytes));
        }
#endif
        Require(block != nullptr, "draw input counters: cannot allocate a guest buffer");
        std::memset(block, 0, bytes);
        if (registered) GuestAllocations::Mutation().Add(block, bytes, true, true, true);
    }

    ~GuestBuffer() {
        if (registered) {
            AgcDriver::Graphics::StorageTexture::FlushPending(AddressOf(block), bytes, nullptr, "test release");
            GuestAllocations::Mutation().Remove(block);
        }
#ifdef _WIN32
        if (watched) {
            GuestArena::GuestArenaReset_nid_postfix(block, bytes);
            GuestArena::GuestArenaRelease_nid_postfix(block, bytes);
        } else {
            VirtualFree(block, 0, MEM_RELEASE);
        }
#else
        if (watched) {
            munmap(block, bytes);
            GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, bytes);
        } else {
            std::free(block);
        }
#endif
    }

    GuestBuffer(const GuestBuffer&) = delete;
    GuestBuffer& operator=(const GuestBuffer&) = delete;

    std::uint8_t* Data() { return block; }

private:
    bool registered;
    std::size_t bytes;
    bool watched;
    std::uint8_t* block = nullptr;
};

Recorder& ActiveRecorder() {
    auto* recorder = Recorder::Active();
    Require(recorder != nullptr, "draw input counters: no active recorder");
    return *recorder;
}

const ColorTargetLayout TargetLayout(Width, Height, ColorTileMode::RenderTarget, 4);

constexpr std::array<std::array<float, 4>, 8> Quads{{
    {-1.0f, -1.0f, 0.5f, 1.0f}, {0.0f, -1.0f, 0.5f, 1.0f}, {0.0f, 0.0f, 0.5f, 1.0f}, {-1.0f, 0.0f, 0.5f, 1.0f},
    {0.0f, 0.0f, 0.5f, 1.0f}, {1.0f, 0.0f, 0.5f, 1.0f}, {1.0f, 1.0f, 0.5f, 1.0f}, {0.0f, 1.0f, 0.5f, 1.0f}
}};

void FillVertices(GuestBuffer& vertices) {
    std::memcpy(vertices.Data(), Quads.data(), sizeof(Quads));
}

void Draw(AgcDriver::VulkanDevice& device, GuestBuffer& pixels, GuestBuffer& vertices, const AgcDriver::Pm4::DrawParameters& draw, bool quadList) {
    const auto target = device.Target();
    static GuestBuffer output(false);
    std::vector<std::uint32_t> vertexUserData(12, 0u);
    vertexUserData[0] = 0x10000000u;
    vertexUserData[1] = 0x00000001u;
    const auto outputBuffer = RawBuffer(output.Data(), static_cast<std::uint32_t>(BufferBytes));
    std::copy(outputBuffer.begin(), outputBuffer.end(), vertexUserData.begin() + 4);
    ShaderRecompiler::ShaderVertexStageInfo info{};
    info.resourcesNum = 1;
    info.resources[0].fields = VertexBuffer(vertices.Data(), 16u, static_cast<std::uint32_t>(BufferBytes / 16u));
    info.resourcesDst[0] = {8, 4, 0, 0};
    info.fetchAttribReg = 8;
    info.fetchBufferReg = 10;
    info.fetchEmbedded = true;
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {64, 0, vertexUserData, std::nullopt, std::nullopt, info, vertexMemory},
        target,
        {0, 0, 0, 64}
    };
    vertex.useCache = false;
    const auto vertexResult = ShaderRecompiler::Recompile(vertex);
    Require(vertexResult.vertexAttributes.size() == 1u, "draw input counters: the fetch did not become a vertex attribute");
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());

    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionX) | ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionY);
    pixel.posX = true;
    pixel.posY = true;
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    std::vector<std::uint32_t> pixelUserData(4, 0u);
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(PixelCode.data()), std::as_bytes(std::span(PixelCode))}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}},
        {64, 0, pixelUserData, std::nullopt, pixel, std::nullopt, pixelMemory},
        target,
        PixelPushLayout(vertexPush, target)
    };
    fragment.useCache = false;
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertexResult, 0},
        {ShaderStage::Fragment, &pixelResult, PixelPushOffset(vertexPush, target)}
    }};

    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 64u, 64u, std::nullopt, std::nullopt};
    state.color = {AddressOf(pixels.Data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, TargetLayout.Bytes(), 0xe4u, ColorTileMode::RenderTarget};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.quadList = quadList;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    state.blendConstants = {};
    device.Draw(state, draw, shaders);
}

std::uint64_t Calls(const DrawInputCounters& before, const DrawInputCounters& after, DrawInputStage stage) {
    const auto index = static_cast<std::size_t>(stage);
    return after.calls[index] - before.calls[index];
}

std::uint64_t Amount(const DrawInputCounters& before, const DrawInputCounters& after, DrawInputStage stage) {
    const auto index = static_cast<std::size_t>(stage);
    return after.amounts[index] - before.amounts[index];
}

std::uint64_t Outcomes(const DrawInputCounters& before, const DrawInputCounters& after, DrawInputOutcome outcome) {
    const auto index = static_cast<std::size_t>(outcome);
    return after.outcomes[index] - before.outcomes[index];
}

void Expect(const std::string& what, std::uint64_t actual, std::uint64_t expected) {
    Require(actual == expected, "draw input counters: " + what + " counted " + std::to_string(actual) + ", expected " + std::to_string(expected));
}

void CopiedDraw(AgcDriver::VulkanDevice& device, GuestBuffer& pixels) {
    GuestBuffer vertices(false);
    FillVertices(vertices);
    device.WaitIdle();
    const auto before = AgcDriver::Graphics::DrawInputCountersNow();
    Draw(device, pixels, vertices, {0, 3, 0, 1, 0, false}, false);
    const auto after = AgcDriver::Graphics::DrawInputCountersNow();
    device.WaitIdle();
    Expect("a copied vertex draw's fetch plans", Calls(before, after, DrawInputStage::FetchPlan), 1);
    Expect("a copied vertex draw's copy lookups", Calls(before, after, DrawInputStage::Lookup), 1);
    Expect("a copied vertex draw's allocations", Calls(before, after, DrawInputStage::Allocate), 1);
    Expect("a copied vertex draw's reads", Calls(before, after, DrawInputStage::Read), 1);
    Expect("a copied vertex draw's keeps", Calls(before, after, DrawInputStage::Keep), 1);
    Expect("a copied vertex draw's copied ranges", Outcomes(before, after, DrawInputOutcome::Copied), 1);
    Expect("a copied vertex draw's in-place ranges", Outcomes(before, after, DrawInputOutcome::InPlace), 0);
    Expect("a copied vertex draw's expansions", Calls(before, after, DrawInputStage::Expand), 0);
    Expect("a copied vertex draw's index scans", Calls(before, after, DrawInputStage::IndexScan), 0);
    Expect("a copied vertex draw's read bytes", Amount(before, after, DrawInputStage::Read), Amount(before, after, DrawInputStage::Lookup));
    Require(Amount(before, after, DrawInputStage::Read) >= 3u * 16u, "draw input counters: a copied vertex draw read " + std::to_string(Amount(before, after, DrawInputStage::Read)) + " bytes for three 16-byte vertices");
}

void IndexedDraw(AgcDriver::VulkanDevice& device, GuestBuffer& pixels) {
    GuestBuffer vertices(false);
    GuestBuffer indices(false, 4096);
    FillVertices(vertices);
    constexpr std::array<std::uint16_t, 6> Triangles{0, 1, 2, 0, 2, 3};
    std::memcpy(indices.Data(), Triangles.data(), sizeof(Triangles));
    device.WaitIdle();
    const auto before = AgcDriver::Graphics::DrawInputCountersNow();
    Draw(device, pixels, vertices, {AddressOf(indices.Data()), static_cast<std::uint32_t>(Triangles.size()), 2, 1, 0, true}, false);
    const auto after = AgcDriver::Graphics::DrawInputCountersNow();
    device.WaitIdle();
    Expect("an indexed draw's copy lookups", Calls(before, after, DrawInputStage::Lookup), 2);
    Expect("an indexed draw's index scans", Calls(before, after, DrawInputStage::IndexScan), 1);
    Expect("an indexed draw's scanned bytes", Amount(before, after, DrawInputStage::IndexScan), sizeof(Triangles));
    Expect("an indexed draw's copied ranges", Outcomes(before, after, DrawInputOutcome::Copied), 2);
    Expect("an indexed draw's keeps", Calls(before, after, DrawInputStage::Keep), 2);
    Expect("an indexed draw's expansions", Calls(before, after, DrawInputStage::Expand), 0);
}

void QuadDraws(AgcDriver::VulkanDevice& device, GuestBuffer& pixels) {
    GuestBuffer vertices(false);
    GuestBuffer indices(false, 4096);
    FillVertices(vertices);
    constexpr std::array<std::uint32_t, 8> Corners{0, 1, 2, 3, 4, 5, 6, 7};
    std::memcpy(indices.Data(), Corners.data(), sizeof(Corners));
    device.WaitIdle();
    auto before = AgcDriver::Graphics::DrawInputCountersNow();
    Draw(device, pixels, vertices, {0, static_cast<std::uint32_t>(Quads.size()), 0, 1, 0, false}, true);
    auto after = AgcDriver::Graphics::DrawInputCountersNow();
    device.WaitIdle();
    Expect("an auto quad-list draw's expansions", Calls(before, after, DrawInputStage::Expand), 1);
    Expect("an auto quad-list draw's expanded indices", Amount(before, after, DrawInputStage::Expand), Quads.size());
    Expect("an auto quad-list draw's index scans", Calls(before, after, DrawInputStage::IndexScan), 0);
    before = AgcDriver::Graphics::DrawInputCountersNow();
    Draw(device, pixels, vertices, {AddressOf(indices.Data()), static_cast<std::uint32_t>(Corners.size()) + 3u, 4, 1, 0, true}, true);
    after = AgcDriver::Graphics::DrawInputCountersNow();
    device.WaitIdle();
    Expect("an indexed quad-list draw's expansions", Calls(before, after, DrawInputStage::Expand), 1);
    Expect("an indexed quad-list draw's expanded indices", Amount(before, after, DrawInputStage::Expand), Corners.size());
    Expect("an indexed quad-list draw's copy lookups", Calls(before, after, DrawInputStage::Lookup), 1);
}

bool SnapshotDraws(AgcDriver::VulkanDevice& device, GuestBuffer& pixels) {
    GuestBuffer vertices(true);
    FillVertices(vertices);
    device.WaitIdle();
    Draw(device, pixels, vertices, {0, 3, 0, 1, 0, false}, false);
    auto& recorder = ActiveRecorder();
    if (recorder.ReusableDrawSnapshot(AddressOf(vertices.Data()), BufferBytes) == nullptr) {
        device.WaitIdle();
        std::puts("the draw took no reusable snapshot (no host import, or the buffer is not write-watched): snapshot reuse is not tested");
        return false;
    }
    const auto before = AgcDriver::Graphics::DrawInputCountersNow();
    Draw(device, pixels, vertices, {0, 3, 0, 1, 0, false}, false);
    const auto after = AgcDriver::Graphics::DrawInputCountersNow();
    device.WaitIdle();
    Expect("a second draw's reused snapshots", Outcomes(before, after, DrawInputOutcome::SnapshotReused), 1);
    Expect("a second draw's copied ranges", Outcomes(before, after, DrawInputOutcome::Copied), 0);
    Expect("a second draw's allocations", Calls(before, after, DrawInputStage::Allocate), 0);
    Expect("a second draw's keeps", Calls(before, after, DrawInputStage::Keep), 0);
    return true;
}

}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_PROFILE_DRAW", "1");
    _putenv_s("APS5_NO_DRAW_INPUTS_IN_PLACE", "1");
#else
    setenv("APS5_PROFILE_DRAW", "1", 1);
    setenv("APS5_NO_DRAW_INPUTS_IN_PLACE", "1", 1);
#endif
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        GuestBuffer pixels(true, TargetLayout.Bytes());
        {
            GuestBuffer vertices(false);
            FillVertices(vertices);
            Draw(*device, pixels, vertices, {0, 3, 0, 1, 0, false}, false);
            device->WaitIdle();
        }
        CopiedDraw(*device, pixels);
        IndexedDraw(*device, pixels);
        QuadDraws(*device, pixels);
        const bool snapshots = SnapshotDraws(*device, pixels);
        std::printf("draw input counters tests passed%s\n", snapshots ? "" : " (snapshot reuse not tested)");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
