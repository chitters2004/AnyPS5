#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
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
constexpr std::uint32_t Words = 21;
constexpr std::uint32_t LaneStride = 32;
constexpr std::uint32_t Format32Float = 22;
constexpr std::uint32_t Format8888UNorm = 56;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t IdentitySwizzle = 0xfacu;
constexpr std::array<std::uint32_t, 4> EdgeSampler{0x00000092u, 0x00fff000u, 0x00000000u, 0u};
constexpr std::array<std::uint32_t, 4> BorderSampler{0x000001b6u, 0x00fff000u, 0x00000000u, 0x80000000u};
constexpr std::array<std::uint8_t, 4> Quad{128u, 255u, 0u, 1u};

alignas(256) std::array<std::uint32_t, Threads * LaneStride> Output{};
alignas(256) std::array<std::uint8_t, 4096> Texels{};

alignas(256) constexpr std::array<std::uint32_t, 52> Code{
    0x34060087u, 0x7e5002ffu, 0x3e000000u, 0x7e5202ffu, 0x3e000000u, 0xbe9403ffu, 0x7e017e01u, 0xf11c0108u,
    0x80610a28u, 0xbf8c3f70u, 0xcc10400cu, 0x1802170au, 0xcc0e400du, 0x1c32170au, 0xd74b000eu, 0x0436170au,
    0xcc0f400fu, 0x1802190du, 0x6a2016ffu, 0x00007800u, 0x64222110u, 0x66242311u, 0x7e26a90au, 0x6a282680u,
    0x722a170au, 0x742c1712u, 0x742e1714u, 0x6a302280u, 0x74321718u, 0xcc0e401au, 0x18510111u, 0x7d98150bu,
    0x0236290bu, 0xd4c90016u, 0x0002150bu, 0xd501001cu, 0x005a1714u, 0x743a171bu, 0x743c171cu, 0xe0781000u,
    0x80000a03u, 0xe0781010u, 0x80000e03u, 0xe0781020u, 0x80001203u, 0xe0781030u, 0x80001603u, 0xe0781040u,
    0x80001a03u, 0xe0701050u, 0x80001e03u, 0xbf810000u,
};

struct Run {
    std::array<std::uint32_t, Words> words{};
    std::size_t queries = 0;
};

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

void Fill(std::uint32_t format) {
    const auto mips = AgcDriver::Graphics::ComputeMipLayout(AgcDriver::Graphics::TextureTileMode::kLinear, format, Size, Size, 1u);
    Require(AgcDriver::Graphics::ComputeSurfaceSize(mips, 1) <= Texels.size(), "image gather normalized halves: the texture does not fit its storage");
    const auto pitch = mips[0].pitchBytes;
    Texels.fill(0u);
    for (std::uint32_t y = 0; y < Size; ++y) {
        for (std::uint32_t x = 0; x < Size; ++x) {
            const std::uint8_t red = x < 2u && y < 2u ? Quad[y * 2u + x] : 64u;
            if (format == Format8888UNorm) {
                auto* texel = &Texels[static_cast<std::size_t>(y) * pitch + x * 4u];
                texel[0] = red;
                texel[1] = 0u;
                texel[2] = 0u;
                texel[3] = 255u;
                continue;
            }
            const auto bits = std::bit_cast<std::uint32_t>(static_cast<float>(red) / 255.0f);
            for (std::uint32_t byte = 0; byte < 4u; ++byte) Texels[static_cast<std::size_t>(y) * pitch + x * 4u + byte] = static_cast<std::uint8_t>(bits >> (byte * 8u));
        }
    }
}

std::size_t HalfQueries(std::span<const std::uint32_t> words) {
    std::set<std::uint32_t> limits;
    std::size_t queries = 0;
    for (std::size_t offset = 5; offset < words.size();) {
        const auto count = words[offset] >> spv::WordCountShift;
        Require(count != 0u && count <= words.size() - offset, "image gather normalized halves: the recompiler returned a malformed SPIR-V instruction");
        const auto opcode = static_cast<spv::Op>(words[offset] & spv::OpCodeMask);
        if (opcode == spv::OpConstant && count == 4u && words[offset + 3u] == 0x7c00u) limits.insert(words[offset + 2u]);
        const bool compare = opcode == spv::OpUGreaterThan || opcode == spv::OpIEqual || opcode == spv::OpINotEqual;
        if (compare && count == 5u && (limits.contains(words[offset + 3u]) || limits.contains(words[offset + 4u]))) ++queries;
        offset += count;
    }
    return queries;
}

