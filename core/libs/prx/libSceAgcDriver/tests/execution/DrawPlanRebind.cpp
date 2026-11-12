#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Recipe.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
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
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::ColorTargetLayout;
using AgcDriver::Graphics::ColorTileMode;
using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 64;
constexpr std::size_t BlockBytes = 65536;
constexpr std::size_t VertexCodeOffset = 0;
constexpr std::size_t PixelCodeOffset = 256;

constexpr std::array<std::uint32_t, 6> VertexCode{
    0xe0382000u, 0x80000005u, 0xbf8c3f70u, 0xf80008cfu, 0x03020100u, 0xbf810000u,
};
constexpr std::array<std::uint32_t, 5> PixelCode{0x7e000200u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u};

using Triangle = std::array<std::array<float, 4>, 3>;
constexpr Triangle FullScreen{{{-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}}};
constexpr Triangle LowerLeft{{{-1.0f, -1.0f, 0.5f, 1.0f}, {1.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 1.0f, 0.5f, 1.0f}}};
constexpr Triangle UpperRight{{{1.0f, 1.0f, 0.5f, 1.0f}, {-1.0f, 1.0f, 0.5f, 1.0f}, {1.0f, -1.0f, 0.5f, 1.0f}}};

const ColorTargetLayout TargetLayout(Width, Height, ColorTileMode::RenderTarget, 4);

class Pages {
public:
    explicit Pages(std::size_t bytes) : bytes(bytes) {
#ifdef _WIN32
        data = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, BlockBytes));
        if (data != nullptr) GuestArena::GuestArenaCommit_nid_postfix(data, bytes, PAGE_READWRITE, bytes);
#else
        void* mapped = mmap(nullptr, bytes + BlockBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Require(mapped != MAP_FAILED, "draw plan rebind: cannot map guest pages");
        const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
        const auto aligned = (begin + BlockBytes - 1) & ~(static_cast<std::uintptr_t>(BlockBytes) - 1);
        if (aligned != begin) munmap(mapped, aligned - begin);
        if (aligned + bytes != begin + bytes + BlockBytes) munmap(reinterpret_cast<void*>(aligned + bytes), begin + BlockBytes - aligned);
        data = reinterpret_cast<std::uint8_t*>(aligned);
        GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(data, bytes);
#endif
        Require(data != nullptr, "draw plan rebind: cannot allocate guest pages");
        std::memset(data, 0, bytes);
    }

    ~Pages() {
        AgcDriver::Graphics::StorageTexture::FlushPending(Address(), bytes, nullptr, "test release");
        if (registered) GuestAllocations::Mutation().Remove(data);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(data, bytes);
        GuestArena::GuestArenaRelease_nid_postfix(data, bytes);
#else
        munmap(data, bytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(data, bytes);
#endif
    }

    Pages(const Pages&) = delete;
    Pages& operator=(const Pages&) = delete;

    void Register(bool writable) {
        GuestAllocations::Mutation().Add(data, bytes, true, writable, true);
        registered = true;
    }

    std::uint64_t Address(std::size_t offset = 0) const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data + offset)); }
    std::uint8_t* Data() { return data; }

private:
    std::size_t bytes;
    std::uint8_t* data = nullptr;
    bool registered = false;
};

struct Scene {
    AgcDriver::VulkanDevice& device;
    Pages pixels{TargetLayout.Bytes()};
    Pages vertices{BlockBytes};
    Pages spare{BlockBytes};
    Pages code{BlockBytes};
    std::deque<ShaderRecompiler::RecompileResult> vertexResults;
    std::deque<ShaderRecompiler::RecompileResult> pixelResults;
    std::uint32_t vertexPush = 0;
    std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{};
    std::vector<AgcDriver::Graphics::GuestMemorySnapshot> snapshots;
    AgcDriver::Graphics::State state{};
    float colour = 0.25f;

