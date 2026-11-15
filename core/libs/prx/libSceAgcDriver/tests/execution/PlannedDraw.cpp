#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Extent = 64u;
alignas(256) std::array<std::byte, Extent * Extent * 4u> pixels{};
alignas(256) std::array<std::byte, 256> vertices{};
constexpr std::array<std::array<float, 4>, 3> positions{{{-1.0f, -1.0f, 0.0f, 1.0f}, {3.0f, -1.0f, 0.0f, 1.0f}, {-1.0f, 3.0f, 0.0f, 1.0f}}};
constexpr std::array<std::uint32_t, 11> vertexCode{0xf4080100u, 0xfa000000u, 0x4a0a0a02u, 0x4a0a0b08u, 0x4a0a0a03u, 0xe00c2000u, 0x80010005u, 0xbf8c3f70u, 0xf80008cfu, 0x03020100u, 0xbf810000u};
constexpr std::array<std::uint32_t, 5> pixelCode{0x7e0002f2u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u};

struct Scene {
    AgcDriver::VulkanDevice& device;
    std::array<std::uint32_t, 4> descriptor{};
    ShaderRecompiler::RecompileResult vertex;
    ShaderRecompiler::RecompileResult pixel;
    std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{};
    AgcDriver::Graphics::State state{};
    AgcDriver::Pm4::DrawParameters draw{0u, 3u, 0u, 1u, 0u, false};

    explicit Scene(AgcDriver::VulkanDevice& device) : device(device) {
        constexpr std::uint32_t stride = 16u;
        constexpr std::uint32_t offset = 1u;
        const auto tableAddress = reinterpret_cast<std::uintptr_t>(descriptor.data());
        const std::array<std::uint32_t, 4> userData{static_cast<std::uint32_t>(tableAddress), static_cast<std::uint32_t>(tableAddress >> 32u), offset, 1u};
        const auto firstRecord = offset + userData[3] + 2u;
        for (std::size_t index = 0; index < positions.size(); ++index) std::memcpy(vertices.data() + (firstRecord + index) * stride, positions[index].data(), 16u);
        const auto address = reinterpret_cast<std::uintptr_t>(vertices.data());
        descriptor = {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) | (stride << 16u), 8u, 0x01000facu | (77u << 12u)};
        const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{tableAddress, std::as_bytes(std::span(descriptor))}}};
        ShaderRecompiler::ShaderVertexStageInfo vertexInfo{};
        vertexInfo.fetchEmbedded = true;
        vertexInfo.resourcesNum = 1u;
        vertexInfo.resourcesDst[0] = {0, 4, 0, 0};
        ShaderRecompiler::RecompileRequest request{{ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(vertexCode.data()), vertexCode, 0, {}}, {32u, 0u, userData, std::nullopt, std::nullopt, vertexInfo, memory}, device.Target(), {0u, 0u, 0u, 96u}};
        request.context.vertex->resources[0].fields = descriptor;
        vertex = ShaderRecompiler::Recompile(request);
        const auto pushBytes = static_cast<std::uint32_t>(vertex.pushConstants.size());
        ShaderRecompiler::ShaderPixelStageInfo pixelInfo{};
        pixelInfo.wave32 = true;
        pixelInfo.targetOutputMode[0] = 9u;
        pixelInfo.targetExportMapping.fill(0xe4u);
        ShaderRecompiler::RecompileRequest pixelRequest{{ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(pixelCode.data()), pixelCode, 0, {}}, {32u, 0u, {}, std::nullopt, pixelInfo, std::nullopt, {}}, device.Target(), PixelPushLayout(pushBytes, device.Target())};
        pixel = ShaderRecompiler::Recompile(pixelRequest);
        shaders = {{{ShaderStage::Vertex, &vertex, 0u}, {ShaderStage::Fragment, &pixel, PixelPushOffset(pushBytes, device.Target())}}};
        state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 32u, 32u, std::nullopt, std::nullopt};
        state.color = {reinterpret_cast<std::uintptr_t>(pixels.data()), {Extent, Extent}, VK_FORMAT_R8G8B8A8_UNORM, pixels.size(), 0xe4u};
        state.colors = {state.color};
        state.hasColorTarget = true;
        state.renderExtent = {Extent, Extent};
        state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        state.viewport = {0.0f, static_cast<float>(Extent), static_cast<float>(Extent), -static_cast<float>(Extent), 0.0f, 1.0f};
        state.scissor = {{0, 0}, {Extent, Extent}};
        state.cullMode = VK_CULL_MODE_NONE;
        state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        state.blend.colorWriteMask = 15u;
        state.blends = {state.blend};
        draw.firstVertex = 1u;
        draw.firstInstance = 1u;
    }

    void Draw(std::shared_ptr<AgcDriver::Graphics::PlannedDraw>* planned, const AgcDriver::Pm4::DrawParameters& parameters, bool drawn, const char* what) {
        pixels.fill(std::byte{0});
        device.Draw(state, parameters, shaders, {}, nullptr, nullptr, 0, planned);
        device.WaitIdle();
        const auto red = pixels[0] == std::byte{255} && pixels[1] == std::byte{0} && pixels[2] == std::byte{0} && pixels[3] == std::byte{255};
        const auto untouched = pixels[0] == std::byte{0} && pixels[3] == std::byte{0};
        Require(drawn ? red : untouched, std::string("planned draw: ") + what + (drawn ? " did not draw the triangle" : " drew although it draws nothing"));
    }
};

