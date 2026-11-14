#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
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
using AgcDriver::Graphics::StorageTexture;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Lanes = 64;
constexpr std::uint32_t Textures = 8;
constexpr std::uint32_t Stride = 32;
constexpr std::size_t SurfaceBytes = 256;
constexpr std::uint32_t Format8888UNorm = 56;
constexpr std::uint32_t Type2D = 9;
constexpr std::size_t SrtOffset = 4096;
constexpr std::size_t InputOffset = 8192;
constexpr std::size_t OutputOffset = 12288;
constexpr std::size_t HeapOffset = 65536;
constexpr std::size_t BlockBytes = (HeapOffset + std::size_t{Textures} * Stride + 65535u) & ~std::size_t{65535};
constexpr std::size_t SurfaceBlockBytes = (std::size_t{Textures} * SurfaceBytes + 65535u) & ~std::size_t{65535};

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
        Require(block != nullptr, "scoped lookup proof: cannot allocate a guest block");
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
        const auto offset = static_cast<std::size_t>(texture) * SurfaceBytes;
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

using AgcDriver::Graphics::LookupC2Count;
using AgcDriver::Graphics::LookupC2Mutant;

std::uint64_t Count(LookupC2Count count) {
    return AgcDriver::Graphics::LookupC2Totals()[static_cast<std::size_t>(count)];
}

struct Proof {
    bool current = false;
    ShaderResources::ProofReport report;
};

Proof Prove(ShaderResources& resources, const CompiledShader& shader) {
    Proof proof;
    proof.current = resources.Revalidate(shader, &proof.report);
    return proof;
}

std::string Describe(const Proof& proof) {
    std::string text = proof.current ? "current" : "refused";
    text += proof.report.scoped ? " by the scoped proof" : proof.report.epoch ? " by the epoch proof" : proof.report.path == ShaderResources::ProofPath::Fast ? " by the fast proof" : " by the full proof";
    return text;
}

GuestBlock* RacingSurfaces = nullptr;

void WriteDuringProof() {
    auto* byte = RacingSurfaces->Bytes(SurfaceBytes);
    *byte = static_cast<std::uint8_t>(*byte ^ 0xffu);
    AgcDriver::GuestMemory::CollectWritesUncached(RacingSurfaces->At(SurfaceBytes), SurfaceBytes);
}

