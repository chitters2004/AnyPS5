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
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 64;
constexpr std::uint32_t Batch = 512;
constexpr std::uint32_t QuietBit = 0x0200u;
constexpr std::uint32_t DefaultNan = 0xfe00u;

enum class Operation { Add, Mul, Fma, Min, Max, Pack };

struct Variant {
    Operation operation;
    std::uint32_t opSel;
    std::uint32_t opSelHi;
    std::uint32_t negLo;
    std::uint32_t negHi;
    std::uint32_t abs;
};

constexpr std::array<std::uint16_t, 16> Halves{
    0x0000u, 0x8000u, 0x3e00u, 0xc000u, 0x3400u, 0x7bffu, 0x0001u, 0x7c00u,
    0xfc00u, 0x7e01u, 0x7c05u, 0xfe33u, 0x3c00u, 0x83ffu, 0x5640u, 0xbc00u,
};

alignas(256) std::array<std::uint32_t, Threads * 4> Inputs{};
alignas(256) std::array<std::uint32_t, Batch * Threads> Outputs{};

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

bool Nan(std::uint32_t half) {
    return (half & 0x7fffu) > 0x7c00u;
}

bool SignalingNan(std::uint32_t half) {
    return Nan(half) && (half & QuietBit) == 0u;
}

std::uint32_t HalfOf(std::uint32_t word, bool high) {
    return high ? word >> 16u : word & 0xffffu;
}

__int128 Scaled(std::uint32_t half) {
    const auto exponent = (half >> 10u) & 0x1fu;
    const auto mantissa = half & 0x3ffu;
    const __int128 magnitude = exponent == 0u ? static_cast<__int128>(mantissa) : static_cast<__int128>(mantissa | 0x400u) << (exponent - 1u);
    return (half & 0x8000u) != 0u ? -magnitude : magnitude;
}

std::uint32_t RoundedHalf(__int128 value, int scale, bool negativeZero) {
    if (value == 0) return negativeZero ? 0x8000u : 0u;
    const std::uint32_t sign = value < 0 ? 0x8000u : 0u;
    auto magnitude = static_cast<unsigned __int128>(value < 0 ? -value : value);
    int top = 127;
    while (((magnitude >> top) & 1u) == 0u) --top;
    const int exponent = top + scale;
    const int quantum = std::max(exponent, -14) - 10;
    const int shift = quantum - scale;
    unsigned __int128 kept = magnitude;
    int keptQuantum = scale;
    if (shift > 0) {
        kept = magnitude >> shift;
        const auto remainder = magnitude & ((static_cast<unsigned __int128>(1) << shift) - 1u);
        const auto half = static_cast<unsigned __int128>(1) << (shift - 1);
        if (remainder > half || (remainder == half && (kept & 1u) != 0u)) ++kept;
        keptQuantum = quantum;
    } else {
        kept = magnitude << -shift;
        keptQuantum = quantum;
    }
    if (kept == 2048u) {
        kept = 1024u;
        ++keptQuantum;
    }
    if (kept < 1024u) return sign | static_cast<std::uint32_t>(kept);
    const int biased = keptQuantum + 25;
    if (biased >= 31) return sign | 0x7c00u;
    return sign | (static_cast<std::uint32_t>(biased) << 10u) | static_cast<std::uint32_t>(kept - 1024u);
}

bool Infinite(std::uint32_t half) {
    return (half & 0x7fffu) == 0x7c00u;
}

bool Zero(std::uint32_t half) {
    return (half & 0x7fffu) == 0u;
}

