#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureDetiler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;
namespace GuestMemory = AgcDriver::GuestMemory;

constexpr std::uint32_t TexelWidth = 64;
constexpr std::uint32_t TexelHeight = 4;
constexpr std::uint32_t Format32Float = 22;
constexpr std::uint32_t Type2D = 9;
constexpr std::size_t BlockBytes = 0x10000;
constexpr std::uint32_t TargetWidth = 64;
constexpr std::uint32_t TargetHeight = 32;
constexpr std::uint32_t Images = 8;
constexpr std::uint32_t Key = 3;
constexpr std::uint32_t Other = 5;

alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{0xe0382000u, 0x80000005u, 0xbf8c3f70u, 0xf80008cfu, 0x03020100u, 0xbf810000u};

alignas(256) constexpr std::array<std::uint32_t, 16> PixelTableCode{
    0x7e040f00u, 0x2c04049fu, 0x4a040404u, 0x7e080502u, 0x8f058404u, 0xf4280300u, 0x0a000000u, 0x7e140280u,
    0x7e160280u, 0xbf8cc07fu, 0xf0008f08u, 0x0003100au, 0xbf8c3f70u, 0xf800180fu, 0x13121110u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 7> StoreCode{
    0x7e040300u, 0x7e060280u, 0x7e0202ffu, 0x12345678u, 0xf0200108u, 0x00020102u, 0xbf810000u,
};

std::size_t TargetBytes = 0;
const std::array<std::array<float, 4>, 3> Triangle{{{-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}}};
const AgcDriver::Pm4::DrawParameters TriangleDraw{0, 3, 0, 1, 0, false};

std::array<std::uint32_t, 4> Buffer(const void* base, std::uint32_t bytes) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(base));
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::array<std::uint32_t, 4> VertexBuffer(const void* data, std::uint32_t stride, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), count, 0x01016facu};
}

std::array<std::uint32_t, 4> Image(const void* texels) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(texels));
    return {static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format32Float << 20u) | (((TexelWidth - 1u) & 3u) << 30u), ((TexelWidth - 1u) >> 2u) | ((TexelHeight - 1u) << 14u) | (1u << 31u), 0xfacu | (Type2D << 28u)};
}

class GuestBlocks {
public:
    explicit GuestBlocks(std::size_t bytes) : bytes(bytes) {
#ifdef _WIN32
        base = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, BlockBytes));
        if (base != nullptr) GuestArena::GuestArenaCommit_nid_postfix(base, bytes, PAGE_READWRITE, bytes);
#else
        void* mapped = mmap(nullptr, bytes + BlockBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + BlockBytes - 1) & ~(static_cast<std::uintptr_t>(BlockBytes) - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + bytes != begin + bytes + BlockBytes) munmap(reinterpret_cast<void*>(aligned + bytes), begin + BlockBytes - aligned);
            base = reinterpret_cast<std::uint8_t*>(aligned);
            if (GuestMemory::WriteWatched()) GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(base, bytes);
        }
#endif
        Require(base != nullptr, "certified proofs: cannot allocate guest memory");
        GuestAllocations::Mutation mutation;
        mutation.Add(base, bytes, true, true, true);
    }

    ~GuestBlocks() {
        {
            GuestAllocations::Mutation mutation;
            mutation.Remove(base);
        }
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(base, bytes);
        GuestArena::GuestArenaRelease_nid_postfix(base, bytes);
#else
        munmap(base, bytes);
        if (GuestMemory::WriteWatched()) GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(base, bytes);
#endif
    }

    GuestBlocks(const GuestBlocks&) = delete;
    GuestBlocks& operator=(const GuestBlocks&) = delete;

    std::uint8_t* base = nullptr;
    std::size_t bytes = 0;
};

GuestBlocks* Target = nullptr;

struct Scene {
    GuestBlocks blocks;
    GuestBlocks table;
    GuestBlocks textures;
    ShaderRecompiler::RecompileResult vertex;
    ShaderRecompiler::RecompileResult pixel;
    std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{};
    AgcDriver::Graphics::State state{};

    Scene() : blocks(2u * BlockBytes), table(BlockBytes), textures(static_cast<std::size_t>(Images) * BlockBytes) {}

    float* Texels(std::uint32_t image) const { return reinterpret_cast<float*>(textures.base + static_cast<std::size_t>(image) * BlockBytes); }
    std::uint32_t* Table() const { return reinterpret_cast<std::uint32_t*>(table.base); }
    std::uint32_t* Constants() const { return reinterpret_cast<std::uint32_t*>(blocks.base + BlockBytes); }
};

std::vector<std::unique_ptr<Scene>> Scenes;

Scene& NewScene() {
    return *Scenes.emplace_back(std::make_unique<Scene>());
}

void Build(AgcDriver::VulkanDevice& device, Scene& scene) {
    const auto target = device.Target();
    const std::uint32_t stride = 4u;
    for (std::uint32_t image = 0; image < Images; ++image) {
        std::fill_n(scene.Texels(image), TexelWidth * TexelHeight, image == Key ? 1.0f : 0.0f);
        const auto words = Image(scene.Texels(image));
        std::copy(words.begin(), words.end(), scene.Table() + image * stride);
    }
    std::vector<std::uint32_t> vertexUserData(4, 0u);
    auto* triangle = reinterpret_cast<std::array<float, 4>*>(scene.Constants() + 64);
    std::copy(Triangle.begin(), Triangle.end(), triangle);
    const auto vertices = VertexBuffer(triangle, 16u, static_cast<std::uint32_t>(Triangle.size()));
    std::copy(vertices.begin(), vertices.end(), vertexUserData.begin());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {64, 0, vertexUserData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
        target,
        {0, 0, 0, 64}
    };
    scene.vertex = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(scene.vertex.pushConstants.size());
    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.wave32 = false;
    pixel.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionX) | ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionY);
    pixel.posX = true;
    pixel.posY = true;
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    std::vector<std::uint32_t> pixelUserData(12, 0u);
    const std::span<const std::uint32_t> code(PixelTableCode);
    const auto tableWords = Buffer(scene.Table(), Images * stride * 4u);
    std::copy(tableWords.begin(), tableWords.end(), pixelUserData.begin());
    pixelUserData[4] = Key;
    const std::array<ShaderRecompiler::MemoryRegion, 2> pixelMemory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}, {reinterpret_cast<std::uintptr_t>(scene.Table()), std::as_bytes(std::span(scene.Table(), Images * stride))}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {64, 0, pixelUserData, std::nullopt, pixel, std::nullopt, pixelMemory},
        target,
        PixelPushLayout(vertexPush, target)
    };
    scene.pixel = ShaderRecompiler::Recompile(fragment);
    scene.shaders = {{{ShaderStage::Vertex, &scene.vertex, 0}, {ShaderStage::Fragment, &scene.pixel, PixelPushOffset(vertexPush, target)}}};
    auto& state = scene.state;
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 64u, 64u, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(Target->base), {TargetWidth, TargetHeight}, VK_FORMAT_R8G8B8A8_UNORM, TargetBytes, 0xe4u};
    state.color.surfaceAddress = state.color.address;
    state.color.tileMode = AgcDriver::Graphics::ColorTileMode::RenderTarget;
    state.color.elementBytes = 4;
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {TargetWidth, TargetHeight};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(TargetHeight), static_cast<float>(TargetWidth), -static_cast<float>(TargetHeight), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {TargetWidth, TargetHeight}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    state.blendConstants = {};
}

unsigned Brightest() {
    GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(Target->base), TargetBytes);
    unsigned brightest = 0;
    for (std::size_t pixel = 0; pixel < TargetBytes; pixel += 4) brightest = std::max(brightest, static_cast<unsigned>(Target->base[pixel]));
    return brightest;
}

using AgcDriver::Graphics::CertifiedMiss;
using AgcDriver::Graphics::CertifyRefusal;

struct Outcome {
    unsigned brightest = 0;
    AgcDriver::Graphics::CertifiedProofOutcomes counts;

    std::uint64_t Missed(CertifiedMiss miss) const { return counts.missed[static_cast<std::size_t>(miss)]; }
    std::uint64_t Refused(CertifyRefusal refusal) const { return counts.refused[static_cast<std::size_t>(refusal)]; }
    std::uint64_t Uncertified(AgcDriver::Graphics::UncertifiedCause cause) const { return counts.uncertified[static_cast<std::size_t>(cause)]; }
    std::uint64_t UncertifiedRefused(CertifyRefusal refusal) const { return counts.uncertifiedRefused[static_cast<std::size_t>(refusal)]; }
    std::uint64_t ProofSource(GuestMemory::ProofBreak source) const { return counts.proofMissSources[static_cast<std::size_t>(source)]; }

