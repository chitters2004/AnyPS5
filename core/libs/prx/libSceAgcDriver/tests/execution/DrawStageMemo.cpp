#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawStageMemo.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
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
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

struct DriverTestAccess {
    static bool Unchanged(const DrawStageRecord& record) { return Driver::Get().drawStageUnchanged(record); }
    static DrawStageProof Prove(DrawStageRecord& record) { return Driver::Get().proveDrawStage(record, 0, 0); }
};

}

namespace {

using AgcDriver::DriverDetail::BuildDrawStageKey;
using AgcDriver::DriverDetail::DrawStageProof;
using AgcDriver::DriverDetail::DriverTestAccess;
using AgcDriver::DriverDetail::DrawStageKey;
using AgcDriver::DriverDetail::DrawStageMemo;
using AgcDriver::DriverDetail::DrawStageMemoCounts;
using AgcDriver::DriverDetail::DrawStageRecord;
using AgcDriver::DriverDetail::DrawStageSpans;
using AgcDriver::DriverDetail::ShaderSnapshot;
using AgcDriver::Graphics::Require;
using Stage = ShaderRecompiler::ShaderStage;

void Expect(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error("draw stage memo: " + what);
}

struct RequestParts {
    std::array<std::uint32_t, 4> code{0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
    std::array<std::byte, 16> header{};
    std::vector<std::uint32_t> userData{0x1000u, 0x2u, 0x3u, 0x4u};
    std::optional<ShaderRecompiler::ShaderPixelStageInfo> pixel;
    std::optional<ShaderRecompiler::ShaderVertexStageInfo> vertex;
    std::array<std::uint32_t, 4> memoryWords{1u, 2u, 3u, 4u};
    std::vector<ShaderRecompiler::MemoryRegion> memory;
    std::vector<ShaderRecompiler::LinkedProgram> linked;
    std::optional<ShaderRecompiler::ShaderFloatMode> floatMode;
    std::optional<ShaderRecompiler::MeshConfiguration> mesh;
    std::optional<ShaderRecompiler::TessellationConfiguration> tessellation;
    ShaderRecompiler::GraphicsDrawParameters draw{0x8000u, 3u, 4u, 1u};
    Stage stage = Stage::Vertex;
    std::uint32_t waveSize = 64u;
    std::uint32_t pushOffset = 0u;
    std::uint32_t pushSize = 128u;
    bool convertedSamples = false;

    ShaderRecompiler::RecompileRequest Request() const {
        ShaderRecompiler::RecompileRequest request{
            {stage, 0x10000u, code, 0x20000u, header},
            {waveSize, 8u, userData, std::nullopt, pixel, vertex, memory, floatMode},
            ShaderRecompiler::SpirvTarget{},
            {0u, 0u, pushOffset, pushSize},
            ShaderRecompiler::GraphicsCompileContext{8u, linked, mesh, tessellation, draw, std::nullopt}
        };
        request.context.convertedSamples = convertedSamples;
        return request;
    }
};

ShaderRecompiler::ShaderVertexStageInfo VertexInfo() {
    ShaderRecompiler::ShaderVertexStageInfo info{};
    info.resourcesNum = 1u;
    info.resources[0].fields = {0x1000u, 0x100000u, 3u, 0x0104dfacu};
    info.resourcesDst[0] = {0, 4, 0, 0u};
    info.fetchAttribReg = 4u;
    info.fetchBufferReg = 2u;
    return info;
}

DrawStageKey KeyOf(const RequestParts& parts, const ShaderSnapshot& snapshot, std::size_t codeOffset = 0, std::uint64_t serial = 1) {
    DrawStageKey key;
    Expect(BuildDrawStageKey(parts.Request(), snapshot, codeOffset, serial, key), "a plain graphics request was refused a key");
    return key;
}

void KeysFollowEveryInput() {
    ShaderSnapshot snapshot{};
    ShaderSnapshot other{};
    RequestParts base;
    base.vertex = VertexInfo();
    const auto reference = KeyOf(base, snapshot);
    Expect(KeyOf(base, snapshot) == reference, "the same inputs built different keys");
    const auto differs = [&](const char* what, const std::function<void(RequestParts&)>& change) {
        auto parts = base;
        change(parts);
        Expect(!(KeyOf(parts, snapshot) == reference), std::string("the key ignores ") + what);
    };
    const auto same = [&](const char* what, const std::function<void(RequestParts&)>& change) {
        auto parts = base;
        change(parts);
        Expect(KeyOf(parts, snapshot) == reference, std::string("the key depends on ") + what + ", which the stage build does not read");
    };
    differs("a user data word", [](RequestParts& parts) { parts.userData[1] ^= 1u; });
    differs("the user data count", [](RequestParts& parts) { parts.userData.push_back(0u); });
    differs("a vertex buffer descriptor word of the fetch layout", [](RequestParts& parts) { parts.vertex->resources[0].fields[1] ^= 0x10000u; });
    differs("a vertex attribute destination", [](RequestParts& parts) { parts.vertex->resourcesDst[0].registersNum = 3; });
    differs("the position export layout", [](RequestParts& parts) { parts.vertex->paClVsOutCntl = 1u; });
    differs("the point size", [](RequestParts& parts) { parts.vertex->pointSize = 2.0f; });
    differs("the push constant offset", [](RequestParts& parts) { parts.pushOffset = 64u; });
    differs("the push constant size", [](RequestParts& parts) { parts.pushSize = 64u; });
    differs("the wave size", [](RequestParts& parts) { parts.waveSize = 32u; });
    differs("the float mode", [](RequestParts& parts) { parts.floatMode = ShaderRecompiler::ShaderFloatMode{0xc0u, true, false, false}; });
    differs("the converted sample mode", [](RequestParts& parts) { parts.convertedSamples = true; });
    differs("the stage", [](RequestParts& parts) { parts.stage = Stage::Local; });
    differs("the mesh configuration", [](RequestParts& parts) { parts.mesh = ShaderRecompiler::MeshConfiguration{}; });
    differs("the pixel stage info", [](RequestParts& parts) { parts.pixel = ShaderRecompiler::ShaderPixelStageInfo{}; });
    same("guest memory bytes", [](RequestParts& parts) { parts.memory.push_back({0x3000u, std::as_bytes(std::span(parts.memoryWords))}); });
    same("the other linked programs", [](RequestParts& parts) { parts.linked.push_back({ShaderRecompiler::ProgramRole::Fragment, {Stage::Fragment, 0x40000u, parts.code, 0u, {}}, 0x0cu, 0u, parts.userData}); });
    same("the index range of a draw without tessellation", [](RequestParts& parts) { parts.draw = {0x9000u, 300u, 2u, 7u}; });
    auto tessellated = base;
    tessellated.tessellation = ShaderRecompiler::TessellationConfiguration{};
    const auto withTessellation = KeyOf(tessellated, snapshot);
    Expect(!(withTessellation == reference), "the key ignores the tessellation configuration");
    tessellated.draw.instanceCount = 4u;
    Expect(!(KeyOf(tessellated, snapshot) == withTessellation), "the key ignores the instance count of a tessellated draw");

    auto pixelParts = base;
    pixelParts.stage = Stage::Fragment;
    pixelParts.vertex.reset();
    pixelParts.pixel = ShaderRecompiler::ShaderPixelStageInfo{};
    pixelParts.pixel->targetMerges.push_back({});
    const auto pixelKey = KeyOf(pixelParts, snapshot);
    const auto pixelDiffers = [&](const char* what, const std::function<void(ShaderRecompiler::ShaderPixelStageInfo&)>& change) {
        auto parts = pixelParts;
        change(*parts.pixel);
        Expect(!(KeyOf(parts, snapshot) == pixelKey), std::string("the key ignores the pixel stage's ") + what);
    };
    pixelDiffers("export mapping", [](auto& pixel) { pixel.targetExportMapping[0] = 1u; });
    pixelDiffers("export packing", [](auto& pixel) { pixel.targetExportPacking[1] = ShaderRecompiler::ColorExportPacking::None == pixel.targetExportPacking[1] ? static_cast<ShaderRecompiler::ColorExportPacking>(1) : ShaderRecompiler::ColorExportPacking::None; });
    pixelDiffers("merge descriptor", [](auto& pixel) { pixel.targetMerges[0].descriptor[3] = 7u; });
    pixelDiffers("blend constants", [](auto& pixel) { pixel.targetMerges[0].blend.enabled = true; pixel.targetMerges[0].blend.constants[0] = 1u; });
    pixelDiffers("dual source blend", [](auto& pixel) { pixel.dualSourceBlend = !pixel.dualSourceBlend; });
    pixelDiffers("exported layer", [](auto& pixel) { pixel.layerExported = !pixel.layerExported; });
    pixelDiffers("depth export range", [](auto& pixel) { pixel.depthExportMax = 0.5f; });

    Expect(!(KeyOf(base, other) == reference), "the key ignores the registered shader");
    Expect(!(KeyOf(base, snapshot, 4) == reference), "the key ignores the code offset");
    Expect(!(KeyOf(base, snapshot, 0, 2) == reference), "the key ignores the device");

    DrawStageKey refused;
    auto barycentric = base.Request();
    barycentric.graphics->meshBarycentrics = ShaderRecompiler::MeshBarycentricLayout{};
    Expect(!BuildDrawStageKey(barycentric, snapshot, 0, 1, refused), "a request paired with a mesh barycentric layout was given a key");
    auto uncached = base.Request();
    uncached.useCache = false;
    Expect(!BuildDrawStageKey(uncached, snapshot, 0, 1, refused), "a request that bypasses the shader cache was given a key");
    std::puts("draw stage keys change with every input the stage build reads and with none it does not");
}

std::shared_ptr<DrawStageRecord> Record(std::vector<std::uint64_t> words, std::uint64_t hash) {
    auto record = std::make_shared<DrawStageRecord>();
    record->key.words = std::move(words);
    record->key.hash = hash;
    record->variant = std::make_shared<AgcDriver::DriverDetail::DispatchVariant>();
    return record;
}

void MemoStoresByWholeKey() {
    DrawStageMemo memo(4);
    bool admit = true;
    const auto first = Record({1u, 2u}, 7u);
    Expect(memo.Find(first->key, admit) == nullptr && !admit, "a key seen for the first time was admitted");
    Expect(memo.Find(first->key, admit) == nullptr && admit, "a key seen a second time was not admitted");
    Expect(memo.Insert(first) == 0, "an insert below capacity evicted a record");
    Expect(memo.Find(first->key, admit) == first, "an inserted record was not found");
    const auto colliding = Record({1u, 3u}, 7u);
    Expect(memo.Find(colliding->key, admit) == nullptr, "a key with the same hash but other words found another key's record");
    const auto replacement = Record({1u, 2u}, 7u);
    memo.Insert(replacement);
    Expect(memo.Size() == 1 && memo.Find(first->key, admit) == replacement, "a second insert of a key did not replace its record");
    for (std::uint64_t k = 0; k < 3; ++k) memo.Insert(Record({k + 10u}, k + 10u));
    Expect(memo.Find(first->key, admit) == replacement, "the touched record was lost");
    Expect(memo.Insert(Record({20u}, 20u)) == 1 && memo.Size() == 4, "a full memo did not evict exactly one record");
    Expect(memo.Find(Record({10u}, 10u)->key, admit) == nullptr, "the least recently used record was not the one evicted");
    Expect(memo.Find(first->key, admit) == replacement, "a recently used record was evicted");
    Expect(memo.Erase(replacement) && memo.Find(first->key, admit) == nullptr, "an erased record was still found");
    Expect(!memo.Erase(replacement), "a record was erased twice");
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> runs{{0x30000u, 0x30010u}, {0x10000u, 0x10040u}, {0x10100u, 0x10104u}, {0x18000u, 0x18010u}, {0x200000u, 0x200008u}};
    const auto spans = DrawStageSpans(runs);
    Expect(spans.size() == 3 && spans[0] == std::pair<std::uint64_t, std::size_t>{0x10000u, 0x8010u} && spans[1] == std::pair<std::uint64_t, std::size_t>{0x30000u, 0x10u} && spans[2] == std::pair<std::uint64_t, std::size_t>{0x200000u, 8u}, "stage spans do not merge runs within a tracker block of each other and keep distant runs apart");
    std::puts("the draw stage memo finds records by their whole key, admits keys seen twice and evicts the least recently used");
}

constexpr std::size_t BlockBytes = 65536;
constexpr std::size_t TableOffset = 0x300;
constexpr std::size_t SecondTableOffset = 0x400;
constexpr std::size_t MovedOffset = 0x800;
constexpr std::size_t ShiftOffset = 0x900;
constexpr std::size_t StillOffset = 0xa00;
constexpr std::size_t ColorBytes = 65536;
constexpr std::uint32_t RenderTargetTileMode = 0x1b;
constexpr std::size_t PixelCopies = 16;

alignas(256) constexpr std::array<std::uint32_t, 40> VertexCode{
    0x7e000d05u, 0x080200f4u, 0x10020300u, 0x100202f4u, 0x060202f3u, 0x060400f3u, 0x10040500u, 0x060404f3u,
    0x7e060280u, 0x7e0802f2u,
    0xf4080104u, 0xfa000000u, 0xf4080204u, 0xfa000010u, 0xf4080304u, 0xfa000020u,
    0xbf8cc07fu,
    0xe00c2000u, 0x80010a05u, 0xe00c2000u, 0x80020e05u, 0xe00c2000u, 0x80031205u,
    0xbf8c3f70u,
    0x0602030au, 0x0604050bu, 0x0606070cu, 0x0608090du,
    0x0602030eu, 0x0604050fu, 0x06060710u, 0x06080911u,
    0x06020312u, 0x06040513u, 0x06060714u, 0x06080915u,
    0xf80008cfu, 0x04030201u,
    0xbf810000u, 0xbf810000u,
};
alignas(256) std::array<std::uint32_t, 40> SecondVertexCode = VertexCode;

alignas(256) constexpr std::array<std::uint32_t, 11> PixelRead{0x7e1a0280u, 0xdc308000u, 0x0c02000du, 0xbf8c3f70u, 0x7e10030cu, 0x7e120280u, 0x7e140280u, 0x7e160280u, 0xf800180fu, 0x0b0a0908u, 0xbf810000u};

struct alignas(256) PixelProgram {
    std::array<std::uint32_t, 64> code{};
};

std::array<PixelProgram, PixelCopies> Pixels{};

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header SecondVertexHeader{};
alignas(8) std::array<Header, PixelCopies> PixelHeaders{};

void Register(Header& header, const std::uint32_t* code, std::size_t bytes, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(bytes);
    header.shader.code = code;
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

class GuestBlock {
public:
    GuestBlock() {
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
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
        }
#endif
        Require(block != nullptr, "draw stage memo: cannot allocate a guest block");
        std::memset(block, 0, BlockBytes);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint64_t At(std::size_t offset) const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block + offset)); }
    std::uint8_t* Bytes(std::size_t offset) const { return block + offset; }

private:
    std::uint8_t* block = nullptr;
};

