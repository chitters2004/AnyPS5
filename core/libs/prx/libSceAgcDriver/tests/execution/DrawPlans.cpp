#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/CommitPhases.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libSceAgcDriver/Execution/include/AggregateFields.hpp"
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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace AggregateFields = AgcDriver::AggregateFields;

using AgcDriver::Graphics::Require;
using AgcDriver::DriverDetail::DrawPlanCount;
using AgcDriver::DriverDetail::DrawPlanCounts;
using AgcDriver::DriverDetail::DrawPlanCountKinds;
constexpr std::size_t PlanSignatureSetLimitForTest = AgcDriver::DriverDetail::PlanSignatureSetLimit;
constexpr std::size_t PlanLookupDistanceBucketsForTest = AgcDriver::DriverDetail::PlanLookupDistanceBuckets;

alignas(256) constexpr std::array<std::uint32_t, 16> TriangleVertexCode{
    0x7e000d05u, 0x080200f4u, 0x10020300u, 0x100202f4u, 0x060202f3u, 0x060400f3u, 0x10040500u, 0x060404f3u,
    0x7e060280u, 0x7e0802f2u, 0xf80008cfu, 0x04030201u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 8> LoadingPixelCode{0xe0300000u, 0x80000000u, 0xbf8c3f70u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u};
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
alignas(8) Header LoadingPixelHeader{};

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
    const GuestBlock* colourBuffer = nullptr;
    std::uint32_t colourStride = 0;
    float colour = 1.0f;
    float scale = 32.0f;
    std::uint32_t primitive = List;
    bool blended = false;
    float blendConstant = 1.0f;
    const GuestBlock* indices = nullptr;
    const GuestBlock* depth = nullptr;
    const GuestBlock* stencil = nullptr;
    std::uint32_t depthControl = 0;
    float depthOffset = 0.0f;
};

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

void Draw(const GuestBlock& target, const Look& look) {
    const auto color = target.At(0);
    const auto vertex = reinterpret_cast<std::uintptr_t>(TriangleVertexCode.data());
    const auto pixel = reinterpret_cast<std::uintptr_t>(look.colourBuffer != nullptr ? LoadingPixelCode.data() : PixelCode.data());
    const auto loaded = look.colourBuffer != nullptr ? look.colourBuffer->At(0) : 0u;
    constexpr std::uint32_t ConstantColour = 13u;
    const std::uint32_t blendControl = look.blended ? (1u << 30u) | ConstantColour : 0u;
    const auto constant = std::bit_cast<std::uint32_t>(look.blendConstant);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
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
    if (look.depth != nullptr) {
        const auto depth = look.depth->At(0);
        for (const auto& entry : std::initializer_list<std::pair<std::uint32_t, std::uint32_t>>{
                 {0x200, look.depthControl}, {0x114, std::bit_cast<std::uint32_t>(look.depthOffset)},
                 {0x000, 0}, {0x002, 0}, {0x007, (3u << 16u) | 63u},
                 {0x008, 0}, {0x009, std::bit_cast<std::uint32_t>(1.0f)}, {0x00a, 0}, {0x00b, 0},
                 {0x010, 3u | (AgcDriver::Graphics::XorSwizzleMode(AgcDriver::Graphics::TextureTileMode::kZ64KBX) << 4u)}, {0x011, 0},
                 {0x012, static_cast<std::uint32_t>(depth >> 8u)}, {0x014, static_cast<std::uint32_t>(depth >> 8u)},
                 {0x01a, static_cast<std::uint32_t>(depth >> 40u)}, {0x01c, static_cast<std::uint32_t>(depth >> 40u)}}) context.push_back(entry);
        if (look.stencil != nullptr) {
            const auto stencil = look.stencil->At(0);
            for (const auto& entry : std::initializer_list<std::pair<std::uint32_t, std::uint32_t>>{
                     {0x011, 1u | (AgcDriver::Graphics::XorSwizzleMode(AgcDriver::Graphics::TextureTileMode::kZ64KBX) << 4u)},
                     {0x013, static_cast<std::uint32_t>(stencil >> 8u)}, {0x015, static_cast<std::uint32_t>(stencil >> 8u)},
                     {0x01b, static_cast<std::uint32_t>(stencil >> 40u)}, {0x01d, static_cast<std::uint32_t>(stencil >> 40u)}}) context.push_back(entry);
        }
    }
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> shader{
        {0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, 0}, {0x8c, 0},
        {0x008, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009, static_cast<std::uint32_t>(pixel >> 40u)}, {0x00b, 1u << 1u},
        {0x00c, std::bit_cast<std::uint32_t>(look.colour)},
    };
    std::vector<std::uint32_t> commands;
    Append(commands, 0x79, {0x242, look.primitive});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : shader) Append(commands, 0x76, {offset, value});
    if (look.colourBuffer != nullptr) {
        for (const auto& [offset, value] : std::initializer_list<std::pair<std::uint32_t, std::uint32_t>>{{0x00b, 4u << 1u}, {0x00c, static_cast<std::uint32_t>(loaded)}, {0x00d, static_cast<std::uint32_t>((loaded >> 32u) & 0xffffu) | (look.colourStride << 16u)}, {0x00e, 4u}, {0x00f, 0x31016facu}}) Append(commands, 0x76, {offset, value});
    }
    if (look.indices != nullptr) {
        const auto indices = look.indices->At(0);
        Append(commands, 0x2a, {1});
        Append(commands, 0x27, {3, static_cast<std::uint32_t>(indices), static_cast<std::uint32_t>(indices >> 32u), 3, 0});
    } else {
        Append(commands, 0x2d, {3, 2});
    }
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

std::vector<std::uint8_t> DepthBytes(const GuestBlock& depth) {
    static_cast<void>(AgcDriver::Graphics::FlushDepthPlanes(depth.At(0), BlockBytes, AgcDriver::Graphics::DepthPlaneScope::Whole));
    AgcDriverWaitIdle_nid_postfix();
    return Pixels(depth);
}

