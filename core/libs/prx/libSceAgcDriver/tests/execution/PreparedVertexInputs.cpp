#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PreparedDrawInputs.hpp"
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
#include <thread>
#include <chrono>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 32;
constexpr std::uint32_t IdentitySelect = 0xfacu;
constexpr std::size_t Block = 65536;
constexpr std::byte Background{0x40};
constexpr std::byte Covered{0xff};
alignas(256) std::array<std::byte, Width * Height * 4> Pixels{};
alignas(256) std::array<std::uint32_t, 24> Output{};

alignas(256) constexpr std::array<std::uint32_t, 7> VertexCode{
    0xbe822100, 0x34020a84, 0xe0781000, 0x80010801, 0xf80008cf, 0x0b0a0908, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 4> PixelCode{
    0x7e0e02f2, 0xf800180f, 0x07070707, 0xbf810000,
};

using Position = std::array<float, 4>;
constexpr std::array<Position, 3> Triangle{{{-0.75f, -0.75f, 0.5f, 1.0f}, {0.75f, -0.75f, 0.5f, 1.0f}, {0.0f, 0.75f, 0.5f, 1.0f}}};
constexpr std::array<std::uint16_t, 3> Indices{0, 1, 2};
constexpr std::array<std::uint16_t, 3> Degenerate{0, 0, 0};

std::byte* IndexBlock = nullptr;
std::byte* VertexBlock = nullptr;
std::byte* SpareBlock = nullptr;

std::byte* AllocateWatched(std::size_t bytes) {
#ifdef _WIN32
    void* block = GuestArena::GuestArenaAllocate_nid_postfix(bytes, Block);
    GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
#else
    void* raw = mmap(nullptr, bytes + Block, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) throw std::runtime_error("prepared vertex inputs: cannot map a watched block");
    const auto begin = reinterpret_cast<std::uintptr_t>(raw);
    const auto aligned = (begin + Block - 1) & ~static_cast<std::uintptr_t>(Block - 1);
    if (aligned != begin) munmap(raw, aligned - begin);
    if (aligned + bytes != begin + bytes + Block) munmap(reinterpret_cast<void*>(aligned + bytes), begin + Block - aligned);
    void* block = reinterpret_cast<void*>(aligned);
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
#endif
    Require(block != nullptr, "prepared vertex inputs: cannot allocate a watched block");
    std::memset(block, 0, bytes);
    return static_cast<std::byte*>(block);
}

std::array<std::uint32_t, 4> VertexBufferDescriptor() {
    const auto address = reinterpret_cast<std::uintptr_t>(VertexBlock);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (16u << 16u), static_cast<std::uint32_t>(Triangle.size()), 0x0104d000u | IdentitySelect};
}

std::array<std::uint32_t, 4> OutputDescriptor() {
    const auto address = reinterpret_cast<std::uintptr_t>(Output.data());
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), static_cast<std::uint32_t>(sizeof(Output)), 0x31016facu};
}

struct Programs {
    ShaderRecompiler::RecompileResult vertex;
    ShaderRecompiler::RecompileResult pixel;
    std::array<AgcDriver::Graphics::CompiledShader, 2> shaders;
};

std::unique_ptr<Programs> Compile(AgcDriver::VulkanDevice& device) {
    std::vector<std::uint32_t> userData(12, 0u);
    userData[0] = 0x10000000u;
    userData[1] = 0x00000001u;
    const auto output = OutputDescriptor();
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    ShaderRecompiler::ShaderVertexStageInfo info{};
    info.resourcesNum = 1;
    info.resources[0].fields = VertexBufferDescriptor();
    info.resourcesDst[0] = {8, 4, 0, 0};
    info.fetchAttribReg = 8;
    info.fetchBufferReg = 10;
    info.fetchEmbedded = true;
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {64u, 0, userData, std::nullopt, std::nullopt, info, vertexMemory},
        device.Target(),
        {0, 0, 0, 64}
    };
    vertex.useCache = false;
    auto programs = std::make_unique<Programs>();
    programs->vertex = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(programs->vertex.pushConstants.size());
    Require(programs->vertex.vertexAttributes.size() == 1, "prepared vertex inputs: the vertex program fetches no vertex attribute");
    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionX) | ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionY);
    pixel.posX = true;
    pixel.posY = true;
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    const std::vector<std::uint32_t> pixelUserData(8, 0u);
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(PixelCode.data()), std::as_bytes(std::span(PixelCode))}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}},
        {64u, 0, pixelUserData, std::nullopt, pixel, std::nullopt, pixelMemory},
        device.Target(),
        PixelPushLayout(vertexPush, device.Target())
    };
    fragment.useCache = false;
    programs->pixel = ShaderRecompiler::Recompile(fragment);
    programs->shaders = {{
        {ShaderStage::Vertex, &programs->vertex, 0},
        {ShaderStage::Fragment, &programs->pixel, PixelPushOffset(vertexPush, device.Target())}
    }};
    return programs;
}

