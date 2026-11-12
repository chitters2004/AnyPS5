#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <cstring>
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::DriverDetail::DrawPlanCount;
using AgcDriver::DriverDetail::DrawPlanCounts;
using AgcDriver::DriverDetail::DrawPlanCountKinds;

alignas(256) constexpr std::array<std::uint32_t, 16> TriangleVertexCode{
    0x7e000d05u, 0x080200f4u, 0x10020300u, 0x100202f4u, 0x060202f3u, 0x060400f3u, 0x10040500u, 0x060404f3u,
    0x7e060280u, 0x7e0802f2u, 0xf80008cfu, 0x04030201u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 8> PixelCode{0x7e000200u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
constexpr std::size_t BlockBytes = 65536;

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header PixelHeader{};

void Register(Header& header, const std::uint32_t* code, std::size_t bytes, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(bytes);
    header.shader.code = code;
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, 65536));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
#else
        constexpr std::uintptr_t alignment = 65536;
        void* mapped = mmap(nullptr, BlockBytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + BlockBytes != begin + BlockBytes + alignment) munmap(reinterpret_cast<void*>(aligned + BlockBytes), begin + alignment - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
        }
#endif
        Require(block != nullptr, "draw plan shadow: cannot allocate a guest block");
        std::memset(block, 0, BlockBytes);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint64_t At(std::size_t offset) const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block + offset)); }
    std::uint8_t* Bytes(std::size_t offset) const { return block + offset; }

private:
    std::uint8_t* block = nullptr;
};

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

void Draw(const GuestBlock& target, std::uint32_t primitiveType) {
    const auto color = target.At(0);
    const auto vertex = reinterpret_cast<std::uintptr_t>(TriangleVertexCode.data());
    const auto pixel = reinterpret_cast<std::uintptr_t>(PixelCode.data());
    const float scale = 32.0f;
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x2000},
        {0x1b6, 0}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, (63u << 14u) | 3u},
        {0x3b8, 0x9000000u | (0x1bu << 14u)}, {0x1e0, 0},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040},
        {0x318, static_cast<std::uint32_t>(color >> 8u)}, {0x390, static_cast<std::uint32_t>(color >> 40u)},
        {0x10f, std::bit_cast<std::uint32_t>(scale)}, {0x110, std::bit_cast<std::uint32_t>(scale)},
        {0x111, std::bit_cast<std::uint32_t>(-2.0f)}, {0x112, std::bit_cast<std::uint32_t>(2.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
        {0x2fe, 0}, {0x302, 0}, {0x306, 0}, {0x30a, 0},
    };
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> shader{
        {0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, 0}, {0x8c, 0},
        {0x008, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009, static_cast<std::uint32_t>(pixel >> 40u)}, {0x00b, 1u << 1u},
        {0x00c, std::bit_cast<std::uint32_t>(1.0f)},
    };
    std::vector<std::uint32_t> commands;
    Append(commands, 0x79, {0x242, primitiveType});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : shader) Append(commands, 0x76, {offset, value});
    Append(commands, 0x2d, {3, 2});
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "draw plan shadow: the draw submission failed");
    AgcDriverWaitIdle_nid_postfix();
}

void Expect(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error("draw plan shadow: " + what);
}

DrawPlanCounts Measure(const GuestBlock& target, std::uint32_t primitiveType, const char* label) {
    auto& driver = AgcDriver::DriverDetail::Driver::Get();
    const auto before = driver.DrawPlanShadowCounts();
    Draw(target, primitiveType);
    const auto after = driver.DrawPlanShadowCounts();
    DrawPlanCounts since;
    for (std::size_t kind = 0; kind < DrawPlanCountKinds; ++kind) since.values[kind] = after.values[kind] - before.values[kind];
    std::printf("%s: plan lookups %llu, absent %llu, present %llu, agreed %llu, pipeline differing %llu, same instance %llu, instance differs %llu, no decision %llu\n", label,
        static_cast<unsigned long long>(since[DrawPlanCount::Lookups]), static_cast<unsigned long long>(since[DrawPlanCount::Absent]), static_cast<unsigned long long>(since[DrawPlanCount::Present]),
        static_cast<unsigned long long>(since[DrawPlanCount::Agreed]), static_cast<unsigned long long>(since[DrawPlanCount::Pipeline]), static_cast<unsigned long long>(since[DrawPlanCount::SameInstance]),
        static_cast<unsigned long long>(since[DrawPlanCount::InstanceDiffers]), static_cast<unsigned long long>(since[DrawPlanCount::NoDecision]));
    return since;
}

