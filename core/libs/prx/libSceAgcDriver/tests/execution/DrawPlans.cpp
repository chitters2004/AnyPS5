#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <cstring>
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::DriverDetail::DrawPlanCount;
using AgcDriver::DriverDetail::DrawPlanCounts;
using AgcDriver::DriverDetail::DrawPlanCountKinds;

alignas(256) constexpr std::array<std::uint32_t, 16> TriangleVertexCode{
    0x7e000d05u, 0x080200f4u, 0x10020300u, 0x100202f4u, 0x060202f3u, 0x060400f3u, 0x10040500u, 0x060404f3u,
    0x7e060280u, 0x7e0802f2u, 0xf80008cfu, 0x04030201u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 8> PixelCode{0x7e000200u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t List = 4;
constexpr std::uint32_t Strip = 6;

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header PixelHeader{};

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
        Require(block != nullptr, "draw plans: cannot allocate a guest block");
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

struct Look {
    float colour = 1.0f;
    float scale = 32.0f;
    std::uint32_t primitive = List;
    bool blended = false;
    float blendConstant = 1.0f;
};

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

void Draw(const GuestBlock& target, const Look& look) {
    const auto color = target.At(0);
    const auto vertex = reinterpret_cast<std::uintptr_t>(TriangleVertexCode.data());
    const auto pixel = reinterpret_cast<std::uintptr_t>(PixelCode.data());
    constexpr std::uint32_t ConstantColour = 13u;
    const std::uint32_t blendControl = look.blended ? (1u << 30u) | ConstantColour : 0u;
    const auto constant = std::bit_cast<std::uint32_t>(look.blendConstant);
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
        {0x3b8, 0x9000000u | (0x1bu << 14u)}, {0x1e0, blendControl},
        {0x105, constant}, {0x106, constant}, {0x107, constant}, {0x108, constant},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040},
        {0x318, static_cast<std::uint32_t>(color >> 8u)}, {0x390, static_cast<std::uint32_t>(color >> 40u)},
        {0x10f, std::bit_cast<std::uint32_t>(look.scale)}, {0x110, std::bit_cast<std::uint32_t>(look.scale)},
        {0x111, std::bit_cast<std::uint32_t>(-2.0f)}, {0x112, std::bit_cast<std::uint32_t>(2.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
        {0x2fe, 0}, {0x302, 0}, {0x306, 0}, {0x30a, 0},
    };
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> shader{
        {0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, 0}, {0x8c, 0},
        {0x008, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009, static_cast<std::uint32_t>(pixel >> 40u)}, {0x00b, 1u << 1u},
        {0x00c, std::bit_cast<std::uint32_t>(look.colour)},
    };
    std::vector<std::uint32_t> commands;
    Append(commands, 0x79, {0x242, look.primitive});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : shader) Append(commands, 0x76, {offset, value});
    Append(commands, 0x2d, {3, 2});
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "draw plans: the draw submission failed");
    AgcDriverWaitIdle_nid_postfix();
}

void Expect(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error("draw plans: " + what);
}

bool PlansOn() {
    return AgcDriver::Graphics::DrawPlans();
}

DrawPlanCounts Counts() {
    return AgcDriver::DriverDetail::Driver::Get().DrawPlanShadowCounts();
}

DrawPlanCounts Since(const DrawPlanCounts& before) {
    const auto after = Counts();
    DrawPlanCounts since;
    for (std::size_t kind = 0; kind < DrawPlanCountKinds; ++kind) since.values[kind] = after.values[kind] - before.values[kind];
    return since;
}

DrawPlanCounts Measure(const GuestBlock& target, const Look& look) {
    const auto before = Counts();
    Draw(target, look);
    return Since(before);
}

std::vector<std::uint8_t> Pixels(const GuestBlock& target) {
    static_cast<void>(AgcDriver::Graphics::StorageTexture::FlushPending(target.At(0), BlockBytes, nullptr, "draw plans test"));
    AgcDriver::GuestMemory::FlushGpuWrites(target.At(0), BlockBytes);
    return {target.Bytes(0), target.Bytes(0) + BlockBytes};
}

bool Drawn(const std::vector<std::uint8_t>& pixels) {
    return std::any_of(pixels.begin(), pixels.end(), [](std::uint8_t byte) { return byte != 0; });
}

class FullPath {
public:
    FullPath() { AgcDriver::Graphics::SetDrawPlansForTest(0); }
    FullPath(const FullPath&) = delete;
    FullPath& operator=(const FullPath&) = delete;
    ~FullPath() { AgcDriver::Graphics::SetDrawPlansForTest(-1); }
};

