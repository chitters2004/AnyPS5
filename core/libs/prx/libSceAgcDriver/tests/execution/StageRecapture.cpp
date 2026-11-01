#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "Recompiler.hpp"
#include "RuntimeAbi.hpp"
#include "BdaAbi.hpp"
#include "CacheKey.hpp"
#include "CompiledVariant.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <thread>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ShaderRecompiler;
using AgcDriver::DriverDetail::ShaderSnapshot;

constexpr std::array<std::uint32_t, 5> PixelCode{0xbf800000u, 0x7e000200u, 0xf800180fu, 0u, 0xbf810000u};

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

SpirvTarget Target() {
    static constexpr std::array<std::uint32_t, 24> capabilities{
        spv::CapabilityShader, spv::CapabilityImageGatherExtended, spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess,
        spv::CapabilityClipDistance, spv::CapabilityCullDistance, spv::CapabilityFloat16, spv::CapabilityFloat64, spv::CapabilityInt16, spv::CapabilityInt8,
        spv::CapabilityStorageImageExtendedFormats, spv::CapabilityImageQuery, spv::CapabilityDerivativeControl, spv::CapabilitySampleRateShading, spv::CapabilityGroupNonUniform,
        spv::CapabilityGroupNonUniformVote, spv::CapabilityGroupNonUniformArithmetic, spv::CapabilityGroupNonUniformBallot, spv::CapabilityGroupNonUniformShuffle,
        spv::CapabilityStorageImageWriteWithoutFormat, spv::CapabilityStorageImageReadWithoutFormat, spv::CapabilityInt64Atomics, spv::CapabilityStorageBuffer16BitAccess};
    static constexpr std::array<std::string_view, 12> extensions{
        "SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage", "SPV_KHR_16bit_storage", "SPV_KHR_shader_ballot", "SPV_KHR_subgroup_vote", "SPV_EXT_shader_stencil_export",
        "SPV_KHR_float_controls", "SPV_EXT_demote_to_helper_invocation", "SPV_KHR_non_semantic_info", "SPV_EXT_shader_atomic_float_add", "SPV_KHR_shader_clock", "SPV_EXT_fragment_shader_interlock"};
    SpirvTarget target{};
    target.vulkanVersion = 0x00403000u;
    target.spirvVersion = 0x00010600u;
    target.subgroupSize = 32u;
    target.bdaAbiVersion = BdaAbi::Version;
    target.supportedCapabilities = capabilities;
    target.supportedExtensions = extensions;
    return target;
}

struct PixelStage {
    ShaderSnapshot snapshot;
    std::vector<std::uint32_t> users;
    RecompileRequest request{};
    AgcDriver::ShaderMemory memory{{}};

    explicit PixelStage(std::span<const std::uint32_t> code = PixelCode, std::vector<std::uint32_t> words = std::vector<std::uint32_t>(1u), std::uint64_t address = 0x200000u)
        : snapshot{address, 0, 1, {code.begin(), code.end()}, {}}, users(std::move(words)) {
        request.shader = {ShaderStage::Fragment, snapshot.codeAddress, snapshot.code, 0, {}};
        request.context.waveSize = 32u;
        request.context.userDataBaseRegister = 0u;
        request.context.userData = users;
        request.context.pixel = ShaderPixelStageInfo{};
        request.context.pixel->wave32 = true;
        request.context.pixel->targetOutputMode[0] = 9u;
        request.target = Target();
        request.layout = {0u, 0u, 0u, 128u};
        request.useCache = true;
    }

    std::shared_ptr<const RecompileResult> Current() {
        AgcDriver::ShaderMemory draw({});
        auto invocation = AgcDriver::DriverDetail::InvocationFor(snapshot, 0, request);
        const auto capture = AgcDriver::DriverDetail::CaptureInvocation(draw, snapshot, 0, request, invocation);
        return invocation.Materialize(*capture);
    }

    std::shared_ptr<const RecompileResult> CurrentUncached() {
        request.useCache = false;
        auto result = Current();
        request.useCache = true;
        return result;
    }

    PreparedShaderInvocation Invocation(std::uint32_t value) {
        users[0] = value;
        return AgcDriver::DriverDetail::InvocationFor(snapshot, 0, request);
    }

    std::shared_ptr<const RecompileResult> Draw(std::uint32_t value) {
        auto invocation = Invocation(value);
        const auto capture = AgcDriver::DriverDetail::CaptureInvocation(memory, snapshot, 0, request, invocation);
        return invocation.Materialize(*capture);
    }

    std::shared_ptr<const RecompileResult> Uncached(std::uint32_t value) {
        request.useCache = false;
        auto result = Draw(value);
        request.useCache = true;
        return result;
    }
};

void RequireSameResult(const RecompileResult& left, const RecompileResult& right, const std::string& what) {
    Require(left.pushConstants == right.pushConstants, what + ": the push constants differ from a materialization without the memo");
    Require(left.specializationId == right.specializationId, what + ": the specialization differs from a materialization without the memo");
    Require(left.bindings.size() == right.bindings.size(), what + ": the bindings differ from a materialization without the memo");
    for (std::size_t index = 0; index < left.bindings.size(); ++index) {
        Require(left.bindings[index].binding == right.bindings[index].binding && left.bindings[index].guestDescriptor == right.bindings[index].guestDescriptor, what + ": binding " + std::to_string(index) + " differs from a materialization without the memo");
    }
}

