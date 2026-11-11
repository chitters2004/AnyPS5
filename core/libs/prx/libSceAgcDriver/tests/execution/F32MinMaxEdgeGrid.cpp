#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 64;
constexpr std::uint32_t Inputs = 4;
constexpr std::uint32_t Results = 16;
alignas(256) std::array<std::uint32_t, Threads * Inputs> Input{};
alignas(256) std::array<std::uint32_t, Threads * Results> Output{};

alignas(256) constexpr std::array<std::uint32_t, 19> Code{
    0x34020084u, 0x34040086u, 0xe0381000u, 0x80000401u, 0xbf8c3f70u, 0x1e140b04u, 0x20160b04u, 0xd551000cu,
    0x041a0b04u, 0xd554000du, 0x041a0b04u, 0xd557000eu, 0x041a0b04u, 0x201e0880u, 0xe0781000u, 0x80010a02u, 0xe0781010u,
    0x80010e02u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 41> ClampCode{
    0x34020084u, 0x34040086u, 0xe0381000u, 0x80000401u, 0xbf8c3f70u, 0xd557000au, 0x03c90104u, 0xd557000bu,
    0x020208f2u, 0xd557000cu, 0x0411e480u, 0xd557000du, 0x03c9ff04u, 0x80000000u, 0xd557000eu, 0x03fe08f3u,
    0x80000000u, 0xd557000fu, 0x0201ef04u, 0xd5570010u, 0x03d608ffu, 0x7f800000u, 0xd5570011u, 0x03fd0104u,
    0x80000000u, 0xd5570012u, 0x0411e0f0u, 0xd5570013u, 0x03c5e904u, 0xd5570014u, 0x03ca0881u, 0xd5570015u,
    0x03fded04u, 0xff800000u, 0xe0781000u, 0x80010a02u, 0xe0781010u, 0x80010e02u, 0xe0781020u, 0x80011202u,
    0xbf810000u,
};

constexpr std::uint32_t X = 0xffffffffu;

constexpr std::uint32_t ClampOperands[12][3] = {
    {X, 0x00000000u, 0x3f800000u},
    {0x3f800000u, X, 0x00000000u},
    {0x00000000u, 0x3f800000u, X},
    {X, 0x80000000u, 0x3f800000u},
    {0xbf800000u, X, 0x80000000u},
    {X, 0xc0800000u, 0x00000000u},
    {0x7f800000u, X, 0xc0000000u},
    {X, 0x00000000u, 0x80000000u},
    {0x3f000000u, 0x3f000000u, X},
    {X, 0x40000000u, 0xbf000000u},
    {0x00000001u, X, 0x3f800000u},
    {X, 0x40800000u, 0xff800000u},
};

constexpr const char* ClampNames[12] = {
    "v_med3_f32 v10, v4, 0, 1.0", "v_med3_f32 v11, 1.0, v4, 0", "v_med3_f32 v12, 0, 1.0, v4", "v_med3_f32 v13, v4, -0, 1.0",
    "v_med3_f32 v14, -1.0, v4, -0", "v_med3_f32 v15, v4, -4.0, 0", "v_med3_f32 v16, +inf, v4, -2.0", "v_med3_f32 v17, v4, 0, -0",
    "v_med3_f32 v18, 0.5, 0.5, v4", "v_med3_f32 v19, v4, 2.0, -0.5", "v_med3_f32 v20, 0x00000001, v4, 1.0", "v_med3_f32 v21, v4, 4.0, -inf",
};

constexpr std::array<std::uint32_t, 4> ClampExtraValues{0x3e800000u, 0x3f400000u, 0x3fc00000u, 0xbe800000u};

constexpr const char* Names[6] = {"v_min_f32 v10, v4, v5", "v_max_f32 v11, v4, v5", "v_min3_f32 v12, v4, v5, v6", "v_max3_f32 v13, v4, v5, v6", "v_med3_f32 v14, v4, v5, v6", "v_max_f32 v15, 0, v4"};

constexpr std::array<std::uint32_t, 23> PairValues{
    0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x007fffffu, 0x807fffffu, 0x00800000u, 0x80800000u,
    0x3f800000u, 0xbf800000u, 0x3f800001u, 0x40400000u, 0xc0400000u, 0x7f7fffffu, 0xff7fffffu, 0x7f800000u,
    0xff800000u, 0x7fc00000u, 0x7fc12345u, 0xffd54321u, 0x7f800001u, 0xff812345u, 0x7fbfffffu,
};

constexpr std::array<std::uint32_t, 12> TripleValues{
    0x00000000u, 0x80000000u, 0x00000001u, 0x807fffffu, 0x00800000u, 0x3f800000u,
    0xbf800000u, 0x7f800000u, 0xff800000u, 0x7fc12345u, 0xffd54321u, 0x7f800001u,
};

struct Mode {
    std::optional<ShaderRecompiler::ShaderFloatMode> floatMode;
    bool ieee;
    bool flush;
    const char* name;
};

bool IsNan(std::uint32_t bits) {
    return (bits & 0x7fffffffu) > 0x7f800000u;
}

bool IsSignaling(std::uint32_t bits) {
    return IsNan(bits) && (bits & 0x00400000u) == 0u;
}

std::uint32_t Numeric(std::uint32_t lhs, std::uint32_t rhs, bool maxValue) {
    if (((lhs | rhs) & 0x7fffffffu) == 0u) return maxValue ? lhs & rhs : lhs | rhs;
    const float left = std::bit_cast<float>(lhs);
    const float right = std::bit_cast<float>(rhs);
    return (maxValue ? left >= right : left < right) ? lhs : rhs;
}

std::uint32_t Pair(std::uint32_t lhs, std::uint32_t rhs, bool maxValue, bool ieee) {
    if (ieee && IsSignaling(lhs)) return lhs | 0x00400000u;
    if (ieee && IsSignaling(rhs)) return rhs | 0x00400000u;
    return IsNan(rhs) ? lhs : Numeric(lhs, rhs, maxValue);
}

std::uint32_t Median(std::uint32_t a, std::uint32_t b, std::uint32_t c, bool ieee) {
    if (IsNan(a) || IsNan(b) || IsNan(c)) return Pair(Pair(a, b, false, ieee), c, false, ieee);
    const std::uint32_t lower = Numeric(a, b, false);
    return Numeric(lower, Numeric(Numeric(a, b, true), c, false), true);
}

std::uint32_t Flushed(std::uint32_t bits, const Mode& mode) {
    return mode.flush && (bits & 0x7f800000u) == 0u ? bits & 0x80000000u : bits;
}

std::vector<std::uint32_t> Reference(const std::uint32_t* operands, const Mode& mode) {
    const std::uint32_t a = Flushed(operands[0], mode);
    const std::uint32_t b = Flushed(operands[1], mode);
    const std::uint32_t c = Flushed(operands[2], mode);
    return {
        Pair(a, b, false, mode.ieee),
        Pair(a, b, true, mode.ieee),
        Pair(Pair(a, b, false, mode.ieee), c, false, mode.ieee),
        Pair(Pair(a, b, true, mode.ieee), c, true, mode.ieee),
        Median(a, b, c, mode.ieee),
        Pair(0u, a, true, mode.ieee),
    };
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

std::vector<std::uint32_t> ClampReference(const std::uint32_t* operands, const Mode& mode) {
    std::vector<std::uint32_t> results;
    for (const auto& clamp : ClampOperands) {
        std::array<std::uint32_t, 3> value{};
        for (std::uint32_t index = 0; index < 3u; ++index) value[index] = Flushed(clamp[index] == X ? operands[0] : clamp[index], mode);
        results.push_back(Median(value[0], value[1], value[2], mode.ieee));
    }
    return results;
}

std::vector<std::array<std::uint32_t, 3>> ClampGrid() {
    std::vector<std::array<std::uint32_t, 3>> rows;
    for (const auto value : PairValues) rows.push_back({value, 0u, 0u});
    for (const auto value : ClampExtraValues) rows.push_back({value, 0u, 0u});
    return rows;
}

std::vector<std::array<std::uint32_t, 3>> Grid() {
    std::vector<std::array<std::uint32_t, 3>> rows;
    for (const auto lhs : PairValues) {
        for (const auto rhs : PairValues) rows.push_back({lhs, rhs, lhs});
    }
    for (const auto a : TripleValues) {
        for (const auto b : TripleValues) {
            for (const auto c : TripleValues) rows.push_back({a, b, c});
        }
    }
    return rows;
}

struct Program {
    std::span<const std::uint32_t> code;
    std::span<const char* const> names;
    std::vector<std::uint32_t> (*reference)(const std::uint32_t*, const Mode&);
    std::vector<std::array<std::uint32_t, 3>> rows;
};

std::uint32_t Check(AgcDriver::VulkanDevice& device, const Mode& mode, const Program& program) {
    const auto& rows = program.rows;
    std::vector<std::uint32_t> userData(8, 0u);
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size() * 4u));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code = program.code;
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    request.context.floatMode = mode.floatMode;
    const auto result = ShaderRecompiler::Recompile(request);
    std::uint32_t checked = 0u;
    for (std::size_t first = 0; first < rows.size(); first += Threads) {
        Input.fill(0u);
        const std::size_t count = std::min<std::size_t>(Threads, rows.size() - first);
        for (std::size_t lane = 0; lane < count; ++lane) std::copy(rows[first + lane].begin(), rows[first + lane].end(), Input.begin() + lane * Inputs);
        Output.fill(0xdeadbeefu);
        device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
        device.WaitIdle();
        for (std::size_t lane = 0; lane < count; ++lane) {
            const auto& operands = rows[first + lane];
            const auto expected = program.reference(operands.data(), mode);
            for (std::uint32_t instruction = 0; instruction < expected.size(); ++instruction) {
                const auto actual = Output[lane * Results + instruction];
                Require(actual == expected[instruction], std::string(mode.name) + ": " + program.names[instruction] + " of " + Hex(operands[0]) + ", " + Hex(operands[1]) + ", " + Hex(operands[2]) + " is " + Hex(actual) + ", expected " + Hex(expected[instruction]));
                ++checked;
            }
        }
    }
    return checked;
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const std::array<Program, 2> programs{{
            {Code, Names, Reference, Grid()},
            {ClampCode, ClampNames, ClampReference, ClampGrid()},
        }};
        std::vector<Mode> modes{
            {std::nullopt, false, true, "no float mode"},
            {ShaderRecompiler::ShaderFloatMode{0x00u, false, true, false}, true, true, "IEEE=1 f32 denormals flushed"},
        };
        if (device->Target().float32DenormPreserve) {
            modes.push_back({ShaderRecompiler::ShaderFloatMode{0xf0u, true, false, false}, false, false, "IEEE=0 f32 denormals kept"});
            modes.push_back({ShaderRecompiler::ShaderFloatMode{0xf0u, true, true, false}, true, false, "IEEE=1 f32 denormals kept"});
        }
        std::uint32_t checked = 0u;
        for (const auto& mode : modes) {
            for (const auto& program : programs) checked += Check(*device, mode, program);
        }
        std::printf("f32 min max edge grid tests passed (%u results, %zu modes)\n", checked, modes.size());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
