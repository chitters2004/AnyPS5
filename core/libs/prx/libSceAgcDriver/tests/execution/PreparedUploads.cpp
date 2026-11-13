#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PreparedDrawInputs.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
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
constexpr std::array<std::uint32_t, 3> Indices{0, 1, 2};

std::byte* ColorMemory = nullptr;
std::byte* Pool = nullptr;
std::byte* IndexMemory = nullptr;
std::byte* SpareBlock = nullptr;
std::size_t VertexOffset = 0x2000;
bool TiledTarget = false;

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
    if (raw == MAP_FAILED) throw std::runtime_error("prepared uploads: cannot map the watched block");
    const auto begin = reinterpret_cast<std::uintptr_t>(raw);
    const auto aligned = (begin + Block - 1) & ~static_cast<std::uintptr_t>(Block - 1);
    if (aligned != begin) munmap(raw, aligned - begin);
    if (aligned + bytes != begin + bytes + Block) munmap(reinterpret_cast<void*>(aligned + bytes), begin + Block - aligned);
    void* block = reinterpret_cast<void*>(aligned);
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
#endif
    Require(block != nullptr, "prepared uploads: cannot allocate a watched block");
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

std::vector<std::uint32_t> Draw(bool writeVerticesFirst = false) {
    const auto colorAddress = reinterpret_cast<std::uintptr_t>(ColorMemory);
    const auto poolAddress = reinterpret_cast<std::uintptr_t>(Pool);
    const auto indexAddress = reinterpret_cast<std::uintptr_t>(IndexMemory);
    const auto vertexAddress = reinterpret_cast<std::uintptr_t>(VertexCode.data());
    const auto pixelAddress = reinterpret_cast<std::uintptr_t>(PixelCode.data());
    const auto vertices = poolAddress + VertexOffset;
    std::vector<std::uint32_t> words;
    if (writeVerticesFirst) {
        const auto moved = Vertices(16.0f);
        std::array<std::uint32_t, sizeof(moved) / sizeof(std::uint32_t)> movedWords{};
        std::memcpy(movedWords.data(), moved.data(), sizeof(moved));
        words.insert(words.end(), {0xc0003700u | (static_cast<std::uint32_t>(movedWords.size() + 2u) << 16u), 0x00100200u, static_cast<std::uint32_t>(vertices), static_cast<std::uint32_t>(vertices >> 32u)});
        words.insert(words.end(), movedWords.begin(), movedWords.end());
    }
    for (const auto& [offset, value] : ContextBaseline) SetRegister(words, SetContextReg, offset, value);
    SetRegister(words, SetContextReg, 0x207, DistanceVector);
    if (TiledTarget) SetRegister(words, SetContextReg, 0x3b8, 0x9000000u | (5u << 14u));
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
    words.insert(words.end(), {DrawIndexOffset2Header, static_cast<std::uint32_t>(Indices.size()), 0u, static_cast<std::uint32_t>(Indices.size()), 0u});
    return words;
}

void Submit(const std::vector<std::uint32_t>& words) {
    Packet packet{const_cast<std::uint32_t*>(words.data()), static_cast<std::uint32_t>(words.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "prepared uploads: the draw submission failed");
}

void WriteVertices(float shift) {
    const auto vertices = Vertices(shift);
    std::memcpy(Pool + VertexOffset, vertices.data(), sizeof(vertices));
}

std::vector<std::byte> Colors() {
    const auto colorAddress = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ColorMemory));
    static_cast<void>(AgcDriver::Graphics::StorageTexture::FlushPending(colorAddress, ColorBytes, nullptr, "prepared uploads test"));
    AgcDriver::GuestMemory::FlushGpuWrites(colorAddress, ColorBytes);
    AgcDriverWaitIdle_nid_postfix();
    std::vector<std::byte> colors(ColorBytes);
    AgcDriver::GuestMemory::Read(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ColorMemory)), colors);
    return colors;
}

