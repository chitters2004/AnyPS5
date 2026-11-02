#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "Recompiler.hpp"
#include "RuntimeAbi.hpp"
#include "BdaAbi.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace {

using namespace ShaderRecompiler;
using AgcDriver::DriverDetail::ShaderSnapshot;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

SpirvTarget Target() {
    static constexpr std::array<std::uint32_t, 24> capabilities{
        spv::CapabilityShader, spv::CapabilityImageGatherExtended, spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess,
        spv::CapabilityClipDistance, spv::CapabilityCullDistance, spv::CapabilityFloat16, spv::CapabilityFloat64, spv::CapabilityInt16, spv::CapabilityInt8,
        spv::CapabilityStorageImageExtendedFormats, spv::CapabilityImageQuery, spv::CapabilityDerivativeControl, spv::CapabilitySampleRateShading, spv::CapabilityGroupNonUniform,
        spv::CapabilityGroupNonUniformVote, spv::CapabilityGroupNonUniformArithmetic, spv::CapabilityGroupNonUniformBallot, spv::CapabilityGroupNonUniformShuffle,
        spv::CapabilityStorageImageWriteWithoutFormat, spv::CapabilityStorageImageReadWithoutFormat, spv::CapabilityInt64Atomics, spv::CapabilityStorageBuffer16BitAccess};
    static constexpr std::array<std::string_view, 12> extensions{
        "SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage", "SPV_KHR_16bit_storage", "SPV_KHR_shader_ballot", "SPV_KHR_subgroup_vote", "SPV_EXT_shader_stencil_export",
        "SPV_KHR_float_controls", "SPV_EXT_demote_to_helper_invocation", "SPV_KHR_non_semantic_info", "SPV_EXT_shader_atomic_float_add", "SPV_KHR_shader_clock", "SPV_EXT_fragment_shader_interlock"};
    SpirvTarget target{};
    target.vulkanVersion = 0x00403000u;
    target.spirvVersion = 0x00010600u;
    target.subgroupSize = 32u;
    target.bdaAbiVersion = BdaAbi::Version;
    target.supportedCapabilities = capabilities;
    target.supportedExtensions = extensions;
    return target;
}

std::vector<std::uint32_t> PointerWords(const void* table) {
    const auto address = reinterpret_cast<std::uintptr_t>(table);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u)};
}

struct PixelStage {
    ShaderSnapshot snapshot;
    std::vector<std::uint32_t> users;
    RecompileRequest request{};

    PixelStage(std::span<const std::uint32_t> code, std::vector<std::uint32_t> words, std::uint64_t address)
        : snapshot{address, 0, 1, {code.begin(), code.end()}, {}}, users(std::move(words)) {
        request.shader = {ShaderStage::Fragment, snapshot.codeAddress, snapshot.code, 0, {}};
        request.context.waveSize = 32u;
        request.context.userDataBaseRegister = 0u;
        request.context.userData = users;
        request.context.pixel = ShaderPixelStageInfo{};
        request.context.pixel->wave32 = true;
        request.context.pixel->targetOutputMode[0] = 9u;
        request.target = Target();
        request.layout = {0u, 0u, 0u, 128u};
        request.useCache = true;
    }

    std::shared_ptr<const RecompileResult> Current() {
        AgcDriver::ShaderMemory draw({});
        auto invocation = AgcDriver::DriverDetail::InvocationFor(snapshot, 0, request);
        const auto capture = AgcDriver::DriverDetail::CaptureInvocation(draw, snapshot, 0, request, invocation);
        return invocation.Materialize(*capture);
    }

    std::shared_ptr<const RecompileResult> CurrentUncached() {
        request.useCache = false;
        auto result = Current();
        request.useCache = true;
        return result;
    }
};

struct DescriptorTable {
    static constexpr std::uint32_t Buffers = 8;
    alignas(256) std::array<std::uint32_t, Buffers * 4u + 8u> words{};
    std::vector<std::uint8_t> texels = std::vector<std::uint8_t>(std::size_t{1} << 21u);