void Report(const char* label, const DrawPlanCounts& counts) {
    std::printf("%s: hits %llu, absent %llu, unservable %llu, instance %llu, target gone %llu, template gone %llu, objects gone %llu, proof %llu, not recordable %llu, verified %llu\n", label,
        static_cast<unsigned long long>(counts[DrawPlanCount::Hits]), static_cast<unsigned long long>(counts[DrawPlanCount::MissAbsent]), static_cast<unsigned long long>(counts[DrawPlanCount::MissUnservable]),
        static_cast<unsigned long long>(counts[DrawPlanCount::MissInstance]), static_cast<unsigned long long>(counts[DrawPlanCount::MissTargetGone]), static_cast<unsigned long long>(counts[DrawPlanCount::MissTemplateGone]),
        static_cast<unsigned long long>(counts[DrawPlanCount::MissObjectsGone]), static_cast<unsigned long long>(counts[DrawPlanCount::MissProof]), static_cast<unsigned long long>(counts[DrawPlanCount::MissNotRecordable]),
        static_cast<unsigned long long>(counts[DrawPlanCount::Verified]));
}

void ExpectNoHitsWhileOff(const DrawPlanCounts& counts) {
    if (!PlansOn()) Expect(counts[DrawPlanCount::Hits] == 0 && counts[DrawPlanCount::MissAbsent] == 0, "the plan path ran while switched off");
}

void PlanKeysCoverThePipelineKey() {
    namespace Graphics = AgcDriver::Graphics;
    Graphics::Context context{};
    Graphics::State base{};
    base.hasColorTarget = true;
    base.colors.resize(1);
    base.blends.resize(1);
    base.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    base.negativeOneToOne = false;
    base.cullMode = VK_CULL_MODE_NONE;
    base.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    base.blendConstants = {0.25f, 0.5f, 0.75f, 1.0f};
    base.depth.emplace();
    base.depthBias = true;
    base.stencilTest = true;
    base.stencilFront.reference = 3u;
    base.stencilBack.reference = 5u;
    const Graphics::VertexInputLayout layout{};
    const Graphics::PipelineSetLayouts sets{};
    const auto pipelineKey = [&](const Graphics::State& state) {
        std::vector<std::byte> key;
        Graphics::BuildPipelineStoreKey(key, context, state, layout, sets, {}, VK_IMAGE_LAYOUT_GENERAL);
        return key;
    };
    const auto planWords = [](const Graphics::State& state) {
        std::vector<std::uint64_t> words;
        AgcDriver::DriverDetail::AppendDrawPlanStateWords(state, words);
        return words;
    };
    const std::vector<std::pair<const char*, std::function<void(Graphics::State&)>>> changes{
        {"blend constant red", [](Graphics::State& state) { state.blendConstants[0] = 0.5f; }},
        {"blend constant green", [](Graphics::State& state) { state.blendConstants[1] = 0.0f; }},
        {"blend constant blue", [](Graphics::State& state) { state.blendConstants[2] = 0.0f; }},
        {"blend constant alpha", [](Graphics::State& state) { state.blendConstants[3] = 0.0f; }},
        {"front stencil reference", [](Graphics::State& state) { state.stencilFront.reference = 4u; }},
        {"back stencil reference", [](Graphics::State& state) { state.stencilBack.reference = 6u; }},
        {"depth bias clamp", [](Graphics::State& state) { state.depthBiasClamp = 0.5f; }},
        {"front stencil compare mask", [](Graphics::State& state) { state.stencilFront.compareMask = 0x0fu; }},
        {"back stencil write mask", [](Graphics::State& state) { state.stencilBack.writeMask = 0x0fu; }},
        {"depth compare", [](Graphics::State& state) { state.depthCompare = VK_COMPARE_OP_LESS; }},
        {"color format", [](Graphics::State& state) { state.colors[0].format = VK_FORMAT_R8G8B8A8_UNORM; }},
        {"color component mapping", [](Graphics::State& state) { state.colors[0].componentMapping = 0xc6u; }},
        {"color partial write", [](Graphics::State& state) { state.colors[0].partialWriteMask = 1u; }},
        {"blend enable", [](Graphics::State& state) { state.blends[0].blendEnable = VK_TRUE; }},
        {"blend factor", [](Graphics::State& state) { state.blends[0].srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR; }},
        {"color rop", [](Graphics::State& state) { state.colorRop = 0x00u; }},
        {"samples", [](Graphics::State& state) { state.samples = 4u; }},
        {"sample mask", [](Graphics::State& state) { state.quadSampleMask = {1u, 1u, 1u, 1u}; }},
        {"topology", [](Graphics::State& state) { state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; }},
        {"cull mode", [](Graphics::State& state) { state.cullMode = VK_CULL_MODE_BACK_BIT; }},
        {"front face", [](Graphics::State& state) { state.frontFace = VK_FRONT_FACE_CLOCKWISE; }},
        {"polygon mode", [](Graphics::State& state) { state.polygonMode = VK_POLYGON_MODE_LINE; }},
        {"line width", [](Graphics::State& state) { state.lineWidth = 2.0f; }},
        {"primitive restart", [](Graphics::State& state) { state.primitiveRestart = true; }},
        {"depth clip range", [](Graphics::State& state) { state.negativeOneToOne = true; }},
        {"rasterizer discard", [](Graphics::State& state) { state.rasterizerDiscard = true; }},
        {"mesh layer base", [](Graphics::State& state) { state.meshLayerBase = 1u; }},
        {"depth format", [](Graphics::State& state) { state.depth->format = VK_FORMAT_D32_SFLOAT; }},
        {"no depth", [](Graphics::State& state) { state.depth.reset(); }},
    };
    std::size_t covered = 0;
    for (const auto& [name, change] : changes) {
        auto changed = base;
        change(changed);
        if (pipelineKey(changed) == pipelineKey(base)) continue;
        ++covered;
        Expect(planWords(changed) != planWords(base), std::string("the plan key ignores a change of ") + name + " that changes the pipeline key");
    }
    Expect(covered >= 7, "the pipeline key changed for too few of the changes to test the plan key against it");
    std::printf("draw plans: the plan key follows %zu state changes that change the pipeline key\n", covered);
}

