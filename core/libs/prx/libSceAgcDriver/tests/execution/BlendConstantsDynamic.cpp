#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineLibrary.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 16;
constexpr std::uint32_t WaveSize = 64;
alignas(256) std::array<std::byte, Width * Height * 4> Pixels{};

alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{
    0xe0382000, 0x80000005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 11> PixelCode{
    0x7e0802ff, 0x3e4ccccd, 0x7e0a02ff, 0x3ecccccd, 0x7e0c02ff, 0x3f19999a, 0x7e0e02ff, 0x3f4ccccd,
    0xf800180f, 0x07060504, 0xbf810000,
};

constexpr std::array<float, 4> Source{0.2f, 0.4f, 0.6f, 0.8f};

constexpr std::array<std::array<float, 4>, 3> Vertices{{
    {-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}
}};
alignas(256) std::array<std::array<float, 4>, 3> VertexStorage = Vertices;

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t stride, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), count, 0x01016facu};
}

struct Shaders {
    ShaderRecompiler::RecompileResult vertex;
    ShaderRecompiler::RecompileResult pixel;
    std::uint32_t vertexPush = 0;
};

Shaders Compile(AgcDriver::VulkanDevice& device) {
    const auto target = device.Target();

    std::vector<std::uint32_t> vertexUserData(4, 0u);
    const auto vertexBuffer = BufferDescriptor(VertexStorage.data(), 16u, static_cast<std::uint32_t>(VertexStorage.size()));
    std::copy(vertexBuffer.begin(), vertexBuffer.end(), vertexUserData.begin());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {WaveSize, 0, vertexUserData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
        target,
        {0, 0, 0, 64}
    };
    vertex.useCache = false;
    Shaders compiled;
    compiled.vertex = ShaderRecompiler::Recompile(vertex);
    compiled.vertexPush = static_cast<std::uint32_t>(compiled.vertex.pushConstants.size());

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
        {WaveSize, 0, pixelUserData, std::nullopt, pixel, std::nullopt, pixelMemory},
        target,
        PixelPushLayout(compiled.vertexPush, target)
    };
    fragment.useCache = false;
    compiled.pixel = ShaderRecompiler::Recompile(fragment);
    return compiled;
}

std::array<AgcDriver::Graphics::CompiledShader, 2> Stages(AgcDriver::VulkanDevice& device, const Shaders& compiled) {
    return {{
        {ShaderStage::Vertex, &compiled.vertex, 0},
        {ShaderStage::Fragment, &compiled.pixel, PixelPushOffset(compiled.vertexPush, device.Target())}
    }};
}

using Constants = std::array<float, 4>;

AgcDriver::Graphics::State StateFor(const Constants& constants, VkPrimitiveTopology topology) {
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, WaveSize, WaveSize, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size(), 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = topology;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.blendEnable = VK_TRUE;
    state.blend.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
    state.blend.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    state.blend.colorBlendOp = VK_BLEND_OP_ADD;
    state.blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
    state.blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    state.blend.alphaBlendOp = VK_BLEND_OP_ADD;
    state.blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    state.blends = {state.blend};
    state.blendConstants = constants;
    return state;
}

const AgcDriver::Pm4::DrawParameters FullScreen{0, static_cast<std::uint32_t>(VertexStorage.size()), 0, 1, 0, false};

void DrawAndCheck(AgcDriver::VulkanDevice& device, const Shaders& compiled, const Constants& constants, std::string_view what, VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) {
    Pixels.fill(std::byte{0x5a});
    device.Draw(StateFor(constants, topology), FullScreen, Stages(device, compiled));
    device.WaitIdle();
    for (std::uint32_t y = 0; y < Height; ++y) {
        for (std::uint32_t x = 0; x < Width; ++x) {
            for (std::uint32_t channel = 0; channel < 4; ++channel) {
                const auto expected = static_cast<int>(std::lround(Source[channel] * constants[channel] * 255.0f));
                const auto actual = static_cast<int>(Pixels[(y * Width + x) * 4u + channel]);
                Require(std::abs(actual - expected) <= 1, std::string(what) + ": pixel (" + std::to_string(x) + ", " + std::to_string(y) + ") channel " + std::to_string(channel) + " is " + std::to_string(actual) + ", expected " + std::to_string(expected));
            }
        }
    }
}