    explicit Scene(AgcDriver::VulkanDevice& device) : device(device) {
        pixels.Register(true);
        std::memcpy(vertices.Data(), FullScreen.data(), sizeof(FullScreen));
        vertices.Register(false);
        std::memcpy(spare.Data(), LowerLeft.data(), sizeof(LowerLeft));
        spare.Register(true);
        std::memcpy(code.Data() + VertexCodeOffset, VertexCode.data(), sizeof(VertexCode));
        std::memcpy(code.Data() + PixelCodeOffset, PixelCode.data(), sizeof(PixelCode));
        code.Register(false);
        state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 64u, 64u, std::nullopt, std::nullopt};
        state.color = {pixels.Address(), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, TargetLayout.Bytes(), 0xe4u, ColorTileMode::RenderTarget};
        state.colors = {state.color};
        state.hasColorTarget = true;
        state.renderExtent = {Width, Height};
        state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
        state.negativeOneToOne = false;
        state.scissor = {{0, 0}, {Width, Height}};
        state.cullMode = VK_CULL_MODE_NONE;
        state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        state.blend.colorWriteMask = 15;
        state.blends = {state.blend};
        state.blendConstants = {};
        Fetch(vertices);
    }

    void Fetch(Pages& source) {
        const auto address = source.Address();
        const std::vector<std::uint32_t> userData{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (16u << 16u), static_cast<std::uint32_t>(sizeof(Triangle) / 16u), 0x01016facu};
        const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{code.Address(VertexCodeOffset), std::as_bytes(std::span(VertexCode))}}};
        ShaderRecompiler::RecompileRequest vertex{
            {ShaderStage::Vertex, code.Address(VertexCodeOffset), VertexCode, 0, {}},
            {64, 0, userData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
            device.Target(),
            {0, 0, 0, 64}
        };
        vertexResults.push_back(ShaderRecompiler::Recompile(vertex));
        if (vertexResults.size() > 8) vertexResults.pop_front();
        vertexPush = static_cast<std::uint32_t>(vertexResults.back().pushConstants.size());
        shaders[0] = {ShaderStage::Vertex, &vertexResults.back(), 0};
        Paint(colour);
    }

    void Paint(float value) {
        colour = value;
        const auto target = device.Target();
        ShaderRecompiler::ShaderPixelStageInfo pixel{};
        pixel.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionX) | ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionY);
        pixel.posX = true;
        pixel.posY = true;
        pixel.targetOutputMode[0] = 9;
        pixel.targetExportMapping.fill(0xe4u);
        const std::vector<std::uint32_t> userData{std::bit_cast<std::uint32_t>(value), 0u, 0u, 0u};
        const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{code.Address(PixelCodeOffset), std::as_bytes(std::span(PixelCode))}}};
        ShaderRecompiler::RecompileRequest fragment{
            {ShaderStage::Fragment, code.Address(PixelCodeOffset), PixelCode, 0, {}},
            {64, 0, userData, std::nullopt, pixel, std::nullopt, pixelMemory},
            target,
            PixelPushLayout(vertexPush, target)
        };
        pixelResults.push_back(ShaderRecompiler::Recompile(fragment));
        if (pixelResults.size() > 8) pixelResults.pop_front();
        shaders[1] = {ShaderStage::Fragment, &pixelResults.back(), PixelPushOffset(vertexPush, target)};
        snapshots = {
            {code.Address(VertexCodeOffset), std::as_bytes(std::span(reinterpret_cast<const std::uint32_t*>(code.Data() + VertexCodeOffset), VertexCode.size()))},
            {code.Address(PixelCodeOffset), std::as_bytes(std::span(reinterpret_cast<const std::uint32_t*>(code.Data() + PixelCodeOffset), PixelCode.size()))},
        };
    }

    static AgcDriver::Pm4::DrawParameters Parameters() { return {0, 3u, 0, 1, 0, false}; }

    std::shared_ptr<const AgcDriver::DrawRecipe> Draw() {
        std::shared_ptr<const AgcDriver::DrawRecipe> recipe;
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        static_cast<void>(AgcDriver::Graphics::TakeDrawPlanShadowDecision());
        device.Draw(state, Parameters(), shaders, snapshots, &recipe);
        return recipe != nullptr ? recipe : AgcDriver::Graphics::TakeDrawPlanShadowDecision();
    }

    AgcDriver::Graphics::DrawRecipeOutcome Serve(const AgcDriver::DrawRecipe& plan) {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        return device.DrawFromPlan(state, Parameters(), shaders, snapshots, plan);
    }

    void Wait() {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        device.WaitIdle();
    }

    std::vector<std::uint8_t> Pixels() {
        AgcDriver::Graphics::StorageTexture::FlushPending(pixels.Address(), TargetLayout.Bytes(), nullptr, "test read-back");
        Wait();
        std::vector<std::uint8_t> texels(TargetLayout.Bytes());
        AgcDriver::GuestMemory::Read(pixels.Address(), std::as_writable_bytes(std::span(texels)), 1);
        return texels;
    }
};

void Expect(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error("draw plan rebind: " + what);
}

bool Uniform(const std::vector<std::uint8_t>& texels) {
    return std::all_of(texels.begin(), texels.end(), [&](std::uint8_t value) { return value == texels.front(); });
}

void Served(const AgcDriver::Graphics::DrawRecipeOutcome& outcome, std::uint32_t moved, const char* what) {
    Expect(outcome.recorded, std::string(what) + " was not served (miss " + AgcDriver::Graphics::DrawRecipeMissName(outcome.miss) + ")");
    Expect(outcome.movedBuffers == moved, std::string(what) + " rebound " + std::to_string(outcome.movedBuffers) + " buffers, expected " + std::to_string(moved));
}