std::size_t WrittenPixels(const std::vector<std::byte>& colors) {
    std::size_t written = 0;
    for (std::uint32_t pixel = 0; pixel < Width * Height; ++pixel) {
        const auto value = std::to_integer<std::uint8_t>(colors[pixel * 4u]);
        Require(value == 0u || value == 255u, "prepared uploads: pixel " + std::to_string(pixel) + " stored " + std::to_string(value));
        written += value == 255u ? 1u : 0u;
    }
    return written;
}

enum class Change { None, MoveVertices, Registry, ForgetPool };

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
    case Change::Registry:
        GuestAllocations::Mutation().Add(SpareBlock, Block, true, true, true);
        GuestAllocations::Mutation().Remove(SpareBlock);
        break;
    case Change::ForgetPool:
        GuestAllocations::GuestAllocationsInvalidate_nid_postfix(reinterpret_cast<std::uintptr_t>(Pool), PoolBytes);
        break;
    }
}

using Counts = AgcDriver::Graphics::PreparedUploadCounts;

struct Outcome {
    std::vector<std::byte> colors;
    std::size_t written = 0;
    int hooks = 0;
    Counts before;
    Counts after;
    std::uint64_t requests() const { return after.requests - before.requests; }
    std::uint64_t stageAPrepared() const { return after.stageAPrepared - before.stageAPrepared; }
    std::uint64_t stageAAdopted() const { return after.stageAAdopted - before.stageAAdopted; }
    std::uint64_t stageAStale() const { return after.stageAStale - before.stageAStale; }
    std::uint64_t stageAStaleStaging() const { return after.stageAStaleStaging - before.stageAStaleStaging; }
    std::uint64_t stageAStaleBy(AgcDriver::Graphics::PreparedStaleness reason) const {
        const auto index = static_cast<std::size_t>(reason);
        return after.stageAStaleBy[index] - before.stageAStaleBy[index];
    }
    std::uint64_t stageAStaleReasons() const {
        std::uint64_t total = 0;
        for (std::size_t reason = 0; reason < AgcDriver::Graphics::PreparedStalenessCount; ++reason) total += after.stageAStaleBy[reason] - before.stageAStaleBy[reason];
        return total;
    }
    std::uint64_t stageAUnused() const { return after.stageAUnused - before.stageAUnused; }
    std::uint64_t stagingPredicted() const { return after.stageAStagingPredicted - before.stageAStagingPredicted; }
    std::uint64_t stagingFallback() const { return after.stageAStagingFallback - before.stageAStagingFallback; }
    std::uint64_t stagingMispredicted() const { return after.stageAStagingMispredicted - before.stageAStagingMispredicted; }
    std::uint64_t predictedHits() const { return after.stageAPredictedHit - before.stageAPredictedHit; }
    std::uint64_t regionsPrepared() const { return after.regionsPrepared - before.regionsPrepared; }
    std::uint64_t regionsAdopted() const { return after.regionsAdopted - before.regionsAdopted; }
    std::uint64_t regionsReprepared() const { return after.regionsReprepared - before.regionsReprepared; }
    std::uint64_t regionsRefused() const { return after.regionsRefused - before.regionsRefused; }
};

Outcome DrawOnce(Change change, std::size_t vertexOffset, bool writeVerticesFirst = false, float shift = 0.0f) {
    VertexOffset = vertexOffset;
    std::memcpy(IndexMemory, Indices.data(), sizeof(Indices));
    WriteVertices(shift);
    std::memset(ColorMemory, 0, ColorBytes);
    Outcome outcome;
    outcome.before = AgcDriver::Graphics::PreparedUploadCounters();
    const auto hooks = HookRuns.load();
    Armed.store(change);
    Submit(Draw(writeVerticesFirst));
    AgcDriverWaitIdle_nid_postfix();
    outcome.after = AgcDriver::Graphics::PreparedUploadCounters();
    outcome.hooks = HookRuns.load() - hooks;
    outcome.colors = Colors();
    outcome.written = WrittenPixels(outcome.colors);
    Require(Armed.load() == Change::None, "prepared uploads: the armed change never ran between preparation and commit");
    Require(outcome.stageAStaleReasons() == outcome.stageAStale(), "prepared uploads: a stale stage A was not counted under exactly one reason");
    Require(outcome.stagingMispredicted() == 0, "prepared uploads: the preparer predicted a staging decision the committer did not make");
    Require(outcome.after.stageAVerifyDiffered == outcome.before.stageAVerifyDiffered && outcome.after.regionVerifyDiffered == outcome.before.regionVerifyDiffered, "prepared uploads: APS5_VERIFY_EARLY_UPLOADS found an adopted stage A or region copy that differs from the committer's own");
    return outcome;
}

