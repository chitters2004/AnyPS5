#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
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
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

using AgcDriver::Graphics::CompiledShader;
using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::ShaderResources;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Lanes = 64;
constexpr std::uint32_t Textures = 96;
constexpr std::uint32_t Stride = 32;
constexpr std::size_t SurfaceBytes = 256;
constexpr std::size_t SurfaceStride = 65536;
constexpr std::uint32_t Format8888UNorm = 56;
constexpr std::uint32_t Type2D = 9;
constexpr std::size_t SrtOffset = 4096;
constexpr std::size_t InputOffset = 8192;
constexpr std::size_t OutputOffset = 12288;
constexpr std::size_t HeapOffset = 65536;
constexpr std::size_t BlockBytes = (HeapOffset + std::size_t{Textures} * Stride + 65535u) & ~std::size_t{65535};
constexpr std::size_t SurfaceBlockBytes = std::size_t{Textures} * SurfaceStride;

constexpr std::array<std::uint32_t, 25> Code{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080300u, 0xfa000020u, 0xf4080700u, 0xfa000030u,
    0x34080082u, 0x7e200500u, 0x8f108510u, 0xf42c0502u, 0x20000000u, 0xbf8cc07fu,
    0xe0301000u, 0x80030504u, 0xf09c0f08u, 0x00450000u, 0xbf8c3f70u, 0x4a000105u,
    0xe0701000u, 0x80070004u, 0xbf810000u, 0u, 0u};

class GuestBlock {
public:
    GuestBlock(std::size_t bytes, bool gpu) {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, 65536));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
#else
        constexpr std::uintptr_t alignment = 65536;
        void* mapped = mmap(nullptr, bytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + bytes != begin + bytes + alignment) munmap(reinterpret_cast<void*>(aligned + bytes), begin + alignment - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
        }
#endif
        Require(block != nullptr, "changed relook: cannot allocate a guest block");
        std::memset(block, 0, bytes);
        GuestAllocations::Mutation().Add(block, bytes, true, true, gpu);
        AgcDriver::GuestMemory::CollectWritesUncached(reinterpret_cast<std::uintptr_t>(block), bytes);
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint64_t At(std::size_t offset) const { return reinterpret_cast<std::uint64_t>(block + offset); }
    std::uint8_t* Bytes(std::size_t offset) const { return block + offset; }
    std::uint32_t* Words(std::size_t offset) const { return reinterpret_cast<std::uint32_t*>(block + offset); }

private:
    std::uint8_t* block = nullptr;
};

std::array<std::uint32_t, 4> BufferDescriptor(std::uint64_t address, std::uint32_t stride, std::uint32_t records) {
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), records, stride == 0u ? 0x31016facu : 0x01016facu};
}

void Prepare(const GuestBlock& block, const GuestBlock& surfaces) {
    std::memcpy(block.Words(0), Code.data(), sizeof(Code));
    for (std::uint32_t texture = 0; texture < Textures; ++texture) {
        const auto offset = static_cast<std::size_t>(texture) * SurfaceStride;
        std::memset(surfaces.Bytes(offset), static_cast<int>(texture & 0xffu), SurfaceBytes);
        const auto surface = surfaces.At(offset);
        const std::array<std::uint32_t, 8> words{static_cast<std::uint32_t>(surface >> 8u), static_cast<std::uint32_t>((surface >> 40u) & 0xffu) | (Format8888UNorm << 20u) | (3u << 30u), 1u | (7u << 14u), 0xfacu | (Type2D << 28u), 0u, 0u, 0u, 0u};
        std::memcpy(block.Words(HeapOffset + static_cast<std::size_t>(texture) * Stride), words.data(), sizeof(words));
    }
    auto* srt = block.Words(SrtOffset);
    const auto heap = BufferDescriptor(block.At(HeapOffset), Stride, Textures);
    const auto input = BufferDescriptor(block.At(InputOffset), 0u, Lanes * 4u);
    const auto output = BufferDescriptor(block.At(OutputOffset), 0u, Lanes * 4u);
    std::copy(heap.begin(), heap.end(), srt);
    std::copy(input.begin(), input.end(), srt + 8);
    std::copy(output.begin(), output.end(), srt + 12);
    AgcDriver::GuestMemory::CollectWritesUncached(block.At(0), BlockBytes);
    AgcDriver::GuestMemory::CollectWritesUncached(surfaces.At(0), SurfaceBlockBytes);
}