Run Dispatch(AgcDriver::VulkanDevice& device, std::uint32_t format, const std::array<std::uint32_t, 4>& sampler) {
    Fill(format);
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(16, 0u);
    const auto address = reinterpret_cast<std::uintptr_t>(Output.data());
    const std::array<std::uint32_t, 4> output{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), static_cast<std::uint32_t>(Output.size() * 4u), 0x31016facu};
    const auto texels = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Texels.data()));
    const std::array<std::uint32_t, 8> texture{
        static_cast<std::uint32_t>(texels >> 8u),
        static_cast<std::uint32_t>((texels >> 40u) & 0xffu) | (format << 20u) | (((Size - 1u) & 3u) << 30u),
        ((Size - 1u) >> 2u) | ((Size - 1u) << 14u),
        IdentitySwizzle | (Type2D << 28u),
        0u, 0u, 0u, 0u,
    };
    std::copy(output.begin(), output.end(), userData.begin());
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    std::copy(sampler.begin(), sampler.end(), userData.begin() + 12);
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
    Run run;
    run.queries = HalfQueries(result.spirv.Words());
    for (std::uint32_t lane = 0; lane < Threads; ++lane) {
        for (std::uint32_t word = 0; word < Words; ++word) {
            const auto value = Output[lane * LaneStride + word];
            if (lane == 0u) run.words[word] = value;
            Require(value == run.words[word], "image gather normalized halves: lane " + std::to_string(lane) + " word " + std::to_string(word) + " is " + Hex(value) + ", lane 0 has " + Hex(run.words[word]));
        }
    }
    return run;
}

void CheckSpecials(const Run& run, const std::string& name) {
    struct Expected {
        std::uint32_t word;
        std::uint32_t bits;
        const char* what;
    };
    constexpr std::array<Expected, 15> expected{{
        {0u, 0x0000u, "the gathered low half (texel 0)"},
        {1u, 0x3c00u, "the gathered low half of the second word (texel 255)"},
        {6u, 0x7800u, "v_mul_f16 of 32768 and 1.0"},
        {7u, 0x7c00u, "v_add_f16 overflowing to infinity"},
        {8u, 0xfe00u, "v_sub_f16 of infinity and infinity"},
        {9u, 0x7c00u, "v_rcp_f16 of zero"},
        {10u, 0xfe00u, "v_mul_f16 of zero and infinity from v_rcp_f16"},
        {11u, 0x3c00u, "v_max_f16 of 0.0 and 1.0"},
        {12u, 0x3c00u, "v_min_f16 of a NaN from infinity minus infinity and 1.0"},
        {13u, 0x3c00u, "v_min_f16 of a NaN from zero times v_rcp_f16 and 1.0"},
        {14u, 0xfe00u, "v_mul_f16 of zero and an overflowed infinity"},
        {15u, 0x3c00u, "v_min_f16 of a NaN from zero times an overflowed infinity and 1.0"},
        {16u, 0xfe00u, "the low lane of v_pk_fma_f16 of an overflowed infinity, zero and a NaN addend"},
        {19u, 0x3c00u, "v_min_f16 of a NaN chosen by v_cndmask_b32 from its second source and 1.0"},
        {20u, 0x3c00u, "v_min_f16 of a NaN chosen by v_cndmask_b32 from its first source and 1.0"},
    }};
    for (const auto& entry : expected) {
        const auto bits = run.words[entry.word] & 0xffffu;
        Require(bits == (entry.bits & 0xffffu), name + ": " + entry.what + " is " + Hex(bits) + ", expected " + Hex(entry.bits & 0xffffu));
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto folded = Dispatch(*device, Format8888UNorm, EdgeSampler);
        const auto bordered = Dispatch(*device, Format8888UNorm, BorderSampler);
        const auto floating = Dispatch(*device, Format32Float, EdgeSampler);
        CheckSpecials(folded, "8_8_8_8_UNORM with a clamp-to-edge sampler");
        CheckSpecials(bordered, "8_8_8_8_UNORM with a clamp-to-border sampler");
        CheckSpecials(floating, "32_FLOAT with a clamp-to-edge sampler");
        for (std::uint32_t word = 0; word < Words; ++word) {
            Require(folded.words[word] == bordered.words[word], "image gather normalized halves: word " + std::to_string(word) + " is " + Hex(folded.words[word]) + " with the folded NaN checks and " + Hex(bordered.words[word]) + " without them");
        }
        Require(bordered.queries == floating.queries, "image gather normalized halves: a clamp-to-border sampler keeps " + std::to_string(bordered.queries) + " f16 NaN and infinity checks and a 32_FLOAT image " + std::to_string(floating.queries) + "; neither may fold");
        Require(folded.queries < bordered.queries, "image gather normalized halves: " + std::to_string(folded.queries) + " f16 NaN and infinity checks with the clamp-to-edge 8_8_8_8_UNORM gather and " + std::to_string(bordered.queries) + " without the normalized bound");
        std::printf("image gather normalized halves tests passed (%zu of %zu f16 checks kept)\n", folded.queries, bordered.queries);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
