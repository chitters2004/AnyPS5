#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PreparedDrawInputs.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
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
#include <atomic>
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
#include <tuple>
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
    Outcome outcome{CenterCovered(), after.adopted - before.adopted, after.reprepared - before.reprepared, after.prepared - before.prepared, after.sources[copied] - before.sources[copied]};
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
                auto second = device->PrepareDrawInputsAhead(state, draw, programs->shaders);
                Require(first != nullptr && second != nullptr, "prepared vertex inputs: a draw was not prepared ahead of its commit for the context check");
                AgcDriver::Graphics::AwaitPreparedDrawInputs(*first);
                AgcDriver::Graphics::AwaitPreparedDrawInputs(*second);
                return std::array{&AgcDriver::Graphics::PreparedDrawInputsContext(*first), &AgcDriver::Graphics::PreparedDrawInputsContext(*second)};
            };
            const bool initial = Switches::Set(RuntimeSwitch::NoSharedPreparerContext);
            const bool cached = std::getenv("APS5_NO_CONTEXT_CACHE") == nullptr;
            Switches::Assign(RuntimeSwitch::NoSharedPreparerContext, false);
            const auto shared = contexts();
            Switches::Assign(RuntimeSwitch::NoSharedPreparerContext, true);
            const auto copied = contexts();
            const auto copiedDraw = DrawPrepared(*device, *programs, [] {});
            Switches::Assign(RuntimeSwitch::NoSharedPreparerContext, initial);
            std::printf("preparer context: two requests share %s; with APS5_NO_SHARED_PREPARER_CONTEXT they share %s and the draw adopts %llu copies\n", shared[0] == shared[1] ? "one context" : "nothing", copied[0] == copied[1] ? "one context" : "nothing", static_cast<unsigned long long>(copiedDraw.adopted));
            Require(!cached || shared[0] == shared[1], "prepared vertex inputs: two preparations on one device copied the device's context instead of sharing it");
            Require(copied[0] != copied[1] && (!cached || copied[0] != shared[0]), "prepared vertex inputs: APS5_NO_SHARED_PREPARER_CONTEXT did not give each preparation its own context copy");
            Require(copiedDraw.adopted == 2 && copiedDraw.covered, "prepared vertex inputs: with its own context copy an unchanged draw did not adopt both copies");
        }
        {
            using AgcDriver::Graphics::RuntimeSwitch;
            namespace Switches = AgcDriver::Graphics::RuntimeSwitches;
            WriteIndices(Indices);
            const AgcDriver::Pm4::DrawParameters draw{reinterpret_cast<std::uintptr_t>(IndexBlock), static_cast<std::uint32_t>(Indices.size()), 2, 1, 0, true};
            const bool initial = Switches::Set(RuntimeSwitch::NoEarlyDrawInputs);
            Switches::Assign(RuntimeSwitch::NoEarlyDrawInputs, true);
            const auto refused = device->PrepareDrawInputsAhead(DrawState(), draw, programs->shaders);
            Switches::Assign(RuntimeSwitch::NoEarlyDrawInputs, initial);
            std::printf("APS5_NO_EARLY_DRAW_INPUTS: the draw is %s\n", refused == nullptr ? "left to the committer" : "prepared ahead");
            Require(refused == nullptr, "prepared vertex inputs: APS5_NO_EARLY_DRAW_INPUTS did not stop a draw's inputs being prepared ahead of its commit");
        }
        {
            using AgcDriver::Graphics::RuntimeSwitch;
            namespace Switches = AgcDriver::Graphics::RuntimeSwitches;
            static std::atomic<bool> entered{false};
            static std::atomic<bool> released{false};
            const auto finishWhileRunning = [&](std::chrono::milliseconds patience) {
                entered.store(false);
                released.store(false);
                AgcDriver::Graphics::SetDrawInputPreparerHook(+[] {
                    entered.store(true);
                    while (!released.load()) std::this_thread::yield();
                });
                WriteIndices(Indices);
                const AgcDriver::Pm4::DrawParameters draw{reinterpret_cast<std::uintptr_t>(IndexBlock), static_cast<std::uint32_t>(Indices.size()), 2, 1, 0, true};
                const auto before = AgcDriver::Graphics::PreparedDrawInputCounters();
                auto job = device->PrepareDrawInputsAhead(DrawState(), draw, programs->shaders);
                Require(job != nullptr, "prepared vertex inputs: a draw was not prepared ahead of its commit for the abandonment check");
                while (!entered.load()) std::this_thread::yield();
                std::jthread watchdog([patience](std::stop_token token) {
                    const auto deadline = std::chrono::steady_clock::now() + patience;
                    while (!token.stop_requested() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    released.store(true);
                });
                {
                    const AgcDriver::Graphics::PreparedDrawInputsScope scope(std::move(job));
                }
                const bool returnedWhileRunning = !released.load();
                watchdog.request_stop();
                watchdog.join();
                AgcDriver::Graphics::SetDrawInputPreparerHook(nullptr);
                auto after = AgcDriver::Graphics::PreparedDrawInputCounters();
                const auto settledBy = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (after.prepared == before.prepared && std::chrono::steady_clock::now() < settledBy) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    after = AgcDriver::Graphics::PreparedDrawInputCounters();
                }
                return std::tuple{returnedWhileRunning, after.abandoned - before.abandoned, after.waited - before.waited, after.unused - before.unused};
            };
            const bool initial = Switches::Set(RuntimeSwitch::NoDrawInputAbandon);
            Switches::Assign(RuntimeSwitch::NoDrawInputAbandon, false);
            const auto [abandonReturned, abandoned, abandonWaited, abandonUnused] = finishWhileRunning(std::chrono::seconds(5));
            Switches::Assign(RuntimeSwitch::NoDrawInputAbandon, true);
            const auto [settleReturned, settleAbandoned, settleWaited, settleUnused] = finishWhileRunning(std::chrono::milliseconds(20));
            Switches::Assign(RuntimeSwitch::NoDrawInputAbandon, initial);
            std::printf("commit without adopting a running preparation: %s, %llu abandoned, %llu unused ranges counted by the preparer; with APS5_NO_DRAW_INPUT_ABANDON it %s and %llu waited\n", abandonReturned ? "returned at once" : "waited for the preparer", static_cast<unsigned long long>(abandoned), static_cast<unsigned long long>(abandonUnused), settleReturned ? "returned at once" : "waited", static_cast<unsigned long long>(settleWaited));
            Require(abandonReturned && abandoned == 1 && abandonWaited == 0 && abandonUnused != 0, "prepared vertex inputs: a commit that adopted nothing waited for a running preparation instead of abandoning it to the preparer");
            Require(!settleReturned && settleAbandoned == 0 && settleWaited == 1 && settleUnused != 0, "prepared vertex inputs: APS5_NO_DRAW_INPUT_ABANDON did not make the commit wait for the running preparation");
        }
        const auto timing = AgcDriver::Graphics::PreparedDrawInputCounters();
        std::printf("preparer time: %llu ns preparing on preparer threads, %llu ns the committer waited on %llu running preparations\n", static_cast<unsigned long long>(timing.prepareNanoseconds), static_cast<unsigned long long>(timing.waitNanoseconds), static_cast<unsigned long long>(timing.waited));
        if (AgcDriver::Graphics::DrawInputSourcesTimed()) {
            Require(timing.prepareNanoseconds != 0, "prepared vertex inputs: with draw profiling on, no preparer time was counted for the adopted preparations");
            Require(timing.waited != 0 || timing.waitNanoseconds == 0, "prepared vertex inputs: committer wait time was counted although no commit waited for a running preparation");
        } else {
            Require(timing.prepareNanoseconds == 0 && timing.waitNanoseconds == 0, "prepared vertex inputs: preparer or wait time was counted with draw profiling off");
        }
        {
            using AgcDriver::Graphics::RuntimeSwitch;
            namespace Switches = AgcDriver::Graphics::RuntimeSwitches;
            static std::atomic<bool> entered{false};
            static std::atomic<bool> released{false};
            Switches::Assign(RuntimeSwitch::NoDrawInputAbandon, false);
            AgcDriver::Graphics::SetDrawInputPreparerHook(+[] {
                entered.store(true);
                while (!released.load()) std::this_thread::yield();
            });
            WriteIndices(Indices);
            const AgcDriver::Pm4::DrawParameters draw{reinterpret_cast<std::uintptr_t>(IndexBlock), static_cast<std::uint32_t>(Indices.size()), 2, 1, 0, true};
            const auto before = AgcDriver::Graphics::PreparedDrawInputCounters();
            auto job = device->PrepareDrawInputsAhead(DrawState(), draw, programs->shaders);
            Require(job != nullptr, "prepared vertex inputs: a draw was not prepared ahead of its commit for the teardown check");
            while (!entered.load()) std::this_thread::yield();
            {
                const AgcDriver::Graphics::PreparedDrawInputsScope scope(std::move(job));
            }
            std::atomic<bool> destroyed{false};
            std::jthread teardown([&] {
                device.reset();
                destroyed.store(true);
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const bool waitedForPreparer = !destroyed.load();
            released.store(true);
            teardown.join();
            AgcDriver::Graphics::SetDrawInputPreparerHook(nullptr);
            const auto after = AgcDriver::Graphics::PreparedDrawInputCounters();
            std::printf("device teardown with an abandoned preparation still running: %s, %llu abandoned\n", waitedForPreparer ? "waited for the preparer" : "did not wait", static_cast<unsigned long long>(after.abandoned - before.abandoned));
            Require(waitedForPreparer, "prepared vertex inputs: the device was destroyed while a preparer still ran an abandoned preparation on it");
            Require(destroyed.load() && after.abandoned - before.abandoned == 1, "prepared vertex inputs: the preparation running at device teardown was not abandoned to its preparer");
        }
        std::puts("prepared vertex inputs: index and vertex copies made before the commit are adopted only while their ranges and the registry are unchanged");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
