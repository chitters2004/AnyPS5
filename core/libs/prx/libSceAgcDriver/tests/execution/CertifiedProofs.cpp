#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
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
    GuestMemory::MarkWritten(reinterpret_cast<std::uintptr_t>(scene.Texels(Other)), TexelWidth * TexelHeight * 4u);
    outcome = Draw(device, scene);
    RequireMissed(outcome, "causes", 255u);
    if (GuestMemory::ProofBreaksCounted() && outcome.Missed(CertifiedMiss::Proof) == 1) Require(outcome.ProofSource(GuestMemory::ProofBreak::MarkWritten) == 1 && outcome.counts.proofMissUnattributed == 0, "causes: a proof miss after a driver store was not attributed to mark-written (" + outcome.Describe() + ")");
    if (GuestMemory::ProofBreaksCounted()) Require(outcome.Missed(CertifiedMiss::Proof) == 1, "causes: a driver store did not void the certificate through the proof domain (" + outcome.Describe() + ")");
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

}

int main(int argc, char** argv) {
    try {
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
        const std::array<std::pair<const char*, std::function<void(AgcDriver::VulkanDevice&)>>, 13> cases{{{"causes", &Causes}, {"held", &Held}, {"cpu", &CpuWrite}, {"table", &TableWord}, {"driver", &DriverStore}, {"pending", &PendingWrite}, {"release", &Released}, {"unwatch", &UnwatchRange}, {"mapping", &Mapping}, {"import", &Imported}, {"depth", &DepthSurfaces}, {"switch", &Switched}, {"verify", &Verified}}};
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