    std::string Describe() const {
        constexpr std::array<const char*, static_cast<std::size_t>(CertifiedMiss::Count)> misses{"uncertified", "sweep", "subscription", "proof", "unwatch", "mapping", "pending", "depth-surfaces", "cpu-writers", "audit", "imports"};
        constexpr std::array<const char*, static_cast<std::size_t>(CertifyRefusal::Count)> refusals{"unsubscribed", "per-element-textures", "compared-textures", "compared-tables", "depth-copies", "storage-images", "pending-moved", "depth-surfaces-moved"};
        std::string text = "brightest " + std::to_string(brightest) + ", proved " + std::to_string(counts.proved) + ", swept " + std::to_string(counts.swept) + ", issued " + std::to_string(counts.issued) + ", verified " + std::to_string(counts.verified) + ", disagreed " + std::to_string(counts.disagreed);
        for (std::size_t i = 0; i < misses.size(); ++i) {
            if (counts.missed[i] != 0) text += ", missed " + std::string(misses[i]) + " " + std::to_string(counts.missed[i]);
        }
        for (std::size_t i = 0; i < refusals.size(); ++i) {
            if (counts.refused[i] != 0) text += ", refused " + std::string(refusals[i]) + " " + std::to_string(counts.refused[i]);
        }
        for (std::size_t i = 0; i < counts.uncertified.size(); ++i) {
            if (counts.uncertified[i] != 0) text += ", uncertified cause " + std::to_string(i) + " " + std::to_string(counts.uncertified[i]);
        }
        for (std::size_t i = 0; i < counts.proofMissSources.size(); ++i) {
            if (counts.proofMissSources[i] != 0) text += ", proof miss source " + std::string(GuestMemory::ProofBreakName(static_cast<GuestMemory::ProofBreak>(i))) + " " + std::to_string(counts.proofMissSources[i]);
        }
        return text;
    }
};

AgcDriver::Graphics::CertifiedProofOutcomes Since(const AgcDriver::Graphics::CertifiedProofOutcomes& before, const AgcDriver::Graphics::CertifiedProofOutcomes& after) {
    AgcDriver::Graphics::CertifiedProofOutcomes delta;
    delta.proved = after.proved - before.proved;
    delta.swept = after.swept - before.swept;
    delta.issued = after.issued - before.issued;
    delta.verified = after.verified - before.verified;
    delta.disagreed = after.disagreed - before.disagreed;
    for (std::size_t i = 0; i < delta.missed.size(); ++i) delta.missed[i] = after.missed[i] - before.missed[i];
    for (std::size_t i = 0; i < delta.refused.size(); ++i) delta.refused[i] = after.refused[i] - before.refused[i];
    for (std::size_t i = 0; i < delta.uncertified.size(); ++i) delta.uncertified[i] = after.uncertified[i] - before.uncertified[i];
    for (std::size_t i = 0; i < delta.uncertifiedRefused.size(); ++i) delta.uncertifiedRefused[i] = after.uncertifiedRefused[i] - before.uncertifiedRefused[i];
    for (std::size_t i = 0; i < delta.uncertifiedVoided.size(); ++i) delta.uncertifiedVoided[i] = after.uncertifiedVoided[i] - before.uncertifiedVoided[i];
    delta.epochServed = after.epochServed - before.epochServed;
    for (std::size_t i = 0; i < delta.proofMissSources.size(); ++i) delta.proofMissSources[i] = after.proofMissSources[i] - before.proofMissSources[i];
    delta.proofMissUnattributed = after.proofMissUnattributed - before.proofMissUnattributed;
    for (std::size_t i = 0; i < delta.certifiedGateSets.size(); ++i) delta.certifiedGateSets[i] = after.certifiedGateSets[i] - before.certifiedGateSets[i];
    for (std::size_t i = 0; i < delta.epochGateSets.size(); ++i) delta.epochGateSets[i] = after.epochGateSets[i] - before.epochGateSets[i];
    for (std::size_t i = 0; i < delta.pendingMissSources.size(); ++i) delta.pendingMissSources[i] = after.pendingMissSources[i] - before.pendingMissSources[i];
    delta.pendingMissUnattributed = after.pendingMissUnattributed - before.pendingMissUnattributed;
    return delta;
}

Outcome Draw(AgcDriver::VulkanDevice& device, Scene& scene, bool newEpoch = true, bool read = true) {
    if (newEpoch) GuestMemory::BumpCollectEpoch();
    const auto before = AgcDriver::Graphics::CertifiedProofOutcomesCounted();
    device.Draw(scene.state, TriangleDraw, scene.shaders);
    device.WaitIdle();
    Outcome outcome;
    outcome.counts = Since(before, AgcDriver::Graphics::CertifiedProofOutcomesCounted());
    if (read) outcome.brightest = Brightest();
    return outcome;
}

Outcome Certify(AgcDriver::VulkanDevice& device, Scene& scene) {
    Outcome outcome;
    for (int round = 0; round < 6; ++round) {
        outcome = Draw(device, scene, true, false);
        if (outcome.counts.proved == 1) break;
    }
    return outcome;
}

void Settle(AgcDriver::VulkanDevice& device, Scene& scene, const std::string& what, unsigned expected = 255u) {
    auto outcome = Certify(device, scene);
    Require(outcome.counts.proved == 1, what + ": the unchanged draw did not settle on its certificate (" + outcome.Describe() + ")");
    const auto brightest = Brightest();
    Require(brightest == expected, what + ": the certified draw sampled " + std::to_string(brightest) + " instead of " + std::to_string(expected));
    outcome = Certify(device, scene);
    Require(outcome.counts.proved == 1, what + ": the draw did not settle on its certificate again after reading the target back (" + outcome.Describe() + ")");
}

void RequireMissed(const Outcome& outcome, const std::string& what, unsigned expected) {
    Require(outcome.counts.proved == 0 && outcome.counts.disagreed == 0, what + ": the draw after the change took the certificate (" + outcome.Describe() + ")");
    Require(outcome.brightest == expected, what + ": the draw after the change sampled " + std::to_string(outcome.brightest) + " instead of " + std::to_string(expected) + " (" + outcome.Describe() + ")");
}

void Fill(float* texels, float value) {
    std::fill_n(texels, TexelWidth * TexelHeight, value);
}

void Held(AgcDriver::VulkanDevice& device) {
    auto& scene = NewScene();
    Build(device, scene);
    Settle(device, scene, "held");
    auto outcome = Draw(device, scene, true, false);
    Require(outcome.counts.proved == 1 && outcome.counts.swept == 1, "held: a draw in a new epoch with nothing written did not keep the certificate after one sweep (" + outcome.Describe() + ")");
    outcome = Draw(device, scene, false, false);
    Require(outcome.counts.proved == 1, "held: a draw in the same epoch did not keep the certificate (" + outcome.Describe() + ")");
    Require(Brightest() == 255u, "held: the certified draws sampled the wrong texels");
}

void CpuWrite(AgcDriver::VulkanDevice& device) {
    auto& scene = NewScene();
    Build(device, scene);
    Settle(device, scene, "cpu write");
    Fill(scene.Texels(Key), 0.0f);
    auto outcome = Draw(device, scene);
    RequireMissed(outcome, "cpu write", 0u);
    Require(outcome.Missed(CertifiedMiss::Subscription) == 1, "cpu write: a CPU write to a sampled texture in a later epoch was not seen by the subscription (" + outcome.Describe() + ")");
    Settle(device, scene, "cpu write (after)", 0u);
    Fill(scene.Texels(Other), 0.5f);
    outcome = Draw(device, scene);
    RequireMissed(outcome, "cpu write to another element", 0u);
    Fill(scene.Texels(Key), 1.0f);
    outcome = Draw(device, scene);
    Require(outcome.brightest == 255u, "cpu write: restoring the texels was missed (" + outcome.Describe() + ")");
}