void PlansFollowPipelineState() {
    GuestBlock planned;
    GuestBlock full;
    const Look first{.colour = 1.0f, .blended = true, .blendConstant = 1.0f};
    const Look second{.colour = 1.0f, .blended = true, .blendConstant = 0.25f};
    Draw(planned, first);
    Draw(planned, first);
    const auto counts = Measure(planned, second);
    {
        const FullPath fullPath;
        Draw(full, first);
        Draw(full, first);
        Draw(full, second);
    }
    Report("I6 another blend constant", counts);
    ExpectNoHitsWhileOff(counts);
    Expect(counts[DrawPlanCount::Hits] == 0, "a draw with another blend constant was served the plan of the first");
    const auto plannedPixels = Pixels(planned);
    Expect(Drawn(plannedPixels), "the blended draw wrote nothing");
    Expect(plannedPixels == Pixels(full), "a draw with another blend constant differs from the full path's");
    GuestBlock reference;
    {
        const FullPath fullPath;
        Draw(reference, first);
    }
    Expect(Pixels(reference) != plannedPixels, "the blend constant does not change the output, so the test cannot tell plans apart");
    std::puts("draw plans: I6, a pipeline-shaping value outside the old plan key separates plans");
}

void PlansBindTheirOwnTargets() {
    GuestBlock firstPlanned;
    GuestBlock secondPlanned;
    GuestBlock firstFull;
    GuestBlock secondFull;
    const Look red{.colour = 1.0f};
    const Look green{.colour = 0.5f};
    Draw(firstPlanned, red);
    Draw(firstPlanned, red);
    const auto counts = Measure(secondPlanned, green);
    {
        const FullPath fullPath;
        Draw(firstFull, red);
        Draw(firstFull, red);
        Draw(secondFull, green);
    }
    Report("I7 another render target", counts);
    ExpectNoHitsWhileOff(counts);
    Expect(counts[DrawPlanCount::Hits] == 0, "a draw into another render target was served the plan's target");
    if (PlansOn()) Expect(counts[DrawPlanCount::MissInstance] == 1, "a draw into another render target was not refused as another instance");
    Expect(Pixels(firstPlanned) == Pixels(firstFull), "the plan's own target differs from the full path's");
    Expect(Pixels(secondPlanned) == Pixels(secondFull), "the other target differs from the full path's");
    std::puts("draw plans: I7, a plan binds only the target the full path would bind");
}

void PlanRowsFollowTheDraw() {
    GuestBlock planned;
    GuestBlock full;
    const Look red{.colour = 1.0f};
    const Look smallGreen{.colour = 0.5f, .scale = 16.0f};
    Draw(planned, red);
    Draw(planned, red);
    const auto counts = Measure(planned, smallGreen);
    {
        const FullPath fullPath;
        Draw(full, red);
        Draw(full, red);
        Draw(full, smallGreen);
    }
    Report("I8 another colour and viewport", counts);
    ExpectNoHitsWhileOff(counts);
    if (PlansOn()) Expect(counts[DrawPlanCount::Hits] == 1, "a draw that differs from its plan's draw only in its user data and viewport was not served by the plan");
    Expect(Pixels(planned) == Pixels(full), "a plan hit with another user data value and viewport differs from the full path's draw");
    std::puts("draw plans: I8, the user data and the viewport of a plan hit are the draw's own");
}

