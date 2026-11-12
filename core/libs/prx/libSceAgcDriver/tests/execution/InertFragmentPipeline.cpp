#include "DepthFastClearHarness.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

using DepthFastClearHarness::Covered;
using DepthFastClearHarness::Height;
using DepthFastClearHarness::Require;
using DepthFastClearHarness::Shaders;
using DepthFastClearHarness::Width;
using ShaderRecompiler::ShaderStage;

alignas(4096) std::array<float, Width * Height * 2> Depth{};
alignas(4096) std::array<std::uint8_t, Width * Height * 2> Stencil{};
alignas(4096) std::array<float, Width * Height * 2> OtherDepth{};
alignas(4096) std::array<std::uint8_t, Width * Height * 2> OtherStencil{};
alignas(256) constexpr std::array<std::uint32_t, 64> NullPixelCode{0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 7> BufferPixelCode{0x7e0a0280u, 0xe0382000u, 0x80000005u, 0xbf8c3f70u, 0xf800180fu, 0x03020100u, 0xbf810000u};
alignas(256) constexpr std::array<float, 4> BufferColor{1.0f, 1.0f, 1.0f, 1.0f};
alignas(256) constexpr std::array<std::uint32_t, 8> ExportDepth{0x7e0002f2, 0xf800100f, 0x00000000, 0x7e0202ff, 0x3e800000, 0xf8001881, 0x00000001, 0xbf810000};

const std::vector<std::array<float, 4>>& HalfTriangle() {
    static const std::vector<std::array<float, 4>> vertices{
        {-1.0f, -1.0f, 0.5f, 1.0f}, {1.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 1.0f, 0.5f, 1.0f},
    };
    return vertices;
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t stride, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), count, 0x01016facu};
}

ShaderRecompiler::RecompileResult CompilePixel(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, std::uint32_t vertexPush, bool exports, bool depthExport, std::span<const std::uint32_t> userWords = {}) {
    const auto target = device.Target();
    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.wave32 = true;
    if (exports) {
        pixel.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionX) | ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionY);
        pixel.posX = true;
        pixel.posY = true;
        pixel.targetOutputMode[0] = 9;
        pixel.targetExportMapping.fill(0xe4u);
    }
    pixel.depthExportEnable = depthExport;
    pixel.depthExportMin = 0.0f;
    pixel.depthExportMax = 1.0f;
    std::vector<std::uint32_t> userData(4, 0u);
    std::copy(userWords.begin(), userWords.end(), userData.begin());
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, std::nullopt, pixel, std::nullopt, memory},
        target,
        PixelPushLayout(vertexPush, target)
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

struct Pass {
    bool color = false;
    bool depthTest = true;
    bool depthWrite = false;
    VkCompareOp depthCompare = VK_COMPARE_OP_ALWAYS;
    float depth = 0.5f;
    bool stencilTest = false;
    VkStencilOpState stencil{};
    bool depthBounds = false;
    float minBound = 0.0f;
    float maxBound = 1.0f;
    std::uint16_t sampleMask = 0xffffu;
    bool otherDepth = false;
};

alignas(256) std::array<std::byte, Width * Height * 4> Pixels{};

std::uint32_t Draw(AgcDriver::VulkanDevice& device, const Shaders& vertex, const ShaderRecompiler::RecompileResult& pixel, const Pass& pass, bool wait = true) {
    std::fill(Pixels.begin(), Pixels.end(), std::byte{0});
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertex.vertex, 0},
        {ShaderStage::Fragment, &pixel, PixelPushOffset(vertex.vertexPush, device.Target())}
    }};
    const VkExtent2D extent{Width, Height};
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 32u, 32u, std::nullopt, std::nullopt};
    if (pass.color) {
        state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), extent, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size(), 0xe4u};
        state.colors = {state.color};
        state.hasColorTarget = true;
        state.blend.colorWriteMask = 15;
        state.blends = {state.blend};
    }
    state.depth = AgcDriver::Graphics::DepthTarget{reinterpret_cast<std::uintptr_t>(pass.otherDepth ? OtherDepth.data() : Depth.data()), reinterpret_cast<std::uintptr_t>(pass.otherDepth ? OtherStencil.data() : Stencil.data()), extent, VK_FORMAT_D32_SFLOAT_S8_UINT, 1.0f, 0, 0, false};
    state.depthTest = pass.depthTest;
    state.depthWrite = pass.depthWrite;
    state.depthCompare = pass.depthCompare;
    state.stencilTest = pass.stencilTest;
    state.stencilFront = pass.stencil;
    state.stencilBack = pass.stencil;
    state.depthBoundsTest = pass.depthBounds;
    state.minDepthBounds = pass.minBound;
    state.maxDepthBounds = pass.maxBound;
    state.quadSampleMask.fill(pass.sampleMask);
    state.renderExtent = extent;
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), pass.depth, pass.depth};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, extent};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blendConstants = {};
    const AgcDriver::Pm4::DrawParameters draw{0, 3u, 0, 1, 0, false};
    device.Draw(state, draw, shaders);
    if (!wait) return 0;
    device.WaitIdle();
    std::uint32_t covered = 0;
    for (std::size_t index = 0; index < Pixels.size() / 4u; ++index) {
        if (std::to_integer<std::uint8_t>(Pixels[index * 4u]) == 255u) ++covered;
    }
    return covered;
}