std::array<std::uint32_t, 4> VertexDescriptor(std::uint64_t address) {
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (16u << 16u), 3u, 0x0104dfacu};
}

void WriteVertices(const GuestBlock& block, std::size_t offset, float x) {
    for (std::size_t vertex = 0; vertex < 3; ++vertex) {
        const std::array<float, 4> value{x, 0.0f, 0.0f, 0.0f};
        std::memcpy(block.Bytes(offset + vertex * 16u), value.data(), sizeof(value));
    }
}

void WriteDescriptor(const GuestBlock& block, std::size_t table, std::size_t slot, std::array<std::uint32_t, 4> descriptor) {
    std::memcpy(block.Bytes(table + slot * 16u), descriptor.data(), sizeof(descriptor));
}

void WriteTable(const GuestBlock& tables, std::size_t table, const GuestBlock& vertices, bool visible) {
    WriteDescriptor(tables, table, 0, visible ? VertexDescriptor(vertices.At(MovedOffset)) : std::array<std::uint32_t, 4>{});
    WriteDescriptor(tables, table, 1, VertexDescriptor(vertices.At(ShiftOffset)));
    WriteDescriptor(tables, table, 2, VertexDescriptor(vertices.At(StillOffset)));
}

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

struct Scene {
    GuestBlock tables;
    GuestBlock vertices;
    GuestBlock data;
    GuestBlock target;
    std::size_t nextPixel = 0;
};