void Print(const char* what, const Outcome& outcome) {
    std::printf("%s: %zu of %u pixels written, %d hook runs, %llu requests, stage A %llu prepared / %llu adopted / %llu stale / %llu stale staging / %llu unused / %llu predicted hits, regions %llu prepared / %llu adopted / %llu prepared again / %llu refused\n", what, outcome.written, Width * Height, outcome.hooks, static_cast<unsigned long long>(outcome.requests()), static_cast<unsigned long long>(outcome.stageAPrepared()), static_cast<unsigned long long>(outcome.stageAAdopted()), static_cast<unsigned long long>(outcome.stageAStale()), static_cast<unsigned long long>(outcome.stageAStaleStaging()), static_cast<unsigned long long>(outcome.stageAUnused()), static_cast<unsigned long long>(outcome.predictedHits()), static_cast<unsigned long long>(outcome.regionsPrepared()), static_cast<unsigned long long>(outcome.regionsAdopted()), static_cast<unsigned long long>(outcome.regionsReprepared()), static_cast<unsigned long long>(outcome.regionsRefused()));
}

Outcome Reference(std::size_t vertexOffset, bool moved) {
    namespace Switches = AgcDriver::Graphics::RuntimeSwitches;
    using AgcDriver::Graphics::RuntimeSwitch;
    Switches::Assign(RuntimeSwitch::NoEarlyUploadPrepare, true);
    Switches::Assign(RuntimeSwitch::NoEarlyRegionCopies, true);
    auto outcome = DrawOnce(Change::None, vertexOffset, false, moved ? 16.0f : 0.0f);
    Switches::Assign(RuntimeSwitch::NoEarlyUploadPrepare, false);
    Switches::Assign(RuntimeSwitch::NoEarlyRegionCopies, false);
    Require(outcome.requests() == 0 && outcome.stageAPrepared() == 0 && outcome.regionsPrepared() == 0, "prepared uploads: a draw was prepared with both switches set");
    return outcome;
}

}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_PIPELINED_DRAWS", "1");
    _putenv_s("APS5_NO_DRAW_INPUTS_IN_PLACE", "1");
    _putenv_s("APS5_NO_HOST_IMPORT", "1");
    _putenv_s("APS5_VERIFY_EARLY_UPLOADS", "1");
#else
    setenv("APS5_PIPELINED_DRAWS", "1", 1);
    setenv("APS5_NO_DRAW_INPUTS_IN_PLACE", "1", 1);
    setenv("APS5_NO_HOST_IMPORT", "1", 1);
    setenv("APS5_VERIFY_EARLY_UPLOADS", "1", 1);
