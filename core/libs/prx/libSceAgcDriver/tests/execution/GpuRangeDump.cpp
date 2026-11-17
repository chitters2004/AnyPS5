#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineCreates.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
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

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::StorageTexture;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 16;
constexpr std::size_t BlockBytes = 65536;
constexpr std::size_t SettledBytes = 16 * BlockBytes;
constexpr std::size_t TargetBytes = std::size_t{Width} * Height * 16u;
constexpr std::uint32_t Rgba32FloatInfo = 0x8738u;
constexpr std::uint32_t LinearAttrib3 = 0x09000000u;
constexpr std::uint32_t Float32AbgrExport = 9;
constexpr VkColorComponentFlags RedGreen = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
constexpr int Draws = 2;

alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{
    0xe0382000, 0x80000005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000,
};

constexpr std::array<std::array<float, 4>, 3> Triangle{{
    {-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}
}};

alignas(256) std::array<std::uint32_t, 16> PixelCode{};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t stride, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), count, 0x01016facu};
}

std::span<const std::uint32_t> Program() {
    std::size_t at = 0;
    PixelCode[at++] = 0x36000000u | (20u << 17u) | 0xffu;
    PixelCode[at++] = 0x0000ffffu;
    PixelCode[at++] = 0x2c000000u | (21u << 17u) | 0x90u;
    PixelCode[at++] = 0x7e000c00u | (20u << 17u) | (256u + 20u);
    PixelCode[at++] = 0x7e000c00u | (21u << 17u) | (256u + 21u);
    PixelCode[at++] = 0x7e000280u | (22u << 17u);
    PixelCode[at++] = 0x7e0002f2u | (23u << 17u);
    PixelCode[at++] = 0xf800180fu;
    PixelCode[at++] = 0x17161514u;
    PixelCode[at++] = 0xbf810000u;
    return std::span<const std::uint32_t>(PixelCode.data(), at);
}

struct Block {
    explicit Block(std::size_t bytes) : bytes(bytes), arena(AgcDriver::GuestMemory::WriteWatched()) {
#ifdef _WIN32
        if (arena) {
            data = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, BlockBytes));
            GuestArena::GuestArenaCommit_nid_postfix(data, bytes, PAGE_READWRITE, bytes);
        } else {
            data = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        }
#else
        if (arena) {
            void* raw = mmap(nullptr, bytes + BlockBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            Require(raw != MAP_FAILED, "cannot map the color target");
            const auto begin = reinterpret_cast<std::uintptr_t>(raw);
            const auto aligned = (begin + BlockBytes - 1) & ~(static_cast<std::uintptr_t>(BlockBytes) - 1);
            if (aligned != begin) munmap(raw, aligned - begin);
            if (aligned + bytes != begin + bytes + BlockBytes) munmap(reinterpret_cast<void*>(aligned + bytes), begin + BlockBytes - aligned);
            data = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(data, bytes);
        } else {
            data = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, bytes));
        }
#endif
        Require(data != nullptr, "cannot allocate the color target");
        GuestAllocations::Mutation mutation;
        mutation.Add(data, bytes, true, true, true);
    }
    ~Block() {
        StorageTexture::FlushPending(Address(), bytes, nullptr, "test release");
        {
            GuestAllocations::Mutation mutation;
            mutation.Remove(data);
        }
#ifdef _WIN32
        if (arena) {
            GuestArena::GuestArenaReset_nid_postfix(data, bytes);
            GuestArena::GuestArenaRelease_nid_postfix(data, bytes);
        } else {
            VirtualFree(data, 0, MEM_RELEASE);
        }
#else
        if (arena) {
            munmap(data, bytes);
            GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(data, bytes);
        } else {
            std::free(data);
        }
#endif
    }
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(data); }
    std::size_t bytes;
    bool arena;
    std::uint8_t* data = nullptr;
};