struct Created {
    std::uint64_t misses;
    std::uint64_t shared;
    std::uint64_t outputParts;
    std::uint64_t linked;
};

Created CreatedOn(VkDevice device) {
    const auto store = AgcDriver::Graphics::PipelineStoreTotals();
    const auto libraries = AgcDriver::Graphics::PipelineLibraryCountersOf(device);
    return {store.misses, store.blendConstantsShared, libraries.built[3], libraries.linked};
}

void ExpectCreated(const Created& before, const Created& after, std::uint64_t misses, std::uint64_t shared, std::uint64_t outputParts, bool libraries, std::string_view what) {
    const auto text = std::string(what) + ": store misses " + std::to_string(after.misses - before.misses) + " (expected " + std::to_string(misses) + "), shared across blend constants " + std::to_string(after.shared - before.shared) + " (expected " + std::to_string(shared) + "), output parts " + std::to_string(after.outputParts - before.outputParts) + ", links " + std::to_string(after.linked - before.linked);
    Require(after.misses - before.misses == misses && after.shared - before.shared == shared, text);
    if (libraries) Require(after.outputParts - before.outputParts == outputParts && after.linked - before.linked == misses, text);
}

}

int main() {
    try {
#ifdef _WIN32
        _putenv_s("APS5_PROFILE_DRAW", "1");
#else
        setenv("APS5_PROFILE_DRAW", "1", 1);
#endif
        using AgcDriver::Graphics::RuntimeSwitch;
        namespace Switches = AgcDriver::Graphics::RuntimeSwitches;
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto vkDevice = device->graphicsContext().device;
        const bool libraries = device->graphicsContext().graphicsPipelineLibrary;
        const auto compiled = Compile(*device);
        constexpr Constants first{0.5f, 1.0f, 0.25f, 0.5f};
        constexpr Constants second{1.0f, 0.5f, 1.0f, 0.25f};
        constexpr Constants third{0.75f, 0.25f, 0.5f, 1.0f};
        Switches::Assign(RuntimeSwitch::StaticBlendConstants, false);
        auto before = CreatedOn(vkDevice);
        DrawAndCheck(*device, compiled, first, "dynamic constants, first draw");
        ExpectCreated(before, CreatedOn(vkDevice), 1u, 0u, 1u, libraries, "the first draw with dynamic blend constants");
        before = CreatedOn(vkDevice);
        DrawAndCheck(*device, compiled, second, "dynamic constants, other constants");
        DrawAndCheck(*device, compiled, first, "dynamic constants, first constants again");
        DrawAndCheck(*device, compiled, third, "dynamic constants, third constants");
        ExpectCreated(before, CreatedOn(vkDevice), 0u, 2u, 0u, libraries, "draws that differ only in their blend constants");
        before = CreatedOn(vkDevice);
        DrawAndCheck(*device, compiled, second, "dynamic constants, a strip of the same triangle", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
        ExpectCreated(before, CreatedOn(vkDevice), 1u, 0u, 0u, libraries, "a draw of another topology with other blend constants, which shares the fragment output part");
        Switches::Assign(RuntimeSwitch::StaticBlendConstants, true);
        before = CreatedOn(vkDevice);
        DrawAndCheck(*device, compiled, first, "constants in the pipeline, first constants");
        DrawAndCheck(*device, compiled, second, "constants in the pipeline, other constants");
        DrawAndCheck(*device, compiled, first, "constants in the pipeline, first constants again");
        ExpectCreated(before, CreatedOn(vkDevice), 2u, 0u, 2u, libraries, "draws with the blend constants in the pipeline");
        Switches::Assign(RuntimeSwitch::StaticBlendConstants, false);
        before = CreatedOn(vkDevice);
        DrawAndCheck(*device, compiled, second, "dynamic constants after the A/B flip back");
        ExpectCreated(before, CreatedOn(vkDevice), 0u, 0u, 0u, libraries, "a draw after flipping back to dynamic blend constants");
        std::printf("blend constant tests passed (%s)\n", libraries ? "pipeline libraries" : "whole pipelines");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