ShaderRecompiler::RecompileResult Compile(const AgcDriver::VulkanDevice& device, const GuestBlock& block) {
    const auto srt = block.At(SrtOffset);
    const std::array<std::uint32_t, 2> userData{static_cast<std::uint32_t>(srt), static_cast<std::uint32_t>(srt >> 32u)};
    const std::span<const std::uint32_t> code(block.Words(0), Code.size());
    const std::array<ShaderRecompiler::MemoryRegion, 3> memory{{
        {block.At(0), std::as_bytes(code)},
        {srt, std::as_bytes(std::span<const std::uint32_t>(block.Words(SrtOffset), 64))},
        {block.At(HeapOffset), std::as_bytes(std::span<const std::uint32_t>(block.Words(HeapOffset), static_cast<std::size_t>(Textures) * Stride / 4u))},
    }};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, block.At(0), code, 0, {}},
        {Lanes, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    return ShaderRecompiler::Recompile(request);
}

ShaderResources::ProofReport Prove(ShaderResources& resources, const CompiledShader& shader, bool& current) {
    ShaderResources::ProofReport report;
    current = resources.Revalidate(shader, &report);
    return report;
}

std::string Path(const ShaderResources::ProofReport& report) {
    switch (report.path) {
        case ShaderResources::ProofPath::Fast: return "fast";
        case ShaderResources::ProofPath::OwnRefreshed: return "own refresh";
        case ShaderResources::ProofPath::Relooked: return "relook of the changed textures";
        default: return "full walk";
    }
}

std::size_t SampledEntries() {
    return AgcDriver::Graphics::TextureCacheUsage().sampledEntries;
}

void RunTests(const AgcDriver::VulkanDevice& device) {
    GuestBlock block(BlockBytes, true);
    GuestBlock surfaces(SurfaceBlockBytes, false);
    Prepare(block, surfaces);
    const auto result = Compile(device, block);
    const auto table = std::find_if(result.bindings.begin(), result.bindings.end(), [](const auto& binding) { return binding.role == ShaderRecompiler::DescriptorRole::ImageTable; });
    Require(table != result.bindings.end() && table->count == Textures, "changed relook: the program does not bind an image table of " + std::to_string(Textures) + " textures");
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    const CompiledShader shader{ShaderStage::Compute, &result, 0};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    auto resources = std::make_shared<ShaderResources>(context, shader);
    Require(resources->Reusable() && resources->SampledTextures().size() == Textures, "changed relook: the set over " + std::to_string(Textures) + " textures is not reusable");
    const auto built = std::vector<std::shared_ptr<AgcDriver::Graphics::Texture>>(resources->SampledTextures().begin(), resources->SampledTextures().end());
    bool current = false;
    auto report = Prove(*resources, shader, current);
    Require(current && report.path == ShaderResources::ProofPath::Fast, "changed relook: the set took the " + Path(report) + " with its memory unchanged after the build");
    const auto store = [&](std::uint32_t texture, bool change) {
        auto* first = reinterpret_cast<volatile std::uint8_t*>(surfaces.Bytes(static_cast<std::size_t>(texture) * SurfaceStride));
        const std::uint8_t saved = *first;
        *first = change ? static_cast<std::uint8_t>(saved ^ 0xffu) : saved;
    };
    for (const auto& written : std::vector<std::vector<std::uint32_t>>{{Textures / 3u}, {1u, Textures - 1u}}) {
        for (const auto texture : written) store(texture, false);
        const auto lookups = ShaderResources::RelookedLookups();
        report = Prove(*resources, shader, current);
        const auto relooked = ShaderResources::RelookedLookups() - lookups;
        Require(relooked == written.size(), "changed relook: stores over " + std::to_string(written.size()) + " textures looked " + std::to_string(relooked) + " textures up again");
        Require(current && report.path == ShaderResources::ProofPath::Relooked, "changed relook: stores of unchanged bytes over " + std::to_string(written.size()) + " textures took the " + Path(report) + (current ? "" : " and failed") + ", not the relook of those textures alone");
        Require(std::equal(built.begin(), built.end(), resources->SampledTextures().begin(), resources->SampledTextures().end()), "changed relook: the relook replaced a bound texture");
        report = Prove(*resources, shader, current);
        Require(current && report.path == ShaderResources::ProofPath::Fast, "changed relook: the set took the " + Path(report) + " after a relook, so the relook did not move the records of the textures it looked up");
    }
    std::puts("stores of unchanged bytes relook only the stored textures");
    store(Textures / 2u, true);
    report = Prove(*resources, shader, current);
    Require(!current, "changed relook: a set whose texture " + std::to_string(Textures / 2u) + " changed was proved current on the " + Path(report));
    Require(report.path == ShaderResources::ProofPath::Relooked, "changed relook: a changed texture was failed by the " + Path(report) + ", not by its own lookup");
    std::puts("a changed texture fails the proof at its own lookup");
    resources = std::make_shared<ShaderResources>(context, shader);
    report = Prove(*resources, shader, current);
    Require(current && report.path == ShaderResources::ProofPath::Fast, "changed relook: the rebuilt set took the " + Path(report));
    AgcDriver::Graphics::ClearCachedTextures(device.Device());
    Require(SampledEntries() == 0, "changed relook: clearing the sampled cache left entries");
    report = Prove(*resources, shader, current);
    Require(!current, "changed relook: a set whose textures left the sampled cache was proved current on the " + Path(report));
    Require(SampledEntries() == 0, "changed relook: a set whose textures left the sampled cache looked " + std::to_string(SampledEntries()) + " textures up again before failing");
    std::puts("a set whose textures were released fails without looking them up");
}