void CheckInvocation() {
    PixelStage stage;
    const auto first = stage.Invocation(1u);
    Require(stage.snapshot.prepared->entries.size() == 1u, "the first invocation did not prepare one artifact at use");
    const auto again = stage.Invocation(2u);
    Require(stage.snapshot.prepared->entries.size() == 1u, "a second invocation with the same interface prepared another artifact");
    const auto prepared = GetPreparedCode(*stage.snapshot.prepared->entries.front().handle);
    for (const auto* invocation : {&first, &again}) {
        const auto& request = invocation->Request();
        Require(request.shader.code.data() == prepared.data() && request.shader.code.size() == prepared.size(), "the invocation does not refer to the prepared code");
        Require(request.shader.codeAddress == stage.request.shader.codeAddress && request.shader.stage == ShaderStage::Fragment, "the invocation changed the shader it was asked for");
        Require(request.context.userData.data() == stage.users.data() && request.context.userData.size() == stage.users.size(), "the invocation does not read the user data of the draw");
        Require(request.layout.pushConstantOffsetBytes == 0u && request.layout.pushConstantSizeBytes == 128u && request.useCache, "the invocation changed the layout of the draw");
        Require(request.context.pixel.has_value() && request.context.pixel->targetOutputMode[0] == 9u, "the invocation lost the pixel state of the draw");
    }
    stage.request.layout = {0u, 0u, 4u, 124u};
    const auto moved = stage.Invocation(1u);
    Require(stage.snapshot.prepared->entries.size() == 1u && moved.Request().layout.pushConstantOffsetBytes == 4u, "an invocation at another push offset did not reuse the artifact with its own layout");
    stage.request.layout = {0u, 0u, 0u, 128u};
    stage.request.context.pixel->targetOutputMode[0] = 4u;
    const auto other = stage.Invocation(1u);
    Require(stage.snapshot.prepared->entries.size() == 2u, "an invocation with another interface reused an artifact prepared for a different one");
    Require(other.Request().context.pixel->targetOutputMode[0] == 4u, "the invocation for another interface lost its pixel state");
    const auto otherCode = GetPreparedCode(*stage.snapshot.prepared->entries.back().handle);
    Require(other.Request().shader.code.data() == otherCode.data(), "the invocation prepared at use does not refer to its own prepared code");
}

void CheckResultMemo() {
    PixelStage stage;
    const auto first = stage.Draw(0x11u);
    Require(stage.Draw(0x11u) == first, "an unchanged stage did not reuse its memoized result");
    RequireSameResult(*first, *stage.Uncached(0x11u), "the first result");
    const auto patched = stage.Draw(0x22u);
    Require(patched != first, "a stage whose user data changed reused the result of the old user data");
    RequireSameResult(*patched, *stage.Uncached(0x22u), "the result for new user data");
    stage.request.layout = {0u, 0u, 4u, 124u};
    const auto moved = stage.Draw(0x11u);
    Require(moved != first && moved != patched, "a stage whose push offset moved reused a result laid out for the old offset");
    RequireSameResult(*moved, *stage.Uncached(0x11u), "the result at a new push offset");
    const auto movedPatched = stage.Draw(0x33u);
    RequireSameResult(*movedPatched, *stage.Uncached(0x33u), "the result for new user data at a new push offset");
    stage.request.layout = {0u, 0u, 0u, 128u};
    Require(stage.Draw(0x11u) == first, "returning to the first push offset did not reuse the first result");
}

void CheckInterfaceKey() {
    PixelStage stage;
    std::vector<std::uint64_t> key;
    BuildPreparedShaderKey(stage.request, key);
    Require(key.size() > 128u, "the pixel interface key is too short to cross the key writer's staging boundary twice");
    Require(key[0] == RuntimeAbi::Version && key[1] == static_cast<std::uint64_t>(ShaderStage::Fragment), "the interface key does not start with the ABI version and the stage");
    std::vector<std::uint64_t> again{1u, 2u, 3u};
    BuildPreparedShaderKey(stage.request, again);
    Require(again == key, "an interface key built into a used vector differs from one built into an empty vector");
    stage.request.target.meshWindowBuffer = !stage.request.target.meshWindowBuffer;
    BuildPreparedShaderKey(stage.request, again);
    Require(again.size() == key.size(), "the last target field changed the length of the interface key");
    std::vector<std::size_t> changed;
    for (std::size_t index = 0; index < key.size(); ++index) {
        if (key[index] != again[index]) changed.push_back(index);
    }
    Require(changed.size() == 1u && changed.front() == key.size() - 5u, "the last target field did not change exactly its own word, before the probe, ray tracing and subgroup words");
    stage.request.target.meshWindowBuffer = !stage.request.target.meshWindowBuffer;
    stage.request.context.pixel->interpolatorCount = 2u;
    stage.request.context.pixel->interpolatorSettings[1] = 0x405u;
    BuildPreparedShaderKey(stage.request, again);
    Require(again.size() == key.size() + 2u && again.back() == key.back(), "two interpolators did not add exactly two words to the interface key");
    std::vector<std::uint64_t> full;
    RecompileCacheKey::Build(stage.request, full);
    Require(full.size() == again.size() + 1u && full[2] == stage.request.shader.code.size() && full[3] == RecompileCacheKey::HashCode(stage.request.shader.code) && std::equal(again.begin() + 2, again.end() - 1, full.begin() + 4), "the cache key is not the interface key with the code hash after the stage");
}

void CheckSubstituteSlots() {
    BufferSubstituteTable table(std::vector<BufferSubstitute>(2u));
    const auto before = table.Slots();
    table.Learn(1u, {0x11u, 0x22u, 4u, false});
    const auto after = table.Slots();
    Require(before->size() == 2u && !(*before)[1].known, "learning a substitute changed the slots an earlier materialization holds");
    Require((*after)[1].known && (*after)[1].word1 == 0x11u && (*after)[1].word3 == 0x22u && (*after)[1].misalignment == 4u && !(*after)[0].known, "a learned substitute is missing from the slots read after it");
    table.Learn(1u, {0x33u, 0x44u, 0u, false});
    Require((*table.Slots())[1].word1 == 0x11u, "a known substitute was learned again");
    table.Learn(2u, {0x55u, 0x66u, 0u, false});
    Require(table.Slots()->size() == 2u, "a substitute outside the slots was learned");
}

