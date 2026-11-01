#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "Recompiler.hpp"
#include "BdaAbi.hpp"
#include "CacheKey.hpp"
#include "CompiledVariant.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
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
    ShaderSnapshot snapshot{0x200000u, 0, 1, {PixelCode.begin(), PixelCode.end()}, {}};
    std::array<std::uint32_t, 1> users{};
    RecompileRequest request{};
    AgcDriver::ShaderMemory memory{{}};

    PixelStage() {
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