AgcDriver::Graphics::State DrawState() {
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 64u, 64u, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size(), 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.primitiveRestart = false;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    state.blendConstants = {};
    return state;
}

void WriteIndices(const std::array<std::uint16_t, 3>& indices) {
    std::memcpy(IndexBlock, indices.data(), sizeof(indices));
}

void WriteVertices(float shift) {
    auto vertices = Triangle;
    for (auto& vertex : vertices) vertex[0] += shift;
    std::memcpy(VertexBlock, vertices.data(), sizeof(vertices));
}

bool CenterCovered() {
    const auto* pixel = Pixels.data() + (static_cast<std::size_t>(Height / 2u) * Width + Width / 2u) * 4u;
    const bool covered = std::all_of(pixel, pixel + 4, [](std::byte value) { return value == Covered; });
    Require(covered || std::all_of(pixel, pixel + 4, [](std::byte value) { return value == Background; }), "prepared vertex inputs: the center pixel is neither the background nor the triangle colour");
    return covered;
}

struct Outcome {
    bool covered = false;
    std::uint64_t adopted = 0;
    std::uint64_t reprepared = 0;
    std::uint64_t prepared = 0;
    std::uint64_t copied = 0;
};

Outcome DrawPrepared(AgcDriver::VulkanDevice& device, const Programs& programs, const std::function<void()>& betweenPrepareAndCommit) {
    WriteIndices(Indices);
    WriteVertices(0.0f);
    Pixels.fill(Background);
    const auto state = DrawState();
    const AgcDriver::Pm4::DrawParameters draw{reinterpret_cast<std::uintptr_t>(IndexBlock), static_cast<std::uint32_t>(Indices.size()), 2, 1, 0, true};
    const auto before = AgcDriver::Graphics::PreparedDrawInputCounters();
    auto prepared = device.PrepareDrawInputsAhead(state, draw, programs.shaders);
    Require(prepared != nullptr, "prepared vertex inputs: the draw was not prepared ahead of its commit");
    AgcDriver::Graphics::AwaitPreparedDrawInputs(*prepared);
    betweenPrepareAndCommit();
    {
        const AgcDriver::Graphics::PreparedDrawInputsScope scope(std::move(prepared));
        device.Draw(state, draw, programs.shaders);
    }
    device.WaitIdle();
    const auto after = AgcDriver::Graphics::PreparedDrawInputCounters();
    constexpr auto copied = static_cast<std::size_t>(AgcDriver::Graphics::DrawInputSource::Copied);
    Outcome outcome{CenterCovered(), after.adopted - before.adopted, after.reprepared - before.reprepared, after.prepared - before.prepared, after.sources[copied] - before.sources[copied]};
    return outcome;
}

void Print(const char* what, const Outcome& outcome) {
    std::printf("%s: center %s, %llu ranges prepared, %llu adopted, %llu prepared again, %llu copied on the committer\n", what, outcome.covered ? "covered" : "clear", static_cast<unsigned long long>(outcome.prepared), static_cast<unsigned long long>(outcome.adopted), static_cast<unsigned long long>(outcome.reprepared), static_cast<unsigned long long>(outcome.copied));
}

}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_NO_DRAW_INPUTS_IN_PLACE", "1");
#else
    setenv("APS5_NO_DRAW_INPUTS_IN_PLACE", "1", 1);