constexpr std::array<std::uint64_t, 178> GoldenPixelFull{
    0x5ull, 0x6ull, 0xf41b830be394e99bull, 0x20ull, 0x2ull, 0x6ull, 0x0ull, 0x1ull,
    0x3ull, 0x401ull, 0x20ull, 0x7ull, 0x1ull, 0x302ull, 0x0ull, 0x0ull,
    0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull,
    0x0ull, 0x0ull, 0x0ull, 0x1ull, 0x3e800000ull, 0x3f400000ull, 0x0ull, 0x0ull,
    0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x8ull,
    0xfull, 0xffffull, 0xffffull, 0xffffull, 0xffffull, 0x0ull, 0x0ull, 0x0ull,
    0x0ull, 0x0ull, 0x55ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull,
    0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull,
    0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull,
    0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x9ull,
    0x4ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x1ull, 0x1ull,
    0x1ull, 0xfull, 0x4ull, 0x0ull, 0x0ull, 0x1ull, 0x2ull, 0x3ull,
    0x1ull, 0x4ull, 0x5ull, 0x2ull, 0x7ull, 0x3f800000ull, 0x0ull, 0x3f000000ull,
    0x40000000ull, 0x1ull, 0x2ull, 0x3ull, 0x4ull, 0x0ull, 0x1ull, 0xc0ull,
    0x1ull, 0x0ull, 0x1ull, 0x0ull, 0x2ull, 0x40ull, 0x88ull, 0x1ull,
    0x403000ull, 0x10600ull, 0x20ull, 0x3ull, 0xd902deeb29309d7cull, 0x33ae2159d16a384dull, 0x1ull, 0x400ull,
    0x400ull, 0x40ull, 0x400ull, 0x10000ull, 0x1ull, 0x80ull, 0x1ull, 0x1ull,
    0x80ull, 0x7000ull, 0x100ull, 0x100ull, 0x80ull, 0x8000ull, 0x4ull, 0x2ull,
    0x1ull, 0x20ull, 0x80ull, 0x80ull, 0x78ull, 0x1000ull, 0x80ull, 0x80ull,
    0x0ull, 0x5ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x4d2ull,
    0x1ull, 0x1ull, 0x0ull, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x0ull,
    0x0ull, 0x0ull, 0x0ull, 0x309ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull,
    0x0ull, 0x0ull};
constexpr std::array<std::uint64_t, 177> GoldenPixelPrepared{
    0x5ull, 0x20ull, 0x2ull, 0x6ull, 0x0ull, 0x1ull, 0x3ull, 0x401ull,
    0x20ull, 0x7ull, 0x1ull, 0x302ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull,
    0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull,
    0x0ull, 0x1ull, 0x3e800000ull, 0x3f400000ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull,
    0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x8ull, 0xfull, 0xffffull,
    0xffffull, 0xffffull, 0xffffull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull,
    0x55ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull,
    0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull,
    0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull,
    0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x9ull, 0x4ull, 0x0ull,
    0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x1ull, 0x1ull, 0x1ull, 0xfull,
    0x4ull, 0x0ull, 0x0ull, 0x1ull, 0x2ull, 0x3ull, 0x1ull, 0x4ull,
    0x5ull, 0x2ull, 0x7ull, 0x3f800000ull, 0x0ull, 0x3f000000ull, 0x40000000ull, 0x1ull,
    0x2ull, 0x3ull, 0x4ull, 0x0ull, 0x1ull, 0xc0ull, 0x1ull, 0x0ull,
    0x1ull, 0x0ull, 0x2ull, 0x40ull, 0x88ull, 0x1ull, 0x403000ull, 0x10600ull,
    0x20ull, 0x3ull, 0xd902deeb29309d7cull, 0x33ae2159d16a384dull, 0x1ull, 0x400ull, 0x400ull, 0x40ull,
    0x400ull, 0x10000ull, 0x1ull, 0x80ull, 0x1ull, 0x1ull, 0x80ull, 0x7000ull,
    0x100ull, 0x100ull, 0x80ull, 0x8000ull, 0x4ull, 0x2ull, 0x1ull, 0x20ull,
    0x80ull, 0x80ull, 0x78ull, 0x1000ull, 0x80ull, 0x80ull, 0x0ull, 0x5ull,
    0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x4d2ull, 0x1ull, 0x1ull,
    0x0ull, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull,
    0x0ull, 0x309ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull,
    0x20ull};
constexpr std::array<std::uint64_t, 96> GoldenVertexFull{
    0x1ull, 0x6ull, 0xf41b830be394e99bull, 0x40ull, 0x2ull, 0x6ull, 0x0ull, 0x0ull,
    0x1ull, 0x6ull, 0x8ull, 0x0ull, 0x3full, 0x1ull, 0x1ull, 0x40000000ull,
    0x3f800000ull, 0x41000000ull, 0x2ull, 0x1ull, 0x4ull, 0x4ull, 0x0ull, 0x1ull,
    0x1ull, 0x8ull, 0x2ull, 0x1ull, 0x0ull, 0x1ull, 0xc0ull, 0x1ull,
    0x0ull, 0x1ull, 0x0ull, 0x2ull, 0x40ull, 0x88ull, 0x403000ull, 0x10600ull,
    0x20ull, 0x3ull, 0xd902deeb29309d7cull, 0x33ae2159d16a384dull, 0x1ull, 0x400ull, 0x400ull, 0x40ull,
    0x400ull, 0x10000ull, 0x1ull, 0x80ull, 0x1ull, 0x1ull, 0x80ull, 0x7000ull,
    0x100ull, 0x100ull, 0x80ull, 0x8000ull, 0x4ull, 0x2ull, 0x1ull, 0x20ull,
    0x80ull, 0x80ull, 0x78ull, 0x1000ull, 0x80ull, 0x80ull, 0x0ull, 0x5ull,
    0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x4d2ull, 0x1ull, 0x1ull,
    0x0ull, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull,
    0x0ull, 0x309ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull};