    DescriptorTable() {
        for (std::uint32_t buffer = 0; buffer < Buffers; ++buffer) MoveBuffer(buffer, 0x10000000u + buffer * 0x1000u);
        MoveImage(0u);
    }

    void MoveBuffer(std::uint32_t buffer, std::uint32_t base) {
        words[buffer * 4u] = base;
        words[buffer * 4u + 1u] = 0u;
        words[buffer * 4u + 2u] = 0x100u;
        words[buffer * 4u + 3u] = 0x00027facu;
    }

    void MoveImage(std::uint32_t offset) {
        constexpr std::uint32_t Format8888UNorm = 56;
        constexpr std::uint32_t Type2D = 9;
        constexpr std::uint32_t width = 7u;
        constexpr std::uint32_t height = 7u;
        const auto base = (static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(texels.data())) + 255u) / 256u * 256u + offset % 8000u * 256u;
        const std::array<std::uint32_t, 8> image{static_cast<std::uint32_t>(base >> 8u), static_cast<std::uint32_t>((base >> 40u) & 0xffu) | (Format8888UNorm << 20u) | (width << 30u), height << 14u, 0xfacu | (Type2D << 28u), 0u, 0u, 0u, 0u};
        std::copy(image.begin(), image.end(), words.begin() + Buffers * 4u);
    }
};

std::vector<std::uint32_t> DescriptorPixelCode() {
    std::vector<std::uint32_t> code{0xf4100100u, 0xfa000000u, 0xf4100500u, 0xfa000040u, 0xf40c0900u, 0xfa000080u, 0xbf8cc07fu};
    for (std::uint32_t buffer = 0; buffer < DescriptorTable::Buffers; ++buffer) code.insert(code.end(), {0xf4200000u | ((44u + buffer) << 6u) | (2u + buffer * 2u), 0xfa000000u});
    code.insert(code.end(), {0xbf8cc07fu, 0x7e00022cu, 0x7e02022du, 0x7e04022eu, 0x7e06022fu, 0x4a000030u, 0x4a020231u, 0x4a040432u, 0x4a060633u, 0x7e100280u, 0x7e120280u,
        0xf0001f08u, 0x00090408u, 0xbf8c3f70u, 0x4a000104u, 0x4a020305u, 0x4a040506u, 0x4a060707u, 0xf800180fu, 0x03020100u, 0xbf810000u});
    return code;
}

void RequireSameResult(const RecompileResult& left, const RecompileResult& right, const std::string& what) {
    Require(left.pushConstants == right.pushConstants, what + ": the push constants differ from a materialization without the memo");
    Require(left.specializationId == right.specializationId, what + ": the specialization differs from a materialization without the memo");
    Require(left.bindings.size() == right.bindings.size(), what + ": the bindings differ from a materialization without the memo");
    for (std::size_t index = 0; index < left.bindings.size(); ++index) {
        const auto& a = left.bindings[index];
        const auto& b = right.bindings[index];
        const auto at = what + ": binding " + std::to_string(index);
        Require(a.binding == b.binding && a.role == b.role && a.kind == b.kind && a.count == b.count && a.descriptorSet == b.descriptorSet && a.readOnly == b.readOnly && a.imageShape == b.imageShape, at + " has another shape than a materialization without the memo");
        Require(a.guestDescriptor == b.guestDescriptor, at + " has other words than a materialization without the memo");
        Require(*a.traits == *b.traits, at + " has other element traits than a materialization without the memo");
    }
}

bool TableRole(DescriptorRole role) {
    return role == DescriptorRole::ImageTable || role == DescriptorRole::ImageStorageTable || role == DescriptorRole::SamplerTable;
}

