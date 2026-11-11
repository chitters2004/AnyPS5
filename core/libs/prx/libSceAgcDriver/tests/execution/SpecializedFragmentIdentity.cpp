#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
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

alignas(256) std::array<std::array<float, 4>, 3> VertexStorage{{
    {-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}
}};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t stride, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), count, 0x01016facu};
}

struct Drawn {
    std::vector<std::byte> pixels;
    bool registration = false;
};

Drawn Draw(AgcDriver::VulkanDevice& device, std::uint8_t mapping, bool specialized) {
    std::fill(Pixels.begin(), Pixels.end(), std::byte{0x5a});
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
    const auto vertexResult = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());

    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionX) | ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionY);
    pixel.posX = true;
    pixel.posY = true;
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    pixel.targetExportMapping[0] = mapping;
    const std::vector<std::uint32_t> pixelUserData(8, 0u);
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(PixelCode.data()), std::as_bytes(std::span(PixelCode))}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}},
        {WaveSize, 0, pixelUserData, std::nullopt, pixel, std::nullopt, pixelMemory},
        target,
        PixelPushLayout(vertexPush, target)
    };
    if (specialized) fragment.target.registrationWordLimit = 0u;
    fragment.useCache = false;
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    Require(!specialized || !pixelResult.registrationModule, "a word limit of 0 still emitted the registration form");
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertexResult, 0},
        {ShaderStage::Fragment, &pixelResult, PixelPushOffset(vertexPush, target)}
    }};

    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, WaveSize, WaveSize, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size(), mapping};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(VertexStorage.size()), 0, 1, 0, false};
    device.Draw(state, draw, shaders);
    device.WaitIdle();
    return {{Pixels.begin(), Pixels.end()}, pixelResult.registrationModule};
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const bool registrationDevice = device->Target().registrationModules && device->Target().registrationWordLimit != 0u;
        Require(std::getenv("APS5_SPECIALIZED_FRAGMENTS") == nullptr || device->Target().registrationWordLimit == 0u, "APS5_SPECIALIZED_FRAGMENTS did not select specialized fragment shaders");
        std::vector<std::byte> identity;
        bool remapped = false;
        for (const std::uint8_t mapping : {std::uint8_t{0xe4u}, std::uint8_t{0xc6u}, std::uint8_t{0x1bu}, std::uint8_t{0x39u}}) {
            const auto runtime = Draw(*device, mapping, false);
            const auto specialized = Draw(*device, mapping, true);
            char what[96];
            std::snprintf(what, sizeof(what), "export mapping 0x%02x", mapping);
            Require(runtime.registration == registrationDevice, std::string(what) + ": the default fragment shader was not in the device's module form");
            Require(std::ranges::all_of(runtime.pixels, [](std::byte value) { return value != std::byte{0x5a}; }), std::string(what) + ": the draw did not cover the target");
            for (std::size_t index = 0; index < runtime.pixels.size(); ++index) {
                if (runtime.pixels[index] == specialized.pixels[index]) continue;
                const auto texel = index / 4u;
                std::snprintf(what, sizeof(what), "export mapping 0x%02x: pixel (%zu, %zu) channel %zu", mapping, texel % Width, texel / Width, index % 4u);
                throw std::runtime_error(std::string(what) + " differs between the registration and specialized fragment shaders: " + std::to_string(static_cast<unsigned>(runtime.pixels[index])) + " vs " + std::to_string(static_cast<unsigned>(specialized.pixels[index])));
            }
            if (mapping == 0xe4u) identity = runtime.pixels;
            else remapped |= runtime.pixels != identity;
        }
        Require(remapped, "no export mapping changed the stored pixels, so the mapping paths were not compared");
        std::puts(registrationDevice ? "specialized fragment identity tests passed" : "specialized fragment identity tests passed (the device has no registration modules, both forms are specialized)");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