VkStencilOpState StencilOps(VkCompareOp compare, VkStencilOp pass, VkStencilOp depthFail, std::uint32_t reference) {
    VkStencilOpState ops{};
    ops.failOp = VK_STENCIL_OP_KEEP;
    ops.passOp = pass;
    ops.depthFailOp = depthFail;
    ops.compareOp = compare;
    ops.compareMask = 0xffu;
    ops.writeMask = 0xffu;
    ops.reference = reference;
    return ops;
}

class Samples {
public:
    explicit Samples(AgcDriver::VulkanDevice& device) : context(AgcDriver::VulkanDeviceTestAccess::ContextOf(device)) {}

    bool Available() const { return context.recorder != nullptr && context.occlusionQueryPrecise; }

    std::uint64_t Total() {
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        context.recorder->CountSamples();
        return context.recorder->SamplesTotal();
    }

private:
    AgcDriver::Graphics::Context context;
};

std::uint64_t SetsAllocated(AgcDriver::VulkanDevice& device) {
    return AgcDriver::VulkanDeviceTestAccess::ContextOf(device).descriptorCache->Counters().sets;
}

std::uint32_t Equal(AgcDriver::VulkanDevice& device, const Shaders& full, const ShaderRecompiler::RecompileResult& plain, float depth) {
    return Draw(device, full, plain, {.color = true, .depthTest = true, .depthWrite = false, .depthCompare = VK_COMPARE_OP_EQUAL, .depth = depth});
}

std::uint32_t StencilEqual(AgcDriver::VulkanDevice& device, const Shaders& full, const ShaderRecompiler::RecompileResult& plain, std::uint32_t reference) {
    return Draw(device, full, plain, {.color = true, .depthTest = false, .stencilTest = true, .stencil = StencilOps(VK_COMPARE_OP_EQUAL, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, reference)});
}