AgcDriver::Graphics::ColorTarget DecodeTarget(const Block& block) {
    AgcDriver::Registers cx;
    cx[0x318] = static_cast<std::uint32_t>(block.Address() >> 8u);
    cx[0x31b] = 0;
    cx[0x31c] = Rgba32FloatInfo;
    cx[0x31d] = 0;
    cx[0x323] = 0;
    cx[0x324] = 0;
    cx[0x3b0] = ((Width - 1u) << 14u) | (Height - 1u);
    cx[0x3b8] = LinearAttrib3;
    cx[0x390] = static_cast<std::uint32_t>(block.Address() >> 40u);
    auto color = AgcDriver::Graphics::DecodeColorBuffer(cx, 0);
    color.exportIndex = 0;
    Require(color.tileMode == AgcDriver::Graphics::ColorTileMode::Linear && color.bytes == TargetBytes, "the target is not a linear R32G32B32A32 target");
    return color;
}

void Draw(AgcDriver::VulkanDevice& device, const Block& block) {
    const auto color = DecodeTarget(block);
    const auto target = device.Target();
    constexpr std::uint32_t waveSize = 64;
    std::vector<std::uint32_t> vertexUserData(4, 0u);
    const auto vertexBuffer = BufferDescriptor(Triangle.data(), 16u, static_cast<std::uint32_t>(Triangle.size()));
    std::copy(vertexBuffer.begin(), vertexBuffer.end(), vertexUserData.begin());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {waveSize, 0, vertexUserData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
        target,
        {0, 0, 0, 64}
    };
    vertex.useCache = false;
    const auto vertexResult = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());

    const auto pixelCode = Program();
    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionFixedPoint);
    pixel.targetOutputMode.at(0) = Float32AbgrExport;
    pixel.targetExportMapping.fill(0xe4u);
    const std::vector<std::uint32_t> pixelUserData(8, 0u);
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(pixelCode.data()), std::as_bytes(pixelCode)}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(pixelCode.data()), pixelCode, 0, {}},
        {waveSize, 0, pixelUserData, std::nullopt, pixel, std::nullopt, pixelMemory},
        target,
        PixelPushLayout(vertexPush, target)
    };
    fragment.useCache = false;
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertexResult, 0},
        {ShaderStage::Fragment, &pixelResult, PixelPushOffset(vertexPush, target)}
    }};

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_FALSE;
    blend.colorWriteMask = RedGreen;
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, waveSize, waveSize, std::nullopt, std::nullopt};
    state.colors = {color};
    state.color = color;
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blends = {blend};
    state.blend = blend;
    state.blendConstants = {};
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(Triangle.size()), 0, 1, 0, false};
    for (int index = 0; index < Draws; ++index) device.Draw(state, draw, shaders);
}