constexpr std::array<std::uint64_t, 95> GoldenVertexPrepared{
    0x1ull, 0x40ull, 0x2ull, 0x6ull, 0x0ull, 0x0ull, 0x1ull, 0x6ull,
    0x8ull, 0x0ull, 0x3full, 0x1ull, 0x1ull, 0x40000000ull, 0x3f800000ull, 0x41000000ull,
    0x2ull, 0x1ull, 0x4ull, 0x4ull, 0x0ull, 0x1ull, 0x1ull, 0x8ull,
    0x2ull, 0x1ull, 0x0ull, 0x1ull, 0xc0ull, 0x1ull, 0x0ull, 0x1ull,
    0x0ull, 0x2ull, 0x40ull, 0x88ull, 0x403000ull, 0x10600ull, 0x20ull, 0x3ull,
    0xd902deeb29309d7cull, 0x33ae2159d16a384dull, 0x1ull, 0x400ull, 0x400ull, 0x40ull, 0x400ull, 0x10000ull,
    0x1ull, 0x80ull, 0x1ull, 0x1ull, 0x80ull, 0x7000ull, 0x100ull, 0x100ull,
    0x80ull, 0x8000ull, 0x4ull, 0x2ull, 0x1ull, 0x20ull, 0x80ull, 0x80ull,
    0x78ull, 0x1000ull, 0x80ull, 0x80ull, 0x0ull, 0x5ull, 0x0ull, 0x0ull,
    0x0ull, 0x0ull, 0x0ull, 0x4d2ull, 0x1ull, 0x1ull, 0x0ull, 0x0ull,
    0x1ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x309ull,
    0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x20ull};
constexpr std::array<std::uint64_t, 86> GoldenEvaluationFull{
    0x3ull, 0x6ull, 0xf41b830be394e99bull, 0x40ull, 0x2ull, 0x6ull, 0x0ull, 0x0ull,
    0x0ull, 0x1ull, 0xc0ull, 0x1ull, 0x0ull, 0x1ull, 0x0ull, 0x2ull,
    0x40ull, 0x88ull, 0x18ull, 0x1ull, 0x3ull, 0x4ull, 0x1ull, 0x2ull,
    0x3ull, 0x80ull, 0x10ull, 0x4ull, 0x403000ull, 0x10600ull, 0x20ull, 0x3ull,
    0xd902deeb29309d7cull, 0x33ae2159d16a384dull, 0x1ull, 0x400ull, 0x400ull, 0x40ull, 0x400ull, 0x10000ull,
    0x1ull, 0x80ull, 0x1ull, 0x1ull, 0x80ull, 0x7000ull, 0x100ull, 0x100ull,
    0x80ull, 0x8000ull, 0x4ull, 0x2ull, 0x1ull, 0x20ull, 0x80ull, 0x80ull,
    0x78ull, 0x1000ull, 0x80ull, 0x80ull, 0x0ull, 0x5ull, 0x0ull, 0x0ull,
    0x0ull, 0x0ull, 0x0ull, 0x4d2ull, 0x1ull, 0x1ull, 0x0ull, 0x0ull,
    0x1ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x309ull,
    0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull};
constexpr std::array<std::uint64_t, 85> GoldenEvaluationPrepared{
    0x3ull, 0x40ull, 0x2ull, 0x6ull, 0x0ull, 0x0ull, 0x0ull, 0x1ull,
    0xc0ull, 0x1ull, 0x0ull, 0x1ull, 0x0ull, 0x2ull, 0x40ull, 0x88ull,
    0x18ull, 0x1ull, 0x3ull, 0x4ull, 0x1ull, 0x2ull, 0x3ull, 0x80ull,
    0x10ull, 0x4ull, 0x403000ull, 0x10600ull, 0x20ull, 0x3ull, 0xd902deeb29309d7cull, 0x33ae2159d16a384dull,
    0x1ull, 0x400ull, 0x400ull, 0x40ull, 0x400ull, 0x10000ull, 0x1ull, 0x80ull,
    0x1ull, 0x1ull, 0x80ull, 0x7000ull, 0x100ull, 0x100ull, 0x80ull, 0x8000ull,
    0x4ull, 0x2ull, 0x1ull, 0x20ull, 0x80ull, 0x80ull, 0x78ull, 0x1000ull,
    0x80ull, 0x80ull, 0x0ull, 0x5ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull,
    0x0ull, 0x4d2ull, 0x1ull, 0x1ull, 0x0ull, 0x0ull, 0x1ull, 0x0ull,
    0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x309ull, 0x0ull, 0x0ull,
    0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x20ull};
constexpr std::array<std::uint64_t, 99> GoldenMeshFull{
    0x7ull, 0x6ull, 0xf41b830be394e99bull, 0x40ull, 0x2ull, 0x6ull, 0x0ull, 0x0ull,
    0x0ull, 0x1ull, 0xc0ull, 0x1ull, 0x0ull, 0x1ull, 0x0ull, 0x2ull,
    0x40ull, 0x88ull, 0x1ull, 0x3ull, 0x40ull, 0x80ull, 0x100ull, 0x80ull,
    0x40ull, 0x200ull, 0x1ull, 0x18ull, 0x3ull, 0x63617074ull, 0x1ull, 0x1ull,
    0x1ull, 0x0ull, 0x2ull, 0x0ull, 0x1ull, 0x1ull, 0x1ull, 0x3ull,
    0x0ull, 0x403000ull, 0x10600ull, 0x20ull, 0x3ull, 0xd902deeb29309d7cull, 0x33ae2159d16a384dull, 0x1ull,
    0x400ull, 0x400ull, 0x40ull, 0x400ull, 0x10000ull, 0x1ull, 0x80ull, 0x1ull,
    0x1ull, 0x80ull, 0x7000ull, 0x100ull, 0x100ull, 0x80ull, 0x8000ull, 0x4ull,
    0x2ull, 0x1ull, 0x20ull, 0x80ull, 0x80ull, 0x78ull, 0x1000ull, 0x80ull,
    0x80ull, 0x0ull, 0x5ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull,
    0x4d2ull, 0x1ull, 0x1ull, 0x0ull, 0x0ull, 0x1ull, 0x0ull, 0x0ull,
    0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x309ull, 0x0ull, 0x0ull, 0x0ull,
    0x0ull, 0x0ull, 0x0ull};