void FillDepth(const GuestBlock& depth, float value) {
    for (std::size_t offset = 0; offset < BlockBytes; offset += sizeof(value)) std::memcpy(depth.Bytes(offset), &value, sizeof(value));
    AgcDriver::GuestMemory::BumpCollectEpoch();
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

AgcDriver::Graphics::State PlanTestState() {
    AgcDriver::Graphics::State base{};
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
    return base;
}

std::vector<std::uint64_t> WalkedStateWords(const AgcDriver::Graphics::State& state) {
    std::vector<std::uint64_t> words;
    AgcDriver::DriverDetail::AppendDrawPlanStateWords(state, words);
    return words;
}

std::vector<std::uint64_t> HandwrittenStateWords(const AgcDriver::Graphics::State& state) {
    std::vector<std::uint64_t> words;
    AgcDriver::DriverDetail::AppendHandwrittenDrawPlanStateWords(state, words);
    return words;
}

void PlanStateLeavesAreHashedOrRuled() {
    namespace Graphics = AgcDriver::Graphics;
    const auto base = PlanTestState();
    const auto baseWords = WalkedStateWords(base);
    std::size_t ruledLeaves = 0;
    const auto leaves = AggregateFields::ForEachMutation(base, [&](const Graphics::State& changed, const std::string& path, const void* leaf) {
        if (WalkedStateWords(changed) != baseWords) return;
        const auto ruled = AgcDriver::DriverDetail::DrawPlanStateRuledFields(changed);
        const auto* byte = static_cast<const std::byte*>(leaf);
        const bool named = std::any_of(ruled.begin(), ruled.end(), [&](const auto& field) {
            const auto* begin = static_cast<const std::byte*>(field.begin);
            return byte >= begin && byte < begin + field.bytes;
        });
        Expect(named, "the plan state walk ignores state member " + path + ", which no Skip or Presence rule names");
        ++ruledLeaves;
    });
    const auto ruled = AgcDriver::DriverDetail::DrawPlanStateRuledFields(base);
    Expect(!ruled.empty() && ruledLeaves > 0 && ruledLeaves < leaves, "the plan state rules named no member of a state with a depth and a color target");
    std::printf("draw plans: %zu of %zu state leaves leave the plan state words alone, each inside one of %zu ruled members\n", ruledLeaves, leaves, ruled.size());
}

void PlanStateWalkMatchesTheHandwrittenWriter() {
    namespace Graphics = AgcDriver::Graphics;
    std::size_t compared = 0;
    std::vector<std::string> extra;
    const auto agree = [&](const Graphics::State& context, const std::string& contextPath) {
        const auto walked = WalkedStateWords(context);
        const auto handwritten = HandwrittenStateWords(context);
        AggregateFields::ForEachMutation(context, [&](const Graphics::State& changed, const std::string& path) {
            ++compared;
            const bool walkChanged = WalkedStateWords(changed) != walked;
            const bool handChanged = HandwrittenStateWords(changed) != handwritten;
            Expect(walkChanged || !handChanged, "the plan state walk ignores state member " + path + contextPath + ", which the hand-written writer hashed");
            if (walkChanged && !handChanged && std::find(extra.begin(), extra.end(), path) == extra.end()) extra.push_back(path);
        });
    };
    const auto base = PlanTestState();
    agree(base, "");
    AggregateFields::ForEachMutation(base, [&](const Graphics::State& context, const std::string& path) { agree(context, " after a change of " + path); });
    std::string list;
    for (const auto& path : extra) list += " " + path;
    std::printf("draw plans: the plan state walk hashes every member the hand-written writer hashed, over %zu leaf and leaf-pair changes; it also hashes %zu it left out:%s\n", compared, extra.size(), list.c_str());
}

void PlanKeysCoverThePipelineKey() {
    namespace Graphics = AgcDriver::Graphics;
    Graphics::Context context{};
    const auto base = PlanTestState();
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
    std::size_t covered = 0;
    std::size_t pairs = 0;
    std::size_t refused = 0;
    const auto follow = [&](const Graphics::State& context, const std::string& contextPath) {
        const auto contextPipeline = pipelineKey(context);
        const auto contextWords = planWords(context);
        std::size_t followed = 0;
        AggregateFields::ForEachMutation(context, [&](const Graphics::State& changed, const std::string& path) {
            std::vector<std::byte> changedPipeline;
            try {
                changedPipeline = pipelineKey(changed);
            } catch (const std::exception&) {
                ++refused;
                return;
            }
            if (changedPipeline == contextPipeline) return;
            ++followed;
            Expect(planWords(changed) != contextWords, "the plan key ignores state member " + path + contextPath + ", which changes the pipeline key");
        });
        return followed;
    };
    covered = follow(base, "");
    const auto leaves = AggregateFields::ForEachMutation(base, [&](const Graphics::State& context, const std::string& path) {
        try {
            pipelineKey(context);
        } catch (const std::exception&) {
            return;
        }
        pairs += follow(context, " after a change of " + path);
    });
    Expect(leaves > AggregateFields::FieldCount<Graphics::State>() && covered >= 20, "the field walk changed the pipeline key for too few state members to test the plan key against it");
    std::printf("draw plans: the plan key follows all %zu of %zu state leaves and %zu leaf pairs that change the pipeline key (%zu refused by the pipeline key)\n", covered, leaves, pairs, refused);
}

void AbsentPlanKeysAreClassified() {
    using AgcDriver::DriverDetail::DrawPlanAbsence;
    using AgcDriver::DriverDetail::DrawPlanCounters;
    using AgcDriver::DriverDetail::DrawPlanIndex;
    using AgcDriver::DriverDetail::DrawPlanKey;
    const auto owner = std::make_shared<int>(0);
    const auto key = [](std::uint64_t word) {
        auto made = std::make_shared<DrawPlanKey>();
        made->words = {word};
        made->hash = word * 0x9e3779b97f4a7c15ull;
        return std::shared_ptr<const DrawPlanKey>(std::move(made));
    };
    const auto decisions = [&] {
        auto plan = std::make_shared<AgcDriver::DrawRecipe>();
        plan->pipeline = std::shared_ptr<AgcDriver::Graphics::Pipeline>(owner, static_cast<AgcDriver::Graphics::Pipeline*>(nullptr));
        return std::shared_ptr<const AgcDriver::DrawRecipe>(std::move(plan));
    };
    DrawPlanCounters counters;
    DrawPlanIndex index(1);
    const auto first = key(1);
    const auto second = key(2);
    const auto undecided = key(3);
    Expect(index.ClassifyAbsent(*first) == DrawPlanAbsence::New, "a key never inserted was not classified as new");
    index.Insert(first, decisions(), &counters);
    index.Insert(second, decisions(), &counters);
    Expect(index.Find(*first) == nullptr && index.ClassifyAbsent(*first) == DrawPlanAbsence::Evicted, "a key the index evicted was not classified as evicted");
    index.NoteUndecided(*undecided);
    Expect(index.ClassifyAbsent(*undecided) == DrawPlanAbsence::Undecided, "a key seen without a decision was not classified as undecided");
    index.NoteUndecided(*first);
    Expect(index.ClassifyAbsent(*first) == DrawPlanAbsence::Evicted, "an evicted key seen later without a decision lost its eviction");
    index.Insert(undecided, decisions(), &counters);
    Expect(index.ClassifyAbsent(*second) == DrawPlanAbsence::Evicted && index.ClassifyAbsent(*first) == DrawPlanAbsence::Undecided, "the eviction ring kept more keys than the index holds");
    std::puts("draw plans: absent plan keys are classified as evicted, seen without a decision, or new");
}

void PlanKeysFollowTheBindingShape() {
    namespace Graphics = AgcDriver::Graphics;
    ShaderRecompiler::RecompileResult program{};
    program.spirv = std::vector<std::uint32_t>{0x07230203u, 0x00010500u, 0u, 16u, 0u};
    program.variantId = 0x51u;
    ShaderRecompiler::DescriptorBinding buffers{};
    buffers.kind = ShaderRecompiler::DescriptorKind::StorageBuffer;
    buffers.role = ShaderRecompiler::DescriptorRole::GuestBuffers;
    buffers.binding = 1u;
    buffers.count = 2u;
    buffers.guestDescriptor = {4u, 8u};
    buffers.bufferWritten.push_back(false);
    buffers.bufferWritten.push_back(true);
    buffers.bufferExtent = {64u, 128u};
    ShaderRecompiler::DescriptorBinding images{};
    images.kind = ShaderRecompiler::DescriptorKind::SampledImage;
    images.role = ShaderRecompiler::DescriptorRole::GuestImages;
    images.binding = 2u;
    images.count = 1u;
    images.readOnly = true;
    images.imageShape = ShaderRecompiler::DescriptorImageShape::Image2D;
    images.imageShapes = {ShaderRecompiler::DescriptorImageShape::Image2D};
    program.bindings = {buffers, images};
    const AgcDriver::Pm4::DrawParameters draw{};
    const Graphics::State state{};
    const auto keyOf = [&](const ShaderRecompiler::RecompileResult& result) {
        const std::array<Graphics::CompiledShader, 1> stages{Graphics::CompiledShader{ShaderRecompiler::ShaderStage::Vertex, &result, 0u}};
        const auto key = AgcDriver::DriverDetail::BuildDrawPlanKey(stages, state, draw);
        Expect(key != nullptr, "a program with a variant was refused a plan key");
        return key->words;
    };
    const auto baseKey = keyOf(program);
    const auto baseIdentity = program.Identity();
    Expect(!baseIdentity.Empty(), "the test program has no identity");
    std::size_t shaped = 0;
    const auto leaves = AggregateFields::ForEachMutation(program.bindings, [&](const std::vector<ShaderRecompiler::DescriptorBinding>& bindings, const std::string& path) {
        auto changed = program;
        changed.bindings = bindings;
        const auto sameRoles = std::ranges::equal(bindings, program.bindings, [](const auto& a, const auto& b) { return a.role == b.role && a.readOnly == b.readOnly; });
        if (changed.Identity() == baseIdentity && sameRoles) return;
        ++shaped;
        Expect(keyOf(changed) != baseKey, "the plan key ignores binding member " + path + ", which changes the program's binding shape");
    });
    Expect(shaped >= 11, "the field walk changed the binding shape for too few binding members to test the plan key against it");
    std::printf("draw plans: the plan key follows all %zu of %zu binding leaves that change the binding shape\n", shaped, leaves);
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
    const auto phasesBefore = AgcDriver::Graphics::ReadCommitPhases();
    const auto counts = Measure(secondPlanned, green);
    const auto phases = AgcDriver::Graphics::ReadCommitPhases() - phasesBefore;
    {
        const FullPath fullPath;
        Draw(firstFull, red);
        Draw(firstFull, red);
        Draw(secondFull, green);
    }
    Report("I7 another render target", counts);
    ExpectNoHitsWhileOff(counts);
    const bool ownAttachments = PlansOn() && std::getenv("APS5_PLAN_ATTACHMENTS") != nullptr && std::getenv("APS5_NO_DRAW_KEY_TRIM") == nullptr;
    if (!ownAttachments) Expect(counts[DrawPlanCount::Hits] == 0, "a draw into another render target was served the plan's target");
    if (PlansOn() && !ownAttachments) Expect(counts[DrawPlanCount::MissInstance] == 1, "a draw into another render target was not refused as another instance");
    if (ownAttachments) Expect(counts[DrawPlanCount::Hits] == 1 && counts[DrawPlanCount::OwnAttachmentHits] == 1, "a draw into another render target was not served its plan with its own attachments");
    if (ownAttachments && AgcDriver::Graphics::CommitPhasesEnabled()) {
        const auto planHits = phases.kindDraws[static_cast<std::size_t>(AgcDriver::Graphics::CommitDrawKind::PlanHit)];
        const auto targetMisses = phases.shadowAny[static_cast<std::size_t>(AgcDriver::Graphics::ShadowReason::TargetMiss)];
        Expect(planHits == 1 && targetMisses == 1, "an own-attachment plan hit into a target that missed marked " + std::to_string(targetMisses) + " [parallel-shadow] target misses over " + std::to_string(planHits) + " plan hits");
    }
    const auto sum = [&](DrawPlanCount base, std::size_t buckets, std::uint8_t bit) {
        std::uint64_t total = 0;
        for (std::size_t offset = 0; offset < buckets; ++offset) {
            if (bit == 0 || (offset & bit) != 0) total += counts.values[static_cast<std::size_t>(base) + offset];
        }
        return total;
    };
    if (PlansOn() && std::getenv("APS5_PROFILE_DRAW") != nullptr) {
        if (!ownAttachments) Expect(sum(DrawPlanCount::ReplacementSignatureSets, PlanSignatureSetLimitForTest + 1u, 0) >= 1, "the full path's replacement of the plan for another target was not counted by signature set");
        if (!ownAttachments) Expect(sum(DrawPlanCount::InstanceMismatchSets, 32, AgcDriver::Graphics::PlanMismatchTarget) == 1, "the another-instance miss into another render target was not counted with a target surface mismatch");
        const bool trimmed = std::getenv("APS5_NO_DRAW_KEY_TRIM") == nullptr;
        const auto unexpected = static_cast<std::uint8_t>(AgcDriver::Graphics::PlanMismatchShape | AgcDriver::Graphics::PlanMismatchDepth | (trimmed ? AgcDriver::Graphics::PlanMismatchTemplate : 0u));
        Expect(sum(DrawPlanCount::InstanceMismatchSets, 32, unexpected) == 0, trimmed ? "the another-instance miss into another render target counted a shape, template or depth mismatch" : "the another-instance miss into another render target counted a shape or depth mismatch");
        Expect(sum(DrawPlanCount::LookupDistances, PlanLookupDistanceBucketsForTest, 0) >= 1, "repeated plan lookups were not counted by lookup distance");
        if (ownAttachments) {
            Expect(counts[DrawPlanCount::OwnTargetsNanoseconds] != 0 && counts[DrawPlanCount::OwnFramebufferNanoseconds] != 0, "the own-attachment hit charged no time to resolving its targets or acquiring its framebuffer");
            std::printf("I7 own-attachment hit (APS5_PROFILE_DRAW): %.2f us resolving targets, %.2f us acquiring the framebuffer\n", static_cast<double>(counts[DrawPlanCount::OwnTargetsNanoseconds]) / 1e3, static_cast<double>(counts[DrawPlanCount::OwnFramebufferNanoseconds]) / 1e3);
        }
    }
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

void ReboundPlansStayOffTheExactPath() {
    const auto run = [&](const GuestBlock& target, bool planned) {
        GuestBlock first;
        GuestBlock moved;
        const float dim = 0.25f;
        const float bright = 0.75f;
        std::memcpy(first.Bytes(0), &dim, sizeof(dim));
        std::memcpy(moved.Bytes(0), &bright, sizeof(bright));
        const std::unique_ptr<FullPath> fullPath = planned ? nullptr : std::make_unique<FullPath>();
        const auto before = Counts();
        Draw(target, {.colourBuffer = &first});
        Draw(target, {.colourBuffer = &first});
        Draw(target, {.colourBuffer = &moved, .scale = 16.0f});
        static_cast<void>(Pixels(target));
        Draw(target, {.colourBuffer = &first});
        for (int repeat = 0; repeat < 3; ++repeat) Draw(target, {.colourBuffer = &moved, .scale = 16.0f});
        const auto beforeStride = Counts();
        Draw(target, {.colourBuffer = &moved, .colourStride = 4u, .scale = 8.0f});
        const auto stride = Since(beforeStride);
        if (planned && PlansOn()) Expect(stride[DrawPlanCount::Hits] == 0 && stride[DrawPlanCount::MissInstance] == 1, "a moved colour buffer whose descriptor also changed its stride was not refused as another instance");
        const auto beforeRepeat = Counts();
        Draw(target, {.colourBuffer = &moved, .colourStride = 4u, .scale = 8.0f});
        const auto repeat = Since(beforeRepeat);
        if (planned && PlansOn()) {
            if (AgcDriver::Graphics::DrawPlanTemplates()) Expect(repeat[DrawPlanCount::Hits] == 1 && repeat[DrawPlanCount::TemplateInstanceHits] == 1, "a draw whose template the resource cache held under another key than its plan's was not served by the plan with that template");
            else Expect(repeat[DrawPlanCount::Hits] == 0 && repeat[DrawPlanCount::InstanceTemplate] == 1, "a draw with another template than its plan's was served while APS5_NO_DRAW_PLAN_TEMPLATES is set");
        }
        return Since(before);
    };
    GuestBlock planned;
    GuestBlock reference;
    const auto counts = run(planned, true);
    static_cast<void>(run(reference, false));
    Report("I7b a moved read-only colour buffer", counts);
    std::printf("I7b rebound: %llu hits rebound %llu buffers, %llu moves refused\n", static_cast<unsigned long long>(counts[DrawPlanCount::RebasedHits]), static_cast<unsigned long long>(counts[DrawPlanCount::RebasedBuffers]), static_cast<unsigned long long>(counts[DrawPlanCount::MissMoved] + counts[DrawPlanCount::MissPatchedData]));
    ExpectNoHitsWhileOff(counts);
    if (PlansOn()) Expect(counts[DrawPlanCount::RebasedHits] >= 1, "no plan hit rebound the moved colour buffer");
    const auto plannedPixels = Pixels(planned);
    Expect(plannedPixels == Pixels(reference), "draws whose colour buffer moved, served by plans and then by their draw keys, render differently from the full path");
    std::puts("draw plans: I7b, a rebound plan hit is never kept as its draw key's exact recipe");
}

void PlansDrawIntoTheirOwnDepth() {
    constexpr std::uint32_t WriteDepth = 0x76u;
    constexpr std::uint32_t TestDepth = 0x32u;
    const auto run = [](bool usePlans, std::array<const GuestBlock*, 6> blocks) {
        const auto& [firstColour, secondColour, firstDepth, secondDepth, firstStencil, secondStencil] = blocks;
        FillDepth(*firstDepth, 1.0f);
        FillDepth(*secondDepth, 1.0f);
        const std::unique_ptr<FullPath> fullPath = usePlans ? nullptr : std::make_unique<FullPath>();
        Draw(*firstColour, {.colour = 1.0f, .depth = firstDepth, .stencil = firstStencil, .depthControl = WriteDepth, .depthOffset = 0.25f});
        Draw(*firstColour, {.colour = 1.0f, .depth = firstDepth, .stencil = firstStencil, .depthControl = WriteDepth, .depthOffset = 0.25f});
        Draw(*firstColour, {.colour = 0.5f, .depth = firstDepth, .stencil = firstStencil, .depthControl = TestDepth, .depthOffset = 0.5f});
        Draw(*firstColour, {.colour = 0.5f, .depth = firstDepth, .stencil = firstStencil, .depthControl = TestDepth, .depthOffset = 0.5f});
        const auto before = Counts();
        Draw(*secondColour, {.colour = 0.75f, .depth = secondDepth, .stencil = secondStencil, .depthControl = WriteDepth, .depthOffset = 0.5f});
        Draw(*secondColour, {.colour = 0.25f, .depth = secondDepth, .stencil = secondStencil, .depthControl = TestDepth, .depthOffset = 0.25f});
        Draw(*firstColour, {.colour = 0.75f, .depth = firstDepth, .stencil = firstStencil, .depthControl = WriteDepth, .depthOffset = 0.125f});
        Draw(*secondColour, {.colour = 0.5f, .depth = secondDepth, .stencil = secondStencil, .depthControl = TestDepth, .depthOffset = 0.75f});
        return Since(before);
    };
    GuestBlock plannedColours[2];
    GuestBlock plannedDepths[2];
    GuestBlock fullColours[2];
    GuestBlock fullDepths[2];
    GuestBlock plannedStencils[2];
    GuestBlock fullStencils[2];
    const auto counts = run(true, {&plannedColours[0], &plannedColours[1], &plannedDepths[0], &plannedDepths[1], &plannedStencils[0], &plannedStencils[1]});
    static_cast<void>(run(false, {&fullColours[0], &fullColours[1], &fullDepths[0], &fullDepths[1], &fullStencils[0], &fullStencils[1]}));
    Report("I7d two depth/stencil surfaces", counts);
    std::printf("I7d own attachments %llu, depth misses %llu\n", static_cast<unsigned long long>(counts[DrawPlanCount::OwnAttachmentHits]), static_cast<unsigned long long>(counts[DrawPlanCount::InstanceDepth]));
    ExpectNoHitsWhileOff(counts);
    const bool ownAttachments = PlansOn() && std::getenv("APS5_PLAN_ATTACHMENTS") != nullptr && std::getenv("APS5_NO_DRAW_KEY_TRIM") == nullptr;
    if (ownAttachments) Expect(counts[DrawPlanCount::OwnAttachmentHits] >= 3 && counts[DrawPlanCount::InstanceDepth] == 0, "draws into another depth surface were not served their plans with their own depth");
    std::vector<float> depths(BlockBytes / sizeof(float));
    for (std::size_t index = 0; index < 2; ++index) {
        const auto colour = Pixels(plannedColours[index]);
        const auto depth = DepthBytes(plannedDepths[index]);
        Expect(Drawn(colour), "a depth scene draw wrote no colour");
        std::memcpy(depths.data(), depth.data(), BlockBytes);
        Expect(std::ranges::any_of(depths, [](float value) { return value != 1.0f; }), "a depth scene draw wrote no depth");
        Expect(colour == Pixels(fullColours[index]), "a colour target of the depth scene differs from the full path's");
        Expect(depth == DepthBytes(fullDepths[index]), "a depth surface of the depth scene differs from the full path's");
        Expect(DepthBytes(plannedStencils[index]) == DepthBytes(fullStencils[index]), "a stencil plane of the depth scene differs from the full path's");
    }
    std::puts("draw plans: I7d, plan hits write and test the depth/stencil surface of their own draw");
}

void RepeatedOwnAttachmentDrawsBecomeRecipeHits() {
    constexpr std::uint64_t Repeats = 8;
    GuestBlock first;
    GuestBlock second;
    GuestBlock fullFirst;
    GuestBlock fullSecond;
    const Look red{.colour = 1.0f, .blended = true, .blendConstant = 0.375f};
    const Look green{.colour = 0.5f, .blended = true, .blendConstant = 0.375f};
    Draw(first, red);
    Draw(first, red);
    const auto phasesBefore = AgcDriver::Graphics::ReadCommitPhases();
    const auto before = Counts();
    for (std::uint64_t round = 0; round < Repeats; ++round) Draw(second, green);
    const auto counts = Since(before);
    const auto phases = AgcDriver::Graphics::ReadCommitPhases() - phasesBefore;
    {
        const FullPath fullPath;
        Draw(fullFirst, red);
        Draw(fullFirst, red);
        for (std::uint64_t round = 0; round < Repeats; ++round) Draw(fullSecond, green);
    }
    Report("I7f repeated draws into another render target", counts);
    std::printf("I7f exact recipes offered with own attachments: %llu\n", static_cast<unsigned long long>(counts[DrawPlanCount::OwnRecipes]));
    if (AgcDriver::Graphics::CommitPhasesEnabled()) {
        const auto kindDraws = [&](AgcDriver::Graphics::CommitDrawKind kind) { return phases.kindDraws[static_cast<std::size_t>(kind)]; };
        const auto kindUs = [&](AgcDriver::Graphics::CommitDrawKind kind) {
            std::uint64_t ns = 0;
            for (const auto value : phases.kindNs[static_cast<std::size_t>(kind)]) ns += value;
            return kindDraws(kind) == 0 ? 0.0 : static_cast<double>(ns) / 1e3 / static_cast<double>(kindDraws(kind));
        };
        std::printf("I7f committed draws by kind: %llu full at %.2f us, %llu recipe hits at %.2f us, %llu plan hits at %.2f us\n", static_cast<unsigned long long>(kindDraws(AgcDriver::Graphics::CommitDrawKind::Full)), kindUs(AgcDriver::Graphics::CommitDrawKind::Full), static_cast<unsigned long long>(kindDraws(AgcDriver::Graphics::CommitDrawKind::RecipeHit)), kindUs(AgcDriver::Graphics::CommitDrawKind::RecipeHit), static_cast<unsigned long long>(kindDraws(AgcDriver::Graphics::CommitDrawKind::PlanHit)), kindUs(AgcDriver::Graphics::CommitDrawKind::PlanHit));
    }
    ExpectNoHitsWhileOff(counts);
    const bool ownAttachments = PlansOn() && std::getenv("APS5_PLAN_ATTACHMENTS") != nullptr && std::getenv("APS5_NO_DRAW_KEY_TRIM") == nullptr;
    if (ownAttachments) {
        const auto lookups = counts[DrawPlanCount::Hits];
        Expect(lookups != 0 && counts[DrawPlanCount::OwnRecipes] == lookups && counts[DrawPlanCount::MissInstance] == 0, "a draw into another render target did not offer an exact recipe with its own attachments");
        Expect(lookups <= 2, "repeats of a draw served into its own attachments kept reaching the plan instead of its exact recipe: " + std::to_string(lookups) + " plan hits for " + std::to_string(Repeats) + " draws");
        if (AgcDriver::Graphics::CommitPhasesEnabled()) {
            const auto recipeHits = phases.kindDraws[static_cast<std::size_t>(AgcDriver::Graphics::CommitDrawKind::RecipeHit)];
            Expect(recipeHits == Repeats - lookups, "repeats of a draw served into its own attachments were " + std::to_string(recipeHits) + " recipe hits, expected " + std::to_string(Repeats - lookups));
        }
    }
    Expect(Pixels(second) == Pixels(fullSecond), "repeats into another render target differ from the full path's");
    Expect(Pixels(first) == Pixels(fullFirst), "the plan's own target differs from the full path's after repeats into another target");
    std::puts("draw plans: I7f, repeats of a draw served into its own attachments become exact recipe hits");
}

void CopiedTargetsAreHandedToTheFullPath() {
    using AgcDriver::Graphics::RuntimeSwitch;
    namespace Switches = AgcDriver::Graphics::RuntimeSwitches;
    const auto run = [](const GuestBlock& target, bool usePlans) {
        const Look first{.colour = 1.0f, .blended = true, .blendConstant = 0.625f};
        const Look second{.colour = 0.5f, .blended = true, .blendConstant = 0.625f};
        const std::unique_ptr<FullPath> fullPath = usePlans ? nullptr : std::make_unique<FullPath>();
        Draw(target, first);
        Draw(target, first);
        Switches::Assign(RuntimeSwitch::NoResidentTargets, true);
        const auto counts = Measure(target, second);
        Switches::Assign(RuntimeSwitch::NoResidentTargets, false);
        return counts;
    };
    GuestBlock planned;
    GuestBlock full;
    const auto counts = run(planned, true);
    static_cast<void>(run(full, false));
    Report("I7g a target the draw copies", counts);
    std::printf("I7g handed %llu\n", static_cast<unsigned long long>(counts[DrawPlanCount::HandedTargets]));
    ExpectNoHitsWhileOff(counts);
    const bool ownAttachments = PlansOn() && std::getenv("APS5_PLAN_ATTACHMENTS") != nullptr && std::getenv("APS5_NO_DRAW_KEY_TRIM") == nullptr;
    if (ownAttachments) Expect(counts[DrawPlanCount::Hits] == 0 && counts[DrawPlanCount::HandedTargets] == 1, "a plan hit with APS5_NO_RESIDENT_TARGETS set did not resolve its target with copies and hand it to the full path");
    const auto pixels = Pixels(planned);
    Expect(Drawn(pixels), "the draw into a copied target wrote nothing");
    Expect(pixels == Pixels(full), "a draw whose target was copied differs from the full path's");
    std::puts("draw plans: I7g, a plan hit whose target is copied hands it to the full path");
}

void PlansRefuseTemplatesWithOtherSetLayouts() {
    GuestBlock planned;
    GuestBlock reference;
    const auto run = [&](const GuestBlock& target, bool usePlans) {
        GuestBlock first;
        GuestBlock moved;
        const float dim = 0.25f;
        const float bright = 0.75f;
        std::memcpy(first.Bytes(0), &dim, sizeof(dim));
        std::memcpy(moved.Bytes(0), &bright, sizeof(bright));
        const std::unique_ptr<FullPath> fullPath = usePlans ? nullptr : std::make_unique<FullPath>();
        AgcDriver::Graphics::SetDrawPlanSetKeySaltForTest(usePlans ? 0x5a17u : 0u);
        Draw(target, {.colourBuffer = &first, .colour = 0.5f});
        Draw(target, {.colourBuffer = &first, .colour = 0.5f});
        Draw(target, {.colourBuffer = &moved, .colourStride = 8u, .scale = 8.0f});
        const auto before = Counts();
        Draw(target, {.colourBuffer = &moved, .scale = 16.0f});
        const auto repeat = Since(before);
        AgcDriver::Graphics::SetDrawPlanSetKeySaltForTest(0);
        return repeat;
    };
    const auto counts = run(planned, true);
    static_cast<void>(run(reference, false));
    Report("I7c a template with other set layouts", counts);
    Expect(counts[DrawPlanCount::TemplateInstanceHits] == 0, "a plan hit took a cached template whose set layouts differ from the plan's");
    if (PlansOn() && std::getenv("APS5_NO_DRAW_KEY_TRIM") == nullptr) Expect(counts[DrawPlanCount::Hits] == 0 && counts[DrawPlanCount::InstanceTemplate] == 1, "a plan hit took a cached template whose set layouts differ from the plan's");
    Expect(Pixels(planned) == Pixels(reference), "draws refused a template with other set layouts render differently from the full path");
    std::puts("draw plans: I7c, a template whose set layouts differ from the plan's is never served by the plan");
}

void SharedPushDonorsDrawTheirOwnPush() {
    namespace Graphics = AgcDriver::Graphics;
    const bool before = Graphics::RuntimeSwitches::Set(Graphics::RuntimeSwitch::SharePushDonor);
    const std::array<float, 4> colours{0.3125f, 0.4375f, 0.5625f, 0.6875f};
    const auto render = [&](bool share, std::array<GuestBlock, 4>& targets) {
        Graphics::RuntimeSwitches::Assign(Graphics::RuntimeSwitch::SharePushDonor, share);
        for (std::size_t index = 0; index < targets.size(); ++index) Draw(targets[index], {.colour = colours[index]});
    };
    std::array<GuestBlock, 4> shared;
    std::array<GuestBlock, 4> copied;
    const auto sharedBefore = ShaderRecompiler::SharedPushDonorStages();
    render(true, shared);
    const auto sharedStages = ShaderRecompiler::SharedPushDonorStages() - sharedBefore;
    render(false, copied);
    Graphics::RuntimeSwitches::Assign(Graphics::RuntimeSwitch::SharePushDonor, before);
    for (std::size_t index = 0; index < shared.size(); ++index) {
        Expect(Drawn(Pixels(shared[index])), "a draw with a shared push donor drew nothing");
        Expect(Pixels(shared[index]) == Pixels(copied[index]), "the draw of colour " + std::to_string(index) + " with a shared push donor differs from the draw with a patched copy");
        if (index != 0) Expect(Pixels(shared[index]) != Pixels(shared[0]), "draws of different colours with a shared push donor drew the same pixels: the stage read the donor's push data");
    }
    if (std::getenv("APS5_NO_DRAW_DATA_HITS") != nullptr && std::getenv("APS5_NO_DRAW_STAGE_DATA_HITS") != nullptr && std::getenv("APS5_NO_MASKED_RESULT_MEMO") == nullptr && std::getenv("APS5_NO_RESULT_MEMO") == nullptr) Expect(sharedStages != 0, "no draw stage shared its push donor");
    std::printf("draw plans: %llu stages shared their push donor and drew their own colours, as patched copies do\n", static_cast<unsigned long long>(sharedStages));
}

void IndexedPlansFollowTheirIndexRange() {
    GuestBlock firstIndices;
    GuestBlock secondIndices;
    const std::array<std::uint32_t, 3> order{0u, 1u, 2u};
    for (const auto* block : {&firstIndices, &secondIndices}) std::memcpy(block->Bytes(0), order.data(), sizeof(order));
    GuestBlock planned;
    GuestBlock full;
    const Look first{.colour = 0.375f, .indices = &firstIndices};
    const Look second{.colour = 0.4375f, .indices = &secondIndices};
    Draw(planned, first);
    Draw(planned, first);
    const auto counts = Measure(planned, second);
    {
        const FullPath fullPath;
        Draw(full, first);
        Draw(full, first);
        Draw(full, second);
    }
    Report("I11 another index buffer", counts);
    ExpectNoHitsWhileOff(counts);
    const bool untrimmed = std::getenv("APS5_NO_DRAW_KEY_TRIM") != nullptr;
    if (PlansOn() && !untrimmed) Expect(counts[DrawPlanCount::Hits] == 1, "an indexed draw that differs in its index buffer and push data was not served its plan");
    if (PlansOn() && untrimmed) Expect(counts[DrawPlanCount::Hits] == 0 && counts[DrawPlanCount::MissInstance] == 1, "with the index range in the resource key, a draw with another index buffer was served the plan's template");
    Expect(Pixels(planned) == Pixels(full), "the indexed draws with plans differ from the full path's");
    std::puts("draw plans: I11, a plan follows the draw's index range wherever the resource key holds it");
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
    namespace Graphics = AgcDriver::Graphics;
    const auto phasesBefore = Graphics::ReadCommitPhases();
    const auto counts = run(planned);
    const auto phases = Graphics::ReadCommitPhases() - phasesBefore;
    {
        const FullPath fullPath;
        static_cast<void>(run(full));
    }
    Report("I10 a scene", counts);
    ExpectNoHitsWhileOff(counts);
    if (PlansOn()) Expect(counts[DrawPlanCount::Hits] >= 4, "the scene's repeated plans were not served");
    if (PlansOn()) Expect(counts[DrawPlanCount::SharedBindingStages] + counts[DrawPlanCount::ComparedBindingStages] == 2u * counts[DrawPlanCount::Hits], "a plan hit did not prove the binding words of each of its two stages");
    std::printf("I10 binding words: %llu stages proved by the program's memo, %llu compared\n", static_cast<unsigned long long>(counts[DrawPlanCount::SharedBindingStages]), static_cast<unsigned long long>(counts[DrawPlanCount::ComparedBindingStages]));
    if (counts[DrawPlanCount::Hits] != 0 && counts[DrawPlanCount::FullDraws] != 0) std::printf("I10 committer time (APS5_PROFILE_DRAW): %.2f us per plan hit, %.2f us per full-path draw with a plan key\n", static_cast<double>(counts[DrawPlanCount::HitNanoseconds]) / static_cast<double>(counts[DrawPlanCount::Hits]) / 1000.0, static_cast<double>(counts[DrawPlanCount::FullNanoseconds]) / static_cast<double>(counts[DrawPlanCount::FullDraws]) / 1000.0);
    if (Graphics::CommitPhasesEnabled() && counts[DrawPlanCount::Hits] != 0) {
        const auto phaseUs = [&](Graphics::CommitPhase phase) { return static_cast<double>(phases.ns[static_cast<std::size_t>(phase)]) / 1e3 / static_cast<double>(counts[DrawPlanCount::Hits]); };
        const auto targetsPhase = std::getenv("APS5_PLAN_ATTACHMENTS") != nullptr ? Graphics::CommitPhase::Targets : Graphics::CommitPhase::PlanHitTargets;
        for (const auto phase : {Graphics::CommitPhase::PlanHitFind, Graphics::CommitPhase::PlanHitWords, Graphics::CommitPhase::PlanHitInstance, targetsPhase}) Expect(phases.ns[static_cast<std::size_t>(phase)] != 0, std::string("the plan hits charged no time to ") + Graphics::CommitPhaseName(phase));
        std::printf("I10 plan hit phases (APS5_PROFILE_COMMIT), us per hit: find %.2f words %.2f instance %.2f targets %.2f rebind %.2f\n", phaseUs(Graphics::CommitPhase::PlanHitFind), phaseUs(Graphics::CommitPhase::PlanHitWords), phaseUs(Graphics::CommitPhase::PlanHitInstance), phaseUs(Graphics::CommitPhase::PlanHitTargets), phaseUs(Graphics::CommitPhase::PlanHitRebind));
        const auto kindDraws = [&](Graphics::CommitDrawKind kind) { return phases.kindDraws[static_cast<std::size_t>(kind)]; };
        const auto kindUs = [&](Graphics::CommitDrawKind kind) {
            std::uint64_t ns = 0;
            for (const auto value : phases.kindNs[static_cast<std::size_t>(kind)]) ns += value;
            return kindDraws(kind) == 0 ? 0.0 : static_cast<double>(ns) / 1e3 / static_cast<double>(kindDraws(kind));
        };
        Expect(kindDraws(Graphics::CommitDrawKind::PlanHit) == counts[DrawPlanCount::Hits], "the commit phases counted " + std::to_string(kindDraws(Graphics::CommitDrawKind::PlanHit)) + " plan-hit draws for " + std::to_string(counts[DrawPlanCount::Hits]) + " plan hits");
        Expect(kindDraws(Graphics::CommitDrawKind::Full) + kindDraws(Graphics::CommitDrawKind::RecipeHit) + kindDraws(Graphics::CommitDrawKind::PlanHit) == phases.draws, "the draws of each kind do not sum to the committed draws");
        Expect(phases.kindNs[static_cast<std::size_t>(Graphics::CommitDrawKind::Full)][static_cast<std::size_t>(Graphics::CommitPhase::PlanHitWords)] == 0 || counts[DrawPlanCount::MissInstance] != 0, "full-path draws charged plan binding-word time without a missed plan");
        std::printf("I10 committed draws by kind: %llu full at %.2f us, %llu recipe hits at %.2f us, %llu plan hits at %.2f us\n", static_cast<unsigned long long>(kindDraws(Graphics::CommitDrawKind::Full)), kindUs(Graphics::CommitDrawKind::Full), static_cast<unsigned long long>(kindDraws(Graphics::CommitDrawKind::RecipeHit)), kindUs(Graphics::CommitDrawKind::RecipeHit), static_cast<unsigned long long>(kindDraws(Graphics::CommitDrawKind::PlanHit)), kindUs(Graphics::CommitDrawKind::PlanHit));
    }
    for (std::size_t index = 0; index < planned.size(); ++index) Expect(Pixels(planned[index]) == Pixels(full[index]), "target " + std::to_string(index) + " of the planned scene differs from the full path's");
    std::puts("draw plans: I10, a scene recorded with plans renders as the full path renders it");
}

}

int BenchPlanKeys() {
    namespace Graphics = AgcDriver::Graphics;
    Graphics::State state{};
    state.hasColorTarget = true;
    state.colors.resize(1);
    state.blends.resize(1);
    state.depth.emplace();
    std::array<ShaderRecompiler::RecompileResult, 2> programs{};
    for (std::size_t stage = 0; stage < programs.size(); ++stage) {
        auto& program = programs[stage];
        program.spirv = std::vector<std::uint32_t>{0x07230203u, 0x00010500u, 0u, 16u, static_cast<std::uint32_t>(stage)};
        program.variantId = 0x51u + stage;
        for (std::uint32_t binding = 0; binding < 24u; ++binding) {
            ShaderRecompiler::DescriptorBinding entry{};
            entry.kind = binding % 2u == 0u ? ShaderRecompiler::DescriptorKind::StorageBuffer : ShaderRecompiler::DescriptorKind::SampledImage;
            entry.role = binding % 2u == 0u ? ShaderRecompiler::DescriptorRole::GuestBuffers : ShaderRecompiler::DescriptorRole::GuestImages;
            entry.binding = binding;
            entry.count = 1u;
            program.bindings.push_back(entry);
        }
    }
    const std::array<Graphics::CompiledShader, 2> stages{Graphics::CompiledShader{ShaderRecompiler::ShaderStage::Vertex, &programs[0], 0u}, Graphics::CompiledShader{ShaderRecompiler::ShaderStage::Fragment, &programs[1], 64u}};
    const AgcDriver::Pm4::DrawParameters draw{};
    constexpr std::size_t Keys = 200000;
    std::uint64_t sink = 0;
    std::size_t words = 0;
    for (int round = 0; round < 3; ++round) {
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < Keys; ++i) {
            const auto key = AgcDriver::DriverDetail::BuildDrawPlanKey(stages, state, draw);
            sink += key->hash;
            words = key->words.size();
        }
        const auto ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / Keys;
        std::printf("bench plan keys: %.1f ns per key (%zu words, 2 stages of 24 bindings)\n", ns, words);
    }
    return sink == 0 ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--bench") return BenchPlanKeys();
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        PlanKeysCoverThePipelineKey();
        PlanStateLeavesAreHashedOrRuled();
        PlanStateWalkMatchesTheHandwrittenWriter();
        PlanKeysFollowTheBindingShape();
        AbsentPlanKeysAreClassified();
        Register(VertexHeader, TriangleVertexCode.data(), sizeof(TriangleVertexCode), 2);
        Register(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
        Register(LoadingPixelHeader, LoadingPixelCode.data(), sizeof(LoadingPixelCode), 1);
        PlansFollowPipelineState();
        PlansBindTheirOwnTargets();
        PlanRowsFollowTheDraw();
        PlanHitsSeeGuestWrites();
        ReboundPlansStayOffTheExactPath();
        PlansRefuseTemplatesWithOtherSetLayouts();
        PlansDrawIntoTheirOwnDepth();
        RepeatedOwnAttachmentDrawsBecomeRecipeHits();
        CopiedTargetsAreHandedToTheFullPath();
        PlannedSceneMatchesFullPath();
        SharedPushDonorsDrawTheirOwnPush();
        IndexedPlansFollowTheirIndexRange();
        const auto keys = AgcDriver::Graphics::StageKeyMemoCounters();
        std::printf("stage key memo: %llu static hits, %llu static fills, %llu patched segments; unshaped: %llu per-result memo hits, %llu full walks\n", static_cast<unsigned long long>(keys.staticHits), static_cast<unsigned long long>(keys.staticFills), static_cast<unsigned long long>(keys.patchedSegments), static_cast<unsigned long long>(keys.resultHits), static_cast<unsigned long long>(keys.resultWalks));
        AgcDriverShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
