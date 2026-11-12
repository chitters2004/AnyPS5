#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "DepthFastClearHarness.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

using namespace AgcDriver::Graphics;
using DepthFastClearHarness::Compile;
using DepthFastClearHarness::Height;
using DepthFastClearHarness::Shaders;
using DepthFastClearHarness::Triangles;
using DepthFastClearHarness::Width;
using ShaderRecompiler::ShaderStage;

alignas(4096) std::array<float, Width * Height> DepthA{};
alignas(4096) std::array<float, Width * Height> DepthB{};

void Draw(const Context& context, AgcDriver::VulkanDevice& device, const Shaders& compiled, std::uint32_t width, float* depth, bool depthWrite) {
    const std::array<CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &compiled.vertex, 0},
        {ShaderStage::Fragment, &compiled.pixel, PixelPushOffset(compiled.vertexPush, device.Target())}
    }};
    State state{};
    state.stages = {ShaderPath::Vertex, 0u, 32u, 32u, std::nullopt, std::nullopt};
    state.depth = DepthTarget{reinterpret_cast<std::uintptr_t>(depth), 0, {Width, Height}, VK_FORMAT_D32_SFLOAT, 1.0f, 0, 0, false, 1};
    state.depthTest = true;
    state.depthWrite = depthWrite;
    state.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
    state.samples = 1;
    state.renderExtent = {width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(width), -static_cast<float>(Height), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(Triangles().size()), 0, 1, 0, false};
    AgcDriver::Graphics::Draw(context, state, draw, shaders);
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (!Recorder::MergeBarriers()) {
            std::puts("skipped, barrier elision is off");
            return VulkanTestSkipped;
        }
        const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(*device);
        const auto shaders = Compile(*device);
        device->WaitIdle();
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Recorder recorder(context);
        recorder.Activate();
        for (const auto width : {Width, Width / 2}) {
            for (auto* depth : {DepthA.data(), DepthB.data()}) {
                for (const bool write : {true, false}) Draw(context, *device, shaders, width, depth, write);
            }
        }
        recorder.Submit();
        recorder.Sync();
        const auto pair = [&](float* firstDepth, float* secondDepth, bool depthWrite) {
            recorder.Submit();
            recorder.Sync();
            Draw(context, *device, shaders, Width, firstDepth, depthWrite);
            const auto before = Recorder::FusionCounts();
            Draw(context, *device, shaders, Width / 2, secondDepth, depthWrite);
            const auto after = Recorder::FusionCounts();
            recorder.Submit();
            recorder.Sync();
            return std::array<std::uint64_t, 3>{after.fencedKeyOnly - before.fencedKeyOnly, after.fencedKeyOnlyShared - before.fencedKeyOnlyShared, after.fencedKeyOnlyApart - before.fencedKeyOnlyApart};
        };
        const auto written = pair(DepthA.data(), DepthA.data(), true);
        Require(written[0] == 0, "depth pass attachment: two passes writing one depth image broke as key-only, so the draw did not list its depth image as a written attachment");
        const auto read = pair(DepthA.data(), DepthA.data(), false);
        Require(read[0] == 1 && read[1] == 1 && read[2] == 0, "depth pass attachment: two passes only testing one depth image did not break as key-only sharing a read-only attachment (key-only " + std::to_string(read[0]) + ", shared " + std::to_string(read[1]) + ", none " + std::to_string(read[2]) + ")");
        const auto apart = pair(DepthA.data(), DepthB.data(), true);
        Require(apart[0] == 1 && apart[1] == 0 && apart[2] == 1, "depth pass attachment: two passes on different depth images did not break as key-only sharing no attachment (key-only " + std::to_string(apart[0]) + ", shared " + std::to_string(apart[1]) + ", none " + std::to_string(apart[2]) + ")");
        std::puts("depth pass attachment tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
