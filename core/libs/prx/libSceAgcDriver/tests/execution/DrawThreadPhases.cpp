#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawTiming.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;

alignas(256) constexpr std::array<std::uint32_t, 16> VertexCode{
    0x7e000d05u, 0x080200f4u, 0x10020300u, 0x100202f4u, 0x060202f3u, 0x060400f3u, 0x10040500u, 0x060404f3u,
    0x7e060280u, 0x7e0802f2u, 0xf80008cfu, 0x04030201u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 10> PixelCode{0xf4000100u, 0xfa000000u, 0xbf8cc07fu, 0x7e000204u, 0x06000002u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u, 0xbf810000u};
alignas(256) std::array<float, 4> PixelConstant{};
alignas(256) std::array<std::byte, 1024> ColorMemory{};

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header PixelHeader{};

void Registered(Header& header, const std::uint32_t* code, std::size_t bytes, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(bytes);
    header.shader.code = code;
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

std::vector<std::uint32_t> Setup() {
    const auto color = reinterpret_cast<std::uintptr_t>(ColorMemory.data());
    const auto vertex = reinterpret_cast<std::uintptr_t>(VertexCode.data());
    const auto pixel = reinterpret_cast<std::uintptr_t>(PixelCode.data());
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x2000},
        {0x1b6, 0}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, (63u << 14u) | 3u},
        {0x3b8, 0x9000000}, {0x1e0, 0},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040},
        {0x318, static_cast<std::uint32_t>(color >> 8u)}, {0x390, static_cast<std::uint32_t>(color >> 40u)},
        {0x10f, std::bit_cast<std::uint32_t>(32.0f)}, {0x110, std::bit_cast<std::uint32_t>(32.0f)},
        {0x111, std::bit_cast<std::uint32_t>(-2.0f)}, {0x112, std::bit_cast<std::uint32_t>(2.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
        {0x2fe, 0}, {0x302, 0}, {0x306, 0}, {0x30a, 0},
    };
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> shader{
        {0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, 0}, {0x8c, 0},
        {0x008, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009, static_cast<std::uint32_t>(pixel >> 40u)}, {0x00b, 3u << 1u},
    };
    std::vector<std::uint32_t> commands;
    Append(commands, 0x79, {0x242, 4});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : shader) Append(commands, 0x76, {offset, value});
    const auto table = reinterpret_cast<std::uintptr_t>(PixelConstant.data());
    Append(commands, 0x76, {0x00c, static_cast<std::uint32_t>(table)});
    Append(commands, 0x76, {0x00d, static_cast<std::uint32_t>(table >> 32u)});
    Append(commands, 0x76, {0x00e, std::bit_cast<std::uint32_t>(0.25f)});
    return commands;
}

using AgcDriver::DriverDetail::PhaseGroup;
using AgcDriver::DriverDetail::PhaseThread;
using AgcDriver::DriverDetail::ThreadPhaseTotals;

std::uint64_t Group(const ThreadPhaseTotals& totals, PhaseThread thread, PhaseGroup group) {
    return totals.groupNs[static_cast<std::size_t>(thread)][static_cast<std::size_t>(group)];
}

std::uint64_t Grouped(const ThreadPhaseTotals& totals, PhaseThread thread) {
    return Group(totals, thread, PhaseGroup::Prepare) + Group(totals, thread, PhaseGroup::Lookup) + Group(totals, thread, PhaseGroup::Handoff) + Group(totals, thread, PhaseGroup::Graphics);
}

std::uint64_t Draws(const ThreadPhaseTotals& totals, PhaseThread thread) {
    return totals.draws[static_cast<std::size_t>(thread)];
}

std::uint64_t DrawNs(const ThreadPhaseTotals& totals, PhaseThread thread) {
    return totals.drawNs[static_cast<std::size_t>(thread)];
}

std::uint64_t LapNs(const ThreadPhaseTotals& totals, PhaseThread thread) {
    return totals.lapNs[static_cast<std::size_t>(thread)];
}

std::vector<std::uint32_t> Draws(std::uint64_t count, float base) {
    auto commands = Setup();
    for (std::uint64_t index = 0; index < count; ++index) {
        Append(commands, 0x69, {0x10f, std::bit_cast<std::uint32_t>(base + static_cast<float>(index))});
        Append(commands, 0x2d, {3, 2});
    }
    return commands;
}

