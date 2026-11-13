#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PreparedDrawInputs.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
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
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;

constexpr std::uint32_t Width = 192;
constexpr std::uint32_t Height = 128;
constexpr std::uint32_t DistanceVector = 0x00400000u;
constexpr std::uint32_t SetContextReg = 0x69u;
constexpr std::uint32_t SetShReg = 0x76u;
constexpr std::uint32_t SetUconfigReg = 0x79u;
constexpr std::uint32_t DrawIndexOffset2Header = 0xc0033500u;

constexpr std::size_t Block = 65536;
constexpr std::size_t PoolBytes = 2 * Block;
constexpr std::size_t ColorBytes = Width * Height * 4;
constexpr std::size_t VertexOffset = 0x2000;
constexpr std::array<std::uint32_t, 3> Indices{0, 1, 2};
constexpr std::array<std::uint32_t, 3> Degenerate{0, 0, 0};

std::byte* ColorMemory = nullptr;
std::byte* Pool = nullptr;
std::byte* IndexMemory = nullptr;
alignas(256) std::array<std::uint32_t, 5> IndexedRecord{3, 1, 0, 0, 0};

alignas(256) constexpr std::array<std::uint32_t, 10> VertexCode{
    0xe0382000, 0x80020005, 0xe0382010, 0x80020405, 0xbf8c3f70,
    0xf80000cf, 0x03020100, 0xf80008df, 0x07060504, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 7> PixelCode{
    0x7e0002f2, 0x7e0202f2, 0x7e0402f2, 0x7e0602f2, 0xf800080f, 0x03020100, 0xbf810000,
};

struct Vertex {
    std::array<float, 4> position;
    std::array<float, 4> distances;
};

constexpr std::array<float, 3> PositionX{-3.0f, 1.0f, 1.0f};
constexpr std::array<float, 3> PositionY{-1.0f, -1.0f, 3.0f};
constexpr std::array<float, 3> Negative{-1.0f, -1.0f, -1.0f};

std::array<Vertex, 3> Vertices(float shift) {
    std::array<Vertex, 3> vertices{};
    for (std::size_t i = 0; i < vertices.size(); ++i) vertices[i] = {{PositionX[i] + shift, PositionY[i], 0.5f, 1.0f}, {Negative[i], Negative[i], Negative[i], 0.0f}};
    return vertices;
}

struct RegisteredShader {
    Shader shader;
    ShaderUserData userData;
};
RegisteredShader VertexShader{};
RegisteredShader PixelShader{};

const std::vector<std::pair<std::uint32_t, std::uint32_t>> ContextBaseline{
    {0x2d5, 0x2000},
    {0x1b6, 0}, {0x200, 0}, {0x203, 0x800},
    {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
    {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
    {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
    {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
    {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
    {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
    {0x31b, 0}, {0x31d, 0}, {0x3b8, 0x9000000}, {0x1e0, 0},
    {0xc, 0},
    {0x81, 0x80000000}, {0x90, 0x80000000}, {0x94, 0x80000000},
    {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0}, {0x113, std::bit_cast<std::uint32_t>(1.0f)},
};

std::byte* AllocateWatched(std::size_t bytes) {
#ifdef _WIN32
    void* block = GuestArena::GuestArenaAllocate_nid_postfix(bytes, Block);
    GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
#else
    void* raw = mmap(nullptr, bytes + Block, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) throw std::runtime_error("prepared draw inputs: cannot map the watched block");
    const auto begin = reinterpret_cast<std::uintptr_t>(raw);
    const auto aligned = (begin + Block - 1) & ~static_cast<std::uintptr_t>(Block - 1);
    if (aligned != begin) munmap(raw, aligned - begin);
    if (aligned + bytes != begin + bytes + Block) munmap(reinterpret_cast<void*>(aligned + bytes), begin + Block - aligned);
    void* block = reinterpret_cast<void*>(aligned);
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
#endif
    Require(block != nullptr, "prepared draw inputs: cannot allocate a watched block");
    return static_cast<std::byte*>(block);
}

void Register(RegisteredShader& registered, const void* code, std::uint32_t bytes, std::uint8_t type) {
    registered.shader.file_header = 0x34333231u;
    registered.shader.version = 0x18u;
    registered.shader.user_data = &registered.userData;
    registered.shader.code = code;
    registered.shader.header_size = sizeof(RegisteredShader);
    registered.shader.shader_size = bytes;
    registered.shader.type = type;
    AgcDriverRegisterShader_nid_postfix(&registered.shader);
}

void SetRegister(std::vector<std::uint32_t>& words, std::uint32_t opcode, std::uint32_t offset, std::uint32_t value) {
    words.push_back(0xc0010000u | (opcode << 8u));
    words.push_back(offset);
    words.push_back(value);
}

std::vector<std::uint32_t> Draw(bool degenerateFirst = false, bool indirect = false) {
    const auto colorAddress = reinterpret_cast<std::uintptr_t>(ColorMemory);
    const auto poolAddress = reinterpret_cast<std::uintptr_t>(Pool);
    const auto indexAddress = reinterpret_cast<std::uintptr_t>(IndexMemory);
    const auto vertexAddress = reinterpret_cast<std::uintptr_t>(VertexCode.data());
    const auto pixelAddress = reinterpret_cast<std::uintptr_t>(PixelCode.data());
    const auto vertices = poolAddress + VertexOffset;
    std::vector<std::uint32_t> words;
    if (degenerateFirst) {
        const auto second = indexAddress + sizeof(std::uint32_t);
        words.insert(words.end(), {0xc0043700u, 0x00100200u, static_cast<std::uint32_t>(second), static_cast<std::uint32_t>(second >> 32u), Degenerate[1], Degenerate[2]});
    }
    for (const auto& [offset, value] : ContextBaseline) SetRegister(words, SetContextReg, offset, value);
    SetRegister(words, SetContextReg, 0x207, DistanceVector);
    SetRegister(words, SetContextReg, 0x318, static_cast<std::uint32_t>(colorAddress >> 8u));
    SetRegister(words, SetContextReg, 0x390, static_cast<std::uint32_t>(colorAddress >> 40u));
    SetRegister(words, SetContextReg, 0x3b0, ((Width - 1u) << 14u) | (Height - 1u));
    SetRegister(words, SetContextReg, 0x10f, std::bit_cast<std::uint32_t>(Width / 2.0f));
    SetRegister(words, SetContextReg, 0x110, std::bit_cast<std::uint32_t>(Width / 2.0f));
    SetRegister(words, SetContextReg, 0x111, std::bit_cast<std::uint32_t>(-(Height / 2.0f)));
    SetRegister(words, SetContextReg, 0x112, std::bit_cast<std::uint32_t>(Height / 2.0f));
    const auto extent = (Height << 16u) | Width;
    SetRegister(words, SetContextReg, 0xd, extent);
    SetRegister(words, SetContextReg, 0x82, extent);
    SetRegister(words, SetContextReg, 0x91, extent);
    SetRegister(words, SetContextReg, 0x95, extent);
    SetRegister(words, SetUconfigReg, 0x242, 4u);
    SetRegister(words, SetShReg, 0x008, static_cast<std::uint32_t>(pixelAddress >> 8u));
    SetRegister(words, SetShReg, 0x009, static_cast<std::uint32_t>(pixelAddress >> 40u));
    SetRegister(words, SetShReg, 0x00b, 0u);
    SetRegister(words, SetShReg, 0x0c8, static_cast<std::uint32_t>(vertexAddress >> 8u));
    SetRegister(words, SetShReg, 0x0c9, static_cast<std::uint32_t>(vertexAddress >> 40u));
    SetRegister(words, SetShReg, 0x08b, 4u << 1u);
    const std::array<std::uint32_t, 4> descriptor{static_cast<std::uint32_t>(vertices), static_cast<std::uint32_t>((vertices >> 32u) & 0xffffu) | (static_cast<std::uint32_t>(sizeof(Vertex)) << 16u), 3u, 0x01016facu};
    for (std::uint32_t i = 0; i < descriptor.size(); ++i) SetRegister(words, SetShReg, 0x08c + i, descriptor[i]);
    words.insert(words.end(), {0xc0012600u, static_cast<std::uint32_t>(indexAddress), static_cast<std::uint32_t>(indexAddress >> 32u)});
    words.insert(words.end(), {0xc0001300u, static_cast<std::uint32_t>(Indices.size())});
    SetRegister(words, SetUconfigReg, 0x243, 0x401u);
    if (indirect) {
        const auto records = reinterpret_cast<std::uintptr_t>(IndexedRecord.data());
        words.insert(words.end(), {0xc0021100u, 1u, static_cast<std::uint32_t>(records), static_cast<std::uint32_t>(records >> 32u)});
        words.insert(words.end(), {0xc0032500u, 0u, 0u, 0u, 0u});
        return words;
    }
    words.insert(words.end(), {DrawIndexOffset2Header, static_cast<std::uint32_t>(Indices.size()), 0u, static_cast<std::uint32_t>(Indices.size()), 0u});
    return words;
}

void Submit(const std::vector<std::uint32_t>& words) {
    Packet packet{const_cast<std::uint32_t*>(words.data()), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "prepared draw inputs: the draw submission failed");
}

void WriteIndices(const std::array<std::uint32_t, 3>& indices) {
    std::memcpy(IndexMemory, indices.data(), sizeof(indices));
}

void WriteVertices(float shift) {
    const auto vertices = Vertices(shift);
    std::memcpy(Pool + VertexOffset, vertices.data(), sizeof(vertices));
}

std::size_t WrittenPixels() {
    std::vector<std::byte> colors(ColorBytes);
    AgcDriver::GuestMemory::Read(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ColorMemory)), colors);
    std::size_t written = 0;
    for (std::uint32_t pixel = 0; pixel < Width * Height; ++pixel) {
        const auto value = std::to_integer<std::uint8_t>(colors[pixel * 4u]);
        Require(value == 0u || value == 255u, "prepared draw inputs: pixel " + std::to_string(pixel) + " stored " + std::to_string(value));
        written += value == 255u ? 1u : 0u;
    }
    return written;
}

enum class Change { None, MoveVertices, DegenerateIndices };

std::atomic<Change> Armed{Change::None};
std::atomic<int> HookRuns{0};

void BeforeCommit(AgcDriver::Graphics::PreparedDrawInputs& prepared) {
    HookRuns.fetch_add(1);
    AgcDriver::Graphics::AwaitPreparedDrawInputs(prepared);
    switch (Armed.exchange(Change::None)) {
    case Change::None:
        break;
    case Change::MoveVertices:
        WriteVertices(16.0f);
        break;
    case Change::DegenerateIndices:
        WriteIndices(Degenerate);
        break;
    }
}

struct Outcome {
    std::size_t written = 0;
    int hooks = 0;
    AgcDriver::Graphics::PreparedDrawInputCounts before;
    AgcDriver::Graphics::PreparedDrawInputCounts after;
    std::uint64_t adopted() const { return after.adopted - before.adopted; }
    std::uint64_t reprepared() const { return after.reprepared - before.reprepared; }
    std::uint64_t requests() const { return after.requests - before.requests; }
    std::uint64_t notReady() const { return after.notReady - before.notReady; }
    std::uint64_t prepared() const { return after.prepared - before.prepared; }
    std::uint64_t refused() const { return after.refused - before.refused; }
    std::uint64_t unused() const { return after.unused - before.unused; }
    std::uint64_t copied() const { return after.sources[static_cast<std::size_t>(AgcDriver::Graphics::DrawInputSource::Copied)] - before.sources[static_cast<std::size_t>(AgcDriver::Graphics::DrawInputSource::Copied)]; }
};

Outcome DrawOnce(Change change, bool degenerateFirst = false, bool indirect = false) {
    WriteIndices(Indices);
    WriteVertices(0.0f);
    std::memset(ColorMemory, 0, ColorBytes);
    Outcome outcome;
    outcome.before = AgcDriver::Graphics::PreparedDrawInputCounters();
    const auto hooks = HookRuns.load();
    Armed.store(change);
    Submit(Draw(degenerateFirst, indirect));
    AgcDriverWaitIdle_nid_postfix();
    outcome.after = AgcDriver::Graphics::PreparedDrawInputCounters();
    outcome.hooks = HookRuns.load() - hooks;
    outcome.written = WrittenPixels();
    std::uint64_t byReason = 0;
    for (std::size_t reason = 0; reason < AgcDriver::Graphics::PreparedRefusalCount; ++reason) byReason += outcome.after.refusedBy[reason] - outcome.before.refusedBy[reason];
    Require(byReason == outcome.refused(), "prepared draw inputs: the refusals by reason do not add up to the refused ranges");
    Require(Armed.load() == Change::None, "prepared draw inputs: the armed change never ran between preparation and commit");
    return outcome;
}

void Print(const char* what, const Outcome& outcome) {
    std::printf("%s: %zu of %u pixels written, %d commit hook runs, %llu requests, %llu ranges prepared, %llu refused, %llu adopted, %llu prepared again, %llu unused, %llu not ready, %llu copied on the committer\n", what, outcome.written, Width * Height, outcome.hooks, static_cast<unsigned long long>(outcome.requests()), static_cast<unsigned long long>(outcome.prepared()), static_cast<unsigned long long>(outcome.refused()), static_cast<unsigned long long>(outcome.adopted()), static_cast<unsigned long long>(outcome.reprepared()), static_cast<unsigned long long>(outcome.unused()), static_cast<unsigned long long>(outcome.notReady()), static_cast<unsigned long long>(outcome.copied()));
}

void UnchangedInputsAreAdopted() {
    const auto outcome = DrawOnce(Change::None);
    Print("unchanged inputs", outcome);
    Require(outcome.hooks == 1 && outcome.requests() == 1, "prepared draw inputs: the draw was not prepared ahead of its commit");
    Require(outcome.adopted() == 1 && outcome.reprepared() == 0 && outcome.notReady() == 0 && outcome.copied() == 0, "prepared draw inputs: an unchanged draw did not adopt its prepared index copy");
    Require(outcome.written == Width * Height, "prepared draw inputs: the draw with adopted inputs left pixels cleared");
}

void VerticesChangedAfterPreparationAreCopiedAgain() {
    const auto outcome = DrawOnce(Change::MoveVertices);
    Print("vertices moved off the target after preparation", outcome);
    Require(outcome.hooks == 1 && outcome.requests() == 1, "prepared draw inputs: the draw was not prepared ahead of its commit");
    Require(outcome.adopted() == 1 && outcome.reprepared() == 0, "prepared draw inputs: the index copy in a block the CPU left alone was not adopted");
    Require(outcome.written == 0, "prepared draw inputs: the draw drew the vertices from before the CPU moved them off the target");
}

void IndicesChangedAfterPreparationAreCopiedAgain() {
    const auto outcome = DrawOnce(Change::DegenerateIndices);
    Print("indices made degenerate after preparation", outcome);
    Require(outcome.hooks == 1 && outcome.requests() == 1, "prepared draw inputs: the draw was not prepared ahead of its commit");
    Require(outcome.reprepared() == 1 && outcome.adopted() == 0, "prepared draw inputs: an index range the CPU wrote after preparation was not prepared again");
    Require(outcome.written == 0, "prepared draw inputs: the draw drew the indices prepared before the CPU made them degenerate");
}

void GpuWriteBeforeTheDrawReachesItsIndices() {
    const auto outcome = DrawOnce(Change::None, true);
    Print("a queued write of degenerate indices ahead of the draw", outcome);
    Require(outcome.hooks == 1 && outcome.requests() == 1, "prepared draw inputs: the draw behind the index write was not prepared ahead of its commit");
    Require(outcome.written == 0, "prepared draw inputs: the draw read the indices from before the write the queue ordered ahead of it");
    Require(std::memcmp(IndexMemory + sizeof(std::uint32_t), &Degenerate[1], 2 * sizeof(std::uint32_t)) == 0, "prepared draw inputs: the index write never reached guest memory");
}

void IndirectRecordInputsAreAdopted() {
    const auto outcome = DrawOnce(Change::None, false, true);
    Print("indexed indirect draw, unchanged inputs", outcome);
    Require(outcome.hooks == 1 && outcome.requests() == 1, "prepared draw inputs: the indexed indirect draw was not prepared ahead of its commit");
    Require(outcome.adopted() == 1 && outcome.reprepared() == 0 && outcome.copied() == 0, "prepared draw inputs: the indexed indirect draw did not adopt its prepared index copy");
    Require(outcome.written == Width * Height, "prepared draw inputs: the indexed indirect draw with an adopted index copy left pixels cleared");
}

void IndirectRecordIndicesChangedAfterPreparation() {
    const auto outcome = DrawOnce(Change::DegenerateIndices, false, true);
    Print("indexed indirect draw, indices made degenerate after preparation", outcome);
    Require(outcome.hooks == 1 && outcome.requests() == 1, "prepared draw inputs: the indexed indirect draw was not prepared ahead of its commit");
    Require(outcome.reprepared() == 1 && outcome.adopted() == 0, "prepared draw inputs: an indirect draw's index range the CPU wrote after preparation was not prepared again");
    Require(outcome.written == 0, "prepared draw inputs: the indexed indirect draw drew the indices prepared before the CPU made them degenerate");
}

void EmptyFetchSkipDrawsTheSame() {
    using AgcDriver::Graphics::RuntimeSwitch;
    namespace Switches = AgcDriver::Graphics::RuntimeSwitches;
    const auto colors = [] {
        std::vector<std::byte> bytes(ColorBytes);
        AgcDriver::GuestMemory::Read(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ColorMemory)), bytes);
        return bytes;
    };
    Switches::Assign(RuntimeSwitch::NoEmptyFetchSkip, true);
    const auto planned = DrawOnce(Change::None);
    const auto plannedColors = colors();
    Switches::Assign(RuntimeSwitch::NoEmptyFetchSkip, false);
    const auto skipped = DrawOnce(Change::None);
    const auto skippedColors = colors();
    std::printf("no vertex attributes: %zu pixels with the fetch planning, %zu without it\n", planned.written, skipped.written);
    Require(planned.written == Width * Height && skippedColors == plannedColors, "prepared draw inputs: skipping the vertex fetch planning of a draw without vertex attributes changed what it drew");
    Require(skipped.adopted() == planned.adopted() && skipped.copied() == planned.copied(), "prepared draw inputs: skipping the vertex fetch planning changed how the draw's inputs were made");
}

void IdleCommitterCopiesTheInputsItself() {
    using AgcDriver::Graphics::RuntimeSwitch;
    namespace Switches = AgcDriver::Graphics::RuntimeSwitches;
    Switches::Assign(RuntimeSwitch::EarlyDrawInputsAlways, false);
    const auto outcome = DrawOnce(Change::None);
    Switches::Assign(RuntimeSwitch::EarlyDrawInputsAlways, true);
    const auto shallow = outcome.after.shallow - outcome.before.shallow;
    Print("committer idle at the enqueue", outcome);
    std::printf("committer idle at the enqueue: %llu draws left to the committer\n", static_cast<unsigned long long>(shallow));
    Require(outcome.hooks == 0 && outcome.requests() == 0 && outcome.after.opened == outcome.before.opened && shallow == (std::getenv("APS5_PROFILE_DRAW") != nullptr || std::getenv("APS5_PIPELINE_STATS") != nullptr ? 1u : 0u), "prepared draw inputs: a draw enqueued while the committer was idle was prepared ahead of its commit");
    Require(outcome.copied() != 0 && outcome.written == Width * Height, "prepared draw inputs: the draw left to an idle committer did not copy its inputs there and draw");
}

void NoPreparersCopyOnTheCommitter() {
    AgcDriver::Graphics::SetDrawInputPreparers(0);
    const auto outcome = DrawOnce(Change::None);
    Print("no preparer threads", outcome);
    Require(outcome.hooks == 0 && outcome.requests() == 0 && outcome.adopted() == 0, "prepared draw inputs: a draw was prepared with no preparer threads");
    Require(outcome.written == Width * Height, "prepared draw inputs: the draw copied on the committer left pixels cleared");
}

}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_PIPELINED_DRAWS", "1");
    _putenv_s("APS5_NO_DRAW_INPUTS_IN_PLACE", "1");
#else
    setenv("APS5_PIPELINED_DRAWS", "1", 1);
    setenv("APS5_NO_DRAW_INPUTS_IN_PLACE", "1", 1);
#endif
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        ColorMemory = AllocateWatched(2 * Block);
        Pool = AllocateWatched(PoolBytes);
        IndexMemory = AllocateWatched(Block);
        GuestAllocations::Mutation().Add(Pool, PoolBytes, true, true, true);
        GuestAllocations::Mutation().Add(IndexMemory, Block, true, true, true);
        Register(VertexShader, VertexCode.data(), sizeof(VertexCode), 2);
        Register(PixelShader, PixelCode.data(), sizeof(PixelCode), 1);
        AgcDriver::Graphics::SetDrawInputPreparers(1);
        AgcDriver::Graphics::SetPreparedDrawInputsHook(&BeforeCommit);
        AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::EarlyDrawInputsAlways, true);
        UnchangedInputsAreAdopted();
        VerticesChangedAfterPreparationAreCopiedAgain();
        IndicesChangedAfterPreparationAreCopiedAgain();
        UnchangedInputsAreAdopted();
        GpuWriteBeforeTheDrawReachesItsIndices();
        IndirectRecordInputsAreAdopted();
        IndirectRecordIndicesChangedAfterPreparation();
        EmptyFetchSkipDrawsTheSame();
        IdleCommitterCopiesTheInputsItself();
        NoPreparersCopyOnTheCommitter();
        AgcDriver::Graphics::SetPreparedDrawInputsHook(nullptr);
        std::puts("prepared draw inputs: copies made before the commit are adopted only while the CPU left their ranges unchanged");
        AgcDriverShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