constexpr std::array<std::uint64_t, 98> GoldenMeshPrepared{
    0x7ull, 0x40ull, 0x2ull, 0x6ull, 0x0ull, 0x0ull, 0x0ull, 0x1ull,
    0xc0ull, 0x1ull, 0x0ull, 0x1ull, 0x0ull, 0x2ull, 0x40ull, 0x88ull,
    0x1ull, 0x3ull, 0x40ull, 0x80ull, 0x100ull, 0x80ull, 0x40ull, 0x200ull,
    0x1ull, 0x18ull, 0x3ull, 0x63617074ull, 0x1ull, 0x1ull, 0x1ull, 0x0ull,
    0x2ull, 0x0ull, 0x1ull, 0x1ull, 0x1ull, 0x3ull, 0x0ull, 0x403000ull,
    0x10600ull, 0x20ull, 0x3ull, 0xd902deeb29309d7cull, 0x33ae2159d16a384dull, 0x1ull, 0x400ull, 0x400ull,
    0x40ull, 0x400ull, 0x10000ull, 0x1ull, 0x80ull, 0x1ull, 0x1ull, 0x80ull,
    0x7000ull, 0x100ull, 0x100ull, 0x80ull, 0x8000ull, 0x4ull, 0x2ull, 0x1ull,
    0x20ull, 0x80ull, 0x80ull, 0x78ull, 0x1000ull, 0x80ull, 0x80ull, 0x0ull,
    0x5ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x4d2ull, 0x1ull,
    0x1ull, 0x0ull, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull,
    0x0ull, 0x0ull, 0x309ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull,
    0x0ull, 0x20ull};
constexpr std::array<std::uint64_t, 87> GoldenComputeFull{
    0x0ull, 0x6ull, 0xf41b830be394e99bull, 0x40ull, 0x2ull, 0x6ull, 0x1ull, 0x40ull,
    0x2ull, 0x1ull, 0x100ull, 0x1ull, 0x0ull, 0x1ull, 0x1ull, 0x2ull,
    0x1ull, 0x10ull, 0x0ull, 0x0ull, 0x1ull, 0xc0ull, 0x1ull, 0x0ull,
    0x1ull, 0x1ull, 0x2ull, 0x40ull, 0x88ull, 0x403000ull, 0x10600ull, 0x20ull,
    0x3ull, 0xd902deeb29309d7cull, 0x33ae2159d16a384dull, 0x1ull, 0x400ull, 0x400ull, 0x40ull, 0x400ull,
    0x10000ull, 0x1ull, 0x80ull, 0x1ull, 0x1ull, 0x80ull, 0x7000ull, 0x100ull,
    0x100ull, 0x80ull, 0x8000ull, 0x4ull, 0x2ull, 0x1ull, 0x20ull, 0x80ull,
    0x80ull, 0x78ull, 0x1000ull, 0x80ull, 0x80ull, 0x0ull, 0x5ull, 0x0ull,
    0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x4d2ull, 0x1ull, 0x1ull, 0x0ull,
    0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull,
    0x309ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull};
constexpr std::array<std::uint64_t, 86> GoldenComputePrepared{
    0x0ull, 0x40ull, 0x2ull, 0x6ull, 0x1ull, 0x40ull, 0x2ull, 0x1ull,
    0x100ull, 0x1ull, 0x0ull, 0x1ull, 0x1ull, 0x2ull, 0x1ull, 0x10ull,
    0x0ull, 0x0ull, 0x1ull, 0xc0ull, 0x1ull, 0x0ull, 0x1ull, 0x1ull,
    0x2ull, 0x40ull, 0x88ull, 0x403000ull, 0x10600ull, 0x20ull, 0x3ull, 0xd902deeb29309d7cull,
    0x33ae2159d16a384dull, 0x1ull, 0x400ull, 0x400ull, 0x40ull, 0x400ull, 0x10000ull, 0x1ull,
    0x80ull, 0x1ull, 0x1ull, 0x80ull, 0x7000ull, 0x100ull, 0x100ull, 0x80ull,
    0x8000ull, 0x4ull, 0x2ull, 0x1ull, 0x20ull, 0x80ull, 0x80ull, 0x78ull,
    0x1000ull, 0x80ull, 0x80ull, 0x0ull, 0x5ull, 0x0ull, 0x0ull, 0x0ull,
    0x0ull, 0x0ull, 0x4d2ull, 0x1ull, 0x1ull, 0x0ull, 0x0ull, 0x1ull,
    0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x309ull, 0x0ull,
    0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x0ull, 0x20ull};

struct KeyRequests {
    std::array<std::uint32_t, 6> code{0xbf800000u, 0xf4200100u, 0xfa000000u, 0x7e000204u, 0xf800180fu, 0xbf810000u};
    std::array<std::uint32_t, 6> users{1u, 2u, 3u, 4u, 5u, 6u};
    std::array<std::uint32_t, 2> fallbacks{0x40u, 0x88u};
    std::array<std::uint32_t, 3> capabilities{spv::CapabilityShader, spv::CapabilityInt64, spv::CapabilityFloat16};
    std::array<std::string_view, 2> extensions{"SPV_KHR_physical_storage_buffer", "SPV_EXT_demote_to_helper_invocation"};

    SpirvTarget Target() const {
        SpirvTarget target{};
        target.vulkanVersion = 0x00403000u;
        target.spirvVersion = 0x00010600u;
        target.subgroupSize = 32u;
        target.bdaAbiVersion = 3u;
        target.supportedCapabilities = capabilities;
        target.supportedExtensions = extensions;
        target.fragmentShaderBarycentricEnabled = true;
        target.maxWorkgroupSize = {1024u, 1024u, 64u};
        target.maxWorkgroupInvocations = 1024u;
        target.maxWorkgroupSharedMemoryBytes = 65536u;
        target.mesh = MeshTargetLimits{{128u, 1u, 1u}, 128u, 28672u, 256u, 256u, 128u, 32768u, 4u, 2u};
        target.tessellation = TessellationTargetLimits{32u, 128u, 128u, 120u, 4096u, 128u, 128u};
        target.srgbDecodeFormats = 5u;
        target.registrationWordLimit = 1234u;
        target.pixelFunctionStorageDwords = 777u;
        target.robustBufferAccess2 = true;
        return target;
    }

    RecompileRequest Base(ShaderStage stage) const {
        RecompileRequest request{};
        request.shader = {stage, 0x300000u, code, 0, {}};
        request.context.waveSize = 64u;
        request.context.userDataBaseRegister = 2u;
        request.context.userData = users;
        request.context.floatMode = ShaderFloatMode{0xc0u, true, false, true};
        request.context.linkedCallFallbacks = fallbacks;
        request.target = Target();
        request.layout = {0u, 0u, 0u, 128u};
        return request;
    }