void PlanHitsSeeGuestWrites() {
    GuestBlock planned;
    GuestBlock full;
    const Look red{.colour = 1.0f};
    const Look smallGreen{.colour = 0.5f, .scale = 16.0f};
    const auto run = [&](const GuestBlock& target) {
        Draw(target, red);
        Draw(target, red);
        static_cast<void>(Pixels(target));
        std::memset(target.Bytes(0), 0x5a, BlockBytes / 2);
        const auto before = Counts();
        Draw(target, smallGreen);
        return Since(before);
    };
    const auto counts = run(planned);
    {
        const FullPath fullPath;
        static_cast<void>(run(full));
    }
    Report("I9 a guest write before a plan hit", counts);
    ExpectNoHitsWhileOff(counts);
    if (PlansOn()) Expect(counts[DrawPlanCount::Hits] == 1, "a draw after a guest write to its target was not served by its plan");
    const auto plannedPixels = Pixels(planned);
    Expect(std::count(plannedPixels.begin(), plannedPixels.begin() + BlockBytes / 2, std::uint8_t{0x5a}) != 0, "the guest write did not survive outside the plan hit's triangle");
    Expect(plannedPixels == Pixels(full), "a plan hit after a guest write to its target differs from the full path's draw");
    std::puts("draw plans: I9, a plan hit reads its target as the guest left it and its writes reach guest memory");
}

void PlannedSceneMatchesFullPath() {
    const std::vector<std::pair<std::size_t, Look>> scene{
        {0, {.colour = 1.0f}}, {0, {.colour = 1.0f}}, {0, {.colour = 0.5f}}, {1, {.colour = 0.25f}}, {1, {.colour = 0.75f, .scale = 24.0f}},
        {0, {.colour = 0.5f, .primitive = Strip}}, {0, {.colour = 0.125f, .primitive = Strip}}, {2, {.colour = 1.0f, .blended = true, .blendConstant = 0.5f}},
        {2, {.colour = 0.5f, .blended = true, .blendConstant = 0.5f}}, {2, {.colour = 0.5f, .scale = 8.0f}}, {1, {.colour = 0.0625f}}, {0, {.colour = 1.0f, .scale = 12.0f}},
    };
    const auto run = [&](std::array<GuestBlock, 3>& targets) {
        const auto before = Counts();
        for (const auto& [target, look] : scene) Draw(targets[target], look);
        return Since(before);
    };
    std::array<GuestBlock, 3> planned;
    std::array<GuestBlock, 3> full;
    const auto counts = run(planned);
    {
        const FullPath fullPath;
        static_cast<void>(run(full));
    }
    Report("I10 a scene", counts);
    ExpectNoHitsWhileOff(counts);
    if (PlansOn()) Expect(counts[DrawPlanCount::Hits] >= 4, "the scene's repeated plans were not served");
    if (counts[DrawPlanCount::Hits] != 0 && counts[DrawPlanCount::FullDraws] != 0) std::printf("I10 committer time (APS5_PROFILE_DRAW): %.2f us per plan hit, %.2f us per full-path draw with a plan key\n", static_cast<double>(counts[DrawPlanCount::HitNanoseconds]) / static_cast<double>(counts[DrawPlanCount::Hits]) / 1000.0, static_cast<double>(counts[DrawPlanCount::FullNanoseconds]) / static_cast<double>(counts[DrawPlanCount::FullDraws]) / 1000.0);
    for (std::size_t index = 0; index < planned.size(); ++index) Expect(Pixels(planned[index]) == Pixels(full[index]), "target " + std::to_string(index) + " of the planned scene differs from the full path's");
    std::puts("draw plans: I10, a scene recorded with plans renders as the full path renders it");
}

}

int main() {
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        PlanKeysCoverThePipelineKey();
        Register(VertexHeader, TriangleVertexCode.data(), sizeof(TriangleVertexCode), 2);
        Register(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
        PlansFollowPipelineState();
        PlansBindTheirOwnTargets();
        PlanRowsFollowTheDraw();
        PlanHitsSeeGuestWrites();
        PlannedSceneMatchesFullPath();
        AgcDriverShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