void TableWord(AgcDriver::VulkanDevice& device) {
    auto& scene = NewScene();
    Build(device, scene);
    Settle(device, scene, "table word");
    const auto saved = scene.Table()[Other * 4u + 3u];
    scene.Table()[Other * 4u + 3u] = saved ^ 0x00000100u;
    auto outcome = Draw(device, scene);
    Require(outcome.counts.proved == 0, "table word: a T# word change in the table kept the certificate (" + outcome.Describe() + ")");
    scene.Table()[Other * 4u + 3u] = saved;
    outcome = Draw(device, scene);
    Require(outcome.counts.proved == 0, "table word: restoring the T# word kept the certificate (" + outcome.Describe() + ")");
}

void DriverStore(AgcDriver::VulkanDevice& device) {
    auto& scene = NewScene();
    Build(device, scene);
    Settle(device, scene, "driver store");
    GuestMemory::MarkWritten(reinterpret_cast<std::uintptr_t>(scene.Texels(Other)), TexelWidth * TexelHeight * 4u);
    RequireMissed(Draw(device, scene), "driver store", 255u);
    Settle(device, scene, "driver store (after)");
}

void PendingWrite(AgcDriver::VulkanDevice& device) {
    auto& scene = NewScene();
    Build(device, scene);
    Settle(device, scene, "pending write");
    std::vector<std::uint32_t> userData(16, 0u);
    const auto image = Image(scene.Texels(Key));
    std::copy(image.begin(), image.end(), userData.begin() + 8);
    const std::span<const std::uint32_t> code(StoreCode);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{8, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto store = ShaderRecompiler::Recompile(request);
    device.Dispatch(store, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
    const auto surface = reinterpret_cast<std::uintptr_t>(scene.Texels(Key));
    Require(AgcDriver::Graphics::StorageTexture::FindPending(surface, TexelWidth * TexelHeight * 4u) != nullptr, "pending write: the store left no pending storage image over the sampled texture");
    RequireMissed(Draw(device, scene), "pending write", 0u);
    {
        std::lock_guard lock(GuestMemory::GpuMutex());
        AgcDriver::Graphics::FlushCachedTextures(device.Device());
    }
    device.WaitIdle();
    auto outcome = Draw(device, scene);
    Require(outcome.brightest == 0u && outcome.counts.disagreed == 0, "pending write: the draw after the write landed sampled the old texels (" + outcome.Describe() + ")");
    outcome = Draw(device, scene);
    Require(outcome.counts.proved == 0 && outcome.counts.issued == 0 && outcome.Refused(CertifyRefusal::PerElementTextures) == 1 && outcome.brightest == 0u, "pending write: a template sampling a storage image's results was certified (" + outcome.Describe() + ")");
}

void Released(AgcDriver::VulkanDevice& device) {
    auto& scene = NewScene();
    Build(device, scene);
    Settle(device, scene, "release");
    {
        std::lock_guard lock(GuestMemory::GpuMutex());
        AgcDriver::Graphics::ClearCachedTextures(device.Device());
    }
    RequireMissed(Draw(device, scene), "release", 255u);
    Settle(device, scene, "release (after)");
}

void UnwatchRange(AgcDriver::VulkanDevice& device) {
    auto& scene = NewScene();
    Build(device, scene);
    Settle(device, scene, "unwatch");
    GuestMemory::Unwatch(reinterpret_cast<std::uintptr_t>(scene.Texels(Other)), BlockBytes);
    auto outcome = Draw(device, scene);
    RequireMissed(outcome, "unwatch", 255u);
    Require(outcome.Missed(CertifiedMiss::Sweep) == 1 || outcome.Missed(CertifiedMiss::Unwatch) == 1, "unwatch: leaving the write watch was not what voided the certificate (" + outcome.Describe() + ")");
    outcome = Draw(device, scene);
    Require(outcome.counts.proved == 0 && outcome.Refused(CertifyRefusal::ComparedTextures) == 1 && outcome.brightest == 255u, "unwatch: a template over an unwatched texture was certified (" + outcome.Describe() + ")");
    Fill(scene.Texels(Other), 0.25f);
    Fill(scene.Texels(Key), 0.0f);
    outcome = Draw(device, scene);
    Require(outcome.counts.proved == 0 && outcome.brightest == 0u, "unwatch: a write next to the unwatched texture was missed (" + outcome.Describe() + ")");
    Require(outcome.Uncertified(AgcDriver::Graphics::UncertifiedCause::Refused) == 1 && outcome.UncertifiedRefused(CertifyRefusal::ComparedTextures) == 1, "unwatch: the use after a refusal was not counted as uncertified for compared textures (" + outcome.Describe() + ")");
}

bool ThrowOnConsume = false;

void ThrowAfterConsume() {
    if (std::exchange(ThrowOnConsume, false)) throw std::runtime_error("invalidate: the consume hook threw");
}

struct InputRun {
    bool hit = false;
    bool storeSeen = false;
    bool markSeen = false;
    std::uint64_t certifiedHits = 0;
};

InputRun CertifiedInputs(AgcDriver::VulkanDevice& device) {
    using namespace AgcDriver::Graphics;
    using Use = Recorder::SnapshotUse;
    constexpr std::size_t size = 256;
    GuestBlocks memory(BlockBytes);
    const auto address = reinterpret_cast<std::uint64_t>(memory.base);
    auto* words = reinterpret_cast<std::uint32_t*>(memory.base);
    for (std::uint32_t i = 0; i < size / sizeof(std::uint32_t); ++i) words[i] = i * 7u;
    auto context = device.graphicsContext();
    std::lock_guard lock(GuestMemory::GpuMutex());
    auto* recorder = Recorder::Active();
    Require(recorder != nullptr, "inputs: no active recorder");
    const auto equalsGuest = [&](const DrawInputCopy& copy) { return copy.Bytes().size() == size && std::memcmp(copy.Bytes().data(), memory.base, size) == 0; };
    const auto copyKept = [&] {
        auto copy = CopyDrawInput(context, recorder, address, size, 4, Use::Index32);
        Require(equalsGuest(copy), "inputs: a draw input copy differs from guest memory");
        KeepDrawInput(recorder, address, copy, Use::Index32, 0);
        return copy;
    };
    InputRun run;
    const auto before = CertifiedDrawInputsCounted().hits;
    GuestMemory::BumpCollectEpoch();
    static_cast<void>(copyKept());
    run.hit = copyKept().reused && CertifiedDrawInputsCounted().hits == before + 1;
    words[3] = 0xdeadu;
    GuestMemory::BumpCollectEpoch();
    run.storeSeen = !copyKept().reused;
    static_cast<void>(copyKept());
    GuestMemory::MarkWritten(address + 64, 4);
    run.markSeen = !copyKept().reused;
    run.certifiedHits = CertifiedDrawInputsCounted().hits - before;
    return run;
}

void Inputs(AgcDriver::VulkanDevice& device) {
    using AgcDriver::Graphics::RuntimeSwitch;
    const auto run = CertifiedInputs(device);
    Require(run.hit, "inputs: an unchanged draw input was not served by its certificate");
    Require(run.storeSeen, "inputs: a certified draw input outlived a CPU store in a new epoch");
    Require(run.markSeen, "inputs: a certified draw input outlived a stamped GPU store");
    AgcDriver::Graphics::RuntimeSwitches::Assign(RuntimeSwitch::NoCertifiedDrawInputs, true);
    const auto off = CertifiedInputs(device);
    AgcDriver::Graphics::RuntimeSwitches::Assign(RuntimeSwitch::NoCertifiedDrawInputs, false);
    Require(off.certifiedHits == 0 && off.storeSeen && off.markSeen, "inputs: APS5_NO_CERTIFIED_DRAW_INPUTS did not return draw inputs to the collect path");
    GuestMemory::SetCertificateMutantForTest(GuestMemory::CertificateMutant::HideMarkWrittenBumps);
    const auto mutant = CertifiedInputs(device);
    GuestMemory::SetCertificateMutantForTest(GuestMemory::CertificateMutant::None);
    Require(!mutant.markSeen, "inputs: hiding the bumps of a stamped GPU store still voided the certified draw input, so the case tests nothing");
    std::printf("certified proofs: draw inputs served %llu times by certificates, CPU and GPU stores void them, the switch returns them to the collect path\n", static_cast<unsigned long long>(run.certifiedHits));
}

bool InvalidationVoids() {
    GuestBlocks memory(2u * BlockBytes);
    const auto base = reinterpret_cast<std::uint64_t>(memory.base);
    const auto bytes = static_cast<std::size_t>(2u * BlockBytes);
    std::memset(memory.base, 0x11, bytes);
    Require(GuestMemory::CollectWritesUncached(base, bytes) != 0, "invalidate: the fixture was not collected");
    const GuestMemory::WriteOwner owner(base, BlockBytes);
    const std::array ownedRange{std::pair<std::uint64_t, std::uint64_t>{base + 64, base + 128}};
    const std::array blockRange{std::pair<std::uint64_t, std::uint64_t>{base + BlockBytes + 64, base + BlockBytes + 128}};
    const auto owned = GuestMemory::SubscribeWrites(ownedRange);
    const auto counted = GuestMemory::SubscribeWrites(blockRange);
    const auto ownedCertificate = GuestMemory::CertifySubscription(*owned, 0);
    const auto countedCertificate = GuestMemory::CertifySubscription(*counted, 0);
    Require(ownedCertificate.ownerCount == 1 && countedCertificate.blockCount == 1, "invalidate: the subscriptions do not depend on an owner and a block counter");
    Require(GuestMemory::CertificateHolds(*owned, ownedCertificate, 0) && GuestMemory::CertificateHolds(*counted, countedCertificate, 0), "invalidate: a certificate did not hold with nothing changed");
    const auto invalidations = GuestMemory::ConservativeInvalidationsForTest();
    memory.base[8u * 4096u] = std::uint8_t{0x22};
    ThrowOnConsume = true;
    GuestMemory::SetConsumedWalkHookForTest(&ThrowAfterConsume);
    bool threw = false;
    try {
        static_cast<void>(GuestMemory::CollectWritesUncached(base, bytes));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    GuestMemory::SetConsumedWalkHookForTest(nullptr);
    Require(threw && GuestMemory::ConservativeInvalidationsForTest() == invalidations + 1, "invalidate: the throwing collect did not invalidate the tracker");
    return !GuestMemory::CertificateHolds(*owned, ownedCertificate, 0) && !GuestMemory::CertificateHolds(*counted, countedCertificate, 0);
}

void Invalidate(AgcDriver::VulkanDevice&) {
    Require(InvalidationVoids(), "invalidate: a conservative invalidation left a certificate on an owner or a block counter standing");
    GuestMemory::SetCertificateMutantForTest(GuestMemory::CertificateMutant::HideOverflowCounters);
    const bool caught = InvalidationVoids();
    GuestMemory::SetCertificateMutantForTest(GuestMemory::CertificateMutant::None);
    Require(!caught, "invalidate: hiding the counter bumps of an invalidation kept every certificate voided, so the case tests nothing");
    std::printf("certified proofs: a conservative invalidation voids certificates on owners and block counters\n");
}

std::pair<bool, bool> LayerMarkVoids(AgcDriver::VulkanDevice& device) {
    using namespace AgcDriver::Graphics;
    constexpr std::uint32_t side = 128;
    GuestBlocks memory(4u * BlockBytes);
    const auto address = reinterpret_cast<std::uint64_t>(memory.base);
    auto context = device.graphicsContext();
    TextureDetiler detiler(context);
    context.hostImportAlignment = 0;
    context.detiler = &detiler;
    GuestTextureResource resource{};
    resource.baseAddress = address;
    resource.width = side;
    resource.height = side;
    resource.depthOrLastArray = 1;
    resource.mipCount = 1;
    resource.tileMode = TextureTileMode::kLinear;
    resource.dimension = TextureDimension::k2DArray;
    resource.format = 56;
    resource.dstSelX = 4;
    resource.dstSelY = 5;
    resource.dstSelZ = 6;
    resource.dstSelW = 7;
    const auto geometry = DescribeSurface(resource);
    Require(geometry.imageLayers == 2 && geometry.layerBytes == BlockBytes, "layers: the image does not hold two 64 KiB slices");
    const auto slice = [&](std::uint32_t layer) {
        const auto begin = address + geometry.GuestLayerOffset(layer);
        return std::array{std::pair<std::uint64_t, std::uint64_t>{begin, begin + geometry.layerBytes}};
    };
    const auto marked = GuestMemory::SubscribeWrites(slice(0));
    const auto other = GuestMemory::SubscribeWrites(slice(1));
    std::lock_guard lock(GuestMemory::GpuMutex());
    auto image = StorageTexture::Create(context, detiler, resource, 0);
    const auto markedCertificate = GuestMemory::CertifySubscription(*marked, 0);
    const auto otherCertificate = GuestMemory::CertifySubscription(*other, 0);
    auto* recorder = Recorder::Active();
    Require(recorder != nullptr, "layers: no active recorder");
    static_cast<void>(recorder->Commands());
    recorder->Keep(image);
    image->MarkSliceDirty(0);
    const bool markedVoided = !GuestMemory::CertificateHolds(*marked, markedCertificate, 0);
    const bool otherHeld = GuestMemory::CertificateHolds(*other, otherCertificate, 0);
    image->WriteBack();
    recorder->Submit();
    recorder->Sync();
    return {markedVoided, otherHeld};
}

void Layers(AgcDriver::VulkanDevice& device) {
    using AgcDriver::Graphics::RuntimeSwitch;
    const auto [voided, held] = LayerMarkVoids(device);
    Require(voided, "layers: a layer pending mark left a certificate on that layer standing");
    Require(held, "layers: a layer pending mark voided a certificate on another layer");
    AgcDriver::Graphics::RuntimeSwitches::Assign(RuntimeSwitch::WholePendingBumps, true);
    const auto [wholeVoided, wholeHeld] = LayerMarkVoids(device);
    AgcDriver::Graphics::RuntimeSwitches::Assign(RuntimeSwitch::WholePendingBumps, false);
    Require(wholeVoided && !wholeHeld, "layers: with APS5_WHOLE_PENDING_BUMPS a layer pending mark kept the other layer's certificate, so the case tests nothing");
    std::printf("certified proofs: a layer pending mark voids certificates on its layer only\n");
}

bool SceneWatched(AgcDriver::VulkanDevice& device) {
    auto& scene = NewScene();
    Build(device, scene);
    static_cast<void>(Draw(device, scene, true, false));
    device.WaitIdle();
    return GuestMemory::Watched(reinterpret_cast<std::uintptr_t>(scene.table.base), scene.table.bytes) && GuestMemory::Watched(reinterpret_cast<std::uintptr_t>(scene.textures.base), scene.textures.bytes);
}

void Causes(AgcDriver::VulkanDevice& device) {
    using AgcDriver::Graphics::UncertifiedCause;
    auto& scene = NewScene();
    Build(device, scene);
    std::uint64_t neverProved = 0;
    Outcome outcome;
    for (int round = 0; round < 6; ++round) {
        outcome = Draw(device, scene, true, false);
        neverProved += outcome.Uncertified(UncertifiedCause::NeverProved);
        if (outcome.counts.proved == 1) break;
    }
    Require(outcome.counts.proved == 1 && neverProved == 1, "causes: a new template's first proof was not counted once as never proved (" + std::to_string(neverProved) + "; " + outcome.Describe() + ")");
    GuestMemory::BreakProofs(GuestMemory::ProofBreak::GpuLock);
    outcome = Draw(device, scene, false);
    RequireMissed(outcome, "causes", 255u);
    Require(outcome.Missed(CertifiedMiss::Proof) == 1, "causes: an uncovered proof break did not void the certificate through the proof domain (" + outcome.Describe() + ")");
    if (GuestMemory::ProofBreaksCounted()) Require(outcome.ProofSource(GuestMemory::ProofBreak::GpuLock) == 1 && outcome.counts.proofMissUnattributed == 0, "causes: a proof miss after a gpu-lock break was not attributed to it (" + outcome.Describe() + ")");
    Require(outcome.Uncertified(UncertifiedCause::Voided) == 0 && outcome.Uncertified(UncertifiedCause::NeverProved) == 0, "causes: a certified template was counted as uncertified (" + outcome.Describe() + ")");
    Settle(device, scene, "causes (after)");
}

void Mapping(AgcDriver::VulkanDevice& device) {
    auto& scene = NewScene();
    Build(device, scene);
    Settle(device, scene, "mapping");
    const auto flip = [&](bool writable) {
        GuestAllocations::Mutation mutation;
        mutation.Remove(scene.textures.base);
        mutation.Add(scene.textures.base, scene.textures.bytes, true, writable, true);
    };
    flip(false);
    auto outcome = Draw(device, scene);
    RequireMissed(outcome, "mapping", 255u);
    Require(outcome.Missed(CertifiedMiss::Mapping) == 1, "mapping: a permission change of the sampled range was not what voided the certificate (" + outcome.Describe() + ")");
    Settle(device, scene, "mapping (read-only)");
    flip(true);
    RequireMissed(Draw(device, scene), "mapping (restored)", 255u);
    Settle(device, scene, "mapping (restored)");
}

void Imported(AgcDriver::VulkanDevice& device) {
    auto& scene = NewScene();
    Build(device, scene);
    Settle(device, scene, "import");
    Require(GuestMemory::ImportWatched(reinterpret_cast<std::uintptr_t>(scene.Texels(Other)), BlockBytes, [] { return true; }, true), "import: the import of the texture's range was refused");
    RequireMissed(Draw(device, scene), "import", 255u);
    Settle(device, scene, "import (after)");
}

void DepthSurfaces(AgcDriver::VulkanDevice& device) {
    auto& scene = NewScene();
    Build(device, scene);
    Settle(device, scene, "depth surfaces");
    AgcDriver::Graphics::ClearDepthSurfaces(device.Device());
    auto outcome = Draw(device, scene);
    RequireMissed(outcome, "depth surfaces", 255u);
    Require(outcome.Missed(CertifiedMiss::DepthSurfaces) == 1, "depth surfaces: a change of the depth surfaces was not what voided the certificate (" + outcome.Describe() + ")");
    Settle(device, scene, "depth surfaces (after)");
}

void Switched(AgcDriver::VulkanDevice& device) {
    auto& scene = NewScene();
    Build(device, scene);
    Settle(device, scene, "switch");
    AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::CertifiedProofs, false);
    Fill(scene.Texels(Key), 0.0f);
    auto outcome = Draw(device, scene);
    Require(outcome.counts.proved == 0 && outcome.counts.issued == 0 && outcome.brightest == 0u, "switch: with the switch off a draw took or issued a certificate, or missed a write (" + outcome.Describe() + ")");
    AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::CertifiedProofs, true);
    Fill(scene.Texels(Key), 1.0f);
    outcome = Draw(device, scene);
    Require(outcome.counts.proved == 0 && outcome.brightest == 255u, "switch: a certificate issued before the switch went off survived a write made while it was off (" + outcome.Describe() + ")");
    Settle(device, scene, "switch (after)");
}

void Verified(AgcDriver::VulkanDevice& device) {
    if (std::getenv("APS5_VERIFY_DRAW_PIPELINE") == nullptr) {
        std::puts("verify: skipped without APS5_VERIFY_DRAW_PIPELINE");
        return;
    }
    auto& scene = NewScene();
    Build(device, scene);
    Settle(device, scene, "verify");
    AgcDriver::Graphics::CertifiedProofOutcomes total;
    for (int draw = 0; draw < 130; ++draw) {
        const auto outcome = Draw(device, scene, (draw % 3) == 0, false);
        total.verified += outcome.counts.verified;
        total.disagreed += outcome.counts.disagreed;
    }
    Require(Brightest() == 255u, "verify: the certified draws sampled the wrong texels");
    Require(total.verified >= 2 && total.disagreed == 0, "verify: expected the full proof beside every 64th certified draw to agree (" + std::to_string(total.verified) + " checked, " + std::to_string(total.disagreed) + " disagreed)");
}


constexpr std::array<GuestMemory::ProofBreak, 3> CoveredBreaks{GuestMemory::ProofBreak::MarkWritten, GuestMemory::ProofBreak::OwnStore, GuestMemory::ProofBreak::PipelineCommitCovered};

bool UncoveredBreakVoids(AgcDriver::VulkanDevice& device, const std::string& what) {
    auto& scene = NewScene();
    Build(device, scene);
    Settle(device, scene, what);
    GuestMemory::BreakProofs(GuestMemory::ProofBreak::Unlabelled);
    const auto outcome = Draw(device, scene, false);
    Require(outcome.brightest == 255u && outcome.counts.disagreed == 0, what + ": the draw after an uncovered proof break sampled the wrong texels (" + outcome.Describe() + ")");
    return outcome.counts.proved == 0 && outcome.Missed(CertifiedMiss::Proof) == 1;
}

void Scope(AgcDriver::VulkanDevice& device) {
    using AgcDriver::Graphics::RuntimeSwitch;
    for (std::size_t i = 0; i < static_cast<std::size_t>(GuestMemory::ProofBreak::Count); ++i) {
        const auto source = static_cast<GuestMemory::ProofBreak>(i);
        const bool covered = std::find(CoveredBreaks.begin(), CoveredBreaks.end(), source) != CoveredBreaks.end();
        Require(GuestMemory::ProofBreakVoidsCertificates(source) != covered, std::string("scope: ") + GuestMemory::ProofBreakName(source) + " is not in the expected covered set");
        const auto proof = GuestMemory::ProofSerial();
        const auto certificates = GuestMemory::CertificateBreakSerial();
        GuestMemory::BreakProofs(source);
        Require(GuestMemory::ProofSerial() != proof, std::string("scope: a ") + GuestMemory::ProofBreakName(source) + " break did not move the proof serial");
        Require((GuestMemory::CertificateBreakSerial() != certificates) != covered, std::string("scope: a ") + GuestMemory::ProofBreakName(source) + (covered ? " break voided certificates though its writes bump their ranges" : " break left certificates standing"));
    }
    Require(UncoveredBreakVoids(device, "scope"), "scope: an uncovered proof break kept the certificate");
    const std::array<std::pair<const char*, std::function<void(std::uintptr_t)>>, 2> stores{{
        {"a driver store", [](std::uintptr_t address) { GuestMemory::MarkWritten(address, 256); }},
        {"an own store", [](std::uintptr_t address) {
            const std::array<std::byte, 256> bytes{};
            static_cast<void>(GuestMemory::StoreOwnBytes(address, bytes));
        }},
    }};
    for (const auto& [kind, store] : stores) {
        const auto storeElsewhere = [&](const std::string& what) {
            auto& scene = NewScene();
            Build(device, scene);
            Settle(device, scene, what);
            store(reinterpret_cast<std::uintptr_t>(scene.blocks.base));
            const auto outcome = Draw(device, scene, false);
            Require(outcome.brightest == 255u, what + ": the draw after " + kind + " elsewhere sampled the wrong texels (" + outcome.Describe() + ")");
            return outcome;
        };
        auto outcome = storeElsewhere(std::string("scope (") + kind + " elsewhere)");
        Require(outcome.counts.proved == 1, std::string("scope: ") + kind + " to a range the certificate does not subscribe voided it (" + outcome.Describe() + ")");
        AgcDriver::Graphics::RuntimeSwitches::Assign(RuntimeSwitch::NoCertifiedProofScope, true);
        outcome = storeElsewhere(std::string("scope (") + kind + " elsewhere, unscoped)");
        AgcDriver::Graphics::RuntimeSwitches::Assign(RuntimeSwitch::NoCertifiedProofScope, false);
        Require(outcome.counts.proved == 0 && outcome.Missed(CertifiedMiss::Proof) == 1, std::string("scope: with APS5_NO_CERTIFIED_PROOF_SCOPE ") + kind + " elsewhere kept the certificate (" + outcome.Describe() + ")");
    }
    AgcDriver::Graphics::RuntimeSwitches::Assign(RuntimeSwitch::NoCertifiedProofScope, true);
    const bool unscoped = UncoveredBreakVoids(device, "scope (unscoped)");
    AgcDriver::Graphics::RuntimeSwitches::Assign(RuntimeSwitch::NoCertifiedProofScope, false);
    Require(unscoped, "scope: with APS5_NO_CERTIFIED_PROOF_SCOPE an uncovered proof break kept the certificate");
}

void PipelinedWrite(AgcDriver::DriverDetail::DrawPipeline::CommitKind kind, std::uintptr_t address, std::vector<std::byte> bytes) {
    using AgcDriver::DriverDetail::DrawPipeline;
    auto& pipeline = DrawPipeline::Queue0();
    const auto end = address + bytes.size();
    pipeline.Enqueue(kind, [address, bytes = std::move(bytes)] { GuestMemory::Write(address, bytes, 4); }, {{address, end}});
    pipeline.Drain(DrawPipeline::DrainReason::Flush);
}

std::vector<std::byte> TexelBytes(float value) {
    std::vector<float> texels(TexelWidth * TexelHeight, value);
    const auto bytes = std::as_bytes(std::span(texels));
    return {bytes.begin(), bytes.end()};
}

bool RecordedPendingVoids(AgcDriver::VulkanDevice& device) {
    auto& scene = NewScene();
    Build(device, scene);
    Settle(device, scene, "recorded");
    GuestBlocks memory(2u * BlockBytes);
    const auto base = reinterpret_cast<std::uint64_t>(memory.base);
    using Note = std::function<void(AgcDriver::Graphics::Recorder&, std::uint64_t)>;
    const std::array<std::pair<const char*, Note>, 5> notes{{
        {"NotePendingWrite", [](auto& recorder, std::uint64_t address) { recorder.NotePendingWrite(address, 64); }},
        {"NotePendingFill", [](auto& recorder, std::uint64_t address) { recorder.NotePendingFill(address, 64, 0x5a); }},
        {"NotePendingWordFill", [](auto& recorder, std::uint64_t address) { recorder.NotePendingWordFill(address, 64, 0x12345678u); }},
        {"NotePendingWrites", [](auto& recorder, std::uint64_t address) {
             const std::array ranges{std::pair<std::uint64_t, std::uint64_t>{address, address + 64}};
             recorder.NotePendingWrites(ranges);
         }},
        {"AfterCompletions on an in-flight batch", [](auto& recorder, std::uint64_t address) {
             static_cast<void>(recorder.Commands());
             recorder.Submit();
             const std::uint32_t label = 7;
             recorder.AfterCompletions(address, std::as_bytes(std::span(&label, 1)), 1, 0, false);
         }},
    }};
    bool voided = true;
    for (std::size_t i = 0; i < notes.size(); ++i) {
        const auto target = base + i * 4096u;
        const auto beside = base + BlockBytes + i * 4096u;
        const std::array targetRange{std::pair<std::uint64_t, std::uint64_t>{target, target + 64}};
        const std::array besideRange{std::pair<std::uint64_t, std::uint64_t>{beside, beside + 64}};
        const auto targetSubscription = GuestMemory::SubscribeWrites(targetRange);
        const auto besideSubscription = GuestMemory::SubscribeWrites(besideRange);
        const auto targetCertificate = GuestMemory::CertifySubscription(*targetSubscription, 0);
        const auto besideCertificate = GuestMemory::CertifySubscription(*besideSubscription, 0);
        {
            std::lock_guard lock(GuestMemory::GpuMutex());
            auto* recorder = AgcDriver::Graphics::Recorder::Active();
            Require(recorder != nullptr, "recorded: no active recorder after a draw");
            notes[i].second(*recorder, target);
        }
        Require(GuestMemory::CertificateHolds(*besideSubscription, besideCertificate, 0), std::string("recorded: ") + notes[i].first + " voided a certificate on another block");
        if (GuestMemory::CertificateHolds(*targetSubscription, targetCertificate, 0)) voided = false;
    }
    device.WaitIdle();
    return voided;
}

void Recorded(AgcDriver::VulkanDevice& device) {
    Require(RecordedPendingVoids(device), "recorded: a recorded pending GPU write left a certificate on its range standing");
    GuestMemory::SetCertificateMutantForTest(GuestMemory::CertificateMutant::HideRecordedPendingBumps);
    const bool caught = RecordedPendingVoids(device);
    GuestMemory::SetCertificateMutantForTest(GuestMemory::CertificateMutant::None);
    Require(!caught, "recorded: hiding recorded pending bumps kept every certificate voided, so the case tests nothing");
    std::printf("certified proofs: recorded pending writes void certificates on their range only\n");
}

void NonDraw(AgcDriver::VulkanDevice& device) {
    using AgcDriver::DriverDetail::DrawPipeline;
    using AgcDriver::Graphics::RuntimeSwitch;
    const auto elsewhere = [&](DrawPipeline::CommitKind kind, const std::string& what) {
        auto& scene = NewScene();
        Build(device, scene);
        Settle(device, scene, what);
        PipelinedWrite(kind, reinterpret_cast<std::uintptr_t>(scene.blocks.base), std::vector<std::byte>(256));
        const auto outcome = Draw(device, scene, false);
        Require(outcome.brightest == 255u && outcome.counts.disagreed == 0, what + ": the draw after a pipelined store elsewhere sampled the wrong texels (" + outcome.Describe() + ")");
        return outcome;
    };
    auto outcome = elsewhere(DrawPipeline::CommitKind::Store, "nondraw (store elsewhere)");
    Require(outcome.counts.proved + outcome.counts.verified == 1, "nondraw: a pipelined store to a range the certificate does not subscribe voided it (" + outcome.Describe() + ")");
    AgcDriver::Graphics::RuntimeSwitches::Assign(RuntimeSwitch::NoCoveredCommits, true);
    outcome = elsewhere(DrawPipeline::CommitKind::Store, "nondraw (store elsewhere, switch set)");
    AgcDriver::Graphics::RuntimeSwitches::Assign(RuntimeSwitch::NoCoveredCommits, false);
    Require(outcome.counts.proved == 0 && outcome.Missed(CertifiedMiss::Proof) == 1, "nondraw: with APS5_NO_COVERED_COMMITS a pipelined store elsewhere kept the certificate (" + outcome.Describe() + ")");
    outcome = elsewhere(DrawPipeline::CommitKind::DmaFill, "nondraw (unaudited kind elsewhere)");
    Require(outcome.counts.proved == 0 && outcome.Missed(CertifiedMiss::Proof) == 1, "nondraw: a pipelined commit of a kind with an unknown effect kept the certificate (" + outcome.Describe() + ")");
    auto& scene = NewScene();
    Build(device, scene);
    Settle(device, scene, "nondraw (store into the texture)");
    PipelinedWrite(DrawPipeline::CommitKind::Store, reinterpret_cast<std::uintptr_t>(scene.Texels(Key)), TexelBytes(0.0f));
    outcome = Draw(device, scene, false);
    Require(outcome.counts.proved == 0 && outcome.brightest == 0u && outcome.counts.disagreed == 0, "nondraw: a pipelined store into the sampled texture kept the certificate or sampled stale texels (" + outcome.Describe() + ")");
}

void Joint(AgcDriver::VulkanDevice& device) {
    using AgcDriver::Graphics::GateMiss;
    using AgcDriver::Graphics::GateMissBit;
    using AgcDriver::Graphics::PendingBump;
    using AgcDriver::Graphics::StorageTexture;
    if (!GuestMemory::ProofBreaksCounted() || !AgcDriver::Graphics::PendingBumpsCounted()) {
        std::puts("joint: skipped without APS5_PROFILE_DRAW");
        return;
    }
    const auto onlySet = [](const Outcome& outcome, std::uint32_t set) {
        std::uint64_t others = 0;
        for (std::size_t i = 0; i < outcome.counts.certifiedGateSets.size(); ++i) {
            if (i != set) others += outcome.counts.certifiedGateSets[i];
        }
        return outcome.counts.certifiedGateSets[set] == 1 && others == 0;
    };
    auto& scene = NewScene();
    Build(device, scene);
    Settle(device, scene, "joint");
    GuestMemory::BreakProofs(GuestMemory::ProofBreak::GpuLock);
    StorageTexture::BumpPendingSerial(PendingBump::Shadowed);
    auto outcome = Draw(device, scene, false);
    RequireMissed(outcome, "joint", 255u);
    Require(onlySet(outcome, GateMissBit(GateMiss::Proof) | GateMissBit(GateMiss::Pending)), "joint: a miss after an uncovered proof break and a pending bump was not counted once with both gates (" + outcome.Describe() + ")");
    Settle(device, scene, "joint (after)");
    StorageTexture::BumpPendingSerial(PendingBump::Shadowed);
    outcome = Draw(device, scene, false);
    RequireMissed(outcome, "joint (pending only)", 255u);
    Require(onlySet(outcome, GateMissBit(GateMiss::Pending)), "joint: a miss after a pending bump alone was not counted once with only the pending gate (" + outcome.Describe() + ")");
    Require(outcome.counts.pendingMissSources[static_cast<std::size_t>(PendingBump::Shadowed)] == 1 && outcome.counts.pendingMissUnattributed == 0, "joint: a pending miss was not attributed to its bump source (" + outcome.Describe() + ")");
    if (!AgcDriver::Graphics::ShaderResources::EpochProofs()) {
        std::puts("joint: epoch gate skipped without APS5_DRAW_PIPELINE");
        return;
    }
    using AgcDriver::Graphics::EpochGateMiss;
    using AgcDriver::Graphics::EpochGateMissBit;
    AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::CertifiedProofs, false);
    static_cast<void>(Draw(device, scene, true, false));
    static_cast<void>(Draw(device, scene, false, false));
    GuestMemory::BreakProofs(GuestMemory::ProofBreak::GpuLock);
    outcome = Draw(device, scene, true, false);
    AgcDriver::Graphics::RuntimeSwitches::Assign(AgcDriver::Graphics::RuntimeSwitch::CertifiedProofs, true);
    const auto both = EpochGateMissBit(EpochGateMiss::Epoch) | EpochGateMissBit(EpochGateMiss::Proof);
    std::uint64_t withBoth = 0;
    for (std::size_t set = 0; set < outcome.counts.epochGateSets.size(); ++set) {
        if ((set & both) == both) withBoth += outcome.counts.epochGateSets[set];
    }
    Require(withBoth == 1, "joint: an epoch-proof miss after a new epoch and a proof break was not counted with both gates");
    Require(Brightest() == 255u, "joint: the draws around the epoch-gate check sampled the wrong texels");
}