unsigned Draw(Scene& scene, const std::uint32_t* vertexCode, std::size_t table) {
    Expect(scene.nextPixel < PixelCopies, "the test ran out of pixel shader copies");
    std::memset(scene.target.Bytes(0), 0, ColorBytes);
    const auto color = scene.target.At(0);
    const auto vertex = reinterpret_cast<std::uintptr_t>(vertexCode);
    const auto pixel = reinterpret_cast<std::uintptr_t>(Pixels[scene.nextPixel++].code.data());
    const auto tableAddress = scene.tables.At(table);
    const auto pointer = scene.data.At(0);
    const float scale = 32.0f;
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x2000},
        {0x1b6, 0}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, (63u << 14u) | 3u},
        {0x3b8, 0x9000000u | (RenderTargetTileMode << 14u)}, {0x1e0, 0},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040},
        {0x318, static_cast<std::uint32_t>(color >> 8u)}, {0x390, static_cast<std::uint32_t>(color >> 40u)},
        {0x10f, std::bit_cast<std::uint32_t>(scale)}, {0x110, std::bit_cast<std::uint32_t>(scale)},
        {0x111, std::bit_cast<std::uint32_t>(-2.0f)}, {0x112, std::bit_cast<std::uint32_t>(2.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
        {0x2fe, 0}, {0x302, 0}, {0x306, 0}, {0x30a, 0},
    };
    std::vector<std::uint32_t> commands;
    Append(commands, 0x79, {0x242, 4});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : std::initializer_list<std::pair<std::uint32_t, std::uint32_t>>{{0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, 2u << 1u}, {0x8c, static_cast<std::uint32_t>(tableAddress)}, {0x8d, static_cast<std::uint32_t>(tableAddress >> 32u)}, {0x00b, 8u << 1u}, {0x00c, 0}, {0x00d, 0}}) Append(commands, 0x76, {offset, value});
    Append(commands, 0x76, {0x008, static_cast<std::uint32_t>(pixel >> 8u), static_cast<std::uint32_t>(pixel >> 40u)});
    Append(commands, 0x76, {0x00e, static_cast<std::uint32_t>(pointer), static_cast<std::uint32_t>(pointer >> 32u)});
    Append(commands, 0x2d, {3, 2});
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "draw stage memo: the draw submission failed");
    AgcDriverWaitIdle_nid_postfix();
    AgcDriver::GuestMemory::FlushGpuWrites(static_cast<std::uintptr_t>(scene.target.At(0)), ColorBytes);
    unsigned brightest = 0;
    for (std::size_t i = 0; i < ColorBytes; ++i) brightest = std::max(brightest, static_cast<unsigned>(scene.target.Bytes(0)[i]));
    return brightest;
}