void Reset(AgcDriver::VulkanDevice& device, const Shaders& full, const ShaderRecompiler::RecompileResult& plain) {
    Require(Draw(device, full, plain, {.color = true, .depthTest = true, .depthWrite = true, .depth = 1.0f, .stencilTest = true, .stencil = StencilOps(VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, 0u)}) == Covered, "the reset draw did not cover the target");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const bool kept = std::getenv("APS5_KEEP_INERT_FRAGMENT") != nullptr;
        const auto full = DepthFastClearHarness::Compile(*device);
        const auto half = DepthFastClearHarness::Compile(*device, HalfTriangle().data());
        const auto plain = CompilePixel(*device, DepthFastClearHarness::PixelCode, full.vertexPush, true, false);
        const auto null = CompilePixel(*device, NullPixelCode, full.vertexPush, false, false);
        const auto depthExport = CompilePixel(*device, ExportDepth, full.vertexPush, true, true);
        Require(AgcDriver::Graphics::InertFragment(null), "the null pixel program is not an inert fragment program");
        Require(!AgcDriver::Graphics::InertFragment(plain), "a color-exporting pixel program is inert");
        Require(!AgcDriver::Graphics::InertFragment(depthExport), "a depth-exporting pixel program is inert");
        Require(AgcDriver::Graphics::InertFragment(null), "the null pixel program's memoized answer changed");
        auto replaced = null;
        replaced.spirv = plain.spirv;
        Require(!AgcDriver::Graphics::InertFragment(replaced), "a copied result kept its source's inert answer for other SPIR-V");
        const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(*device);
        const std::array<AgcDriver::Graphics::CompiledShader, 2> inertStages{{{ShaderStage::Vertex, &full.vertex, 0}, {ShaderStage::Fragment, &null, PixelPushOffset(full.vertexPush, device->Target())}}};
        Require(AgcDriver::Graphics::SplitsSets(context, inertStages) == context.graphicsPipelineLibrary, "an inert fragment program did not split the descriptor sets, so its fragment library would be keyed by the vertex program's layout");

        Reset(*device, full, plain);
        const auto expected = Draw(*device, half, plain, {.color = true, .depthTest = false});
        Require(expected != 0u && expected != Covered, "the half triangle does not cover part of the target");
        Samples samples(*device);
        static_cast<void>(AgcDriver::Graphics::TakeInertFragmentCounts());

        Reset(*device, full, plain);
        const auto before = samples.Available() ? samples.Total() : 0u;
        Draw(*device, half, null, {.depthTest = true, .depthWrite = true, .depth = 0.25f, .stencilTest = true, .stencil = StencilOps(VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, 7u)});
        if (samples.Available()) Require(samples.Total() - before == expected, "the depth-writing inert draw counted other samples than it covers");
        Require(Equal(*device, full, plain, 0.25f) == expected, "the inert draw wrote depth to other pixels than it covers");
        Require(StencilEqual(*device, full, plain, 7u) == expected, "the inert draw wrote stencil to other pixels than it covers");

        const auto tested = samples.Available() ? samples.Total() : 0u;
        Draw(*device, full, null, {.depthTest = true, .depthWrite = false, .depthCompare = VK_COMPARE_OP_LESS, .depth = 0.5f, .stencilTest = true, .stencil = StencilOps(VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_REPLACE, 3u)});
        if (samples.Available()) Require(samples.Total() - tested == Covered - expected, "the depth-test-only inert draw counted other samples than pass its depth test");
        Require(StencilEqual(*device, full, plain, 3u) == expected, "the depth-test-only inert draw did not replace stencil where its depth test failed");
        Require(StencilEqual(*device, full, plain, 1u) == Covered - expected, "the depth-test-only inert draw did not increment stencil where its depth test passed");
        Require(Equal(*device, full, plain, 0.25f) == expected, "the depth-test-only inert draw changed depth");

        if (AgcDriver::VulkanDeviceTestAccess::ContextOf(*device).depthBounds) {
            Draw(*device, full, null, {.depthTest = true, .depthWrite = true, .depth = 0.75f, .depthBounds = true, .minBound = 0.2f, .maxBound = 0.3f});
            Require(Equal(*device, full, plain, 0.75f) == expected, "the inert draw ignored the depth bounds test");
            Require(Equal(*device, full, plain, 1.0f) == Covered - expected, "the inert draw wrote depth outside its depth bounds");
        }

        const auto masked = samples.Available() ? samples.Total() : 0u;
        Draw(*device, full, null, {.depthTest = true, .depthWrite = true, .depth = 0.1f, .sampleMask = 0u});
        if (samples.Available()) Require(samples.Total() == masked, "the inert draw under an empty sample mask counted samples");
        Require(Equal(*device, full, plain, 0.1f) == 0u, "the inert draw under an empty sample mask wrote depth");

        const auto counts = AgcDriver::Graphics::TakeInertFragmentCounts();
        Require(kept ? counts.omitted == 0u && counts.kept != 0u : counts.omitted != 0u && counts.kept == 0u, "the depth-only inert draws were not recorded with the expected fragment stage");

        Draw(*device, full, null, {.color = true, .depthTest = false});
        const auto colored = AgcDriver::Graphics::TakeInertFragmentCounts();
        Require(colored.omitted == 0u && colored.kept == 1u, "an inert draw with a writable color attachment dropped its fragment stage");

        const auto colorBuffer = BufferDescriptor(BufferColor.data(), 16u, 1u);
        const auto reading = CompilePixel(*device, BufferPixelCode, full.vertexPush, true, false, colorBuffer);
        Require(!AgcDriver::Graphics::InertFragment(reading), "a buffer-reading pixel program is inert");
        Require(!reading.bindings.empty(), "the buffer-reading pixel program has no descriptor binding");
        const std::array<AgcDriver::Graphics::CompiledShader, 2> readingStages{{{ShaderStage::Vertex, &full.vertex, 0}, {ShaderStage::Fragment, &reading, PixelPushOffset(full.vertexPush, device->Target())}}};
        Require(AgcDriver::Graphics::SplitsSets(context, readingStages) == context.graphicsPipelineLibrary, "a buffer-reading pixel program did not split the descriptor sets under graphics pipeline libraries");
        Reset(*device, full, plain);
        const Pass depthOnly{.depthTest = true, .depthWrite = true, .depth = 0.5f};
        for (int round = 0; round < 2; ++round) {
            Draw(*device, full, null, depthOnly, false);
            Draw(*device, full, reading, {.depthTest = true, .depthWrite = true, .depth = 0.25f}, false);
        }
        Draw(*device, full, null, depthOnly);
        Require(Equal(*device, full, plain, 0.5f) == Covered, "a pass that switched between an inert depth-only pipeline and a buffer-reading one left other depth than its last draw");
        Require(Draw(*device, full, reading, {.color = true, .depthTest = false}) == Covered, "the buffer-reading pixel program did not read its color from its fragment set");
        const auto inertBefore = SetsAllocated(*device);
        for (int i = 0; i < 4; ++i) Draw(*device, full, null, depthOnly);
        const auto inertSets = SetsAllocated(*device) - inertBefore;
        const auto readingBefore = SetsAllocated(*device);
        for (int i = 0; i < 4; ++i) Draw(*device, full, reading, depthOnly);
        const auto readingSets = SetsAllocated(*device) - readingBefore;
        Require(readingSets - inertSets == (context.graphicsPipelineLibrary ? 4u : 0u), "draws whose fragment program has no bindings allocated a fragment descriptor set");
        AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::KeepEmptyFragmentSet, true);
        const auto keptBefore = SetsAllocated(*device);
        for (int i = 0; i < 4; ++i) Draw(*device, full, null, depthOnly);
        const auto keptSets = SetsAllocated(*device) - keptBefore;
        AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::KeepEmptyFragmentSet, false);
        Require(keptSets == readingSets, "APS5_KEEP_EMPTY_FRAGMENT_SET did not allocate the empty fragment set again");

        const bool timed = AgcDriver::Graphics::Recorder::GpuTimingEnabled();
        if (timed) {
            const auto before = AgcDriver::Graphics::Recorder::DrawFragmentTimes();
            for (int i = 0; i < 3; ++i) Draw(*device, full, null, {.depthTest = true, .depthWrite = true, .depth = 0.5f});
            for (int i = 0; i < 2; ++i) Draw(*device, full, plain, {.depthTest = true, .depthWrite = true, .depth = 0.5f});
            Draw(*device, full, null, {.depthTest = true, .depthWrite = true, .depth = 0.5f}, false);
            Draw(*device, full, plain, {.depthTest = true, .depthWrite = true, .depth = 0.5f});
            const auto after = AgcDriver::Graphics::Recorder::DrawFragmentTimes();
            Require(after.draws - before.draws == 7u, "the [gputime] draw ranges did not hold every draw of the mix");
            Require(after.inert - before.inert == 4u, "the [gputime] draw ranges did not count the draws with an inert fragment program");
            Require(after.omitted - before.omitted == (kept ? 0u : 4u), "the [gputime] draw ranges did not count the draws recorded without the fragment stage");
            Require(after.inertRanges - before.inertRanges == 3u, "the [gputime] inert-only draw ranges were not the passes made of inert fragment draws alone");
            Require(after.inertMs > before.inertMs, "the [gputime] inert-only draw ranges took no GPU time");
            const auto ended = [](const auto& totals, const auto& earlier, AgcDriver::Graphics::PassBreak by) { return totals.inertBreakRanges[static_cast<std::size_t>(by)] - earlier.inertBreakRanges[static_cast<std::size_t>(by)]; };
            using Break = AgcDriver::Graphics::PassBreak;
            Require(ended(after, before, Break::NewBatch) == 3u, "the [gputime] inert-only draw ranges were not told to end at their batch's wait");
            const auto keyBefore = AgcDriver::Graphics::Recorder::DrawFragmentTimes();
            Draw(*device, full, null, {.depthTest = true, .depthWrite = true, .depth = 0.5f}, false);
            Draw(*device, full, null, {.depthTest = true, .depthWrite = true, .depth = 0.5f, .otherDepth = true}, false);
            Draw(*device, full, null, {.depthTest = true, .depthWrite = true, .depth = 0.5f, .otherDepth = true});
            const auto keyAfter = AgcDriver::Graphics::Recorder::DrawFragmentTimes();
            Require(ended(keyAfter, keyBefore, Break::Command) == 1u && ended(keyAfter, keyBefore, Break::NewBatch) == 1u, "the [gputime] inert-only draw range of a pass left for another depth target did not end by command, or the last one by its batch");
            Require(keyAfter.inertRanges - keyBefore.inertRanges == 2u, "the [gputime] inert-only draw ranges did not count both passes of the depth target change");
            const auto syncBefore = AgcDriver::Graphics::Recorder::DrawFragmentTimes();
            for (int i = 0; i < 3; ++i) Draw(*device, full, plain, {.color = true, .depthTest = false});
            const auto syncAfter = AgcDriver::Graphics::Recorder::DrawFragmentTimes();
            Require(syncAfter.syncRanges - syncBefore.syncRanges == 3u, "the [gputime] draw-sync class did not hold one range per synchronous draw");
            Require(syncAfter.syncMs > syncBefore.syncMs, "the [gputime] draw-sync ranges took no GPU time");
            Require(syncAfter.draws == syncBefore.draws, "a synchronous draw was counted in the recorded draw ranges");
        }

        std::printf("inert fragment pipeline tests passed (fragment stage %s, occlusion samples %s, [gputime] draw fragment share %s)\n", kept ? "kept" : "omitted", samples.Available() ? "checked" : "not checked", timed ? "checked" : "not checked");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