#endif
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        ColorMemory = AllocateWatched(2 * Block);
        Pool = AllocateWatched(PoolBytes);
        IndexMemory = AllocateWatched(Block);
        SpareBlock = AllocateWatched(Block);
        GuestAllocations::Mutation().Add(Pool, PoolBytes, true, true, true);
        GuestAllocations::Mutation().Add(IndexMemory, Block, true, true, true);
        GuestAllocations::Mutation().Add(ColorMemory, 2 * Block, true, true, true);
        Register(VertexShader, VertexCode.data(), sizeof(VertexCode), 2);
        Register(PixelShader, PixelCode.data(), sizeof(PixelCode), 1);
        AgcDriver::Graphics::SetDrawInputPreparers(1);
        AgcDriver::Graphics::SetPreparedDrawInputsHook(&BeforeCommit);

        const auto first = DrawOnce(Change::None, 0x1f00);
        Print("first draw, staging decision predicted from its linear target without host imports", first);
        Require(first.written == Width * Height && first.stageAAdopted() == 1 && first.stageAStaleStaging() == 0 && first.stagingPredicted() == 1, "prepared uploads: the first draw's staging decision was not predicted from its target, so its stage A was refused");
        Require(first.regionsAdopted() >= 1, "prepared uploads: the committer's own build did not adopt the prepared region copy");

        const auto unchanged = DrawOnce(Change::None, 0x2000);
        Print("unchanged vertex buffer", unchanged);
        Require(unchanged.hooks == 1 && unchanged.requests() == 1, "prepared uploads: the draw's resources were not prepared ahead of its commit");
        Require(unchanged.stageAAdopted() == 1 && unchanged.regionsAdopted() >= 1 && unchanged.regionsReprepared() == 0, "prepared uploads: an unchanged draw did not adopt its stage A and its prepared region copy");
        Require(unchanged.written == Width * Height, "prepared uploads: the draw with an adopted stage A left pixels cleared");
        const auto unchangedReference = Reference(0x2100, false);
        Require(unchangedReference.colors == unchanged.colors, "prepared uploads: the draw with an adopted stage A and region copy differs from the draw built on the committer");

        const auto repeated = DrawOnce(Change::None, 0x2000);
        Print("the same draw again, uncacheable because it is not recorded", repeated);
        Require(repeated.predictedHits() == 0 && repeated.stageAAdopted() == 1 && repeated.regionsAdopted() >= 1, "prepared uploads: an uncacheable repeated draw was predicted to hit the resource cache or did not adopt its preparation");
        Require(repeated.written == Width * Height, "prepared uploads: the repeated draw left pixels cleared");

        const auto moved = DrawOnce(Change::MoveVertices, 0x2200);
        Print("vertices moved off the target after preparation", moved);
        Require(moved.regionsReprepared() >= 1 && moved.regionsAdopted() == 0, "prepared uploads: a region the CPU wrote after preparation was adopted");
        Require(moved.written == 0, "prepared uploads: the draw used the vertex buffer prepared before the CPU moved it off the target");
        const auto movedReference = Reference(0x2300, true);
        Require(movedReference.colors == moved.colors, "prepared uploads: the draw whose region was copied again differs from the draw built on the committer");

        using AgcDriver::Graphics::RuntimeSwitch;
        namespace Switches = AgcDriver::Graphics::RuntimeSwitches;
        const bool globalChecks = Switches::Set(RuntimeSwitch::GlobalProofChecks);
        for (const bool global : {false, true}) {
            Switches::Assign(RuntimeSwitch::GlobalProofChecks, global);
            const std::string mode = global ? " (APS5_GLOBAL_PROOF_CHECKS)" : "";
            const auto remapped = DrawOnce(Change::Registry, 0x2400);
            Print(global ? "allocation registry changed elsewhere after preparation, global proof checks" : "allocation registry changed elsewhere after preparation", remapped);
            Require(remapped.stageAStale() == 1 && remapped.stageAAdopted() == 0, "prepared uploads: a stage A prepared under another registry generation was adopted" + mode);
            if (global) Require(remapped.regionsAdopted() == 0, "prepared uploads: a region prepared under another registry generation was adopted under APS5_GLOBAL_PROOF_CHECKS");
            else Require(remapped.regionsAdopted() >= 1 && remapped.regionsReprepared() == 0, "prepared uploads: a registry change outside the prepared region made it prepare again");
            Require(remapped.stageAStaleBy(AgcDriver::Graphics::PreparedStaleness::Registry) == 1, "prepared uploads: a stage A refused for a registry change was counted under another reason" + mode);
            Require(remapped.written == Width * Height, "prepared uploads: the draw built again after the registry changed left pixels cleared" + mode);
            const auto forgotten = DrawOnce(Change::ForgetPool, 0x2400);
            Print(global ? "vertex pool forgotten after preparation, global proof checks" : "vertex pool forgotten after preparation", forgotten);
            Require(forgotten.stageAAdopted() == 0 && forgotten.regionsAdopted() == 0 && forgotten.regionsReprepared() >= 1, "prepared uploads: a stage A or region was adopted across a forget over its range" + mode);
            Require(forgotten.written == Width * Height, "prepared uploads: the draw built again after a forget over its vertex pool left pixels cleared" + mode);
        }
        Switches::Assign(RuntimeSwitch::GlobalProofChecks, false);
        AgcDriver::Graphics::IgnoreRangeForgetsForTest(true);
        const auto mutated = DrawOnce(Change::ForgetPool, 0x2400);
        AgcDriver::Graphics::IgnoreRangeForgetsForTest(false);
        Switches::Assign(RuntimeSwitch::GlobalProofChecks, globalChecks);
        Print("vertex pool forgotten after preparation, range forget stamps ignored (mutant)", mutated);
        Require(mutated.regionsAdopted() >= 1, "prepared uploads: the forget check does not kill a mutant that ignores range forget stamps");

        const auto queued = DrawOnce(Change::None, 0x2500, true);
        Print("a queued write moving the vertices ahead of the draw", queued);
        Require(queued.written == 0, "prepared uploads: the draw read the vertices from before the write the queue ordered ahead of it");

        TiledTarget = true;
        const auto tiledReference = Reference(0x2600, false).colors;
        TiledTarget = false;
        const auto linearReference = Reference(0x2600, false).colors;
        const auto alternate = [&](const char* what) {
            std::uint64_t adopted = 0;
            std::uint64_t staleStaging = 0;
            std::uint64_t predicted = 0;
            for (int round = 0; round < 4; ++round) {
                TiledTarget = (round % 2) == 1;
                const auto outcome = DrawOnce(Change::None, 0x2600);
                adopted += outcome.stageAAdopted();
                staleStaging += outcome.stageAStaleStaging();
                predicted += outcome.stagingPredicted();
                Require(outcome.written == Width * Height && outcome.colors == (TiledTarget ? tiledReference : linearReference), std::string("prepared uploads: ") + what + ": a draw differs from the same draw built on the committer");
            }
            TiledTarget = false;
            std::printf("%s: linear and tiled targets alternating over 4 draws: %llu stage A adopted, %llu refused for the staging decision, %llu decisions predicted\n", what, static_cast<unsigned long long>(adopted), static_cast<unsigned long long>(staleStaging), static_cast<unsigned long long>(predicted));
            return std::array<std::uint64_t, 3>{adopted, staleStaging, predicted};
        };
        const auto predicting = alternate("staging predicted");
        Require(predicting[0] == 4 && predicting[1] == 0 && predicting[2] == 4, "prepared uploads: synchronous and recorded draws alternating refused a stage A for its staging decision while the decision was predicted");
        AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::NoStagingPrediction, true);
        const auto guessing = alternate("APS5_NO_STAGING_PREDICTION");
        AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::NoStagingPrediction, false);
        Require(guessing[1] >= 3 && guessing[2] == 0, "prepared uploads: with APS5_NO_STAGING_PREDICTION the alternating draws were not refused for the last staging decision, so the arm proves nothing");

        bool refused = false;
        {
            const AgcDriver::GuestMemory::DeviceLockForbiddenScope forbidden;
            try {
                const std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            } catch (const std::runtime_error&) {
                refused = true;
            }
        }
        Require(refused, "prepared uploads: a thread that prepares work off the device lock took GuestMemory::GpuMutex");
        {
            const std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        }

        AgcDriver::Graphics::SetPreparedDrawInputsHook(nullptr);
        std::puts("prepared uploads: a draw's stage A and region copies made before its commit are adopted only while the registry, the staging decisions and the regions' bytes are unchanged");
        AgcDriverShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