constexpr std::size_t Slot = 262144;
constexpr std::uint32_t Linear = 0;
constexpr std::uint32_t Tiled64KB = 0x1b;
constexpr std::uint32_t Type2DArray = 13;

std::array<std::uint32_t, 8> SurfaceWords(std::uint64_t address, std::uint32_t side, std::uint32_t tile, std::uint32_t type, std::uint32_t lastSlice, std::uint64_t keys) {
    return {static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format8888UNorm << 20u) | (((side - 1u) & 3u) << 30u), ((side - 1u) >> 2u) | ((side - 1u) << 14u), 0xfacu | (tile << 20u) | (type << 28u), lastSlice, 0u, keys != 0 ? (1u << 21u) | (static_cast<std::uint32_t>((keys >> 8u) & 0xffu) << 24u) : 0u, static_cast<std::uint32_t>(keys >> 16u)};
}

ShaderRecompiler::DescriptorBinding ImageBinding(ShaderRecompiler::DescriptorKind kind, std::uint32_t slot, const std::array<std::uint32_t, 8>& words) {
    ShaderRecompiler::DescriptorBinding binding;
    binding.kind = kind;
    binding.role = ShaderRecompiler::DescriptorRole::GuestImages;
    binding.descriptorSet = 0;
    binding.binding = (kind == ShaderRecompiler::DescriptorKind::StorageImage ? ShaderRecompiler::RuntimeAbi::FirstStorageImageBinding : ShaderRecompiler::RuntimeAbi::FirstImageBinding) + slot;
    binding.count = 1;
    binding.imageShape = ShaderRecompiler::DescriptorImageShape::Image2D;
    if (kind == ShaderRecompiler::DescriptorKind::SampledImage) binding.imageSamplers = {0};
    else binding.imageWritten = {false};
    binding.guestDescriptor.assign(words.begin(), words.end());
    return binding;
}

struct Synthetic {
    ShaderRecompiler::RecompileResult program;
    CompiledShader shader{ShaderStage::Compute, nullptr, 0};
    std::shared_ptr<ShaderResources> resources;
};

void Build(Synthetic& set, const AgcDriver::Graphics::Context& context, std::vector<ShaderRecompiler::DescriptorBinding> bindings, const std::string& name) {
    set.program = {};
    set.program.bindings = std::move(bindings);
    set.shader = CompiledShader{ShaderStage::Compute, &set.program, 0};
    set.resources = std::make_shared<ShaderResources>(context, set.shader);
    Require(set.resources->Reusable(), "changed relook: the " + name + " set is not reusable");
    bool current = false;
    auto report = Prove(*set.resources, set.shader, current);
    if (report.path != ShaderResources::ProofPath::Fast) {
        std::printf("%s: the first proof after the build took the %s (failure %d, current %d)\n", name.c_str(), Path(report).c_str(), static_cast<int>(report.failure), current);
        report = Prove(*set.resources, set.shader, current);
    }
    Require(current && report.path == ShaderResources::ProofPath::Fast, "changed relook: the " + name + " set took the " + Path(report) + " with its memory unchanged after the build");
}

ShaderResources::ProofReport ProveCounted(Synthetic& set, bool& current, std::uint64_t& relooked) {
    const auto before = ShaderResources::RelookedLookups();
    const auto report = Prove(*set.resources, set.shader, current);
    relooked = ShaderResources::RelookedLookups() - before;
    return report;
}

void Touch(const GuestBlock& block, std::size_t offset, bool change) {
    auto* first = reinterpret_cast<volatile std::uint8_t*>(block.Bytes(offset));
    const std::uint8_t saved = *first;
    *first = change ? static_cast<std::uint8_t>(saved ^ 0xffu) : saved;
}