std::uint64_t Mismatches(const DrawPlanCounts& counts) {
    std::uint64_t total = 0;
    for (const auto kind : {DrawPlanCount::Pipeline, DrawPlanCount::FragmentOutputs, DrawPlanCount::ShaderStages, DrawPlanCount::PushStages, DrawPlanCount::HasDepth, DrawPlanCount::VertexInput, DrawPlanCount::MaskedState, DrawPlanCount::PassDiffersSameInstance}) total += counts[kind];
    return total;
}

void PlansFollowTheirKey() {
    constexpr std::uint32_t List = 4;
    constexpr std::uint32_t Strip = 6;
    Register(VertexHeader, TriangleVertexCode.data(), sizeof(TriangleVertexCode), 2);
    Register(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
    GuestBlock FirstTarget;
    GuestBlock SecondTarget;
    const auto first = Measure(FirstTarget, List, "first draw");
    const auto second = Measure(FirstTarget, List, "second draw");
    const auto third = Measure(FirstTarget, List, "third draw, a draw-cache hit");
    const auto otherTarget = Measure(SecondTarget, List, "the same draw into another render target");
    const auto strip = Measure(FirstTarget, Strip, "the same stages as a triangle strip");
    const auto stripAgain = Measure(FirstTarget, Strip, "the triangle strip again");
    for (const auto* step : {&first, &second, &third, &otherTarget, &strip, &stripAgain}) Expect(Mismatches(*step) == 0, "a plan's stored decisions differ from what the full path chose for a draw with the same plan key");
    if (!AgcDriver::Graphics::DrawPlanShadow() && !AgcDriver::Graphics::DrawPlans()) {
        Expect(first[DrawPlanCount::Lookups] + second[DrawPlanCount::Lookups] + third[DrawPlanCount::Lookups] + otherTarget[DrawPlanCount::Lookups] + strip[DrawPlanCount::Lookups] == 0, "the shadow ran while switched off");
        std::puts("draw plan shadow: off, nothing looked up");
        return;
    }
    Expect(first[DrawPlanCount::Absent] == 1, "the first draw did not insert a plan");
    Expect(second[DrawPlanCount::Lookups] == 0 || (second[DrawPlanCount::Present] == 1 && second[DrawPlanCount::Agreed] == 1 && second[DrawPlanCount::SameInstance] == 1), "a repeated draw did not find and agree with its plan on the same instance");
    Expect(third[DrawPlanCount::Lookups] == 0 || (third[DrawPlanCount::Present] == 1 && third[DrawPlanCount::Agreed] == 1 && third[DrawPlanCount::SameInstance] == 1), "a repeated draw that its exact-key recipe did not serve did not find and agree with its plan on the same instance");
    const bool servedIntoOwnTarget = AgcDriver::Graphics::DrawPlans() && AgcDriver::Graphics::RuntimeSwitches::Set(AgcDriver::Graphics::RuntimeSwitch::PlanAttachments) && otherTarget[DrawPlanCount::Lookups] == 0;
    Expect(servedIntoOwnTarget || (otherTarget[DrawPlanCount::Present] == 1 && otherTarget[DrawPlanCount::Agreed] == 1 && otherTarget[DrawPlanCount::InstanceDiffers] == 1), "a draw into another render target did not share the plan as another instance");
    Expect(strip[DrawPlanCount::Absent] == 1 && strip[DrawPlanCount::Present] == 0, "a draw with another topology shared the triangle list's plan");
    Expect(stripAgain[DrawPlanCount::Lookups] == 0 || (stripAgain[DrawPlanCount::Present] == 1 && stripAgain[DrawPlanCount::Agreed] == 1), "the triangle strip did not find its own plan");
    std::puts("draw plan shadow: plans follow the compiled stages and the pipeline-shaping state, and render targets are instances");
}

}

int main() {
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        PlansFollowTheirKey();
        AgcDriverShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