void RequireSharedTraits(const RecompileResult& left, const RecompileResult& right, const std::string& what) {
    Require(left.bindings.size() == right.bindings.size(), what + ": the binding count changed");
    for (std::size_t index = 0; index < left.bindings.size(); ++index) {
        const auto& a = left.bindings[index];
        if (TableRole(a.role) || a.traits.Identity() == nullptr) continue;
        Require(a.traits.Shares(right.bindings[index].traits), what + ": binding " + std::to_string(index) + " holds its own copy of the element traits");
    }
}

std::size_t ElementCount(const DescriptorElementTraits& traits) {
    return traits.samplerDepthCompare.size() + traits.imageWritten.size() + traits.imageDepthCompare.size() + traits.imageAtomic.size() + traits.imageAtomic64.size() + traits.imageShapes.size() + traits.bufferAtomic.size() + traits.bufferWritten.size() + traits.samplerUnnormalized.size() +
        traits.samplerIntegerBorder.size() + traits.samplerCoordinatesIgnored.size() + traits.imageUnnormalized.size() + traits.imageUnnormalizedBaseLevel.size() + traits.imageSamplers.size() + traits.bufferRead.size() + traits.bufferExtent.size();
}

void CheckTraitsHandle() {
    SharedElementTraits empty;
    Require(empty.Identity() == nullptr && ElementCount(*empty) == 0u && empty->bufferWritten.empty(), "an empty traits handle is not empty");
    DescriptorElementTraits built;
    built.bufferWritten = {true, false};
    built.imageSamplers = {3u};
    SharedElementTraits original(built);
    auto copy = original;
    Require(copy.Shares(original) && copy == original, "a copied traits handle does not share its template");
    const auto* before = original.Identity();
    copy.Mutable().bufferWritten[1] = true;
    Require(original.Identity() == before && original->bufferWritten == std::vector<bool>{true, false}, "a write through a copy changed the shared template");
    Require(!copy.Shares(original) && copy->bufferWritten == std::vector<bool>{true, true} && copy->imageSamplers == std::vector<std::uint32_t>{3u}, "a write through a copy did not give the copy its own traits");
    const auto* unique = copy.Identity();
    copy.Mutable().imageSamplers.push_back(4u);
    Require(copy.Identity() == unique, "a write through the only holder copied the traits");
    Require(!(copy == original), "traits with other contents compare equal");
    DescriptorBinding binding{};
    binding.traits = original;
    auto other = binding;
    other.traits.Mutable().bufferWritten.clear();
    Require(binding.traits->bufferWritten.size() == 2u && other.traits->bufferWritten.empty(), "a copied binding wrote through to the binding it was copied from");
}

struct Counted {
    std::size_t buffers = 0;
    std::size_t images = 0;
};

Counted Count(const RecompileResult& result) {
    Counted counted;
    for (const auto& binding : result.bindings) {
        if (binding.role == DescriptorRole::GuestBuffers) counted.buffers += binding.guestDescriptor.size() / 4u;
        if (binding.role == DescriptorRole::GuestImages) counted.images += binding.guestDescriptor.size() / 8u;
    }
    return counted;
}

void CheckSharedTemplates() {
    static DescriptorTable table;
    const auto code = DescriptorPixelCode();
    PixelStage stage(code, PointerWords(table.words.data()), 0x290000u);
    const auto first = stage.Current();
    const auto counted = Count(*first);
    Require(counted.buffers == DescriptorTable::Buffers && counted.images == 1u, "the fixture does not bind its eight buffers and one image");
    std::size_t elements = 0;
    for (const auto& binding : first->bindings) elements += ElementCount(*binding.traits);
    Require(elements != 0u, "the fixture carries no element traits");
    RequireSameResult(*first, *stage.CurrentUncached(), "the first result");
    for (std::uint32_t round = 1; round < 6u; ++round) {
        table.MoveBuffer(round % DescriptorTable::Buffers, 0x20000000u + round * 0x100u);
        const auto moved = stage.Current();
        Require(moved != first, "a moved buffer reused the old result");
        RequireSameResult(*moved, *stage.CurrentUncached(), "a moved buffer in round " + std::to_string(round));
        RequireSharedTraits(*moved, *first, "a moved buffer in round " + std::to_string(round));
        table.MoveImage(round);
        const auto image = stage.Current();
        Require(image != moved, "a moved image reused the old result");
        RequireSameResult(*image, *stage.CurrentUncached(), "a moved image in round " + std::to_string(round));
        RequireSharedTraits(*image, *first, "a moved image in round " + std::to_string(round));
    }
    std::printf("shared templates: %zu bindings, %zu element trait entries per result\n", first->bindings.size(), elements);
}