void SyntheticTests(const AgcDriver::VulkanDevice& device) {
    using Kind = ShaderRecompiler::DescriptorKind;
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    GuestBlock block(8 * Slot, true);
    for (std::size_t i = 0; i < 8 * Slot; ++i) *block.Bytes(i) = static_cast<std::uint8_t>(i * 7u + 3u);
    AgcDriver::GuestMemory::CollectWritesUncached(block.At(0), 8 * Slot);
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    bool current = false;
    std::uint64_t relooked = 0;

    const auto expect = [&](const ShaderResources::ProofReport& report, bool wantCurrent, ShaderResources::ProofPath wantPath, std::uint64_t wantRelooked, const std::string& what) {
        Require(current == wantCurrent && report.path == wantPath && relooked == wantRelooked, "changed relook: " + what + " took the " + Path(report) + (current ? ", current," : ", not current,") + " after " + std::to_string(relooked) + " relooked lookups");
    };
    using ShaderResources::ProofPath::Relooked;
    using ShaderResources::ProofPath::Full;
    if (std::getenv("APS5_NO_HOST_IMPORT") != nullptr) {
        Synthetic alias;
        const auto aliased = SurfaceWords(block.At(0), 64, Linear, Type2D, 0, 0);
        Build(alias, context, {ImageBinding(Kind::StorageImage, 0, aliased), ImageBinding(Kind::SampledImage, 0, aliased)}, "storage and sampled alias");
        Touch(block, 0, false);
        expect(ProveCounted(alias, current, relooked), true, Relooked, 2, "a same-byte store under a storage image and its sampled view");
        Touch(block, 0, true);
        expect(ProveCounted(alias, current, relooked), false, Relooked, 2, "a changed byte under a storage image and its sampled view");
        std::puts("a storage image and its sampled view relook together");
    } else {
        std::puts("imported memory is compared, not write-watched, on this device: the storage alias case runs with APS5_NO_HOST_IMPORT=1");
    }

    Synthetic layered;
    Build(layered, context, {ImageBinding(Kind::SampledImage, 0, SurfaceWords(block.At(Slot), 64, Linear, Type2DArray, 3, 0))}, "first layer of a 2D array");
    Touch(block, Slot, false);
    expect(ProveCounted(layered, current, relooked), true, Relooked, 1, "a same-byte store in the sampled first layer of a 2D array");
    Touch(block, Slot, true);
    expect(ProveCounted(layered, current, relooked), false, Relooked, 1, "a changed first layer of a 2D array");
    std::puts("the first layer of a 2D array relooks through its narrowed descriptor");

    const auto keys = block.At(3 * Slot);
    const auto keyBytes = AgcDriver::Graphics::DccKeyBytes(65536);
    std::memset(block.Bytes(3 * Slot), 0xff, keyBytes);
    AgcDriver::GuestMemory::MarkWritten(keys, keyBytes);
    Synthetic compressed;
    Build(compressed, context, {ImageBinding(Kind::SampledImage, 0, SurfaceWords(block.At(2 * Slot), 64, Tiled64KB, Type2D, 0, keys))}, "DCC surface");
    Touch(block, 2 * Slot, false);
    expect(ProveCounted(compressed, current, relooked), true, Relooked, 1, "a same-byte store under uncompressed DCC keys");
    std::memset(block.Bytes(3 * Slot), 0x00, keyBytes);
    AgcDriver::GuestMemory::MarkWritten(keys, keyBytes);
    expect(ProveCounted(compressed, current, relooked), false, Full, 0, "a fast clear of the DCC keys");
    std::puts("a DCC key change leaves the relook for the full walk");

    Synthetic pair;
    Build(pair, context, {ImageBinding(Kind::SampledImage, 0, SurfaceWords(block.At(4 * Slot), 8, Linear, Type2D, 0, 0)), ImageBinding(Kind::SampledImage, 1, SurfaceWords(block.At(5 * Slot), 8, Linear, Type2D, 0, 0))}, "two textures");
    const auto kept = pair.resources->SampledTextures()[1];
    AgcDriver::Graphics::OverrideCacheBudgetForTest(1);
    Touch(block, 4 * Slot, true);
    const auto evicting = ProveCounted(pair, current, relooked);
    AgcDriver::Graphics::OverrideCacheBudgetForTest(0);
    expect(evicting, false, Relooked, 1, "a changed texture whose new copy evicts the other bound texture");
    Require(kept->Released(), "changed relook: the changed texture's new copy did not evict the other bound texture, so eviction was not exercised");
    std::puts("a relook whose lookup evicts another bound texture fails at that lookup");
}

}

int main() {
#ifdef _WIN32
    if (std::getenv("APS5_TEMPLATE_AUDIT_MS") == nullptr) _putenv_s("APS5_TEMPLATE_AUDIT_MS", "0");
#else
    setenv("APS5_TEMPLATE_AUDIT_MS", "0", 0);
#endif
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (std::getenv("APS5_NO_HOST_IMPORT") == nullptr) RunTests(*device);
        SyntheticTests(*device);
        std::puts("changed relook tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