struct Step {
    unsigned brightest = 0;
    DrawStageMemoCounts counts;
};

DrawStageMemoCounts Since(const DrawStageMemoCounts& before, const DrawStageMemoCounts& after) {
    return {after.lookups - before.lookups, after.refused - before.refused, after.absent - before.absent, after.proved - before.proved, after.compared - before.compared, after.stale - before.stale, after.inserts - before.inserts, after.evictions - before.evictions, after.skipped - before.skipped, after.verified - before.verified};
}

Step Measure(Scene& scene, const std::uint32_t* vertexCode, std::size_t table, const char* label) {
    auto& driver = AgcDriver::DriverDetail::Driver::Get();
    const auto before = driver.StageMemoCounts();
    Step step;
    step.brightest = Draw(scene, vertexCode, table);
    step.counts = Since(before, driver.StageMemoCounts());
    const auto& c = step.counts;
    std::printf("%s: brightest %u; memo lookups %llu, absent %llu, proved %llu, compared %llu, stale %llu, inserts %llu, verified %llu\n", label, step.brightest, static_cast<unsigned long long>(c.lookups), static_cast<unsigned long long>(c.absent), static_cast<unsigned long long>(c.proved), static_cast<unsigned long long>(c.compared), static_cast<unsigned long long>(c.stale), static_cast<unsigned long long>(c.inserts), static_cast<unsigned long long>(c.verified));
    return step;
}