int RunTests(const AgcDriver::VulkanDevice& device) {
    GuestBlock block(BlockBytes, true);
    GuestBlock surfaces(SurfaceBlockBytes, false);
    Prepare(block, surfaces);
    const auto result = Compile(device, block);
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    const CompiledShader shader{ShaderStage::Compute, &result, 0};
    if (!ShaderResources::EpochProofs()) {
        std::puts("scoped lookup proof: epoch proofs are off in this configuration: skipped");
        return VulkanTestSkipped;
    }
    const bool scoped = AgcDriver::Graphics::LookupC2Scoped();
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    auto resources = std::make_shared<ShaderResources>(context, shader);
    Require(resources->Reusable() && resources->SampledTextures().size() == Textures, "scoped lookup proof: the set over " + std::to_string(Textures) + " textures is not reusable");
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto stampsBefore = Count(LookupC2Count::StampsTaken);
    const auto certsBefore = Count(LookupC2Count::CertsStored);
    auto proof = Prove(*resources, shader);
    Require(proof.current && !proof.report.scoped, "scoped lookup proof: the first proof after the build was " + Describe(proof));
    Require(!scoped || Count(LookupC2Count::CertsStored) == certsBefore + 1u, "scoped lookup proof: the first full proof in a collect epoch stored no certificate (" + std::to_string(Count(LookupC2Count::StampsTaken) - stampsBefore) + " stamps taken)");
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto collectedBefore = Count(LookupC2Count::ScopedCollected);
    proof = Prove(*resources, shader);
    Require(proof.current, "scoped lookup proof: an unchanged set was refused in a new collect epoch");
    if (!scoped) {
        Require(!proof.report.scoped && Count(LookupC2Count::StampsTaken) == stampsBefore && Count(LookupC2Count::ScopedHits) == 0u, "scoped lookup proof: APS5_NO_LOOKUP_C2 took a range stamp or a scoped proof");
        std::puts("scoped lookup proofs are off: no range stamp taken, every new epoch proved in full");
        return 0;
    }
    Require(proof.report.scoped && Count(LookupC2Count::ScopedCollected) == collectedBefore + 1u, "scoped lookup proof: an unchanged set in a new collect epoch was " + Describe(proof) + ", not the scoped proof after a collect of its ranges");
    proof = Prove(*resources, shader);
    Require(proof.current && proof.report.epoch, "scoped lookup proof: the proof after a scoped hit in the same epoch was " + Describe(proof) + ", not the epoch proof");
    AgcDriver::GuestMemory::BreakProofs();
    StorageTexture::BumpPendingSerial();
    proof = Prove(*resources, shader);
    Require(proof.current && proof.report.scoped, "scoped lookup proof: a global proof break and a pending serial bump outside the set's ranges left it " + Describe(proof));
    std::puts("a global proof break outside the set's ranges is answered by the scoped proof");
    const auto nonDrawBefore = Count(LookupC2Count::MissUnscopedNonDraw);
    AgcDriver::GuestMemory::BumpUnscopedVersion(AgcDriver::GuestMemory::UnscopedSource::NonDraw);
    AgcDriver::GuestMemory::BreakProofs();
    proof = Prove(*resources, shader);
    Require(proof.current && !proof.report.scoped && Count(LookupC2Count::MissUnscopedNonDraw) == nonDrawBefore + 1u, "scoped lookup proof: an unscoped non-draw break left the set " + Describe(proof) + " and counted " + std::to_string(Count(LookupC2Count::MissUnscopedNonDraw) - nonDrawBefore) + " unscoped non-draw misses");
    std::puts("an unscoped break takes the full proof");
    RacingSurfaces = &surfaces;
    AgcDriver::GuestMemory::BumpUnscopedVersion(AgcDriver::GuestMemory::UnscopedSource::GpuMutex);
    AgcDriver::GuestMemory::BreakProofs();
    AgcDriver::Graphics::SetLookupC2ProofHookForTest(&WriteDuringProof);
    proof = Prove(*resources, shader);
    AgcDriver::Graphics::SetLookupC2ProofHookForTest(nullptr);
    Require(proof.current && !proof.report.scoped, "scoped lookup proof: the full proof a guest write raced was " + Describe(proof));
    AgcDriver::GuestMemory::BreakProofs();
    proof = Prove(*resources, shader);
    Require(!proof.current, "scoped lookup proof: a texture written and collected while the set was proved stayed " + Describe(proof));
    std::puts("a write racing the proof voids the certificate");
    auto rebuilt = std::make_shared<ShaderResources>(context, shader);
    proof = Prove(*rebuilt, shader);
    Require(proof.current, "scoped lookup proof: the rebuilt set was " + Describe(proof));
    auto* byte = surfaces.Bytes(static_cast<std::size_t>(Textures / 2u) * SurfaceBytes);
    *byte = static_cast<std::uint8_t>(*byte ^ 0xffu);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    proof = Prove(*rebuilt, shader);
    Require(!proof.current, "scoped lookup proof: a CPU write to texture " + std::to_string(Textures / 2u) + " before a new collect epoch left the set " + Describe(proof));
    std::puts("a CPU write found by the epoch's collect refuses the set");
    Require(Count(LookupC2Count::Disagreed) == 0u, "scoped lookup proof: APS5_VERIFY_LOOKUP_C2 found a scoped proof the full proof refused");
    return 0;
}

}

int main() {
#ifdef _WIN32
    if (std::getenv("APS5_TEMPLATE_AUDIT_MS") == nullptr) _putenv_s("APS5_TEMPLATE_AUDIT_MS", "0");
#else
    setenv("APS5_TEMPLATE_AUDIT_MS", "0", 0);
#endif
    try {
        if (const char* mutant = std::getenv("APS5_TEST_LOOKUP_C2_MUTANT")) AgcDriver::Graphics::SetLookupC2MutantForTest(static_cast<LookupC2Mutant>(std::strtoul(mutant, nullptr, 10)));
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const int result = RunTests(*device);
        if (result == 0) std::puts("scoped lookup proof tests passed");
        return result;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