    RecompileRequest Pixel() const {
        auto request = Base(ShaderStage::Fragment);
        request.context.waveSize = 32u;
        ShaderPixelStageInfo pixel{};
        pixel.interpolatorCount = 3u;
        pixel.interpolatorSettings[0] = 0x401u;
        pixel.interpolatorSettings[1] = 0x20u;
        pixel.interpolatorSettings[2] = 0x7u;
        pixel.wave32 = true;
        pixel.inputAddr = 0x302u;
        pixel.depthExportEnable = true;
        pixel.depthExportMin = 0.25f;
        pixel.depthExportMax = 0.75f;
        pixel.targetOutputMode = {9u, 4u, 0u, 0u, 0u, 0u, 0u, 1u};
        pixel.sampleLocations[3] = 0x55u;
        pixel.sampleOrder = {1u, 2u, 3u, 4u};
        TargetMerge merge{};
        merge.target = 1u;
        merge.mask = 0xfu;
        merge.samples = 4u;
        merge.blend.enabled = true;
        merge.blend.snorm = true;
        merge.blend.colorSource = 2u;
        merge.blend.colorDestination = 3u;
        merge.blend.colorOperation = 1u;
        merge.blend.alphaSource = 4u;
        merge.blend.alphaDestination = 5u;
        merge.blend.alphaOperation = 2u;
        merge.blend.writeMask = 0x7u;
        merge.blend.constants = {0x3f800000u, 0u, 0x3f000000u, 0x40000000u};
        pixel.targetMerges.push_back(merge);
        request.context.pixel = pixel;
        request.graphics = GraphicsCompileContext{2u, {}, MeshConfiguration{3u, 64u, 128u, 256u, 128u, 64u, 512u, 1u}, std::nullopt, {0u, 3u, 4u, 1u}, std::nullopt};
        return request;
    }

    RecompileRequest Vertex() const {
        auto request = Base(ShaderStage::Vertex);
        ShaderVertexStageInfo vertex{};
        vertex.fetchAttribReg = 6u;
        vertex.fetchBufferReg = 8u;
        vertex.paClVsOutCntl = 0x3fu;
        vertex.depthRemapMode = 1u;
        vertex.pointList = true;
        vertex.pointSize = 2.0f;
        vertex.pointSizeMin = 1.0f;
        vertex.pointSizeMax = 8.0f;
        vertex.resourcesNum = 2u;
        vertex.resources[0].fields = {0x1000u, 0x20u, 0x40u, 0x00027facu};
        vertex.resources[1].fields = {0x2000u, 0x10u, 0x80u, 0x0002cfacu};
        vertex.resourcesDst[0] = {4u, 4u, 0u, 1u};
        vertex.resourcesDst[1] = {8u, 2u, 1u, 0u};
        request.context.vertex = vertex;
        return request;
    }

    RecompileRequest Evaluation() const {
        auto request = Base(ShaderStage::TessellationEvaluation);
        request.graphics = GraphicsCompileContext{2u, {}, MeshConfiguration{3u, 64u, 128u, 256u, 128u, 64u, 512u, 1u, 24u, 3u}, TessellationConfiguration{3u, 4u, 1u, 2u, 3u, 128u, 16u, false, 0u, true, true}, {0u, 12u, 4u, 3u}, std::nullopt};
        return request;
    }

    RecompileRequest Mesh() const {
        auto request = Base(ShaderStage::Mesh);
        MeshBarycentricLayout barycentrics{{true, true, false, true}, {{0u, 1u, false, true}, {1u, 3u, true, false}}};
        request.graphics = GraphicsCompileContext{2u, {}, MeshConfiguration{3u, 64u, 128u, 256u, 128u, 64u, 512u, 1u, 24u, 3u}, TessellationConfiguration{3u, 4u, 1u, 2u, 3u}, {0u, 12u, 4u, 3u}, barycentrics};
        return request;
    }

    RecompileRequest Compute() const {
        auto request = Base(ShaderStage::Compute);
        request.context.compute = ShaderComputeStageInfo{{64u, 2u, 1u}, 256u, {true, false, true}, true, 2u, {33u, 1u, 1u}, 16u};
        request.context.convertedSamples = true;
        return request;
    }

    std::array<RecompileRequest, 5> All() const {
        return {Pixel(), Vertex(), Evaluation(), Mesh(), Compute()};
    }
};

void CheckGoldenKeys() {
    const KeyRequests requests;
    const std::array<const char*, 5> names{"pixel", "vertex", "tessellation evaluation", "mesh", "compute"};
    const std::array<std::span<const std::uint64_t>, 5> full{GoldenPixelFull, GoldenVertexFull, GoldenEvaluationFull, GoldenMeshFull, GoldenComputeFull};
    const std::array<std::span<const std::uint64_t>, 5> prepared{GoldenPixelPrepared, GoldenVertexPrepared, GoldenEvaluationPrepared, GoldenMeshPrepared, GoldenComputePrepared};
    const auto all = requests.All();
    for (std::size_t index = 0; index < all.size(); ++index) {
        for (const bool interface : {false, true}) {
            std::vector<std::uint64_t> key{7u, 8u};
            if (interface) BuildPreparedShaderKey(all[index], key);
            else RecompileCacheKey::Build(all[index], key);
            const auto golden = interface ? prepared[index] : full[index];
            const std::string what = std::string(names[index]) + (interface ? " interface key" : " cache key");
            Require(!key.empty() && key.front() == RuntimeAbi::Version, what + " does not start with the runtime ABI version");
            Require(key.size() == golden.size() + 1u, what + " has " + std::to_string(key.size()) + " words; the key before staged appends had " + std::to_string(golden.size() + 1u));
            for (std::size_t word = 0; word < golden.size(); ++word) {
                Require(key[word + 1u] == golden[word], what + " differs from the key before staged appends at word " + std::to_string(word + 1u));
            }
        }
    }
}

