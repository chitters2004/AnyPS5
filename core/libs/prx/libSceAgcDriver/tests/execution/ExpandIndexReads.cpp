#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
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
alignas(256) std::array<std::uint32_t, Vertices> StoredIndices{};

alignas(256) constexpr std::array<std::uint32_t, 7> VertexCode{
    0xbe822100, 0x34020a84, 0xe0781000, 0x80010801, 0xf80008cf, 0x0b0a0908, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 4> StoreIndexCode{
    0x34020082, 0xe0701000, 0x80000001, 0xbf810000,
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
    std::size_t baseBytes = 0;
    std::uint32_t firstVertex = 0;
};

void Store(std::byte* at, std::span<const std::uint32_t> indices, std::uint32_t indexSize) {
    for (std::size_t i = 0; i < indices.size(); ++i) {
        if (indexSize == 2) {
            const auto index = static_cast<std::uint16_t>(indices[i]);
            std::memcpy(at + i * 2u, &index, sizeof(index));
        } else {
            std::memcpy(at + i * 4u, &indices[i], sizeof(std::uint32_t));
        }
    }
}

void DrawIndexed(AgcDriver::VulkanDevice& device, std::uintptr_t indexAddress, std::uint32_t count, std::uint32_t indexSize, VkProvokingVertexModeEXT provoking, std::uint32_t firstVertex, const char* what) {
    Pixels.fill(std::byte{0x40});
    Output.fill(Unwritten);
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
    Require(vertexResult.vertexAttributes.size() == 1u, std::string(what) + ": the fetch did not become a vertex attribute");
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
    state.provokingVertexMode = provoking;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    state.blendConstants = {};
    AgcDriver::Pm4::DrawParameters draw{indexAddress, count, indexSize, 1, 0, true};
    draw.firstVertex = firstVertex;
    device.Draw(state, draw, shaders);
    device.WaitIdle();
}

void Draw(AgcDriver::VulkanDevice& device, const Case& value) {
    IndexBytes.fill(std::byte{0xff});
    Store(IndexBytes.data() + value.baseBytes, value.indices, value.indexSize);
    DrawIndexed(device, reinterpret_cast<std::uintptr_t>(IndexBytes.data() + value.baseBytes), static_cast<std::uint32_t>(value.indices.size()), value.indexSize, value.provoking, value.firstVertex, value.what);
}

void CheckFetched(const std::string& what, std::span<const std::uint32_t> indices, std::uint32_t firstVertex) {
    const auto used = indices.size() / 4u * 4u;
    for (std::uint32_t vertex = 0; vertex < Vertices; ++vertex) {
        const bool expected = vertex >= firstVertex && std::find(indices.begin(), indices.begin() + static_cast<std::ptrdiff_t>(used), vertex - firstVertex) != indices.begin() + static_cast<std::ptrdiff_t>(used);
        const auto* slot = Output.data() + vertex * 4u;
        const bool fetched = slot[0] != Unwritten;
        Require(fetched == expected, what + ": vertex " + std::to_string(vertex) + (expected ? " was not fetched" : " was fetched although no expanded quad uses it"));
        if (!fetched) continue;
        std::array<std::uint32_t, 4> position{};
        std::memcpy(position.data(), Positions[vertex].data(), sizeof(position));
        Require(std::equal(position.begin(), position.end(), slot), what + ": vertex " + std::to_string(vertex) + " fetched other words than its position");
    }
}

void Check(const Case& value, const char* path) {
    CheckFetched(std::string(value.what) + " (" + path + ")", value.indices, value.firstVertex);
}

const char* PathName(bool oneByOne) {
    return oneByOne ? "indices read one by one" : "indices read at once";
}

void ShortCounts(AgcDriver::VulkanDevice& device, bool oneByOne) {
    const std::vector<std::uint32_t> indices{3, 7, 11};
    for (const std::uint32_t indexSize : {2u, 4u}) {
        for (std::uint32_t count = 0; count <= 3; ++count) {
            const auto what = std::to_string(indexSize * 8u) + "-bit quad list of " + std::to_string(count) + " indices (" + PathName(oneByOne) + ")";
            IndexBytes.fill(std::byte{0xff});
            Store(IndexBytes.data(), std::span(indices).first(count), indexSize);
            DrawIndexed(device, reinterpret_cast<std::uintptr_t>(IndexBytes.data()), count, indexSize, VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT, 0, what.c_str());
            Require(std::all_of(Output.begin(), Output.end(), [](std::uint32_t word) { return word == Unwritten; }), what + ": fetched a vertex although no quad is whole");
        }
    }
}

void Dispatch(AgcDriver::VulkanDevice& device) {
    std::vector<std::uint32_t> userData(16, 0u);
    const auto address = reinterpret_cast<std::uintptr_t>(StoredIndices.data());
    const std::array<std::uint32_t, 4> descriptor{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), static_cast<std::uint32_t>(sizeof(StoredIndices)), 0x31016facu};
    std::copy(descriptor.begin(), descriptor.end(), userData.begin());
    const std::span<const std::uint32_t> code(StoreIndexCode);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Vertices, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
}