struct OracleScene {
    Scene* scene = nullptr;
    std::array<float, Images> values{};
    std::uint32_t selected = Key;
    bool unwatched = false;
    bool readOnly = false;
    std::vector<GuestMemory::WriteOwner> owners;
};

struct OracleFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::uint64_t OracleSeed() {
    if (const char* seed = std::getenv("APS5_ORACLE_SEED"); seed != nullptr) return std::strtoull(seed, nullptr, 0);
    return 0x5eedu;
}

std::uint64_t RunOracle(AgcDriver::VulkanDevice& device, std::uint64_t seed, int steps, int scenesPerRun, bool owners) {
    std::mt19937_64 random(seed);
    const auto pick = [&](std::uint32_t count) { return static_cast<std::uint32_t>(random() % count); };
    std::vector<OracleScene> scenes;
    const auto fresh = [&] {
        OracleScene oracle;
        oracle.scene = &NewScene();
        Build(device, *oracle.scene);
        for (std::uint32_t image = 0; image < Images; ++image) oracle.values[image] = image == Key ? 1.0f : 0.0f;
        if (owners) {
            for (std::uint32_t image = 0; image < Images; ++image) oracle.owners.emplace_back(reinterpret_cast<std::uintptr_t>(oracle.scene->Texels(image)), BlockBytes);
        }
        return oracle;
    };
    for (int i = 0; i < scenesPerRun; ++i) scenes.push_back(fresh());
    std::uint64_t warmed = 0;
    const auto block = [](float* texels) { return reinterpret_cast<std::uintptr_t>(texels) & ~static_cast<std::uintptr_t>(BlockBytes - 1); };
    const auto flipMapping = [](OracleScene& oracle) {
        oracle.readOnly = !oracle.readOnly;
        GuestAllocations::Mutation mutation;
        mutation.Remove(oracle.scene->textures.base);
        mutation.Add(oracle.scene->textures.base, oracle.scene->textures.bytes, true, !oracle.readOnly, true);
    };
    const auto check = [&](OracleScene& oracle, int step, const char* event, int draw, bool newEpoch) {
        const auto outcome = Draw(device, *oracle.scene, newEpoch);
        if (std::getenv("APS5_ORACLE_TRACE") != nullptr) std::printf("oracle %d %s draw %d %s: %s\n", step, event, draw, newEpoch ? "new" : "same", outcome.Describe().c_str());
        const unsigned expected = oracle.values[oracle.selected] != 0.0f ? 255u : 0u;
        if (outcome.brightest != expected || outcome.counts.disagreed != 0) {
            throw OracleFailure("oracle: seed " + std::to_string(seed) + " step " + std::to_string(step) + " event " + event + " draw " + std::to_string(draw) + (newEpoch ? " (new epoch)" : " (same epoch)") + ": sampled " + std::to_string(outcome.brightest) + " instead of " + std::to_string(expected) + " (" + outcome.Describe() + ")");
        }
    };
    for (int step = 0; step < steps; ++step) {
        const auto chosen = pick(static_cast<std::uint32_t>(scenes.size()));
        if (scenes[chosen].unwatched) {
            if (scenes[chosen].readOnly) flipMapping(scenes[chosen]);
            scenes[chosen] = fresh();
        }
        auto& oracle = scenes[chosen];
        auto& scene = *oracle.scene;
        const bool warm = Certify(device, scene).counts.proved == 1;
        warmed += warm ? 1u : 0u;
        const auto other = [&] {
            auto image = pick(Images);
            if (image == oracle.selected) image = (image + 1) % Images;
            return image;
        };
        const char* event = "";
        bool cpuWrite = false;
        switch (pick(12)) {
        case 0:
        case 1: {
            event = "cpu write to the sampled texture";
            if (oracle.readOnly) flipMapping(oracle);
            oracle.values[oracle.selected] = oracle.values[oracle.selected] != 0.0f ? 0.0f : 1.0f;
            Fill(scene.Texels(oracle.selected), oracle.values[oracle.selected]);
            cpuWrite = true;
            break;
        }
        case 2: {
            event = "cpu write to another texture";
            if (oracle.readOnly) flipMapping(oracle);
            const auto image = other();
            oracle.values[image] = pick(2) == 0 ? 0.0f : 1.0f;
            Fill(scene.Texels(image), oracle.values[image]);
            cpuWrite = true;
            break;
        }
        case 3: {
            event = "cpu write beside the sampled texture";
            if (oracle.readOnly) flipMapping(oracle);
            auto* beside = reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(scene.Texels(oracle.selected)) + 4096u * (1u + pick(8)));
            std::fill_n(beside, 256, static_cast<float>(pick(3)));
            cpuWrite = true;
            break;
        }
        case 4:
        case 5: {
            event = "driver store";
            if (oracle.readOnly) flipMapping(oracle);
            const auto image = pick(2) == 0 ? oracle.selected : other();
            oracle.values[image] = oracle.values[image] != 0.0f ? 0.0f : 1.0f;
            Fill(scene.Texels(image), oracle.values[image]);
            GuestMemory::MarkWritten(reinterpret_cast<std::uintptr_t>(scene.Texels(image)), TexelWidth * TexelHeight * 4u);
            break;
        }
        case 6: {
            event = "unwatch, then a write to the sampled texture";
            GuestMemory::Unwatch(block(scene.Texels(oracle.selected)), BlockBytes);
            oracle.unwatched = true;
            oracle.values[oracle.selected] = oracle.values[oracle.selected] != 0.0f ? 0.0f : 1.0f;
            if (oracle.readOnly) flipMapping(oracle);
            Fill(scene.Texels(oracle.selected), oracle.values[oracle.selected]);
            break;
        }
        case 7: {
            event = "a write to the sampled texture, then a mapping change";
            if (oracle.readOnly) flipMapping(oracle);
            oracle.values[oracle.selected] = oracle.values[oracle.selected] != 0.0f ? 0.0f : 1.0f;
            Fill(scene.Texels(oracle.selected), oracle.values[oracle.selected]);
            flipMapping(oracle);
            break;
        }
        case 8: {
            event = "texture release";
            std::lock_guard lock(GuestMemory::GpuMutex());
            AgcDriver::Graphics::ClearCachedTextures(device.Device());
            break;
        }
        case 9: {
            event = "own store";
            if (oracle.readOnly) flipMapping(oracle);
            const auto image = pick(2) == 0 ? oracle.selected : other();
            oracle.values[image] = oracle.values[image] != 0.0f ? 0.0f : 1.0f;
            std::vector<float> texels(TexelWidth * TexelHeight, oracle.values[image]);
            static_cast<void>(GuestMemory::StoreOwnBytes(reinterpret_cast<std::uintptr_t>(scene.Texels(image)), std::as_bytes(std::span(texels))));
            break;
        }
        case 10: {
            event = "pipelined store";
            if (oracle.readOnly) flipMapping(oracle);
            const auto image = pick(2) == 0 ? oracle.selected : other();
            oracle.values[image] = oracle.values[image] != 0.0f ? 0.0f : 1.0f;
            PipelinedWrite(AgcDriver::DriverDetail::DrawPipeline::CommitKind::Store, reinterpret_cast<std::uintptr_t>(scene.Texels(image)), TexelBytes(oracle.values[image]));
            break;
        }
        default: {
            event = "depth surfaces change";
            AgcDriver::Graphics::ClearDepthSurfaces(device.Device());
            break;
        }
        }
        check(oracle, step, event, 0, cpuWrite || pick(2) == 0);
        if (pick(3) == 0) check(oracle, step, event, 1, pick(2) == 0);
    }
    for (auto& oracle : scenes) {
        if (oracle.readOnly) flipMapping(oracle);
    }
    return warmed;
}