void SetVariable(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
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

void CheckPipelineCreates(const AgcDriver::PipelineCreates::Report& report, bool libraries, bool feedback) {
    using AgcDriver::MonolithicReason;
    using AgcDriver::PipelineCreateKind;
    const auto cell = [&](PipelineCreateKind kind) {
        AgcDriver::PipelineCreates::Cell sum;
        for (const auto& side : report.bySite) {
            const auto& one = side[static_cast<std::size_t>(kind)];
            sum.count += one.count;
            sum.hits += one.hits;
            sum.misses += one.misses;
            sum.unknown += one.unknown;
        }
        return sum;
    };
    const auto monolithic = cell(PipelineCreateKind::Monolithic);
    const auto links = cell(PipelineCreateKind::Link);
    std::uint64_t reasons = 0;
    for (const auto count : report.reasons) reasons += count;
    Require(reasons == monolithic.count, "every monolithic create needs exactly one reason: " + AgcDriver::PipelineCreates::Line(report));
    const auto noLibraries = report.reasons[static_cast<std::size_t>(MonolithicReason::NoLibraries)];
    if (libraries) Require(links.count + monolithic.count - noLibraries >= 1 && noLibraries == 0, "a device with pipeline libraries created no linked pipeline and no monolithic one for a reason of its own: " + AgcDriver::PipelineCreates::Line(report));
    else Require(noLibraries >= 1 && links.count == 0, "a device without pipeline libraries created no monolithic pipeline for that reason: " + AgcDriver::PipelineCreates::Line(report));
    const auto& inDraws = report.bySite[1];
    Require(inDraws[static_cast<std::size_t>(PipelineCreateKind::Link)].count + inDraws[static_cast<std::size_t>(PipelineCreateKind::Monolithic)].count >= 1, "the draws' pipeline was not counted inside the committed draws: " + AgcDriver::PipelineCreates::Line(report));
    Require(inDraws[static_cast<std::size_t>(PipelineCreateKind::OptimizedRelink)].count == 0, "an optimized relink, which runs on the library worker, was counted inside a committed draw: " + AgcDriver::PipelineCreates::Line(report));
    std::uint64_t total = 0, unknown = 0;
    for (const auto& side : report.bySite) {
        for (const auto& one : side) {
            total += one.count;
            unknown += one.unknown;
        }
    }
    if (feedback) Require(unknown < total, "VK_EXT_pipeline_creation_feedback is enabled but no create reported valid feedback: " + AgcDriver::PipelineCreates::Line(report));
    else Require(unknown == total, "a create reported cache feedback without the extension: " + AgcDriver::PipelineCreates::Line(report));
    std::printf("%s\n", AgcDriver::PipelineCreates::Line(report).c_str());
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
        SetVariable("APS5_PROFILE_COMMIT_SHAPE", "1");
        AgcDriver::PipelineCreates::Report creates;
        bool libraries = false;
        bool feedback = false;
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            if (!AgcDriver::GuestMemory::WriteWatched()) {
                std::puts("skipped, guest memory is not write-watched");
                return VulkanTestSkipped;
            }
            Require(AgcDriver::Graphics::Recorder::GpuTimingEnabled(), "APS5_PROFILE_GPU_RANGES did not turn GPU timing on");
            const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(*device);
#ifndef _WIN32
            const auto decided = AgcDriver::Graphics::PrepareImportWatch(context);
            AgcDriver::Graphics::SetImportWatch(context, AgcDriver::Graphics::ImportWatch::Watch);
            struct Restore {
                const AgcDriver::Graphics::Context& context;
                AgcDriver::Graphics::ImportWatch decided;
                ~Restore() { AgcDriver::Graphics::SetImportWatch(context, decided); }
            } restore{context, decided};
#endif
            Block block(SettledBytes);
            if (AgcDriver::Graphics::HostImportFor(context, block.Address(), block.bytes) == nullptr) {
                std::puts("skipped, host imports are unavailable");
                return VulkanTestSkipped;
            }
            static_cast<void>(AgcDriver::DevicePipelineCreates().Take());
            Draw(*device, block);
            device->WaitIdle();
            creates = AgcDriver::DevicePipelineCreates().Take();
            libraries = context.graphicsPipelineLibrary;
            feedback = context.pipelineCreationFeedback;
        }
        CheckPipelineCreates(creates, libraries, feedback);
        AgcDriver::Graphics::Recorder::FlushGpuRangeDump();
        std::ifstream file(path);
        Require(file.good(), "no range file was written: the draws into a write-watched linear target were not recorded into a recorder batch");
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
                    const auto colorField = "c" + std::to_string(static_cast<unsigned>(VK_FORMAT_R32G32B32A32_SFLOAT)) + "@" + std::to_string(Width) + "x" + std::to_string(Height) + ":0x";
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
        Require(draws == Draws, "expected one per-draw range for each of the " + std::to_string(Draws) + " draws, found " + std::to_string(draws));
        std::filesystem::remove(path);
        std::printf("gpu range dump tests passed (%zu batches, %zu passes, %zu draws)\n", batches.size(), passes, draws);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