struct Fnv {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    void Mix(std::uint64_t value) {
        hash ^= value;
        hash *= 0x100000001b3ull;
    }
    void Words(std::span<const std::uint32_t> words) {
        Mix(words.size());
        for (const auto word : words) Mix(word);
    }
    void Descriptor(std::span<const std::uint32_t> words, std::uint32_t mask) {
        Mix(words.size());
        Mix(mask);
        for (const auto word : words) Mix(word);
    }
    [[nodiscard]] std::uint64_t Finish() const { return hash; }
};

struct Snapshot {
    static constexpr std::uint32_t Buffers = 8;
    static constexpr std::uint32_t Images = 4;
    static constexpr std::uint32_t Samplers = 2;
    static constexpr std::uint32_t UserWords = 16;
    static constexpr std::uint32_t FlatWords = 24;
    std::array<std::uint32_t, Buffers * 4u + Images * 8u + Samplers * 4u + UserWords + FlatWords> words{};

    std::span<std::uint32_t> Buffer(std::uint32_t index) { return std::span(words).subspan(index * 4u, 4u); }
    std::span<std::uint32_t> Image(std::uint32_t index) { return std::span(words).subspan(Buffers * 4u + index * 8u, 8u); }
    std::span<std::uint32_t> Sampler(std::uint32_t index) { return std::span(words).subspan(Buffers * 4u + Images * 8u + index * 4u, 4u); }
    std::span<std::uint32_t> User() { return std::span(words).subspan(Buffers * 4u + Images * 8u + Samplers * 4u, UserWords); }
    std::span<std::uint32_t> Flat() { return std::span(words).subspan(Buffers * 4u + Images * 8u + Samplers * 4u + UserWords, FlatWords); }
    std::span<const std::uint32_t> Span(std::size_t first, std::size_t count) const { return std::span<const std::uint32_t>(words).subspan(first, count); }

    template <typename Hasher>
    std::uint64_t Hash() const {
        Hasher hasher;
        const auto descriptors = [&](std::size_t first, std::uint32_t count, std::uint32_t width) {
            hasher.Mix(count);
            for (std::uint32_t index = 0; index < count; ++index) {
                const auto value = Span(first + index * width, width);
                if constexpr (std::is_same_v<Hasher, Fnv>) {
                    hasher.Descriptor(value, 0xffu);
                } else {
                    hasher.Mix(width | (std::uint64_t{0xff} << 32u));
                    hasher.Pairs(value);
                }
            }
        };
        descriptors(0, Buffers, 4u);
        descriptors(Buffers * 4u, Images, 8u);
        descriptors(Buffers * 4u + Images * 8u, Samplers, 4u);
        hasher.Words(Span(Buffers * 4u + Images * 8u + Samplers * 4u + UserWords, FlatWords));
        hasher.Words(Span(Buffers * 4u + Images * 8u + Samplers * 4u, UserWords));
        for (std::uint32_t tail = 0; tail < 12u; ++tail) hasher.Mix(tail == 0u ? 1u : 0u);
        return hasher.Finish();
    }
};

