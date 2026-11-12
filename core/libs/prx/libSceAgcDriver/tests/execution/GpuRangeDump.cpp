#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <span>
#include <string>
#include <vector>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

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

void SetVariable(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void Draw(AgcDriver::VulkanDevice& device) {
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
    const std::vector<std::uint32_t> pixelUserData(8, 0u);
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(PixelCode.data()), std::as_bytes(std::span(PixelCode))}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}},
        {WaveSize, 0, pixelUserData, std::nullopt, pixel, std::nullopt, pixelMemory},
        target,
        PixelPushLayout(vertexPush, target)
    };
    fragment.useCache = false;
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertexResult, 0},
        {ShaderStage::Fragment, &pixelResult, PixelPushOffset(vertexPush, target)}
    }};

    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, WaveSize, WaveSize, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size(), 0xe4u};
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
    device.Draw(state, draw, shaders);
    device.WaitIdle();
}

struct Range {
    std::size_t index = 0;
    std::string name;
    std::uint32_t tag = 0;
    long long pass = -1;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
};

}

int main() {
    try {
#ifdef _WIN32
        const auto pid = _getpid();
#else
        const auto pid = getpid();
#endif
        const auto path = std::filesystem::temp_directory_path() / ("aps5-gpu-ranges-" + std::to_string(pid) + ".txt");
        std::filesystem::remove(path);
        SetVariable("APS5_PROFILE_GPU_RANGES", path.string());
        SetVariable("APS5_PROFILE_GPU_DRAWS", "1");
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            Require(AgcDriver::Graphics::Recorder::GpuTimingEnabled(), "APS5_PROFILE_GPU_RANGES did not turn GPU timing on");
            Draw(*device);
        }
        AgcDriver::Graphics::Recorder::FlushGpuRangeDump();
        std::ifstream file(path);
        if (!file.good()) {
            std::puts("skipped: the draws were not recorded into a recorder batch on this device, so no range was timed");
            return VulkanTestSkipped;
        }
        std::string line;
        std::getline(file, line);
        Require(line.rfind("aps5-gpu-ranges 1 period_ns ", 0) == 0, "the range file starts with '" + line + "', not its header");
        std::map<std::uint32_t, std::string> tags;
        std::vector<std::vector<Range>> batches;
        std::size_t declared = 0;
        while (std::getline(file, line)) {
            std::istringstream fields(line);
            std::string kind;
            fields >> kind;
            if (kind == "tag") {
                std::uint32_t id = 0;
                fields >> id;
                std::string text;
                std::getline(fields >> std::ws, text);
                Require(!tags.contains(id), "tag " + std::to_string(id) + " is declared twice");
                tags[id] = text;
            } else if (kind == "batch") {
                Require(batches.empty() || batches.back().size() == declared, "a batch holds fewer range lines than it declared");
                std::string word;
                std::uint64_t serial = 0;
                unsigned queue = 0;
                long long readNs = 0;
                std::size_t dropped = 0;
                fields >> serial >> word >> queue >> word >> readNs >> word >> declared >> word >> dropped;
                Require(!fields.fail() && dropped == 0, "malformed batch line or dropped ranges: " + line);
                batches.emplace_back();
            } else if (kind == "range") {
                Require(!batches.empty(), "a range line comes before any batch line");
                Range range;
                std::string word;
                std::uint64_t bytes = 0;
                fields >> range.index >> range.name >> word >> range.tag >> word >> range.pass >> word >> range.begin >> range.end >> word >> bytes;
                Require(!fields.fail(), "malformed range line: " + line);
                Require(range.index == batches.back().size(), "range lines are not in index order: " + line);
                Require(range.end >= range.begin, "a range ends before it begins: " + line);
                batches.back().push_back(range);
            } else if (kind != "present" && kind != "lost") {
                throw std::runtime_error("unknown line in the range file: " + line);
            }
        }
        std::size_t passes = 0, draws = 0, batchRanges = 0;
        for (const auto& ranges : batches) {
            for (const auto& range : ranges) {
                if (range.name == "batch") ++batchRanges;
                if (range.name == "class:draw") {
                    ++passes;
                    Require(range.pass == -1, "a draw class range names an enclosing pass");
                    Require(tags.contains(range.tag), "a draw class range has no declared pass tag");
                    const auto& tag = tags.at(range.tag);
                    const auto colorField = "c" + std::to_string(static_cast<unsigned>(VK_FORMAT_R8G8B8A8_UNORM)) + "@" + std::to_string(Width) + "x" + std::to_string(Height) + ":0x";
                    Require(tag.find(colorField) != std::string::npos && tag.find("colors=1,") != std::string::npos && tag.find(",samples=1,") != std::string::npos && tag.find("extent=64x16") != std::string::npos, "the pass tag does not describe the color target: " + tag);
                }
                if (range.name.rfind("draw:0x", 0) == 0) {
                    ++draws;
                    Require(range.pass >= 0 && static_cast<std::size_t>(range.pass) < ranges.size(), "a per-draw range names no pass of its batch");
                    const auto& pass = ranges[static_cast<std::size_t>(range.pass)];
                    Require(pass.name == "class:draw", "a per-draw range's pass is a " + pass.name + " range");
                    Require(pass.begin <= range.begin && range.end <= pass.end, "a per-draw range lies outside its pass range");
                }
            }
        }
        Require(!batches.empty() && batchRanges == batches.size(), "not every batch has its whole-batch range");
        Require(passes >= 1, "no draw class range was written");
        Require(draws == 2, "expected one per-draw range for each of the 2 draws, found " + std::to_string(draws));
        std::filesystem::remove(path);
        std::printf("gpu range dump tests passed (%zu batches, %zu passes, %zu draws)\n", batches.size(), passes, draws);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
