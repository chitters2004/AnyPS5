#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::RuntimeSwitch;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 32;
constexpr std::uint32_t Vertices = 16;
constexpr std::uint32_t Unwritten = 0xdeadbeefu;
alignas(256) std::array<std::byte, Width * Height * 4> Pixels{};
alignas(256) std::array<std::uint32_t, Vertices * 4> Output{};
alignas(256) std::array<std::array<float, 4>, Vertices> Positions{};
alignas(256) std::array<std::byte, 256> IndexBytes{};

alignas(256) constexpr std::array<std::uint32_t, 7> VertexCode{
    0xbe822100, 0x34020a84, 0xe0781000, 0x80010801, 0xf80008cf, 0x0b0a0908, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 4> PixelCode{
    0x7e0e02f2, 0xf800180f, 0x07070707, 0xbf810000,
};

std::array<std::uint32_t, 4> VertexBufferDescriptor() {
    const auto address = reinterpret_cast<std::uintptr_t>(Positions.data());
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (16u << 16u), Vertices, (0x0104dfacu & ~0xfffu) | 0xfacu | (3u << 28u)};
}

std::array<std::uint32_t, 4> OutputDescriptor() {
    const auto address = reinterpret_cast<std::uintptr_t>(Output.data());
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), static_cast<std::uint32_t>(sizeof(Output)), 0x31016facu};
}

struct Case {
    const char* what;
    std::vector<std::uint32_t> indices;
    std::uint32_t indexSize;
    VkProvokingVertexModeEXT provoking;
};

void Draw(AgcDriver::VulkanDevice& device, const Case& value) {
    Pixels.fill(std::byte{0x40});
    Output.fill(Unwritten);
    IndexBytes.fill(std::byte{0xff});
    for (std::size_t i = 0; i < value.indices.size(); ++i) {
        if (value.indexSize == 2) {
            const auto index = static_cast<std::uint16_t>(value.indices[i]);
            std::memcpy(IndexBytes.data() + i * 2u, &index, sizeof(index));
        } else {
            std::memcpy(IndexBytes.data() + i * 4u, &value.indices[i], sizeof(std::uint32_t));
        }
    }
    std::vector<std::uint32_t> userData(12, 0u);
    userData[0] = 0x10000000u;
    userData[1] = 0x00000001u;
    const auto output = OutputDescriptor();
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    ShaderRecompiler::ShaderVertexStageInfo info{};
    info.resourcesNum = 1;
    info.resources[0].fields = VertexBufferDescriptor();
    info.resourcesDst[0] = {8, 4, 0, 0};
    info.fetchAttribReg = 8;
    info.fetchBufferReg = 10;
    info.fetchEmbedded = true;
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertexRequest{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {64u, 0, userData, std::nullopt, std::nullopt, info, vertexMemory},
        device.Target(),
        {0, 0, 0, 64}
    };
    vertexRequest.useCache = false;
    const auto vertexResult = ShaderRecompiler::Recompile(vertexRequest);
    Require(vertexResult.vertexAttributes.size() == 1u, std::string(value.what) + ": the fetch did not become a vertex attribute");
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());

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
        {64u, 0, pixelUserData, std::nullopt, pixel, std::nullopt, pixelMemory},
        device.Target(),
        PixelPushLayout(vertexPush, device.Target())
    };
    fragment.useCache = false;
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertexResult, 0},
        {ShaderStage::Fragment, &pixelResult, PixelPushOffset(vertexPush, device.Target())}
    }};

    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 64u, 64u, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size(), 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.quadList = true;
    state.provokingVertexMode = value.provoking;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    state.blendConstants = {};
    const AgcDriver::Pm4::DrawParameters draw{reinterpret_cast<std::uintptr_t>(IndexBytes.data()), static_cast<std::uint32_t>(value.indices.size()), value.indexSize, 1, 0, true};
    device.Draw(state, draw, shaders);
    device.WaitIdle();
}

void Check(const Case& value, const char* path) {
    const auto used = value.indices.size() / 4u * 4u;
    for (std::uint32_t vertex = 0; vertex < Vertices; ++vertex) {
        const bool expected = std::find(value.indices.begin(), value.indices.begin() + static_cast<std::ptrdiff_t>(used), vertex) != value.indices.begin() + static_cast<std::ptrdiff_t>(used);
        const auto* slot = Output.data() + vertex * 4u;
        const bool fetched = slot[0] != Unwritten;
        Require(fetched == expected, std::string(value.what) + " (" + path + "): vertex " + std::to_string(vertex) + (expected ? " was not fetched" : " was fetched although no expanded quad uses it"));
        if (!fetched) continue;
        std::array<std::uint32_t, 4> position{};
        std::memcpy(position.data(), Positions[vertex].data(), sizeof(position));
        Require(std::equal(position.begin(), position.end(), slot), std::string(value.what) + " (" + path + "): vertex " + std::to_string(vertex) + " fetched other words than its position");
    }
}

}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_VERIFY_EXPAND_READS", "1");
#else
    setenv("APS5_VERIFY_EXPAND_READS", "1", 1);
#endif
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        for (std::uint32_t vertex = 0; vertex < Vertices; ++vertex) {
            const auto x = -0.9f + 0.1f * static_cast<float>(vertex % 4u);
            const auto y = -0.9f + 0.1f * static_cast<float>(vertex / 4u);
            Positions[vertex] = {x, y, 0.5f, 1.0f};
        }
        const std::array<Case, 6> cases{{
            {"16-bit indices, first vertex provoking", {5, 9, 2, 7, 11, 3, 0, 14}, 2, VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT},
            {"16-bit indices, last vertex provoking", {5, 9, 2, 7, 11, 3, 0, 14}, 2, VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT},
            {"32-bit indices, first vertex provoking", {15, 1, 8, 4, 12, 13, 10, 6}, 4, VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT},
            {"32-bit indices, last vertex provoking", {15, 1, 8, 4, 12, 13, 10, 6}, 4, VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT},
            {"16-bit indices past the last whole quad", {5, 9, 2, 7, 11, 3, 0, 14, 6, 1, 8}, 2, VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT},
            {"32-bit indices past the last whole quad", {15, 1, 8, 4, 12, 13, 10, 6, 0, 2, 3}, 4, VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT},
        }};
        for (const bool oneByOne : {false, true}) {
            AgcDriver::Graphics::RuntimeSwitches::Assign(RuntimeSwitch::NoBulkExpandReads, oneByOne);
            for (const auto& value : cases) {
                Draw(*device, value);
                Check(value, oneByOne ? "indices read one by one" : "indices read at once");
            }
        }
        std::puts("expand index reads tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