Snapshot RealisticSnapshot(std::mt19937_64& random) {
    Snapshot snapshot;
    const auto heap = 0x200000000ull + (random() % 0x4000u) * 0x100000ull;
    for (std::uint32_t index = 0; index < Snapshot::Buffers; ++index) {
        const auto base = heap + (random() % 0x10000u) * 0x100u;
        const auto stride = std::array<std::uint32_t, 4>{0u, 4u, 16u, 32u}[random() % 4u];
        auto words = snapshot.Buffer(index);
        words[0] = static_cast<std::uint32_t>(base);
        words[1] = static_cast<std::uint32_t>(base >> 32u) | (stride << 16u);
        words[2] = static_cast<std::uint32_t>(random() % 0x10000u) * 16u;
        words[3] = std::array<std::uint32_t, 3>{0x00027facu, 0x20014facu, 0x0003afacu}[random() % 3u];
    }
    for (std::uint32_t index = 0; index < Snapshot::Images; ++index) {
        const auto base = heap + 0x40000000ull + (random() % 0x8000u) * 0x10000ull;
        const auto width = 1u + static_cast<std::uint32_t>(random() % 4096u);
        const auto height = 1u + static_cast<std::uint32_t>(random() % 4096u);
        const auto format = std::array<std::uint32_t, 5>{56u, 57u, 10u, 71u, 35u}[random() % 5u];
        auto words = snapshot.Image(index);
        words[0] = static_cast<std::uint32_t>(base >> 8u);
        words[1] = static_cast<std::uint32_t>((base >> 40u) & 0xffu) | (format << 20u) | ((width - 1u) << 30u);
        words[2] = ((width - 1u) >> 2u) | ((height - 1u) << 14u);
        words[3] = 0xfacu | (static_cast<std::uint32_t>(random() % 12u) << 16u) | (9u << 28u);
        words[4] = static_cast<std::uint32_t>(random() % 2u) << 13u;
        words[5] = 0u;
        words[6] = static_cast<std::uint32_t>(random() % 4u) == 0u ? static_cast<std::uint32_t>(random()) & 0xfffff00u : 0u;
        words[7] = 0u;
    }
    for (std::uint32_t index = 0; index < Snapshot::Samplers; ++index) {
        auto words = snapshot.Sampler(index);
        words[0] = static_cast<std::uint32_t>(random() % 8u) * 0x249u;
        words[1] = 0xfff000u;
        words[2] = std::array<std::uint32_t, 3>{0x00500000u, 0x05500000u, 0x0aa00000u}[random() % 3u];
        words[3] = 0u;
    }
    auto user = snapshot.User();
    for (std::uint32_t word = 0; word < Snapshot::UserWords; word += 2u) {
        if (random() % 2u == 0u) {
            const auto pointer = heap + (random() % 0x100000u) * 0x40u;
            user[word] = static_cast<std::uint32_t>(pointer);
            user[word + 1u] = static_cast<std::uint32_t>(pointer >> 32u);
        } else {
            user[word] = static_cast<std::uint32_t>(random() % 1024u);
            user[word + 1u] = std::bit_cast<std::uint32_t>(static_cast<float>(random() % 2048u) / 64.0f);
        }
    }
    auto flat = snapshot.Flat();
    for (std::uint32_t word = 0; word < Snapshot::FlatWords; ++word) flat[word] = word < 16u ? snapshot.words[word] : user[word - 16u];
    return snapshot;
}

struct Avalanche {
    double worst = 0.0;
    double expectedWorst = 0.0;
    double meanBias = 0.0;
    std::size_t stuck = 0;
};