void QueuedJobsKeepTheirOwnTimes() {
    using AgcDriver::DriverDetail::DrawFinisher;
    using AgcDriver::DriverDetail::ThreadPhases;
    constexpr std::uint64_t Queued = 4;
    constexpr auto Hold = std::chrono::milliseconds(200);
    static_cast<void>(ThreadPhases::Take());
    std::atomic<bool> filled{false};
    std::atomic<std::int64_t> heldNs{0};
    DrawFinisher::Pending blocker;
    blocker.unknown = false;
    DrawFinisher::Get().Enqueue([&] {
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + std::chrono::seconds(10);
        while (DrawFinisher::Outstanding() < Queued + 1 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        filled.store(DrawFinisher::Outstanding() >= Queued + 1);
        std::this_thread::sleep_for(Hold);
        heldNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    }, std::move(blocker));
    auto commands = Draws(Queued, 96.0f);
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "the queued draw submission failed");
    AgcDriverWaitIdle_nid_postfix();
    DrawFinisher::Get().Drain();
    const auto totals = ThreadPhases::Take();
    const auto text = ThreadPhases::Text(totals);
    std::cout << text << '\n';
    Require(filled.load(), "the worker did not queue " + std::to_string(Queued) + " draw jobs behind a held finisher job: " + text);
    Require(Draws(totals, PhaseThread::Finisher) == Queued, "the finisher ran " + std::to_string(Draws(totals, PhaseThread::Finisher)) + " of " + std::to_string(Queued) + " queued jobs: " + text);
    Require(DrawNs(totals, PhaseThread::Finisher) < static_cast<std::uint64_t>(heldNs.load()), "the finisher's job times include the " + std::to_string(heldNs.load()) + " ns they waited in the queue: " + text);
    Require(Grouped(totals, PhaseThread::Finisher) <= LapNs(totals, PhaseThread::Finisher), "the finisher's phase groups exceed all of its phase laps: " + text);
    Require(LapNs(totals, PhaseThread::Finisher) <= DrawNs(totals, PhaseThread::Finisher), "the finisher's phase laps for queued jobs sum past their job times, so a lap started before its job did: " + text);
}

}

int main() {
    try {
        bool libraries = false;
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            libraries = device->GraphicsPipelineLibraries();
        }
        if (!libraries && std::getenv("APS5_NO_GPL") == nullptr) return VulkanTestSkipped;
        using AgcDriver::DriverDetail::DrawFinisher;
        using AgcDriver::DriverDetail::ThreadPhases;
        const bool timed = ThreadPhases::Enabled();
        const bool finisher = DrawFinisher::Enabled() && DrawFinisher::Overlap();
        Registered(VertexHeader, VertexCode.data(), sizeof(VertexCode), 2);
        Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
        PixelConstant[0] = 0.25f;
        static_cast<void>(ThreadPhases::Take());
        constexpr std::uint64_t Count = 16;
        auto commands = Draws(Count, 32.0f);
        Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
        Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
        AgcDriverWaitIdle_nid_postfix();
        if (finisher) DrawFinisher::Get().Drain();
        const auto totals = ThreadPhases::Take();
        const auto text = ThreadPhases::Text(totals);
        std::cout << text << '\n';
        if (!timed) {
            Require(totals.submissions == 0 && LapNs(totals, PhaseThread::Worker) == 0 && LapNs(totals, PhaseThread::Finisher) == 0 && Draws(totals, PhaseThread::Worker) == 0 && Draws(totals, PhaseThread::Finisher) == 0 && totals.busyNs == 0, "draw thread phases were counted without APS5_DRAW_THREAD_TIMES, APS5_PIPELINE_STATS or APS5_PROFILE_DRAW: " + text);
            AgcDriverShutdown_nid_postfix();
            std::cout << "draw thread phases stay off without their gate\n";
            return 0;
        }
        Require(totals.submissions >= 1, "no queue 0 submission was timed: " + text);
        Require(totals.busyNs <= totals.windowNs, "the worker was busy longer than the window: " + text);
        Require(Draws(totals, PhaseThread::Worker) == Count, "the worker timed " + std::to_string(Draws(totals, PhaseThread::Worker)) + " draws, expected " + std::to_string(Count) + ": " + text);
        Require(Draws(totals, PhaseThread::Other) == 0 && Grouped(totals, PhaseThread::Other) == 0, "draws on queue 0 were counted on another queue: " + text);
        Require(DrawNs(totals, PhaseThread::Worker) <= totals.busyNs, "the worker's draws took longer than its submissions: " + text);
        Require(Grouped(totals, PhaseThread::Worker) <= LapNs(totals, PhaseThread::Worker) && LapNs(totals, PhaseThread::Worker) <= DrawNs(totals, PhaseThread::Worker), "the worker's phase laps exceed its draw time: " + text);
        Require(Group(totals, PhaseThread::Worker, PhaseGroup::Prepare) > 0, "the worker's decode and prepare were not timed: " + text);
        if (finisher) {
            Require(Draws(totals, PhaseThread::Finisher) >= 1 && Draws(totals, PhaseThread::Finisher) <= Count, "the finisher ran " + std::to_string(Draws(totals, PhaseThread::Finisher)) + " jobs for " + std::to_string(Count) + " draws: " + text);
            Require(Grouped(totals, PhaseThread::Finisher) <= LapNs(totals, PhaseThread::Finisher) && LapNs(totals, PhaseThread::Finisher) <= DrawNs(totals, PhaseThread::Finisher), "the finisher's phase laps exceed its job time: " + text);
            Require(Group(totals, PhaseThread::Finisher, PhaseGroup::Handoff) + Group(totals, PhaseThread::Finisher, PhaseGroup::Graphics) > 0, "the finisher's commit hand-off and Graphics::Draw were not timed: " + text);
            QueuedJobsKeepTheirOwnTimes();
        } else {
            Require(Draws(totals, PhaseThread::Finisher) == 0 && Grouped(totals, PhaseThread::Finisher) == 0, "finisher time was counted with no finisher: " + text);
            Require(Group(totals, PhaseThread::Worker, PhaseGroup::Handoff) + Group(totals, PhaseThread::Worker, PhaseGroup::Graphics) > 0, "the worker's commit hand-off and Graphics::Draw were not timed: " + text);
        }
        AgcDriverShutdown_nid_postfix();
        std::cout << "draw thread phases accounted " << (finisher ? "across the worker and the finisher, queued jobs included" : "on the worker") << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