std::uint32_t Arithmetic(Operation operation, std::uint32_t a, std::uint32_t b, std::uint32_t c, bool ieee) {
    const auto quiet = [&](std::uint32_t half) { return ieee ? half | QuietBit : half; };
    const bool invalidProduct = (Infinite(a) && Zero(b)) || (Infinite(b) && Zero(a));
    if (Nan(a)) return quiet(a);
    if (Nan(b)) return quiet(b);
    if (operation == Operation::Fma && Nan(c) && !invalidProduct) return quiet(c);
    if (operation == Operation::Fma && Nan(c)) return DefaultNan;
    const bool productNegative = ((a ^ b) & 0x8000u) != 0u;
    if (operation == Operation::Add) {
        if (Infinite(a) && Infinite(b)) return (a == b) ? a : DefaultNan;
        if (Infinite(a)) return a;
        if (Infinite(b)) return b;
        return RoundedHalf(Scaled(a) + Scaled(b), -24, (a & b & 0x8000u) != 0u);
    }
    if (invalidProduct) return DefaultNan;
    if (operation == Operation::Mul) {
        if (Infinite(a) || Infinite(b)) return (productNegative ? 0x8000u : 0u) | 0x7c00u;
        return RoundedHalf(Scaled(a) * Scaled(b), -48, productNegative);
    }
    if (Infinite(a) || Infinite(b)) {
        const std::uint32_t product = (productNegative ? 0x8000u : 0u) | 0x7c00u;
        if (Infinite(c) && c != product) return DefaultNan;
        return product;
    }
    if (Infinite(c)) return c;
    const __int128 sum = Scaled(a) * Scaled(b) + (Scaled(c) << 24);
    return RoundedHalf(sum, -48, productNegative && (c & 0x8000u) != 0u);
}

std::int32_t OrderKey(std::uint32_t half) {
    const auto value = static_cast<std::int32_t>(static_cast<std::int16_t>(half));
    return value ^ ((value >> 31) & 0x7fff);
}

std::uint32_t MinMax(bool maximum, std::uint32_t a, std::uint32_t b, bool ieee) {
    std::uint32_t lane = maximum ? (OrderKey(a) >= OrderKey(b) ? a : b) : (OrderKey(a) < OrderKey(b) ? a : b);
    if (Nan(a)) lane = b;
    if (Nan(b)) lane = a;
    if (!ieee) return lane;
    if (SignalingNan(b)) lane = b | QuietBit;
    if (SignalingNan(a)) lane = a | QuietBit;
    return lane;
}

std::uint32_t Expected(const Variant& variant, const std::array<std::uint32_t, 3>& sources, bool ieee) {
    if (variant.operation == Operation::Pack) {
        const auto half = [&](std::uint32_t index) {
            std::uint32_t bits = HalfOf(sources[index], ((variant.opSel >> index) & 1u) != 0u);
            if (((variant.abs >> index) & 1u) != 0u) bits &= 0x7fffu;
            if (((variant.negLo >> index) & 1u) != 0u) bits ^= 0x8000u;
            return Nan(bits) && ieee ? bits | QuietBit : bits;
        };
        return half(0u) | (half(1u) << 16u);
    }
    std::uint32_t result = 0u;
    for (std::uint32_t lane = 0; lane < 2u; ++lane) {
        const bool high = lane == 1u;
        std::array<std::uint32_t, 3> operands{};
        for (std::uint32_t index = 0; index < 3u; ++index) {
            const auto select = high ? variant.opSelHi : variant.opSel;
            const auto negate = high ? variant.negHi : variant.negLo;
            operands[index] = HalfOf(sources[index], ((select >> index) & 1u) != 0u) ^ (((negate >> index) & 1u) != 0u ? 0x8000u : 0u);
        }
        const auto value = variant.operation == Operation::Min ? MinMax(false, operands[0], operands[1], ieee)
            : variant.operation == Operation::Max ? MinMax(true, operands[0], operands[1], ieee)
            : Arithmetic(variant.operation, operands[0], operands[1], operands[2], ieee);
        result |= (value & 0xffffu) << (16u * lane);
    }
    return result;
}

std::vector<Variant> Variants() {
    std::vector<Variant> variants;
    for (const auto operation : {Operation::Add, Operation::Mul, Operation::Min, Operation::Max}) {
        for (std::uint32_t bits = 0; bits < 256u; ++bits) variants.push_back({operation, bits & 3u, (bits >> 2u) & 3u, (bits >> 4u) & 3u, (bits >> 6u) & 3u, 0u});
    }
    for (std::uint32_t bits = 0; bits < 4096u; ++bits) variants.push_back({Operation::Fma, bits & 7u, (bits >> 3u) & 7u, (bits >> 6u) & 7u, (bits >> 9u) & 7u, 0u});
    for (std::uint32_t bits = 0; bits < 64u; ++bits) variants.push_back({Operation::Pack, bits & 3u, 0u, (bits >> 2u) & 3u, 0u, (bits >> 4u) & 3u});
    return variants;
}