template <typename Hasher>
Avalanche MeasureAvalanche(std::size_t samples, std::uint64_t seed) {
    std::mt19937_64 random(seed);
    constexpr std::size_t inputBits = std::tuple_size_v<decltype(Snapshot::words)> * 32u;
    std::vector<std::uint32_t> flips(inputBits * 64u);
    for (std::size_t sample = 0; sample < samples; ++sample) {
        auto snapshot = RealisticSnapshot(random);
        const auto base = snapshot.Hash<Hasher>();
        for (std::size_t bit = 0; bit < inputBits; ++bit) {
            snapshot.words[bit / 32u] ^= 1u << (bit % 32u);
            auto difference = snapshot.Hash<Hasher>() ^ base;
            snapshot.words[bit / 32u] ^= 1u << (bit % 32u);
            auto* row = flips.data() + bit * 64u;
            while (difference != 0u) {
                ++row[std::countr_zero(difference)];
                difference &= difference - 1u;
            }
        }
    }
    Avalanche result;
    double total = 0.0;
    for (const auto count : flips) {
        const auto bias = std::abs(2.0 * static_cast<double>(count) / static_cast<double>(samples) - 1.0);
        result.worst = std::max(result.worst, bias);
        total += bias;
        if (count == 0u || count == samples) ++result.stuck;
    }
    result.meanBias = total / static_cast<double>(flips.size());
    result.expectedWorst = std::sqrt(2.0 * std::log(static_cast<double>(flips.size()))) / std::sqrt(static_cast<double>(samples));
    return result;
}

struct Collisions {
    std::size_t keys = 0;
    std::size_t full = 0;
    std::size_t low = 0;
    std::size_t high = 0;
    double expected32 = 0.0;
    double setChiSquare = 0.0;
};

std::size_t CountDuplicates(std::vector<std::uint64_t> values) {
    std::ranges::sort(values);
    std::size_t duplicates = 0;
    for (std::size_t index = 1; index < values.size(); ++index) duplicates += values[index] == values[index - 1u];
    return duplicates;
}

template <typename Hasher>
Collisions MeasureCollisions(std::size_t keys, std::uint64_t seed) {
    std::mt19937_64 random(seed);
    auto snapshot = RealisticSnapshot(random);
    std::vector<std::uint64_t> hashes;
    hashes.reserve(keys);
    for (std::size_t key = 0; key < keys; ++key) {
        const auto field = key % 4u;
        const auto step = static_cast<std::uint32_t>(key / 4u);
        auto copy = snapshot;
        if (field == 0u) copy.Buffer(step % Snapshot::Buffers)[0] += (step / Snapshot::Buffers + 1u) * 0x100u;
        if (field == 1u) copy.User()[0] = step;
        if (field == 2u) copy.Image(step % Snapshot::Images)[0] += step / Snapshot::Images + 1u;
        if (field == 3u) {
            copy.Buffer(step % Snapshot::Buffers)[0] += (step % 251u + 1u) * 0x100u;
            copy.Buffer((step / 251u) % Snapshot::Buffers)[2] += (step / 2008u + 1u) * 16u;
        }
        for (std::uint32_t word = 0; word < 16u; ++word) copy.Flat()[word] = copy.words[word];
        hashes.push_back(copy.Hash<Hasher>());
    }
    Collisions result;
    result.keys = keys;
    std::vector<std::uint64_t> low(keys), high(keys);
    std::array<std::size_t, 64> sets{};
    for (std::size_t key = 0; key < keys; ++key) {
        low[key] = hashes[key] & 0xffffffffu;
        high[key] = hashes[key] >> 32u;
        const auto index = (0x1234ull * 0x9e3779b97f4a7c15ull) ^ hashes[key];
        ++sets[(index ^ (index >> 32u)) % 64u];
    }
    result.full = CountDuplicates(hashes);
    result.low = CountDuplicates(low);
    result.high = CountDuplicates(high);
    const auto n = static_cast<double>(keys);
    result.expected32 = n * (n - 1.0) / 2.0 / 4294967296.0;
    const auto expectedSet = n / 64.0;
    for (const auto count : sets) result.setChiSquare += (static_cast<double>(count) - expectedSet) * (static_cast<double>(count) - expectedSet) / expectedSet;
    return result;
}