void CheckFailedMatchKeepsRequest() {
    PixelStage stage;
    static_cast<void>(stage.Invocation(1u));
    const auto handle = stage.snapshot.prepared->entries.front().handle;
    auto request = stage.request;
    request.shader.code = GetPreparedCode(*handle);
    TargetMerge merge{};
    merge.target = 2u;
    merge.mask = 0xfu;
    request.context.pixel->targetMerges = {merge, merge};
    request.graphics = GraphicsCompileContext{0u, {}, std::nullopt, std::nullopt, {0u, 3u, 4u, 1u}, MeshBarycentricLayout{{true, true, false, false}, {{0u, 1u, false, false}, {1u, 2u, true, false}, {2u, 0u, false, true}}}};
    std::vector<std::uint64_t> key;
    BuildPreparedShaderKey(request, key);
    const auto code = request.shader.code;
    Require(!PreparedShaderInvocation::TryCreate(std::move(request), handle, key).has_value(), "a request with other target merges matched an artifact prepared without them");
    Require(request.context.pixel.has_value() && request.context.pixel->targetMerges.size() == 2u && request.context.pixel->targetMerges[1].target == 2u, "a failed match took the target merges of the request");
    Require(request.graphics.has_value() && request.graphics->meshBarycentrics.has_value() && request.graphics->meshBarycentrics->parameters.size() == 3u && request.graphics->meshBarycentrics->parameters[2].sourceLocation == 0u, "a failed match took the barycentric parameters of the request");
    Require(request.shader.code.data() == code.data() && request.context.userData.data() == stage.users.data(), "a failed match changed the spans of the request");
    request.context.pixel->targetMerges.clear();
    BuildPreparedShaderKey(request, key);
    auto matched = PreparedShaderInvocation::TryCreate(std::move(request), handle, key);
    Require(matched.has_value() && matched->Request().graphics->meshBarycentrics->parameters.size() == 3u, "a matching request did not carry its barycentric parameters into the invocation");
}

void CheckDifferingCandidateRestoresSource() {
    PixelStage stage(PixelCode, std::vector<std::uint32_t>(1u), 0x210000u);
    const std::vector<std::uint32_t> otherCode{0xbf800001u, 0x7e000200u, 0xf800180fu, 0u, 0xbf800000u, 0xbf810000u};
    auto other = stage.request;
    other.shader.code = otherCode;
    other.shader.codeAddress = 0x220000u;
    other.context.pixel->targetOutputMode[0] = 4u;
    stage.snapshot.prepared->entries.push_back({0u, PrepareShader(other)});
    const auto& kept = stage.request;
    const auto invocation = stage.Invocation(1u);
    Require(stage.snapshot.prepared->entries.size() == 2u, "a candidate with other code and another interface was used, or nothing was prepared at use");
    const auto prepared = GetPreparedCode(*stage.snapshot.prepared->entries.back().handle);
    Require(std::ranges::equal(prepared, stage.snapshot.code), "the artifact prepared at use after a differing candidate was not prepared from the shader's own code");
    Require(invocation.Request().shader.code.data() == prepared.data(), "the invocation after a differing candidate does not refer to the artifact prepared for it");
    Require(kept.shader.code.data() == stage.snapshot.code.data() && kept.context.pixel->targetOutputMode[0] == 9u, "a differing candidate changed the draw's request");
}

void CheckConcurrentSubstitutes() {
    constexpr std::size_t slots = 48;
    constexpr std::size_t writers = 6;
    constexpr std::size_t rounds = 300;
    for (std::size_t round = 0; round < rounds; ++round) {
        BufferSubstituteTable table{std::vector<BufferSubstitute>(slots)};
        std::atomic<bool> start{false};
        std::atomic<bool> finished{false};
        std::atomic<std::size_t> changed{0};
        std::atomic<std::size_t> regressed{0};
        std::vector<std::thread> threads;
        for (std::size_t writer = 0; writer < writers; ++writer) {
            threads.emplace_back([&, writer] {
                while (!start.load()) std::this_thread::yield();
                for (std::size_t slot = 0; slot < slots; ++slot) {
                    const auto tag = static_cast<std::uint32_t>(writer + 1u);
                    table.Learn((slot + writer * 7u) % slots, {tag, tag << 8u, tag << 2u, false});
                }
            });
        }
        std::thread reader([&] {
            std::vector<BufferSubstitute> previous(slots);
            while (!start.load()) std::this_thread::yield();
            while (!finished.load()) {
                const auto held = table.Slots();
                const auto copy = *held;
                for (std::size_t slot = 0; slot < slots; ++slot) {
                    if (previous[slot].known && !(copy[slot] == previous[slot])) regressed.fetch_add(1u);
                }
                std::this_thread::yield();
                if (!(*held == copy)) changed.fetch_add(1u);
                previous = copy;
            }
        });
        start.store(true);
        for (auto& thread : threads) thread.join();
        finished.store(true);
        reader.join();
        Require(changed.load() == 0u, "slots a reader held changed while substitutes were learned");
        Require(regressed.load() == 0u, "a learned substitute was lost or replaced in a later read");
        const auto final = table.Slots();
        for (std::size_t slot = 0; slot < slots; ++slot) {
            const auto& value = (*final)[slot];
            Require(value.known && value.word1 >= 1u && value.word1 <= writers && value.word3 == value.word1 << 8u && value.misalignment == value.word1 << 2u, "slot " + std::to_string(slot) + " lost its learned substitute or mixed two of them in round " + std::to_string(round));
        }
    }
}

std::vector<std::uint32_t> BufferPixelCode(std::uint32_t variant) {
    return {0xbf800000u | variant, 0xf4200100u, 0xfa000000u, 0xbf8cc07fu, 0x7e000204u, 0xf800180fu, 0u, 0xbf810000u};
}

std::vector<std::uint32_t> TablePixelCode(std::uint32_t variant) {
    return {0xbf800000u | variant, 0xf4080100u, 0xfa000000u, 0xbf8cc07fu, 0xf4200202u, 0xfa000000u, 0xbf8cc07fu, 0x7e000208u, 0xf800180fu, 0u, 0xbf810000u};
}