void RebindsAMovedVertexBuffer(AgcDriver::VulkanDevice& device) {
    Scene planned(device);
    Scene full(device);
    const auto plan = planned.Draw();
    Expect(plan != nullptr && plan->servable && plan->plannable && plan->movedBases == 0, "the first draw left no servable plan");
    planned.Fetch(planned.spare);
    planned.Paint(0.75f);
    Served(planned.Serve(*plan), 1, "a draw whose vertex buffer moved");
    static_cast<void>(full.Draw());
    full.Fetch(full.spare);
    full.Paint(0.75f);
    static_cast<void>(full.Draw());
    const auto pixels = planned.Pixels();
    Expect(!Uniform(pixels), "the moved vertex buffer's half-screen triangle did not show, so the test cannot tell the bases apart");
    Expect(pixels == full.Pixels(), "a plan hit that rebound its vertex buffer renders differently from the full path");
    std::puts("draw plan rebind: a plan hit binds a moved read-only vertex buffer anew");
}

void RebindsFromAMovedPlan(AgcDriver::VulkanDevice& device) {
    Scene planned(device);
    Scene full(device);
    const auto run = [](Scene& scene, bool usePlan) {
        static_cast<void>(scene.Draw());
        scene.Fetch(scene.spare);
        scene.Paint(0.5f);
        const auto plan = scene.Draw();
        scene.Fetch(scene.vertices);
        scene.Paint(0.75f);
        if (usePlan) {
            Expect(plan != nullptr && plan->plannable && !plan->servable && plan->movedBases == 1, "a draw served by a moved template left no plannable plan with one moved base");
            Served(scene.Serve(*plan), 0, "a draw at the template's own base from a moved plan");
        } else {
            static_cast<void>(scene.Draw());
        }
        scene.Fetch(scene.spare);
        scene.Paint(1.0f);
        if (usePlan) Served(scene.Serve(*plan), 1, "a draw with the plan's own moved words");
        else static_cast<void>(scene.Draw());
    };
    run(planned, true);
    run(full, false);
    const auto pixels = planned.Pixels();
    Expect(!Uniform(pixels), "the last half-screen draw did not show");
    Expect(pixels == full.Pixels(), "plan hits from a plan recorded with a moved base render differently from the full path");
    std::puts("draw plan rebind: a plan recorded with a moved base rebinds against its template, not its words");
}

void RefusesAMoveOverAPendingWrite(AgcDriver::VulkanDevice& device) {
    Scene planned(device);
    const auto plan = planned.Draw();
    Expect(plan != nullptr && plan->servable, "the first draw left no servable plan");
    planned.Fetch(planned.spare);
    const std::array<std::uint32_t, 4> pattern{0u, 0u, 0u, 0u};
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Expect(device.FillBuffer(planned.spare.Address(sizeof(Triangle) - 16u), 16, pattern), "the fill over the moved vertex buffer was not recorded");
    }
    const auto outcome = planned.Serve(*plan);
    Expect(!outcome.recorded && outcome.miss == AgcDriver::Graphics::DrawRecipeMiss::Moved, std::string("a plan hit whose moved vertex buffer a pending GPU write overlaps was not refused as a move (miss ") + AgcDriver::Graphics::DrawRecipeMissName(outcome.miss) + ")");
    planned.Wait();
    std::puts("draw plan rebind: a move over a pending GPU write is refused");
}

void SeesGuestWritesToTheMovedBuffer(AgcDriver::VulkanDevice& device) {
    Scene planned(device);
    Scene full(device);
    const auto run = [](Scene& scene, bool usePlan) {
        const auto plan = scene.Draw();
        scene.Fetch(scene.spare);
        scene.Paint(0.75f);
        if (usePlan) Served(scene.Serve(*plan), 1, "the first moved draw");
        else static_cast<void>(scene.Draw());
        static_cast<void>(scene.Pixels());
        std::memcpy(scene.spare.Data(), UpperRight.data(), sizeof(UpperRight));
        scene.Paint(0.5f);
        if (usePlan) Served(scene.Serve(*plan), 1, "the moved draw after a guest write to its vertices");
        else static_cast<void>(scene.Draw());
    };
    run(planned, true);
    run(full, false);
    Expect(planned.Pixels() == full.Pixels(), "a plan hit after a guest write to its moved vertex buffer renders differently from the full path");
    std::puts("draw plan rebind: a rebound vertex buffer is read as the guest left it");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        AgcDriver::Graphics::SetDrawPlansForTest(1);
        RebindsAMovedVertexBuffer(*device);
        RebindsFromAMovedPlan(*device);
        RefusesAMoveOverAPendingWrite(*device);
        SeesGuestWritesToTheMovedBuffer(*device);
        AgcDriver::Graphics::SetDrawPlansForTest(-1);
        std::printf("draw plan rebind tests passed%s\n", AgcDriver::Graphics::VerifyDrawPlans() ? " (verified)" : "");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