template <typename Hasher>
std::uint64_t DistinctKeys(std::size_t keys, std::uint64_t seed) {
    std::mt19937_64 random(seed);
    const auto snapshot = RealisticSnapshot(random);
    std::vector<std::uint64_t> hashes;
    for (std::size_t key = 0; key < keys; ++key) {
        auto copy = snapshot;
        copy.Buffer(key % Snapshot::Buffers)[0] += static_cast<std::uint32_t>(key / Snapshot::Buffers + 1u) * 0x100u;
        hashes.push_back(copy.Hash<Hasher>());
    }
    return CountDuplicates(hashes);
}

void PrintQuality(const char* name, const Avalanche& avalanche, const Collisions& collisions) {
    std::printf("%-6s avalanche: worst bias %.4f (ideal about %.4f), mean bias %.4f, %zu stuck input/output bit pairs\n", name, avalanche.worst, avalanche.expectedWorst, avalanche.meanBias, avalanche.stuck);
    std::printf("%-6s collisions over %zu realistic near keys: %zu in 64 bits, %zu in the low 32 bits, %zu in the high 32 bits (expected %.1f for 32 bits); memo set chi-square %.1f over 63 degrees of freedom\n", name, collisions.keys, collisions.full, collisions.low, collisions.high, collisions.expected32, collisions.setChiSquare);
}

void CheckHashQuality(bool large) {
    const std::size_t samples = large ? 4000u : 120u;
    const std::size_t keys = large ? std::size_t{1} << 24u : std::size_t{1} << 20u;
    const auto avalanche = MeasureAvalanche<SnapshotHasher>(samples, 0x5eedu);
    const auto collisions = MeasureCollisions<SnapshotHasher>(keys, 0x5eed1u);
    PrintQuality("new", avalanche, collisions);
    if (large) PrintQuality("FNV", MeasureAvalanche<Fnv>(samples, 0x5eedu), MeasureCollisions<Fnv>(keys, 0x5eed1u));
    Require(avalanche.stuck == 0u, "an input bit of a realistic snapshot never or always flips an output bit of the snapshot hash");
    Require(avalanche.worst < 2.0 * avalanche.expectedWorst, "the snapshot hash avalanche bias exceeds twice that of an ideal hash");
    Require(collisions.full == 0u, "realistic near snapshots collide in all 64 bits of the snapshot hash");
    const auto slack = 6.0 * std::sqrt(collisions.expected32) + 4.0;
    Require(std::abs(static_cast<double>(collisions.low) - collisions.expected32) < slack && std::abs(static_cast<double>(collisions.high) - collisions.expected32) < slack, "the snapshot hash halves collide more or less often than an ideal 32-bit hash");
    Require(collisions.setChiSquare < 130.0, "the snapshot hash spreads realistic keys unevenly over the memo sets");
    Require(DistinctKeys<SnapshotHasher>(std::size_t{1} << 16u, 0x5eed2u) == 0u, "single buffer moves collide in the snapshot hash");
}

struct Spread {
    double median;
    double low;
    double high;
};

Spread Measure(std::size_t batches, std::size_t iterations, auto&& body) {
    std::vector<double> samples;
    for (std::size_t warm = 0; warm < iterations; ++warm) body(warm);
    for (std::size_t batch = 0; batch < batches; ++batch) {
        const auto started = std::chrono::steady_clock::now();
        for (std::size_t iteration = 0; iteration < iterations; ++iteration) body(batch * iterations + iteration);
        samples.push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - started).count() / static_cast<double>(iterations));
    }
    std::ranges::sort(samples);
    const auto at = [&](double fraction) { return samples[static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1u))]; };
    return {at(0.5), at(0.1), at(0.9)};
}

void Print(const std::string& name, const Spread& spread) {
    std::printf("%-48s median %8.1f ns  p10 %8.1f  p90 %8.1f\n", name.c_str(), spread.median, spread.low, spread.high);
}