std::vector<std::uint32_t> PointerWords(const void* table) {
    const auto address = reinterpret_cast<std::uintptr_t>(table);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u)};
}

void RequireSeen(PixelStage& stage, const std::shared_ptr<const RecompileResult>& before, const std::string& what, auto&& change, auto&& restore) {
    change();
    const auto after = stage.Current();
    Require(after != before, what + " did not reach the result memo: the old result was reused");
    RequireSameResult(*after, *stage.CurrentUncached(), what);
    restore();
    Require(stage.Current() == before, what + ": restoring it did not reuse the first result");
}

void CheckMemoHashInputs() {
    const auto code = BufferPixelCode(0x21u);
    PixelStage stage(code, {0x10000000u, 0u, 0x40u, 0x00027facu}, 0x230000u);
    const auto first = stage.Current();
    RequireSameResult(*first, *stage.CurrentUncached(), "the buffer fixture");
    Require(stage.Current() == first, "the buffer fixture did not reuse its memoized result");
    RequireSeen(stage, first, "reversed blending", [&] { stage.request.context.pixel->reversedBlend = 1u; }, [&] { stage.request.context.pixel->reversedBlend = 0u; });
    RequireSeen(stage, first, "a target export mapping", [&] { stage.request.context.pixel->targetExportMapping[0] = 1u; }, [&] { stage.request.context.pixel->targetExportMapping[0] = 0u; });
    alignas(64) static std::array<std::uint32_t, 4> table{0x10000000u, 0u, 0x40u, 0x00027facu};
    const auto tableCode = TablePixelCode(0x22u);
    PixelStage indirect(tableCode, PointerWords(table.data()), 0x240000u);
    const auto loaded = indirect.Current();
    RequireSameResult(*loaded, *indirect.CurrentUncached(), "the fixture that loads its buffer from a table");
    Require(indirect.Current() == loaded, "the table fixture did not reuse its memoized result");
    RequireSeen(indirect, loaded, "a moved buffer base in a table", [&] { table[0] = 0x10000100u; }, [&] { table[0] = 0x10000000u; });
    ShaderVertexStageInfo vertex{};
    vertex.resourcesNum = 1u;
    vertex.resources[0].fields = {0x20000000u, 0x10u, 0x100u, 0x00027facu};
    vertex.resourcesDst[0] = {4u, 4u, 0u, 0u};
    stage.request.context.vertex = vertex;
    const auto fetched = stage.Current();
    RequireSameResult(*fetched, *stage.CurrentUncached(), "the fixture with a vertex buffer");
    RequireSeen(stage, fetched, "a moved vertex buffer", [&] { stage.request.context.vertex->resources[0].fields[0] = 0x20000400u; }, [&] { stage.request.context.vertex->resources[0].fields[0] = 0x20000000u; });
}

struct Spread {
    double median;
    double low;
    double high;
    double min;
    double max;
};

Spread Measure(std::size_t batches, std::size_t iterations, auto&& body) {
    std::vector<double> samples;
    for (std::size_t warm = 0; warm < iterations; ++warm) body(warm);
    for (std::size_t batch = 0; batch < batches; ++batch) {
        const auto started = std::chrono::steady_clock::now();
        for (std::size_t iteration = 0; iteration < iterations; ++iteration) body(batch * iterations + iteration);
        const auto elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - started).count();
        samples.push_back(elapsed / static_cast<double>(iterations));
    }
    std::ranges::sort(samples);
    const auto at = [&](double fraction) { return samples[static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1u))]; };
    return {at(0.5), at(0.1), at(0.9), samples.front(), samples.back()};
}

void Print(const char* name, const Spread& spread) {
    std::printf("%-28s median %8.1f ns  p10 %8.1f  p90 %8.1f  min %8.1f  max %8.1f\n", name, spread.median, spread.low, spread.high, spread.min, spread.max);
}

void Bench() {
    constexpr std::size_t batches = 31;
    constexpr std::size_t iterations = 4000;
    PixelStage stage;
    static_cast<void>(stage.Draw(1u));
    Print("InvocationFor", Measure(batches, iterations, [&](std::size_t) { static_cast<void>(stage.Invocation(1u)); }));
    std::vector<std::uint64_t> key;
    Print("BuildPreparedShaderKey", Measure(batches, iterations, [&](std::size_t) { BuildPreparedShaderKey(stage.request, key); }));
    Print("RecompileRequest copy", Measure(batches, iterations, [&](std::size_t) {
        auto copy = stage.request;
        asm volatile("" : : "r"(&copy) : "memory");
    }));
    auto prepared = stage.request;
    const auto handle = stage.snapshot.prepared->entries.front().handle;
    prepared.shader.code = GetPreparedCode(*handle);
    BuildPreparedShaderKey(prepared, key);
    Print("TryCreate", Measure(batches, iterations, [&](std::size_t) { static_cast<void>(PreparedShaderInvocation::TryCreate(prepared, handle, key)); }));
    Print("stage, memo hit", Measure(batches, iterations, [&](std::size_t) { static_cast<void>(stage.Draw(1u)); }));
    Print("stage, new user data", Measure(batches, iterations, [&](std::size_t index) { static_cast<void>(stage.Draw(static_cast<std::uint32_t>(index) + 2u)); }));
    auto invocation = stage.Invocation(1u);
    const auto capture = AgcDriver::DriverDetail::CaptureInvocation(stage.memory, stage.snapshot, 0, stage.request, invocation);
    Print("Materialize, memo hit", Measure(batches, iterations, [&](std::size_t) { static_cast<void>(invocation.Materialize(*capture)); }));
}

}

int main(int argc, char** argv) {
    try {
        Require(argc <= 2, "invalid test arguments");
        if (argc == 2) {
            Require(std::string_view(argv[1]) == "--bench", "invalid test arguments");
            Bench();
            return 0;
        }
        CheckInterfaceKey();
        CheckGoldenKeys();
        CheckFailedMatchKeepsRequest();
        CheckDifferingCandidateRestoresSource();
        CheckConcurrentSubstitutes();
        CheckMemoHashInputs();
        CheckSubstituteSlots();
        CheckInvocation();
        CheckResultMemo();
        std::cout << "stage recapture tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