bool PendingGpuIndices(AgcDriver::VulkanDevice& device, bool oneByOne) {
    const auto what = std::string("indices a dispatch stored just before the draw (") + PathName(oneByOne) + ")";
    StoredIndices.fill(Vertices - 1u);
    device.WaitIdle();
    Dispatch(device);
    const bool pending = StoredIndices.front() == Vertices - 1u && StoredIndices.back() == Vertices - 1u;
    DrawIndexed(device, reinterpret_cast<std::uintptr_t>(StoredIndices.data()), Vertices, 4, VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT, 0, what.c_str());
    std::vector<std::uint32_t> expected(Vertices);
    for (std::uint32_t i = 0; i < Vertices; ++i) expected[i] = i;
    Require(std::equal(expected.begin(), expected.end(), StoredIndices.begin()), what + ": the dispatch did not store the indices");
    CheckFetched(what, expected, 0);
    return pending;
}

class Pages {
public:
    Pages() {
#ifdef _WIN32
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        page = info.dwPageSize;
        block = static_cast<std::byte*>(VirtualAlloc(nullptr, page * 3u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        Require(block != nullptr, "expand index reads: cannot allocate pages");
#else
        page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        void* mapped = mmap(nullptr, page * 3u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Require(mapped != MAP_FAILED, "expand index reads: cannot map pages");
        block = static_cast<std::byte*>(mapped);
#endif
        std::memset(block, 0xff, page * 3u);
    }

    ~Pages() {
#ifdef _WIN32
        VirtualFree(block, 0, MEM_RELEASE);
#else
        munmap(block, page * 3u);
#endif
    }

    Pages(const Pages&) = delete;
    Pages& operator=(const Pages&) = delete;

    void Seal() {
#ifdef _WIN32
        DWORD old = 0;
        Require(VirtualProtect(block + page, page, PAGE_READONLY, &old) != 0, "expand index reads: cannot make the middle page read-only");
        Require(VirtualProtect(block + page * 2u, page, PAGE_NOACCESS, &old) != 0, "expand index reads: cannot make the last page inaccessible");
#else
        Require(mprotect(block + page, page, PROT_READ) == 0, "expand index reads: cannot make the middle page read-only");
        Require(mprotect(block + page * 2u, page, PROT_NONE) == 0, "expand index reads: cannot make the last page inaccessible");
#endif
    }

    std::byte* At(std::size_t offset) const { return block + offset; }
    std::size_t Page() const { return page; }

private:
    std::size_t page = 0;
    std::byte* block = nullptr;
};

std::optional<std::string> DrawFailure(AgcDriver::VulkanDevice& device, std::uintptr_t address, std::uint32_t count, std::uint32_t indexSize, const char* what) {
    try {
        DrawIndexed(device, address, count, indexSize, VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT, 0, what);
    } catch (const std::exception& error) {
        device.WaitIdle();
        return std::string(error.what());
    }
    return std::nullopt;
}

void MappingBoundaries(AgcDriver::VulkanDevice& device) {
    Pages pages;
    const std::vector<std::uint32_t> across{5, 9, 2, 7, 11, 3, 0, 14};
    const auto acrossAt = pages.Page() - 6u;
    Store(pages.At(acrossAt), across, 2);
    const std::vector<std::uint32_t> tail{4, 8, 12, 1, 6, 10, 13, 15, 2, 3, 5};
    const auto tailAt = pages.Page() * 2u - 16u;
    Store(pages.At(tailAt), std::span(tail).first(8), 2);
    pages.Seal();
    std::array<std::optional<std::string>, 2> tailFailures;
    for (const bool oneByOne : {false, true}) {
        AgcDriver::Graphics::RuntimeSwitches::Assign(RuntimeSwitch::NoBulkExpandReads, oneByOne);
        const auto what = std::string("16-bit quad list across a writable and a read-only mapping (") + PathName(oneByOne) + ")";
        DrawIndexed(device, reinterpret_cast<std::uintptr_t>(pages.At(acrossAt)), static_cast<std::uint32_t>(across.size()), 2, VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT, 0, what.c_str());
        CheckFetched(what, across, 0);
        tailFailures[oneByOne ? 1 : 0] = DrawFailure(device, reinterpret_cast<std::uintptr_t>(pages.At(tailAt)), static_cast<std::uint32_t>(tail.size()), 2, "16-bit quad list whose unused tail is unmapped");
    }
    AgcDriver::Graphics::RuntimeSwitches::Assign(RuntimeSwitch::NoBulkExpandReads, false);
    Require(tailFailures[0].has_value(), "a quad list whose unused index tail is inaccessible was drawn with the indices read at once");
    Require(tailFailures[1].has_value(), "a quad list whose unused index tail is inaccessible was drawn with the indices read one by one");
    Require(*tailFailures[0] == *tailFailures[1], "a quad list whose unused index tail is inaccessible failed differently read at once (" + *tailFailures[0] + ") and one by one (" + *tailFailures[1] + ")");
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
        const std::array<Case, 9> cases{{
            {"16-bit indices, first vertex provoking", {5, 9, 2, 7, 11, 3, 0, 14}, 2, VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT},
            {"16-bit indices, last vertex provoking", {5, 9, 2, 7, 11, 3, 0, 14}, 2, VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT},
            {"32-bit indices, first vertex provoking", {15, 1, 8, 4, 12, 13, 10, 6}, 4, VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT},
            {"32-bit indices, last vertex provoking", {15, 1, 8, 4, 12, 13, 10, 6}, 4, VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT},
            {"16-bit indices past the last whole quad", {5, 9, 2, 7, 11, 3, 0, 14, 6, 1, 8}, 2, VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT},
            {"32-bit indices past the last whole quad", {15, 1, 8, 4, 12, 13, 10, 6, 0, 2, 3}, 4, VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT},
            {"16-bit indices at a nonzero index buffer offset", {5, 9, 2, 7, 11, 3, 0, 14}, 2, VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT, 6},
            {"32-bit indices at a nonzero index buffer offset", {15, 1, 8, 4, 12, 13, 10, 6}, 4, VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT, 52},
            {"16-bit indices with a nonzero first vertex", {5, 9, 2, 7, 11, 3, 0, 1}, 2, VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT, 0, 4},
        }};
        for (const bool oneByOne : {false, true}) {
            AgcDriver::Graphics::RuntimeSwitches::Assign(RuntimeSwitch::NoBulkExpandReads, oneByOne);
            for (const auto& value : cases) {
                Draw(*device, value);
                Check(value, PathName(oneByOne));
            }
            ShortCounts(*device, oneByOne);
        }
        bool pending = true;
        for (const bool oneByOne : {false, true}) {
            AgcDriver::Graphics::RuntimeSwitches::Assign(RuntimeSwitch::NoBulkExpandReads, oneByOne);
            pending = PendingGpuIndices(*device, oneByOne) && pending;
        }
        MappingBoundaries(*device);
        std::printf("expand index reads tests passed%s\n", pending ? "" : " (a dispatch's stores were visible before the draw, so the pending-write flush was not tested)");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
