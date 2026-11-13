#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Optimization/NanTestEliminator.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Results = 16;
alignas(256) std::array<std::uint32_t, Threads * Results> Output{};

alignas(256) constexpr std::array<std::uint32_t, 13> Code{
    0x34060086u, 0x7e100b00u, 0x101410ffu, 0x7e800000u, 0x061210f7u, 0x1016130au, 0x101810f0u, 0x081a150au,
    0xe0781000u, 0x80010a03u, 0xbf8c3f70u, 0xbf810000u, 0xbf810000u,
};

constexpr const char* Names[4] = {"tid * 2^126", "tid * 2^126 * (tid - 4)", "tid * 0.5", "tid * 2^126 - tid * 2^126"};

std::uint32_t Rdna(float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    return (bits & 0x7fffffffu) > 0x7f800000u ? 0xffc00000u : bits;
}

std::array<std::uint32_t, 4> Expected(std::uint32_t tid) {
    const float converted = static_cast<float>(static_cast<std::int32_t>(tid));
    const float scaled = converted * std::bit_cast<float>(0x7e800000u);
    const float offset = converted + -4.0f;
    return {Rdna(scaled), Rdna(scaled * offset), Rdna(converted * 0.5f), Rdna(scaled - scaled)};
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

void Run(AgcDriver::VulkanDevice& device, const std::optional<ShaderRecompiler::ShaderFloatMode>& floatMode, bool fused, const char* mode) {
    Output.fill(0xdeadbeefu);
    std::array<std::uint32_t, 8> userData{};
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(Code.data()), std::as_bytes(std::span(Code))}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(Code.data()), Code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    request.context.floatMode = floatMode;
    request.target.fusedExactArithmetic = fused;
    const auto before = ShaderRecompiler::NanTestTotals();
    const auto result = ShaderRecompiler::Recompile(request);
    const auto after = ShaderRecompiler::NanTestTotals();
    const auto removed = after.removedTests - before.removedTests;
    if (ShaderRecompiler::ExactF32Arithmetic()) {
        Require(ShaderRecompiler::NanProofDisabled() ? removed == 0u : removed != 0u, std::string("nan proof chains: ") + mode + " removed " + std::to_string(removed) + " NaN tests");
        Require(ShaderRecompiler::NanProofDisabled() || after.tests - before.tests > removed, std::string("nan proof chains: ") + mode + " kept no NaN test, but the overflowed product and the infinite difference need theirs");
    }
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(Code.data()));
    device.WaitIdle();
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const auto expected = Expected(tid);
        for (std::uint32_t i = 0; i < 4u; ++i) {
            const std::uint32_t actual = Output[tid * Results + i];
            const bool nan = (expected[i] & 0x7fffffffu) > 0x7f800000u && (actual & 0x7fffffffu) > 0x7f800000u;
            if (!ShaderRecompiler::ExactF32Arithmetic() && nan) continue;
            Require(actual == expected[i], std::string("nan proof chains: ") + mode + (fused ? " fused" : "") + " lane " + std::to_string(tid) + " " + Names[i] + " is " + Hex(actual) + ", expected " + Hex(expected[i]));
        }
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        for (const bool fused : {false, true}) {
            if (!fused && device->Target().fusedExactArithmetic) continue;
            Run(*device, std::nullopt, fused, "no float mode");
            Run(*device, ShaderRecompiler::ShaderFloatMode{0xc0u, true, false, false}, fused, "IEEE=0");
            Run(*device, ShaderRecompiler::ShaderFloatMode{0xc0u, true, true, false}, fused, "IEEE=1");
        }
        std::puts("nan proof chain tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
