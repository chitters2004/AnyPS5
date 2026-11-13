#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PreparedDrawInputs.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
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

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

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
constexpr std::array<std::uint16_t, 6> PaddedIndices{0, 1, 2, 0, 1, 3};

std::byte* IndexBlock = nullptr;
std::byte* VertexBlock = nullptr;
std::byte* SpareBlock = nullptr;
constexpr std::uint32_t NoLocation = 0x280u;
alignas(256) std::array<std::uint32_t, 5> IndirectRecord{3, 1, 0, 0, 0};

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

std::array<std::uint32_t, 4> VertexBufferDescriptor(std::uint32_t records) {
    const auto address = reinterpret_cast<std::uintptr_t>(VertexBlock);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (16u << 16u), records, 0x0104d000u | IdentitySelect};
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

std::unique_ptr<Programs> Compile(AgcDriver::VulkanDevice& device, std::uint32_t records = static_cast<std::uint32_t>(Triangle.size())) {
    std::vector<std::uint32_t> userData(12, 0u);
    userData[0] = 0x10000000u;
    userData[1] = 0x00000001u;
    const auto output = OutputDescriptor();
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    ShaderRecompiler::ShaderVertexStageInfo info{};
    info.resourcesNum = 1;
    info.resources[0].fields = VertexBufferDescriptor(records);
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

void WriteIndices(std::span<const std::uint16_t> indices) {
    std::memcpy(IndexBlock, indices.data(), indices.size_bytes());
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
    std::uint64_t spares = 0;
};

Outcome DrawPrepared(AgcDriver::VulkanDevice& device, const Programs& programs, const std::function<void()>& betweenPrepareAndCommit, bool indirect = false, std::span<const std::uint16_t> indices = Indices) {
    WriteIndices(indices);
    WriteVertices(0.0f);
    Pixels.fill(Background);
    const auto state = DrawState();
    AgcDriver::Pm4::DrawParameters draw{reinterpret_cast<std::uintptr_t>(IndexBlock), static_cast<std::uint32_t>(indices.size()), 2, 1, 0, true};
    if (indirect) {
        draw.instanceCount = 0;
        draw.indirect = AgcDriver::Pm4::DrawParameters::IndirectDraw{reinterpret_cast<std::uintptr_t>(IndirectRecord.data()), 0x25u, 20u, 20u, 1u, false, 0u, NoLocation, NoLocation, NoLocation, false, 0u};
    }
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
    Outcome outcome{CenterCovered(), after.adopted - before.adopted, after.reprepared - before.reprepared, after.prepared - before.prepared, after.sources[copied] - before.sources[copied], after.spareReused - before.spareReused};
    return outcome;
}

std::array<std::byte, Width * Height * 4> DrawInline(AgcDriver::VulkanDevice& device, const Programs& programs, float shift) {
    WriteIndices(PaddedIndices);
    WriteVertices(shift);
    Pixels.fill(Background);
    const auto state = DrawState();
    const AgcDriver::Pm4::DrawParameters draw{reinterpret_cast<std::uintptr_t>(IndexBlock), static_cast<std::uint32_t>(PaddedIndices.size()), 2, 1, 0, true};
    device.Draw(state, draw, programs.shaders);
    device.WaitIdle();
    return Pixels;
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
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        IndexBlock = AllocateWatched(Block);
        VertexBlock = AllocateWatched(Block);
        SpareBlock = AllocateWatched(Block);
        GuestAllocations::Mutation().Add(IndexBlock, Block, true, true, true);
        GuestAllocations::Mutation().Add(VertexBlock, Block, true, true, true);
        AgcDriver::Graphics::SetDrawInputPreparers(1);
        const auto programs = Compile(*device);

        const auto ringBefore = AgcDriver::Graphics::DrawInputRingCounters();
        const auto unchanged = DrawPrepared(*device, *programs, [] {});
        const auto ringAfter = AgcDriver::Graphics::DrawInputRingCounters();
        Print("unchanged inputs", unchanged);
        std::printf("unchanged inputs: %llu ring slices, %llu ring chunks made\n", static_cast<unsigned long long>(ringAfter.slices - ringBefore.slices), static_cast<unsigned long long>(ringAfter.chunks - ringBefore.chunks));
        Require(ringAfter.slices - ringBefore.slices == 2, "prepared vertex inputs: the preparer did not copy the index and vertex ranges into ring slices");
        Require(unchanged.prepared == 2 && unchanged.adopted == 2 && unchanged.reprepared == 0 && unchanged.copied == 0, "prepared vertex inputs: an unchanged draw did not adopt its prepared index and vertex copies");
        Require(unchanged.covered, "prepared vertex inputs: the draw with adopted inputs did not cover the center");

        const auto moved = DrawPrepared(*device, *programs, [] { WriteVertices(4.0f); });
        Print("vertices moved off the target after preparation", moved);
        Require(moved.adopted == 1 && moved.reprepared == 1 && moved.copied == 1, "prepared vertex inputs: a vertex range the CPU wrote after preparation was adopted, or the untouched index range was not");
        Require(!moved.covered, "prepared vertex inputs: the draw used the vertices prepared before the CPU moved them off the target");
        Require(moved.spares == 1, "prepared vertex inputs: the committer did not copy the refused vertex range into the buffer its preparation left");
        AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::NoDrawInputRecycling, true);
        const auto movedFresh = DrawPrepared(*device, *programs, [] { WriteVertices(4.0f); });
        AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::NoDrawInputRecycling, false);
        Require(movedFresh.spares == 0 && movedFresh.copied == 1 && !movedFresh.covered, "prepared vertex inputs: with recycling off the refused range was not copied into a buffer of its own");

        const auto degenerate = DrawPrepared(*device, *programs, [] { WriteIndices(Degenerate); });
        Print("indices made degenerate after preparation", degenerate);
        Require(degenerate.reprepared >= 1 && degenerate.copied >= 1, "prepared vertex inputs: an index range the CPU wrote after preparation was adopted");
        Require(!degenerate.covered, "prepared vertex inputs: the draw used the indices prepared before the CPU made them degenerate");

        using AgcDriver::Graphics::RuntimeSwitch;
        namespace Switches = AgcDriver::Graphics::RuntimeSwitches;
        const bool globalChecks = Switches::Set(RuntimeSwitch::GlobalProofChecks);
        const auto elsewhere = [] {
            GuestAllocations::Mutation().Add(SpareBlock, Block, true, true, true);
            GuestAllocations::Mutation().Remove(SpareBlock);
        };
        const auto covering = [] { GuestAllocations::GuestAllocationsInvalidate_nid_postfix(reinterpret_cast<std::uintptr_t>(VertexBlock), Block); };
        const auto coveringReprepared = [](const Outcome& outcome, bool global) {
            return global ? outcome.adopted == 0 && outcome.reprepared == 2 && outcome.copied == 2 : outcome.adopted == 1 && outcome.reprepared == 1 && outcome.copied == 1;
        };
        for (const bool global : {false, true}) {
            Switches::Assign(RuntimeSwitch::GlobalProofChecks, global);
            const char* mode = global ? " (APS5_GLOBAL_PROOF_CHECKS)" : "";
            const auto remapped = DrawPrepared(*device, *programs, elsewhere);
            Print(global ? "allocation registry changed elsewhere after preparation, global proof checks" : "allocation registry changed elsewhere after preparation", remapped);
            if (global) Require(remapped.adopted == 0 && remapped.reprepared == 2 && remapped.copied == 2, "prepared vertex inputs: a copy prepared under another registry generation was adopted under APS5_GLOBAL_PROOF_CHECKS");
            else Require(remapped.adopted == 2 && remapped.reprepared == 0 && remapped.copied == 0, "prepared vertex inputs: a registry change outside the prepared ranges made them prepare again");
            Require(remapped.covered, std::string("prepared vertex inputs: the draw after a registry change elsewhere did not cover the center") + mode);
            const auto forgotten = DrawPrepared(*device, *programs, covering);
            Print(global ? "vertex range forgotten after preparation, global proof checks" : "vertex range forgotten after preparation", forgotten);
            Require(coveringReprepared(forgotten, global), std::string("prepared vertex inputs: a copy was adopted across a forget over its range") + mode);
            Require(forgotten.covered, std::string("prepared vertex inputs: the draw after a forget over its vertex range did not cover the center") + mode);
        }
        Switches::Assign(RuntimeSwitch::GlobalProofChecks, false);
        AgcDriver::Graphics::IgnoreRangeForgetsForTest(true);
        const auto mutated = DrawPrepared(*device, *programs, covering);
        AgcDriver::Graphics::IgnoreRangeForgetsForTest(false);
        Switches::Assign(RuntimeSwitch::GlobalProofChecks, globalChecks);
        Print("vertex range forgotten after preparation, range forget stamps ignored (mutant)", mutated);
        Require(!coveringReprepared(mutated, false), "prepared vertex inputs: the forget check does not kill a mutant that ignores range forget stamps");

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

        const auto indirectUnchanged = DrawPrepared(*device, *programs, [] {}, true);
        Print("indexed indirect draw, unchanged inputs", indirectUnchanged);
        Require(indirectUnchanged.prepared == 2 && indirectUnchanged.adopted == 2 && indirectUnchanged.copied == 0, "prepared vertex inputs: an unchanged indexed indirect draw did not adopt its prepared index copy and vertex extent");
        Require(indirectUnchanged.covered, "prepared vertex inputs: the indexed indirect draw with adopted inputs did not cover the center");

        const auto indirectMoved = DrawPrepared(*device, *programs, [] { WriteVertices(4.0f); }, true);
        Print("indexed indirect draw, vertices moved off the target after preparation", indirectMoved);
        Require(indirectMoved.adopted == 1 && indirectMoved.reprepared == 1 && indirectMoved.copied == 1, "prepared vertex inputs: an indirect draw's vertex extent the CPU wrote after preparation was adopted, or its untouched index range was not");
        Require(!indirectMoved.covered, "prepared vertex inputs: the indexed indirect draw used the vertices prepared before the CPU moved them off the target");

        const auto padded = Compile(*device, 3u);
        const auto paddedUnchanged = DrawPrepared(*device, *padded, [] {}, false, PaddedIndices);
        const auto paddedPixels = Pixels;
        const auto paddedCovered = static_cast<std::size_t>(std::count(paddedPixels.begin(), paddedPixels.end(), Covered) / 4);
        std::printf("zero-padded draw covers %zu of %u pixels\n", paddedCovered, Width * Height);
        Require(paddedCovered != 0, "prepared vertex inputs: the zero-padded draw covers nothing, so comparing it proves nothing");
        Print("zero-padded vertex fetch, unchanged inputs", paddedUnchanged);
        Require(paddedUnchanged.prepared == 2 && paddedUnchanged.adopted == 2 && paddedUnchanged.copied == 0, "prepared vertex inputs: an unchanged draw did not adopt its prepared index copy and zero-padded vertex fetch");
        const auto paddedMoved = DrawPrepared(*device, *padded, [] { WriteVertices(0.5f); }, false, PaddedIndices);
        const auto paddedMovedPixels = Pixels;
        Require(paddedMovedPixels != paddedPixels, "prepared vertex inputs: moving the vertices did not change the zero-padded draw, so comparing it proves nothing");
        Print("zero-padded vertex fetch, vertices moved after preparation", paddedMoved);
        Require(paddedMoved.adopted == 1 && paddedMoved.reprepared == 1 && paddedMoved.copied == 1, "prepared vertex inputs: a zero-padded vertex fetch the CPU wrote after preparation was adopted, or the untouched index range was not");
        AgcDriver::Graphics::SetDrawInputPreparers(0);
        Require(DrawInline(*device, *padded, 0.0f) == paddedPixels, "prepared vertex inputs: the draw with an adopted zero-padded vertex fetch differs from the same draw copied on the committer");
        Require(DrawInline(*device, *padded, 0.5f) == paddedMovedPixels, "prepared vertex inputs: the draw whose zero-padded vertex fetch was prepared again differs from the same draw copied on the committer");
        AgcDriver::Graphics::SetDrawInputPreparers(1);

        AgcDriver::Graphics::SetDrawInputPreparers(0);
        AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::NoSharedVertexLayout, true);
        const auto copiedLayout = DrawInline(*device, *padded, 0.0f);
        AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::NoSharedVertexLayout, false);
        const auto sharedLayout = DrawInline(*device, *padded, 0.0f);
        AgcDriver::Graphics::SetDrawInputPreparers(1);
        std::printf("vertex input layout copied into the inputs and shared with them: %s\n", copiedLayout == sharedLayout ? "same pixels" : "different pixels");
        Require(copiedLayout == paddedPixels && sharedLayout == paddedPixels, "prepared vertex inputs: sharing the plan's vertex input layout with the draw's inputs changed what it drew");

        const auto again = DrawPrepared(*device, *programs, [] {});
        Print("unchanged inputs again", again);
        Require(again.adopted == 2 && again.covered, "prepared vertex inputs: an unchanged draw after the others did not adopt both copies");

        {
            using AgcDriver::Graphics::RuntimeSwitch;
            namespace Switches = AgcDriver::Graphics::RuntimeSwitches;
            const auto contexts = [&] {
                WriteIndices(Indices);
                const auto state = DrawState();
                const AgcDriver::Pm4::DrawParameters draw{reinterpret_cast<std::uintptr_t>(IndexBlock), static_cast<std::uint32_t>(Indices.size()), 2, 1, 0, true};
                auto first = device->PrepareDrawInputsAhead(state, draw, programs->shaders);
                auto second = device->PrepareDrawResourcesAhead(nullptr, state, draw, programs->shaders, {});
                Require(first != nullptr && second != nullptr, "prepared vertex inputs: a draw was not prepared ahead of its commit for the context check");
                AgcDriver::Graphics::AwaitPreparedDrawInputs(*first);
                return std::array{&AgcDriver::Graphics::PreparedDrawInputsContext(*first), &AgcDriver::Graphics::PreparedDrawInputsContext(*second), AgcDriver::Graphics::PreparedDrawResourcesContext(*second)};
            };
            const bool initial = Switches::Set(RuntimeSwitch::NoSharedPreparerContext);
            const bool cached = std::getenv("APS5_NO_CONTEXT_CACHE") == nullptr;
            Switches::Assign(RuntimeSwitch::NoSharedPreparerContext, false);
            const auto shared = contexts();
            Switches::Assign(RuntimeSwitch::NoSharedPreparerContext, true);
            const auto copied = contexts();
            const auto copiedDraw = DrawPrepared(*device, *programs, [] {});
            Switches::Assign(RuntimeSwitch::NoSharedPreparerContext, initial);
            std::printf("preparer context: two requests share %s; with APS5_NO_SHARED_PREPARER_CONTEXT they share %s and the draw adopts %llu copies\n", shared[0] == shared[1] && shared[1] == shared[2] ? "one context" : "nothing", copied[0] == copied[1] ? "one context" : "nothing", static_cast<unsigned long long>(copiedDraw.adopted));
            Require(shared[2] != nullptr && copied[2] != nullptr, "prepared vertex inputs: the draw's resources were not prepared for the context check");
            Require(!cached || (shared[0] == shared[1] && shared[1] == shared[2]), "prepared vertex inputs: two preparations on one device copied the device's context instead of sharing it");
            Require(copied[0] != copied[1] && (!cached || (copied[0] != shared[0] && copied[2] != shared[0])), "prepared vertex inputs: APS5_NO_SHARED_PREPARER_CONTEXT did not give each preparation its own context copy");
            Require(copiedDraw.adopted == 2 && copiedDraw.covered, "prepared vertex inputs: with its own context copy an unchanged draw did not adopt both copies");
        }
        const auto epochState = DrawState();
        const AgcDriver::Pm4::DrawParameters epochDraw{reinterpret_cast<std::uintptr_t>(IndexBlock), static_cast<std::uint32_t>(Indices.size()), 2, 1, 0, true};
        const auto prepareUnused = [&](std::uint64_t firstEpoch) {
            const auto before = AgcDriver::Graphics::PreparedDrawInputCounters();
            for (const auto epoch : {firstEpoch, firstEpoch, firstEpoch + 1u}) {
                auto job = device->PrepareDrawInputsAhead(epochState, epochDraw, programs->shaders, epoch);
                Require(job != nullptr, "prepared vertex inputs: a draw with a collect epoch was not prepared");
                AgcDriver::Graphics::AwaitPreparedDrawInputs(*job);
                const AgcDriver::Graphics::PreparedDrawInputsScope scope(std::move(job));
            }
            const auto after = AgcDriver::Graphics::PreparedDrawInputCounters();
            return std::array<std::uint64_t, 3>{after.epochFollows - before.epochFollows, after.stashHits[0] - before.stashHits[0], after.stashReturned - before.stashReturned};
        };
        const auto ringSlices = AgcDriver::Graphics::DrawInputRingCounters().slices;
        const auto sliced = prepareUnused((std::uint64_t{1} << 62u) - 8u);
        std::printf("three unused preparations in ring slices: %llu ring slices, %llu staging buffers taken from the stash, %llu returned to it\n", static_cast<unsigned long long>(AgcDriver::Graphics::DrawInputRingCounters().slices - ringSlices), static_cast<unsigned long long>(sliced[1]), static_cast<unsigned long long>(sliced[2]));
        Require(AgcDriver::Graphics::DrawInputRingCounters().slices - ringSlices == 6 && sliced[1] == 0 && sliced[2] == 0, "prepared vertex inputs: preparations of ring-sized ranges did not take ring slices, or put slices in the stash of whole buffers");
        AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::NoDrawInputRing, true);
        const auto recycled = prepareUnused(std::uint64_t{1} << 62u);
        std::printf("three unused preparations in epochs E, E and E+1: %llu epoch follows, %llu staging buffers taken from the stash, %llu returned to it\n", static_cast<unsigned long long>(recycled[0]), static_cast<unsigned long long>(recycled[1]), static_cast<unsigned long long>(recycled[2]));
        Require(recycled[0] == 2, "prepared vertex inputs: the preparer did not follow each new collect epoch of its draws exactly once");
        Require(recycled[1] == 4 && recycled[2] == 6, "prepared vertex inputs: the unused copies of a preparation did not stage the next preparations of the same ranges");
        AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::NoDrawInputRecycling, true);
        const auto fresh = prepareUnused((std::uint64_t{1} << 62u) + 8u);
        AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::NoDrawInputRecycling, false);
        Require(fresh[1] == 0 && fresh[2] == 0, "prepared vertex inputs: staging buffers were recycled with APS5_NO_DRAW_INPUT_RECYCLING set");
        const auto afterRecycling = DrawPrepared(*device, *programs, [] {});
        Require(afterRecycling.adopted == 2 && afterRecycling.covered, "prepared vertex inputs: a draw whose copies were staged in recycled buffers did not adopt both or drew wrong");
        {
            const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(*device);
            using Use = AgcDriver::Graphics::Recorder::SnapshotUse;
            const auto mapped = AgcDriver::Graphics::DrawInputMemoryProperties(context, Use::Vertex);
            const auto host = AgcDriver::Graphics::DrawInputMemoryProperties(context, Use::Index16);
            Require(AgcDriver::Graphics::DrawInputMemoryProperties(context, Use::PaddedVertex) == mapped, "prepared vertex inputs: a zero-padded vertex fetch, which the CPU only writes, is not in the snapshot memory class");
            Require(mapped == context.SnapshotProperties() && mapped != host, "prepared vertex inputs: the device offers no memory class for vertex copies apart from host memory, so keeping the class cannot be tested");
            static_cast<void>(prepareUnused((std::uint64_t{1} << 62u) + 20u));
            constexpr std::size_t vertexBytes = Triangle.size() * 16u;
            const auto beforeMismatch = AgcDriver::Graphics::PreparedDrawInputCounters();
            const auto mismatched = AgcDriver::Graphics::NewDrawInputBuffer(context, vertexBytes, Use::Index16);
            const auto afterMismatch = AgcDriver::Graphics::PreparedDrawInputCounters();
            const auto matched = AgcDriver::Graphics::NewDrawInputBuffer(context, vertexBytes, Use::Vertex);
            const auto afterMatch = AgcDriver::Graphics::PreparedDrawInputCounters();
            std::printf("recycled %zu-byte vertex buffers: an index request, host class, took %llu from the stash (properties 0x%x), a vertex request took %llu (properties 0x%x, the snapshot class is 0x%x)\n", vertexBytes, static_cast<unsigned long long>(afterMismatch.stashHits[1] - beforeMismatch.stashHits[1]), mismatched->Properties(), static_cast<unsigned long long>(afterMatch.stashHits[1] - afterMismatch.stashHits[1]), matched->Properties(), mapped);
            Require(afterMismatch.stashHits[1] == beforeMismatch.stashHits[1] && mismatched->Properties() == host, "prepared vertex inputs: a host-class index request reused a recycled buffer of the snapshot memory class");
            Require(afterMatch.stashHits[1] == afterMismatch.stashHits[1] + 1 && matched->Properties() == mapped, "prepared vertex inputs: the recycled vertex copy did not keep its snapshot memory class, or a vertex request did not reuse it");
            const auto hitKey = AgcDriver::Graphics::DrawResourceCacheKey(context, epochState, epochDraw, programs->shaders);
            Require(!hitKey.empty(), "prepared vertex inputs: the draw has no resource cache key, so a cache hit cannot be predicted for it");
            bool predicted = true;
            bool stagingPredicted = false;
            auto cachedBuild = AgcDriver::Graphics::PrepareDrawResourcesOffDevice(context, epochState, epochDraw, programs->shaders, {}, predicted, stagingPredicted);
            Require(!predicted && cachedBuild != nullptr, "prepared vertex inputs: a cache hit was predicted for a key the resource cache does not hold");
            auto& cache = AgcDriver::Graphics::SharedResourceCache();
            cache.Insert(hitKey, cachedBuild);
            Require(cache.Peek(hitKey) != nullptr, "prepared vertex inputs: the resource cache refused the entry the predicted hit needs");
            const auto findsBefore = AgcDriver::Graphics::ResourceCache::Finds();
            const auto touchesBefore = AgcDriver::Graphics::ResourceCache::Touches();
            const auto uploadsBefore = AgcDriver::Graphics::PreparedUploadCounters();
            {
                auto job = device->PrepareDrawResourcesAhead(nullptr, epochState, epochDraw, programs->shaders, {}, (std::uint64_t{1} << 62u) + 12u);
                Require(job != nullptr, "prepared vertex inputs: the draw's resources were not queued for preparation");
                const auto pin = std::make_shared<int>(0);
                AgcDriver::Graphics::SubmitPreparedDrawResources(job, pin);
                AgcDriver::Graphics::AwaitPreparedDrawInputs(*job);
                const AgcDriver::Graphics::PreparedDrawInputsScope scope(std::move(job));
            }
            const auto uploadsAfter = AgcDriver::Graphics::PreparedUploadCounters();
            std::printf("draw whose resources the cache holds: %llu predicted hits, %llu stage A builds, %llu cache finds and %llu touches by the prediction\n", static_cast<unsigned long long>(uploadsAfter.stageAPredictedHit - uploadsBefore.stageAPredictedHit), static_cast<unsigned long long>(uploadsAfter.stageAPrepared - uploadsBefore.stageAPrepared), static_cast<unsigned long long>(AgcDriver::Graphics::ResourceCache::Finds() - findsBefore), static_cast<unsigned long long>(AgcDriver::Graphics::ResourceCache::Touches() - touchesBefore));
            Require(uploadsAfter.stageAPredictedHit - uploadsBefore.stageAPredictedHit == 1 && uploadsAfter.stageAPrepared == uploadsBefore.stageAPrepared, "prepared vertex inputs: a preparer built stage A for a draw whose resources the cache holds");
            Require(AgcDriver::Graphics::ResourceCache::Finds() == findsBefore && AgcDriver::Graphics::ResourceCache::Touches() == touchesBefore, "prepared vertex inputs: predicting the cache hit found or touched the entry, which reorders the cache and its counts");
            cache.Remove(hitKey, cachedBuild.get());
            cachedBuild.reset();
            using AgcDriver::Graphics::TargetResidency;
            Require(context.detiler != nullptr, "prepared vertex inputs: the device has no detiler, so color target residency cannot be predicted");
            AgcDriver::Graphics::ColorTarget linear{};
            auto* const targetBlock = AllocateWatched(Block);
            GuestAllocations::Mutation().Add(targetBlock, Block, true, true, true);
            linear.address = reinterpret_cast<std::uintptr_t>(targetBlock);
            linear.extent = {64, 32};
            linear.format = VK_FORMAT_R8G8B8A8_UNORM;
            linear.bytes = 64 * 32 * 4;
            linear.tileMode = AgcDriver::Graphics::ColorTileMode::Linear;
            linear.elementBytes = 4;
            auto tiled = linear;
            tiled.tileMode = AgcDriver::Graphics::ColorTileMode::Standard4KB;
            auto multisampled = linear;
            multisampled.samples = 4;
            auto gcnLinear = linear;
            gcnLinear.tileMode = AgcDriver::Graphics::ColorTileMode::GcnLinear;
            const auto residency = [&](const AgcDriver::Graphics::ColorTarget& color, bool residentTargets = true, bool residentLinear = true) { return AgcDriver::Graphics::ColorTargetResidency(context, color, residentTargets, residentLinear); };
            Require(residency(tiled) == TargetResidency::Resident && residency(multisampled) == TargetResidency::Multisampled && residency(gcnLinear) == TargetResidency::Copied, "prepared vertex inputs: a tiled, multisampled or GCN linear color target got the wrong residency");
            Require(residency(tiled, false) == TargetResidency::Copied && residency(linear, true, false) == TargetResidency::Copied, "prepared vertex inputs: color target residency ignored APS5_NO_RESIDENT_TARGETS or APS5_NO_RESIDENT_LINEAR");
            auto noImports = context;
            noImports.hostImportAlignment = 0;
            auto noDetiler = context;
            noDetiler.detiler = nullptr;
            Require(AgcDriver::Graphics::ColorTargetResidency(noImports, linear, true, true) == TargetResidency::Copied && AgcDriver::Graphics::ColorTargetResidency(noDetiler, tiled, true, true) == TargetResidency::Copied, "prepared vertex inputs: a linear target without host imports, or any target without a detiler, was not predicted copied");
            if (context.hostImportAlignment != 0) {
                Require(residency(linear) == TargetResidency::LinearImport, "prepared vertex inputs: a linear color target on a device with host imports does not depend on its import");
                auto state = epochState;
                state.colors = {tiled, linear};
                const auto predict = [&] {
                    const AgcDriver::GuestMemory::DeviceLockForbiddenScope forbidden;
                    return AgcDriver::Graphics::PredictDrawDeviceStaging(context, state);
                };
                Require(!predict().has_value(), "prepared vertex inputs: a draw into a linear target without a host import was predicted, though the committer may still import it");
                state.colors = {multisampled, gcnLinear};
                const auto copied = predict();
                Require(copied.has_value() && !*copied, "prepared vertex inputs: a draw into a copied target was not predicted synchronous");
                std::puts("color target residency: tiled resident, multisampled, GCN linear copied; a linear target without a host import is left to the committer, off the device lock");
            } else {
                std::puts("the device has no host imports: linear target import prediction not tested");
            }
        }

        const auto beforeTeardown = AgcDriver::Graphics::PreparedDrawInputCounters();
        static_cast<void>(prepareUnused((std::uint64_t{1} << 62u) + 16u));
        Require(AgcDriver::Graphics::PreparedDrawInputCounters().stashReturned > beforeTeardown.stashReturned, "prepared vertex inputs: no unused copy was kept for recycling, so destroying the device after it proves nothing");
        AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::NoDrawInputRing, false);
        Require(AgcDriver::Graphics::DrawInputRingCounters().outstanding != 0, "prepared vertex inputs: no ring chunk is in use, so destroying the device after the ring proves nothing");
        device.reset();
        AgcDriver::Graphics::SetDrawInputPreparers(0);
        std::puts("prepared vertex inputs: index and vertex copies made before the commit are adopted only while their ranges and the registry are unchanged");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