std::uint64_t Hits(const Step& step) {
    return step.counts.proved + step.counts.compared;
}

void DrawsReuseAndInvalidateTheirStages() {
    const bool memo = std::getenv("APS5_NO_DRAW_STAGE_MEMO") == nullptr;
    const bool verify = std::getenv("APS5_VERIFY_DRAW_STAGE_MEMO") != nullptr;
    Register(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
    Register(SecondVertexHeader, SecondVertexCode.data(), sizeof(SecondVertexCode), 2);
    for (std::size_t k = 0; k < PixelCopies; ++k) {
        std::copy(PixelRead.begin(), PixelRead.end(), Pixels[k].code.begin());
        Register(PixelHeaders[k], Pixels[k].code.data(), sizeof(PixelRead), 1);
    }
    Scene scene;
    WriteVertices(scene.vertices, MovedOffset, -8.0f);
    WriteVertices(scene.vertices, ShiftOffset, 8.0f);
    WriteVertices(scene.vertices, StillOffset, 0.0f);
    WriteTable(scene.tables, TableOffset, scene.vertices, true);
    WriteTable(scene.tables, SecondTableOffset, scene.vertices, true);
    const float one = 1.0f;
    std::memcpy(scene.data.Bytes(0), &one, sizeof(one));

    const auto first = Measure(scene, VertexCode.data(), TableOffset, "first draw of the vertex stage");

    const auto second = Measure(scene, VertexCode.data(), TableOffset, "second draw, another pixel shader");
    const auto third = Measure(scene, VertexCode.data(), TableOffset, "third draw, another pixel shader");
    const bool stampable = AgcDriver::GuestMemory::Watched(scene.tables.At(TableOffset), 0x30) && AgcDriver::GuestMemory::Watched(scene.vertices.At(MovedOffset), StillOffset + 0x30 - MovedOffset);
    std::printf("the stage's descriptor table and vertex data %s write-watched after the draws, so an untouched stage is %s\n", stampable ? "stay" : "are not", stampable ? "proved by write stamps" : "compared");
    const auto fourth = Measure(scene, VertexCode.data(), TableOffset, "fourth draw, nothing written");
    for (const auto* step : {&first, &second, &third, &fourth}) Expect(step->brightest == 255u, "a draw with a valid descriptor table did not draw");
    if (memo) {
        Expect(Hits(first) == 0 && first.counts.inserts == 0, "a stage seen once was memoized or served");
        Expect(Hits(second) == 0 && second.counts.inserts == 1, "a stage seen twice was not memoized exactly once");
        Expect(Hits(third) == 1 && third.counts.stale == 0, "the third draw of an unchanged stage was not served from the memo");
        Expect(Hits(fourth) == 1 && fourth.counts.stale == 0, "the fourth draw of an unchanged stage was not served from the memo");
        if (stampable) Expect(fourth.counts.proved == 1, "a stage whose memory no write touched was compared again instead of proved by its write stamps");
        else Expect(fourth.counts.compared == 1, "a stage whose memory is not write-watched was not compared");
        if (verify) Expect(third.counts.verified == 1 && fourth.counts.verified == 1, "a memo hit was not verified against its full build");
    } else {
        Expect(first.counts.lookups + second.counts.lookups + third.counts.lookups + fourth.counts.lookups == 0, "the memo was consulted while switched off");
    }

    WriteTable(scene.tables, TableOffset, scene.vertices, false);
    const auto cleared = Measure(scene, VertexCode.data(), TableOffset, "after the guest cleared a descriptor the stage read");
    const auto clearedAgain = Measure(scene, VertexCode.data(), TableOffset, "the cleared table drawn again");
    Expect(cleared.brightest != 255u, "the draw after the guest cleared a descriptor still bound the memoized descriptor");
    Expect(clearedAgain.brightest != 255u, "the second draw after the guest cleared a descriptor bound the old descriptor");
    if (memo) {
        Expect(cleared.counts.stale == 1 && Hits(cleared) == 0, "the memo did not drop the stage whose descriptor memory changed");
        Expect(cleared.counts.inserts == 1, "the rebuilt stage was not memoized again");
        Expect(Hits(clearedAgain) == 1, "the rebuilt stage was not served on its next draw");
    }

    WriteTable(scene.tables, TableOffset, scene.vertices, true);
    const auto restored = Measure(scene, VertexCode.data(), TableOffset, "after the guest restored the descriptor");
    Expect(restored.brightest == 255u, "the draw after the guest restored the descriptor bound the cleared descriptor");
    if (memo) Expect(restored.counts.stale == 1 && Hits(restored) == 0, "the memo served a stage across a guest write that restored a descriptor");

    const auto settled = Measure(scene, VertexCode.data(), TableOffset, "the restored table drawn again");
    WriteTable(scene.tables, TableOffset, scene.vertices, true);
    const auto rewritten = Measure(scene, VertexCode.data(), TableOffset, "after the guest wrote the same descriptors again");
    Expect(settled.brightest == 255u && rewritten.brightest == 255u, "an unchanged or identically rewritten table did not draw");
    if (memo) {
        Expect(Hits(settled) == 1, "the restored stage was not served on its next draw");
        Expect(rewritten.counts.stale == 0 && Hits(rewritten) == 1, "a stage whose memory was rewritten with the same bytes was not served");
        Expect(rewritten.counts.compared == 1 && rewritten.counts.proved == 0, "a stage whose memory was written was proved by write stamps instead of compared");
    }

    const auto otherUserData = Measure(scene, VertexCode.data(), SecondTableOffset, "the same table contents through another table address in user data");
    Expect(otherUserData.brightest == 255u, "the draw through the second table did not draw");
    if (memo) Expect(Hits(otherUserData) == 0 && otherUserData.counts.stale == 0, "a stage with other user data was served another user data's build");

    const auto otherProgram = Measure(scene, SecondVertexCode.data(), TableOffset, "the same vertex code registered at another address");
    Expect(otherProgram.brightest == 255u, "the draw with the second vertex program did not draw");
    if (memo) Expect(Hits(otherProgram) == 0 && otherProgram.counts.stale == 0, "a stage of another registered program was served the first program's build");
    std::puts("draw stages repeat from the memo, and a guest write under a memoized stage rebuilds it");
}

void ProofsFollowWriteStamps() {
    GuestBlock block;
    const auto run = std::pair<std::uint64_t, std::uint64_t>{block.At(0x100), block.At(0x140)};
    for (std::uint32_t k = 0; k < 16; ++k) std::memcpy(block.Bytes(0x100 + k * 4u), &k, sizeof(k));
    auto record = std::make_shared<DrawStageRecord>();
    record->variant = std::make_shared<AgcDriver::DriverDetail::DispatchVariant>();
    record->variant->compiled = std::make_shared<ShaderRecompiler::RecompileResult>();
    record->variant->runs.push_back(run);
    record->variant->words.resize(16);
    std::memcpy(record->variant->words.data(), block.Bytes(0x100), 64);
    record->spans = DrawStageSpans(record->variant->runs);
    if (!AgcDriver::GuestMemory::Watched(run.first, 64)) {
        std::puts("guest memory is not write-watched here; the write stamp proof is not exercised");
        Expect(DriverTestAccess::Prove(*record) == DrawStageProof::Compared, "an unchanged stage outside watched memory was not served by a compare");
        return;
    }
    Expect(!DriverTestAccess::Unchanged(*record), "a stage never stamped was proved current");
    Expect(DriverTestAccess::Prove(*record) == DrawStageProof::Compared, "a stage never stamped was not compared");
    Expect(record->Stamp().generation != 0, "a compared stage was not stamped");
    Expect(DriverTestAccess::Unchanged(*record) && DriverTestAccess::Prove(*record) == DrawStageProof::Proved, "a stamped stage whose memory no write touched was not proved");
    const std::uint32_t changed = 99u;
    std::memcpy(block.Bytes(0x104), &changed, sizeof(changed));
    Expect(!DriverTestAccess::Unchanged(*record), "a stamped stage was proved current after a guest write changed a word it read");
    Expect(DriverTestAccess::Prove(*record) == DrawStageProof::Stale, "a stage whose word changed was served");
    const std::uint32_t original = 1u;
    std::memcpy(block.Bytes(0x104), &original, sizeof(original));
    Expect(DriverTestAccess::Prove(*record) == DrawStageProof::Compared, "a stage whose word was restored was not served by a compare");
    Expect(DriverTestAccess::Prove(*record) == DrawStageProof::Proved, "a restamped stage was not proved");
    std::memcpy(block.Bytes(0x200), &changed, sizeof(changed));
    Expect(!DriverTestAccess::Unchanged(*record), "a write on the page of the stage's words left the proof standing");
    Expect(DriverTestAccess::Prove(*record) == DrawStageProof::Compared, "a stage whose words were unchanged beside a write on their page was not served by a compare");
    Expect(DriverTestAccess::Prove(*record) == DrawStageProof::Proved, "a stage restamped after a write beside its words was not proved");
    std::memcpy(block.Bytes(0x8000), &changed, sizeof(changed));
    Expect(DriverTestAccess::Prove(*record) == DrawStageProof::Proved, "a write on a page the stage never read broke its proof");
    std::puts("draw stage proofs hold until a guest write lands in the stage's memory, and a compare restamps them");
}

}

int main() {
    try {
        KeysFollowEveryInput();
        MemoStoresByWholeKey();
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        DrawsReuseAndInvalidateTheirStages();
        ProofsFollowWriteStamps();
        AgcDriverShutdown_nid_postfix();
        std::puts("draw stage memo: hits, misses and invalidations follow the stage inputs");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