std::string Describe(const Variant& variant) {
    constexpr std::array<const char*, 6> Names{"v_pk_add_f16", "v_pk_mul_f16", "v_pk_fma_f16", "v_pk_min_f16", "v_pk_max_f16", "v_pack_b32_f16"};
    return std::string(Names[static_cast<std::size_t>(variant.operation)]) + " op_sel " + std::to_string(variant.opSel) + " op_sel_hi " + std::to_string(variant.opSelHi) + " neg_lo " + std::to_string(variant.negLo) + " neg_hi " + std::to_string(variant.negHi) + " abs " + std::to_string(variant.abs);
}

void Encode(std::vector<std::uint32_t>& code, const Variant& variant) {
    if (variant.operation == Operation::Pack) {
        code.push_back(0xd7110008u | (variant.abs << 8u) | (variant.opSel << 11u));
        code.push_back(0x00020501u | (variant.negLo << 29u));
        return;
    }
    constexpr std::array<std::uint32_t, 5> Opcodes{0x0fu, 0x10u, 0x0eu, 0x11u, 0x12u};
    const bool ternary = variant.operation == Operation::Fma;
    const auto selectHi = ternary ? variant.opSelHi : variant.opSelHi | 4u;
    code.push_back(0xcc000008u | (Opcodes[static_cast<std::size_t>(variant.operation)] << 16u) | (variant.negHi << 8u) | (variant.opSel << 11u) | ((selectHi >> 2u) << 14u));
    code.push_back(0x00020501u | (ternary ? 0x103u << 18u : 0u) | ((selectHi & 3u) << 27u) | (variant.negLo << 29u));
}

std::vector<std::uint32_t> Program(std::span<const Variant> variants) {
    std::vector<std::uint32_t> code{0x34080084u, 0x340a0082u, 0xe03c1000u, 0x80010104u, 0xbf8c3f70u};
    for (std::size_t index = 0; index < variants.size(); ++index) {
        Encode(code, variants[index]);
        code.push_back(0xbe8a03ffu);
        code.push_back(static_cast<std::uint32_t>(index * Threads * 4u));
        code.push_back(0xe0701000u);
        code.push_back(0x0a000805u);
    }
    code.push_back(0xbf810000u);
    return code;
}

void Fill() {
    std::uint32_t state = 0x12345678u;
    const auto next = [&] {
        state = state * 1664525u + 1013904223u;
        return state >> 28u;
    };
    for (std::uint32_t lane = 0; lane < Threads; ++lane) {
        for (std::uint32_t source = 0; source < 3u; ++source) {
            const auto low = lane < 16u ? Halves[(lane + source * 5u) % 16u] : Halves[next()];
            const auto high = lane < 16u ? Halves[(lane * 7u + source * 3u + 1u) % 16u] : Halves[next()];
            Inputs[lane * 4u + source] = low | (static_cast<std::uint32_t>(high) << 16u);
        }
        Inputs[lane * 4u + 3u] = 0u;
    }
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::size_t Run(AgcDriver::VulkanDevice& device, std::span<const Variant> variants, bool ieee) {
    Outputs.fill(0xdeadbeefu);
    const auto code = Program(variants);
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
    for (std::size_t index = 0; index < variants.size(); ++index) {
        for (std::uint32_t lane = 0; lane < Threads; ++lane) {
            const std::array<std::uint32_t, 3> sources{Inputs[lane * 4u], Inputs[lane * 4u + 1u], Inputs[lane * 4u + 2u]};
            const auto expected = Expected(variants[index], sources, ieee);
            const auto actual = Outputs[index * Threads + lane];
            if (actual == expected) continue;
            if (failures++ < 12u) {
                std::cerr << Describe(variants[index]) << (ieee ? " IEEE 1" : " IEEE 0") << " lane " << lane << " sources " << Hex(sources[0]) << " " << Hex(sources[1]) << " " << Hex(sources[2]) << ": " << Hex(actual) << ", expected " << Hex(expected) << '\n';
            }
        }
    }
    return failures;
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Fill();
        const auto variants = Variants();
        std::size_t failures = 0;
        for (const bool ieee : {false, true}) {
            for (std::size_t first = 0; first < variants.size(); first += Batch) {
                const auto count = std::min<std::size_t>(Batch, variants.size() - first);
                failures += Run(*device, std::span<const Variant>(variants).subspan(first, count), ieee);
            }
        }
        Require(failures == 0u, "packed f16 lane modifiers: " + std::to_string(failures) + " results differ from the lane model");
        std::printf("packed f16 lane modifier tests passed (%zu variants, %u lanes, 2 IEEE modes)\n", variants.size(), Threads);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