void Run(AgcDriver::VulkanDevice& device) {
    Scene scene(device);
    static_cast<void>(AgcDriver::Graphics::TakePlannedDrawCounts());

    auto planned = device.PlanDraw(scene.state, scene.draw, scene.shaders);
    Require(planned != nullptr, "planned draw: a valid draw was not planned");
    scene.Draw(&planned, scene.draw, true, "a draw with its plan");
    auto counts = AgcDriver::Graphics::TakePlannedDrawCounts();
    Require(counts.planned == 1 && counts.used == 1 && counts.missing == 0 && counts.otherDevice == 0 && planned == nullptr, "planned draw: the committer did not take the plan (planned " + std::to_string(counts.planned) + ", used " + std::to_string(counts.used) + ")");

    scene.Draw(&planned, scene.draw, true, "a draw whose plan is not ready");
    counts = AgcDriver::Graphics::TakePlannedDrawCounts();
    Require(counts.missing == 1 && counts.used == 0, "planned draw: an empty plan slot was not counted as not ready");

    planned = device.PlanDraw(scene.state, scene.draw, scene.shaders);
    AgcDriver::Graphics::SetPlannedDrawDeviceForTest(*planned, VK_NULL_HANDLE);
    scene.Draw(&planned, scene.draw, true, "a draw planned for another device");
    counts = AgcDriver::Graphics::TakePlannedDrawCounts();
    Require(counts.otherDevice == 1 && counts.used == 0 && planned == nullptr, "planned draw: a plan for another device was taken");

    auto empty = scene.draw;
    empty.indexCount = 0u;
    planned = device.PlanDraw(scene.state, empty, scene.shaders);
    scene.Draw(&planned, empty, false, "an empty draw with its plan");
    counts = AgcDriver::Graphics::TakePlannedDrawCounts();
    Require(counts.planned == 1 && counts.used == 1, "planned draw: the plan of an empty draw was not taken");

    auto modified = scene.draw;
    modified.flags = 1u;
    planned = device.PlanDraw(scene.state, modified, scene.shaders);
    counts = AgcDriver::Graphics::TakePlannedDrawCounts();
    Require(planned == nullptr && counts.failed == 1 && counts.planned == 0, "planned draw: an invalid draw was planned instead of refused");
    std::string inlineError;
    try {
        scene.Draw(&planned, modified, false, "an invalid draw");
    } catch (const std::exception& error) {
        inlineError = error.what();
    }
    Require(inlineError.find("draw modifiers are unsupported") != std::string::npos, "planned draw: a refused plan did not leave the committer to report the draw's error (got '" + inlineError + "')");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device);
        std::cout << "planned draw tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
