#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include "F16LaneModel.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;
using namespace F16LaneModel;

constexpr std::uint32_t Threads = 64;
constexpr std::uint32_t Rounds = 64;
constexpr std::uint32_t PerDispatch = Threads * Rounds;
constexpr std::uint32_t Dispatches = 4;

constexpr std::array<std::uint16_t, 16> Edges{
    0x0000u, 0x8000u, 0x0001u, 0x83ffu, 0x0400u, 0x3c00u, 0xbc01u, 0x3555u,
    0x7bffu, 0xfbffu, 0x7c00u, 0xfc00u, 0x7e01u, 0x7c05u, 0xfe33u, 0x1400u,
};

constexpr std::array<std::array<std::uint16_t, 3>, 24> DoubleRounding{{
    {0x8632u, 0xf485u, 0xe276u}, {0x69b6u, 0xc200u, 0x8bcdu}, {0x4020u, 0xf5f0u, 0x1162u}, {0xc989u, 0xadc8u, 0x6880u},
    {0xc700u, 0x6882u, 0x0ac8u}, {0xea00u, 0x4877u, 0x9159u}, {0x9305u, 0x908fu, 0x142fu}, {0x4820u, 0xc0b0u, 0x0009u},
    {0x5480u, 0x6204u, 0x11e5u}, {0x0c51u, 0xccb1u, 0x31beu}, {0xf7d6u, 0x34dau, 0x337eu}, {0xd98bu, 0x91ddu, 0xc535u},
    {0xae70u, 0x24a9u, 0x3478u}, {0x41ffu, 0x9201u, 0x385du}, {0xf020u, 0xbc50u, 0x0d17u}, {0xc24au, 0xddb5u, 0xa91fu},
    {0xb2a5u, 0xa4d1u, 0xca10u}, {0xc80bu, 0x9e2fu, 0x46aeu}, {0x5cc0u, 0xc6a8u, 0x0057u}, {0x4300u, 0xebfcu, 0x8054u},
    {0xb445u, 0x6364u, 0x95f9u}, {0x235eu, 0xda84u, 0xea9du}, {0xca04u, 0x5381u, 0x13c3u}, {0x1b31u, 0x4873u, 0x57e3u},
}};

alignas(256) std::array<std::uint32_t, PerDispatch * 4> Inputs{};
alignas(256) std::array<std::uint32_t, PerDispatch * 2> Outputs{};

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

std::vector<std::array<std::uint16_t, 3>> Triples() {
    std::vector<std::array<std::uint16_t, 3>> triples;
    for (const auto a : Edges) {
        for (const auto b : Edges) {
            for (const auto c : Edges) triples.push_back({a, b, c});
        }
    }
    for (const auto& triple : DoubleRounding) triples.push_back(triple);
    std::uint32_t state = 0x2545f491u;
    const auto next = [&] {
        state ^= state << 13u;
        state ^= state >> 17u;
        state ^= state << 5u;
        return static_cast<std::uint16_t>(state & 0xffffu);
    };
    while (triples.size() < PerDispatch * Dispatches * 2u) triples.push_back({next(), next(), next()});
    return triples;
}

std::vector<std::uint32_t> Program() {
    std::vector<std::uint32_t> code{0x34080084u, 0x340a0083u};
    for (std::uint32_t round = 0; round < Rounds; ++round) {
        code.insert(code.end(), {0xbe8b03ffu, round * Threads * 16u, 0xe03c1000u, 0x0b010104u, 0xbf8c3f70u, 0x7e100280u, 0xd74b0008u, 0x040e0501u, 0xcc0e4009u, 0x1c0e0501u, 0xbe8a03ffu, round * Threads * 8u, 0xe0741000u, 0x0a000805u});
    }
    code.push_back(0xbf810000u);
    return code;
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

bool ScalarMatches(std::uint32_t actual, std::uint32_t a, std::uint32_t b, std::uint32_t c, bool ieee) {
    const bool nanSource = Nan(a) || Nan(b) || Nan(c);
    const auto expected = Arithmetic(HalfOperation::Fma, a, b, c, ieee);
    if (!nanSource) return actual == expected;
    return Nan(actual);
}

std::size_t Run(AgcDriver::VulkanDevice& device, const std::vector<std::uint32_t>& code, std::span<const std::array<std::uint16_t, 3>> triples, bool ieee) {
    for (std::uint32_t index = 0; index < PerDispatch; ++index) {
        const auto& low = triples[index * 2u];
        const auto& high = triples[index * 2u + 1u];
        const auto lane = index % Threads;
        const auto round = index / Threads;
        for (std::uint32_t source = 0; source < 3u; ++source) Inputs[(round * Threads + lane) * 4u + source] = low[source] | (static_cast<std::uint32_t>(high[source]) << 16u);
    }
    Outputs.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(8, 0u);
    const auto output = BufferDescriptor(Outputs.data(), static_cast<std::uint32_t>(Outputs.size() * 4u));
    const auto input = BufferDescriptor(Inputs.data(), static_cast<std::uint32_t>(Inputs.size() * 4u));
    std::copy(output.begin(), output.end(), userData.begin());
    std::copy(input.begin(), input.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> words(code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(words.data()), std::as_bytes(words)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(words.data()), words, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.context.floatMode = ShaderRecompiler::ShaderFloatMode{0xc0u, true, ieee, false};
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(words.data()));
    device.WaitIdle();
    std::size_t failures = 0;
    for (std::uint32_t index = 0; index < PerDispatch; ++index) {
        const auto& low = triples[index * 2u];
        const auto& high = triples[index * 2u + 1u];
        const auto scalar = Outputs[index * 2u] & 0xffffu;
        const auto packed = Outputs[index * 2u + 1u];
        const auto packedExpected = Arithmetic(HalfOperation::Fma, low[0], low[1], low[2], ieee) | (Arithmetic(HalfOperation::Fma, high[0], high[1], high[2], ieee) << 16u);
        const bool scalarOk = ScalarMatches(scalar, low[0], low[1], low[2], ieee);
        if (scalarOk && packed == packedExpected) continue;
        if (failures++ < 12u) {
            std::cerr << (ieee ? "IEEE 1" : "IEEE 0") << " fma(" << Hex(low[0]) << ", " << Hex(low[1]) << ", " << Hex(low[2]) << ") / (" << Hex(high[0]) << ", " << Hex(high[1]) << ", " << Hex(high[2]) << "): v_fma_f16 " << Hex(scalar) << (scalarOk ? " ok" : " wrong") << ", v_pk_fma_f16 " << Hex(packed) << ", expected " << Hex(packedExpected) << '\n';
        }
    }
    return failures;
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto triples = Triples();
        const auto code = Program();
        std::size_t failures = 0;
        for (const bool ieee : {false, true}) {
            for (std::uint32_t dispatch = 0; dispatch < Dispatches; ++dispatch) {
                failures += Run(*device, code, std::span<const std::array<std::uint16_t, 3>>(triples).subspan(dispatch * PerDispatch * 2u, PerDispatch * 2u), ieee);
            }
        }
        Require(failures == 0u, "f16 fma rounded to odd: " + std::to_string(failures) + " results differ from the exact fused product rounded to nearest even");
        std::printf("f16 fma rounded to odd tests passed (%zu triples, 2 IEEE modes)\n", triples.size());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