enum class OracleMode { Handles, Owners, Blocks };

std::uint64_t RunOracleWith(AgcDriver::VulkanDevice& device, std::uint64_t seed, int steps, int scenesPerRun, OracleMode mode) {
    using AgcDriver::Graphics::RuntimeSwitch;
    struct Restore {
        ~Restore() {
            AgcDriver::Graphics::RuntimeSwitches::Assign(RuntimeSwitch::NoWriteOwners, false);
            GuestMemory::SetWriteOwnersIgnoredForTest(false);
        }
    } restore;
    AgcDriver::Graphics::RuntimeSwitches::Assign(RuntimeSwitch::NoWriteOwners, mode == OracleMode::Handles);
    GuestMemory::SetWriteOwnersIgnoredForTest(mode == OracleMode::Blocks);
    return RunOracle(device, seed, steps, scenesPerRun, mode == OracleMode::Owners);
}

void Oracle(AgcDriver::VulkanDevice& device) {
    const auto seed = OracleSeed();
    constexpr int steps = 160;
    for (const auto oracleMode : {OracleMode::Handles, OracleMode::Owners, OracleMode::Blocks}) {
        const auto before = GuestMemory::WriteIndexCounted();
        const auto warmed = RunOracleWith(device, seed, steps, 3, oracleMode);
        const auto after = GuestMemory::WriteIndexCounted();
        const auto dependencies = after.ownerDependencies - before.ownerDependencies;
        const auto blockDependencies = after.blockDependencies - before.blockDependencies;
        const auto ownerBumps = after.ownerBumps + after.ownerFastBumps - before.ownerBumps - before.ownerFastBumps;
        const char* mode = oracleMode == OracleMode::Handles ? "on handles" : oracleMode == OracleMode::Owners ? "with owners" : "on block counters";
        const bool shaped = oracleMode == OracleMode::Handles ? dependencies == 0 && blockDependencies == 0 : oracleMode == OracleMode::Owners ? dependencies != 0 && ownerBumps != 0 : dependencies == 0 && blockDependencies != 0;
        Require(warmed * 4 >= steps * 3, std::string("oracle ") + mode + ": only " + std::to_string(warmed) + " of " + std::to_string(steps) + " events met a live certificate, too few to test anything");
        Require(shaped, std::string("oracle ") + mode + ": " + std::to_string(dependencies) + " owner dependencies, " + std::to_string(blockDependencies) + " block dependencies and " + std::to_string(ownerBumps) + " owner bumps");
        std::printf("certified proofs: oracle %s seed %llu, %llu of %d events met a live certificate, %llu owner dependencies, %llu block dependencies, %llu owner bumps\n", mode, static_cast<unsigned long long>(seed), static_cast<unsigned long long>(warmed), steps, static_cast<unsigned long long>(dependencies), static_cast<unsigned long long>(blockDependencies), static_cast<unsigned long long>(ownerBumps));
    }
}