#endif
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        IndexBlock = AllocateWatched(Block);
        VertexBlock = AllocateWatched(Block);
        SpareBlock = AllocateWatched(Block);
        GuestAllocations::Mutation().Add(IndexBlock, Block, true, true, true);
        GuestAllocations::Mutation().Add(VertexBlock, Block, true, true, true);
        AgcDriver::Graphics::SetDrawInputPreparers(1);
        const auto programs = Compile(*device);

        const auto unchanged = DrawPrepared(*device, *programs, [] {});
        Print("unchanged inputs", unchanged);
        Require(unchanged.prepared == 2 && unchanged.adopted == 2 && unchanged.reprepared == 0 && unchanged.copied == 0, "prepared vertex inputs: an unchanged draw did not adopt its prepared index and vertex copies");
        Require(unchanged.covered, "prepared vertex inputs: the draw with adopted inputs did not cover the center");

        const auto moved = DrawPrepared(*device, *programs, [] { WriteVertices(4.0f); });
        Print("vertices moved off the target after preparation", moved);
        Require(moved.adopted == 1 && moved.reprepared == 1 && moved.copied == 1, "prepared vertex inputs: a vertex range the CPU wrote after preparation was adopted, or the untouched index range was not");
        Require(!moved.covered, "prepared vertex inputs: the draw used the vertices prepared before the CPU moved them off the target");

        const auto degenerate = DrawPrepared(*device, *programs, [] { WriteIndices(Degenerate); });
        Print("indices made degenerate after preparation", degenerate);
        Require(degenerate.reprepared >= 1 && degenerate.copied >= 1, "prepared vertex inputs: an index range the CPU wrote after preparation was adopted");
        Require(!degenerate.covered, "prepared vertex inputs: the draw used the indices prepared before the CPU made them degenerate");

        const auto remapped = DrawPrepared(*device, *programs, [] { GuestAllocations::Mutation().Add(SpareBlock, Block, true, true, true); });
        Print("allocation registry changed after preparation", remapped);
        Require(remapped.adopted == 0 && remapped.reprepared == 2 && remapped.copied == 2, "prepared vertex inputs: a copy prepared under another registry generation was adopted");
        Require(remapped.covered, "prepared vertex inputs: the draw copied again after the registry changed did not cover the center");

        AgcDriver::Graphics::HoldDrawInputPreparers(true);
        WriteIndices(Indices);
        WriteVertices(0.0f);
        Pixels.fill(Background);
        const auto heldState = DrawState();
        const AgcDriver::Pm4::DrawParameters heldDraw{reinterpret_cast<std::uintptr_t>(IndexBlock), static_cast<std::uint32_t>(Indices.size()), 2, 1, 0, true};
        const auto beforeHeld = AgcDriver::Graphics::PreparedDrawInputCounters();
        auto kept = device->PrepareDrawInputsAhead(heldState, heldDraw, programs->shaders);
        Require(kept != nullptr, "prepared vertex inputs: the held draw was not queued for preparation");
        {
            const AgcDriver::Graphics::PreparedDrawInputsScope scope(kept);
            device->Draw(heldState, heldDraw, programs->shaders);
        }
        device->WaitIdle();
        AgcDriver::Graphics::HoldDrawInputPreparers(false);
        const auto settleBy = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (std::chrono::steady_clock::now() < settleBy) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto afterHeld = AgcDriver::Graphics::PreparedDrawInputCounters();
        kept.reset();
        constexpr auto copiedSource = static_cast<std::size_t>(AgcDriver::Graphics::DrawInputSource::Copied);
        std::printf("preparers held at commit: center %s, %llu not ready, %llu adopted, %llu ranges prepared after the release, %llu copied on the committer\n", CenterCovered() ? "covered" : "clear", static_cast<unsigned long long>(afterHeld.notReady - beforeHeld.notReady), static_cast<unsigned long long>(afterHeld.adopted - beforeHeld.adopted), static_cast<unsigned long long>(afterHeld.ranges - beforeHeld.ranges), static_cast<unsigned long long>(afterHeld.sources[copiedSource] - beforeHeld.sources[copiedSource]));
        Require(afterHeld.notReady - beforeHeld.notReady == 1 && afterHeld.adopted == beforeHeld.adopted, "prepared vertex inputs: a draw whose preparation had not started was not cancelled at its commit");
        Require(afterHeld.sources[copiedSource] - beforeHeld.sources[copiedSource] == 2, "prepared vertex inputs: the cancelled draw did not copy its index and vertex ranges on the committer");
        Require(afterHeld.ranges == beforeHeld.ranges, "prepared vertex inputs: a preparer copied guest memory for a draw after its commit cancelled the job");
        Require(CenterCovered(), "prepared vertex inputs: the draw copied on the committer after the cancel did not cover the center");

        const auto again = DrawPrepared(*device, *programs, [] {});
        Print("unchanged inputs again", again);
        Require(again.adopted == 2 && again.covered, "prepared vertex inputs: an unchanged draw after the others did not adopt both copies");
        std::puts("prepared vertex inputs: index and vertex copies made before the commit are adopted only while their ranges and the registry are unchanged");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