void Bench() {
    constexpr std::size_t batches = 21;
    constexpr std::size_t iterations = 2000;
    static DescriptorTable table;
    const auto code = DescriptorPixelCode();
    PixelStage stage(code, PointerWords(table.words.data()), 0x2a0000u);
    static_cast<void>(stage.Current());
    const auto phases = [&](const char* name, auto&& change) {
        std::array<std::vector<double>, 3> samples;
        std::uint64_t sequence = 0;
        for (std::size_t batch = 0; batch <= batches; ++batch) {
            std::array<double, 3> spent{};
            for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                change(++sequence);
                AgcDriver::ShaderMemory draw({});
                const auto started = std::chrono::steady_clock::now();
                auto invocation = AgcDriver::DriverDetail::InvocationFor(stage.snapshot, 0, stage.request);
                const auto invoked = std::chrono::steady_clock::now();
                const auto capture = AgcDriver::DriverDetail::CaptureInvocation(draw, stage.snapshot, 0, stage.request, invocation);
                const auto captured = std::chrono::steady_clock::now();
                static_cast<void>(invocation.Materialize(*capture));
                const auto materialized = std::chrono::steady_clock::now();
                spent[0] += std::chrono::duration<double, std::nano>(invoked - started).count();
                spent[1] += std::chrono::duration<double, std::nano>(captured - invoked).count();
                spent[2] += std::chrono::duration<double, std::nano>(materialized - captured).count();
            }
            if (batch == 0) continue;
            for (std::size_t phase = 0; phase < 3; ++phase) samples[phase].push_back(spent[phase] / static_cast<double>(iterations));
        }
        const std::array<const char*, 3> names{"invocation", "capture", "materialize"};
        for (std::size_t phase = 0; phase < 3; ++phase) {
            auto& values = samples[phase];
            std::ranges::sort(values);
            const auto at = [&](double fraction) { return values[static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1u))]; };
            Print(std::string(name) + ", " + names[phase], {at(0.5), at(0.1), at(0.9)});
        }
    };
    phases("memo hit", [](std::uint64_t) {});
    phases("moved buffer (patched)", [&](std::uint64_t sequence) { table.MoveBuffer(static_cast<std::uint32_t>(sequence % DescriptorTable::Buffers), 0x20000000u + static_cast<std::uint32_t>(sequence) * 0x100u); });
    phases("moved image (miss)", [&](std::uint64_t sequence) { table.MoveImage(static_cast<std::uint32_t>(sequence)); });
    std::mt19937_64 random(0x5eedu);
    std::vector<Snapshot> snapshots;
    for (std::size_t index = 0; index < 64u; ++index) snapshots.push_back(RealisticSnapshot(random));
    std::uint64_t sink = 0;
    Print("realistic snapshot stream, FNV", Measure(batches, iterations * 10u, [&](std::size_t index) { sink += snapshots[index % snapshots.size()].Hash<Fnv>(); }));
    Print("realistic snapshot stream, new", Measure(batches, iterations * 10u, [&](std::size_t index) { sink += snapshots[index % snapshots.size()].Hash<SnapshotHasher>(); }));
    const auto result = stage.Current();
    Print("RecompileResult copy", Measure(batches, iterations, [&](std::size_t) {
        auto copy = std::make_shared<RecompileResult>(*result);
        asm volatile("" : : "r"(copy.get()) : "memory");
    }));
    std::printf("sink %llu\n", static_cast<unsigned long long>(sink & 1u));
}

}

int main(int argc, char** argv) {
    try {
        Require(argc <= 2, "invalid test arguments");
        if (argc == 2) {
            const std::string_view mode(argv[1]);
            if (mode == "--quality") {
                CheckHashQuality(true);
                return 0;
            }
            Require(mode == "--bench", "invalid test arguments");
            Bench();
            return 0;
        }
        CheckTraitsHandle();
        CheckSharedTemplates();
        CheckHashQuality(false);
        std::cout << "patched path tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