void Mutants(AgcDriver::VulkanDevice& device) {
    using GuestMemory::CertificateMutant;
    struct Mutant {
        CertificateMutant mutant;
        const char* name;
        OracleMode mode;
    };
    constexpr auto Handles = OracleMode::Handles;
    constexpr auto Owners = OracleMode::Owners;
    constexpr auto Blocks = OracleMode::Blocks;
    constexpr std::array<Mutant, 12> mutants{{{CertificateMutant::HidePipelineCommitBumps, "a stale certificate after a pipelined store that bumps nothing", Handles}, {CertificateMutant::HideOwnStoreBumps, "a stale certificate after an own store that bumps nothing", Handles}, {CertificateMutant::IgnoreVersion, "a stale certificate after a write", Handles}, {CertificateMutant::HideUnwatch, "a stale certificate after an unwatch", Handles}, {CertificateMutant::HideMapping, "a stale certificate after a mapping change", Handles}, {CertificateMutant::HideMarkWrittenBumps, "a stale certificate after a driver store that bumps nothing", Handles}, {CertificateMutant::IgnoreOwners, "a stale certificate after a write into its owner", Owners}, {CertificateMutant::HideOwnerBumps, "a stale certificate after a write its owner never counts", Owners}, {CertificateMutant::CertificateDropsOwners, "a stale certificate that does not carry its owners", Owners}, {CertificateMutant::HideBlockBumps, "a stale certificate after a write its block never counts", Blocks}, {CertificateMutant::CertificateDropsBlocks, "a stale certificate that does not carry its blocks", Blocks}, {CertificateMutant::CertificateSkipsVersion, "a stale certificate on handles that skips its version", Handles}}};
    for (const auto& [mutant, name, mode] : mutants) {
        GuestMemory::SetCertificateMutantForTest(mutant);
        std::string caught;
        for (std::uint64_t attempt = 0; attempt < 4 && caught.empty(); ++attempt) {
            try {
                RunOracleWith(device, OracleSeed() + attempt, 160, 3, mode);
            } catch (const OracleFailure& failure) {
                caught = failure.what();
            }
        }
        GuestMemory::SetCertificateMutantForTest(CertificateMutant::None);
        {
            std::lock_guard lock(GuestMemory::GpuMutex());
            AgcDriver::Graphics::ClearCachedTextures(device.Device());
        }
        Require(!caught.empty(), std::string("mutants: the oracle did not catch ") + name);
        std::printf("certified proofs: mutant caught, %s: %s\n", name, caught.c_str());
    }
    GuestMemory::SetCertificateMutantForTest(CertificateMutant::HideCertifiedProof);
    const bool voided = UncoveredBreakVoids(device, "mutants (hidden certificate breaks)");
    GuestMemory::SetCertificateMutantForTest(CertificateMutant::None);
    Require(!voided, "mutants: hiding the certificate break serial did not keep a certificate across an uncovered proof break, so the scope check tests nothing");
    std::printf("certified proofs: mutant caught, hidden certificate breaks\n");
}
}

