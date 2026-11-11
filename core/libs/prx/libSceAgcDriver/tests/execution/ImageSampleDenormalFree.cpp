#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <set>
#include <span>
#include <string>
#include <vector>
#include <spirv/unified1/spirv.hpp>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Size = 8;
constexpr std::uint32_t Format32Float = 22;
constexpr std::uint32_t Format8888UNorm = 56;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t IdentitySwizzle = 0xfacu;
constexpr std::uint32_t SampleX = 3;
constexpr std::uint32_t SampleY = 5;
constexpr std::array<std::uint32_t, 4> PointSampler{0x00000092u, 0x00fff000u, 0x00000000u, 0u};
constexpr std::array<std::uint32_t, 4> LinearSampler{0x00000092u, 0x00fff000u, 0x00500000u, 0u};

alignas(256) std::array<std::uint32_t, Threads * 16> Output{};
alignas(256) std::array<std::uint8_t, 4096> Colors{};
alignas(256) std::array<std::uint8_t, 4096> Floats{};

alignas(256) constexpr std::array<std::uint32_t, 25> Code{
    0x34060086u, 0x7e2802ffu, 0x3ef00000u, 0x7e2a02ffu, 0x3f300000u, 0xf09c0f08u, 0x00610a14u, 0xf09c0f08u,
    0x00640e14u, 0xbf8c3f70u, 0x2024170au, 0x1e26150cu, 0x202c1f0eu, 0x7c021d0fu, 0xd5010017u, 0x01a9e480u,
    0xe0781000u, 0x80000a03u, 0xe0781010u, 0x80000e03u, 0xe0781020u, 0x80001203u, 0xe0741030u, 0x80001603u,
    0xbf810000u,
};

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

std::uint32_t PitchBytes(std::uint32_t format, std::size_t storage) {
    const auto mips = AgcDriver::Graphics::ComputeMipLayout(AgcDriver::Graphics::TextureTileMode::kLinear, format, Size, Size, 1u);
    Require(AgcDriver::Graphics::ComputeSurfaceSize(mips, 1) <= storage, "image sample denormal free: a texture does not fit its storage");
    return mips[0].pitchBytes;
}

void Fill() {
    const auto colorPitch = PitchBytes(Format8888UNorm, Colors.size());
    const auto floatPitch = PitchBytes(Format32Float, Floats.size());
    Colors.fill(0u);
    Floats.fill(0u);
    for (std::uint32_t y = 0; y < Size; ++y) {
        for (std::uint32_t x = 0; x < Size; ++x) {
            auto* color = &Colors[static_cast<std::size_t>(y) * colorPitch + x * 4u];
            color[0] = 1u;
            color[1] = 0u;
            color[2] = 128u;
            color[3] = 255u;
            const std::uint32_t bits = x == SampleX && y == SampleY ? 0x00000001u : 0x00000003u;
            auto* value = &Floats[static_cast<std::size_t>(y) * floatPitch + x * 4u];
            for (std::uint32_t byte = 0; byte < 4u; ++byte) value[byte] = static_cast<std::uint8_t>(bits >> (byte * 8u));
        }
    }
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::array<std::uint32_t, 8> TextureDescriptor(const void* texels, std::uint32_t format) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(texels));
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (format << 20u) | (((Size - 1u) & 3u) << 30u),
        ((Size - 1u) >> 2u) | ((Size - 1u) << 14u),
        IdentitySwizzle | (Type2D << 28u),
        0u,
        0u,
        0u,
        0u,
    };
}

std::size_t Run(AgcDriver::VulkanDevice& device, const std::array<std::uint32_t, 4>& sampler) {
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(24, 0u);
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    const auto colors = TextureDescriptor(Colors.data(), Format8888UNorm);
    const auto floats = TextureDescriptor(Floats.data(), Format32Float);
    std::copy(output.begin(), output.end(), userData.begin());
    std::copy(colors.begin(), colors.end(), userData.begin() + 4);
    std::copy(sampler.begin(), sampler.end(), userData.begin() + 12);
    std::copy(floats.begin(), floats.end(), userData.begin() + 16);
    const std::span<const std::uint32_t> code(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {true, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
    const auto words = result.spirv.Words();
    std::set<std::uint32_t> exponents;
    std::size_t masks = 0;
    for (std::size_t offset = 5; offset < words.size();) {
        const auto count = words[offset] >> spv::WordCountShift;
        Require(count != 0u && count <= words.size() - offset, "image sample denormal free: the recompiler returned a malformed SPIR-V instruction");
        const auto opcode = words[offset] & spv::OpCodeMask;
        if (opcode == spv::OpConstant && count == 4u && words[offset + 3u] == 0x7f800000u) exponents.insert(words[offset + 2u]);
        if (opcode == spv::OpBitwiseAnd && count == 5u && (exponents.contains(words[offset + 3u]) || exponents.contains(words[offset + 4u]))) ++masks;
        offset += count;
    }
    return masks;
}

std::size_t Check(AgcDriver::VulkanDevice& device, const std::array<std::uint32_t, 4>& sampler, const char* name) {
    Fill();
    const auto masks = Run(device, sampler);
    for (std::uint32_t lane = 0; lane < Threads; ++lane) {
        const auto* out = &Output[lane * 16u];
        const auto red = out[0];
        Require(std::fabs(std::bit_cast<float>(red) - 1.0f / 255.0f) <= 1.0f / 1024.0f, std::string(name) + ": lane " + std::to_string(lane) + " 8_8_8_8_UNORM red is " + Hex(red));
        Require(out[8] == red, std::string(name) + ": lane " + std::to_string(lane) + " v_max_f32 of red and green is " + Hex(out[8]) + ", expected " + Hex(red));
        Require(out[9] == red, std::string(name) + ": lane " + std::to_string(lane) + " v_min_f32 of blue and red is " + Hex(out[9]) + ", expected " + Hex(red));
        Require(out[12] == 0u, std::string(name) + ": lane " + std::to_string(lane) + " v_max_f32 of a 32_FLOAT denormal and zero is " + Hex(out[12]) + ", expected 0x00000000");
        Require(out[13] == 0u, std::string(name) + ": lane " + std::to_string(lane) + " v_cmp_lt_f32 of zero and a 32_FLOAT denormal is " + Hex(out[13]) + ", expected 0x00000000");
    }
    return masks;
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto point = Check(*device, PointSampler, "point sampler");
        const auto bilinear = Check(*device, LinearSampler, "bilinear sampler");
        Require(point != 0u && point < bilinear, "image sample denormal free: " + std::to_string(point) + " f32 exponent masks with the point sampler and " + std::to_string(bilinear) + " with the bilinear one; the 8_8_8_8_UNORM flushes should fold away only with the point sampler, the 32_FLOAT ones never");
        std::puts("image sample denormal free tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