int main(int argc, char** argv) {
    try {
        GuestMemory::SetWriteIndexCountingForTest(true);
#ifdef _WIN32
        _putenv_s("APS5_CERTIFIED_PROOFS", "1");
#else
        setenv("APS5_CERTIFIED_PROOFS", "1", 1);
#endif
        const std::vector<std::string> args(argv + 1, argv + argc);
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Require(AgcDriver::Graphics::CertifiedProofs(), "certified proofs are not enabled");
        GuestMemory::MarkProofThread();
        if (!GuestMemory::WriteWatched()) {
            std::puts("skipped, guest memory has no write watch");
            return VulkanTestSkipped;
        }
        TargetBytes = AgcDriver::Graphics::ColorTargetLayout(TargetWidth, TargetHeight, AgcDriver::Graphics::ColorTileMode::RenderTarget, 4).Bytes();
        GuestBlocks target((TargetBytes + BlockBytes - 1) / BlockBytes * BlockBytes);
        Target = &target;
        if (!SceneWatched(*device)) {
            std::puts("skipped, the scene's tables or texels are host imports this platform compares instead of watching");
            return VulkanTestSkipped;
        }
        const std::array<std::pair<const char*, std::function<void(AgcDriver::VulkanDevice&)>>, 22> cases{{{"causes", &Causes}, {"held", &Held}, {"cpu", &CpuWrite}, {"table", &TableWord}, {"driver", &DriverStore}, {"pending", &PendingWrite}, {"release", &Released}, {"unwatch", &UnwatchRange}, {"mapping", &Mapping}, {"import", &Imported}, {"depth", &DepthSurfaces}, {"switch", &Switched}, {"verify", &Verified}, {"scope", &Scope}, {"nondraw", &NonDraw}, {"recorded", &Recorded}, {"invalidate", &Invalidate}, {"inputs", &Inputs}, {"layers", &Layers}, {"joint", &Joint}, {"oracle", &Oracle}, {"mutants", &Mutants}}};
        for (const auto& [name, run] : cases) {
            if (!args.empty() && std::find(args.begin(), args.end(), name) == args.end()) continue;
            run(*device);
            std::printf("certified proofs: %s passed\n", name);
        }
        device->WaitIdle();
        Scenes.clear();
        std::puts("certified proofs tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
