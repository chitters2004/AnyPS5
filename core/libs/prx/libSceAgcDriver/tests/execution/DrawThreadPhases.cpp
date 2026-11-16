#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawTiming.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
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


void Add(AgcDriver::DriverDetail::ThreadPhaseTotals& totals, const AgcDriver::DriverDetail::ThreadPhaseTotals& more) {
    for (std::size_t thread = 0; thread < totals.groupNs.size(); ++thread) {
        for (std::size_t group = 0; group < totals.groupNs[thread].size(); ++group) totals.groupNs[thread][group] += more.groupNs[thread][group];
        totals.lapNs[thread] += more.lapNs[thread];
        totals.drawNs[thread] += more.drawNs[thread];
        totals.draws[thread] += more.draws[thread];
    }
    totals.busyNs += more.busyNs;
    totals.submissions += more.submissions;
    totals.windowNs += more.windowNs;
}

void RewindTailsCountOnce() {
    using AgcDriver::DriverDetail::ThreadPhases;
    constexpr std::size_t Tails = 12;
    alignas(8) static std::array<std::uint32_t, Tails> gates{};
    alignas(8) static std::uint32_t done = 0;
    const auto low = [](const void* address) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(address)); };
    const auto high = [](const void* address) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(address) >> 32u); };
    std::vector<std::uint32_t> commands;
    for (auto& gate : gates) {
        Append(commands, 0x59, {0x80000000u});
        Append(commands, 0x3c, {0x13, low(&gate), high(&gate), 1, 0xffffffffu, 0x19});
    }
    Append(commands, 0x37, {0x00100200, low(&done), high(&done), 1});
    static_cast<void>(ThreadPhases::Take());
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "the rewind submission failed");
    for (auto& gate : gates) {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        std::atomic_ref<std::uint32_t>(gate).store(1);
    }
    AgcDriverWaitIdle_nid_postfix();
    auto totals = ThreadPhases::Take();
    auto text = ThreadPhases::Text(totals);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (totals.submissions == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Add(totals, ThreadPhases::Take());
        text = ThreadPhases::Text(totals);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto after = ThreadPhases::Take();
    std::cout << text << '\n';
    Require(after.submissions == 0, "a nested rewind tail was timed as a submission after the outer submission ended: " + ThreadPhases::Text(after));
    Require(std::atomic_ref<std::uint32_t>(done).load() == 1, "the last rewind tail did not run");
    Require(totals.submissions == 1, "one submission with " + std::to_string(Tails) + " nested rewind tails was timed " + std::to_string(totals.submissions) + " times: " + text);
    Require(totals.busyNs <= totals.windowNs, "the worker's busy time counts nested rewind tails more than once: " + text);
}

}

std::string BoundaryText(const AgcDriver::DriverDetail::BoundaryTotals& totals) {
    return AgcDriver::DriverDetail::BoundaryTrace::Text(totals);
}

void FoldSeparatesStarvationFromSaturation() {
    using AgcDriver::DriverDetail::BoundaryEvent;
    using AgcDriver::DriverDetail::BoundaryRecord;
    using AgcDriver::DriverDetail::BoundaryState;
    using AgcDriver::DriverDetail::BoundaryTotals;
    using AgcDriver::DriverDetail::BoundaryTrace;
    constexpr std::int64_t Ms = 1000000;
    const std::vector<BoundaryRecord> timeline{
        {0, BoundaryEvent::DrainBegin, 3, 1},
        {0, BoundaryEvent::CommitBegin},
        {1 * Ms, BoundaryEvent::CommitEnd},
        {10 * Ms, BoundaryEvent::DrainEnd},
        {12 * Ms, BoundaryEvent::CommitBegin},
        {13 * Ms, BoundaryEvent::CommitEnd},
        {20 * Ms, BoundaryEvent::CommitBegin},
        {21 * Ms, BoundaryEvent::DrainBegin, 1, 0},
        {22 * Ms, BoundaryEvent::RoomWaitBegin},
        {29 * Ms, BoundaryEvent::RoomWaitEnd},
        {30 * Ms, BoundaryEvent::DrainEnd},
        {31 * Ms, BoundaryEvent::CommitEnd},
        {40 * Ms, BoundaryEvent::CommitBegin},
        {40 * Ms, BoundaryEvent::DrainBegin, 0, 2},
        {42 * Ms, BoundaryEvent::CommitEnd},
        {44 * Ms, BoundaryEvent::DrainEnd},
    };
    BoundaryState whole;
    BoundaryTotals totals;
    auto records = timeline;
    std::reverse(records.begin(), records.end());
    BoundaryTrace::Fold(records, 50 * Ms, whole, totals);
    const auto text = BoundaryText(totals);
    std::cout << text << '\n';
    Require(totals.drains == 3 && totals.drainNs == static_cast<std::uint64_t>(10 * Ms + 9 * Ms + 4 * Ms), "the fold counted " + std::to_string(totals.drains) + " drains: " + text);
    Require(totals.starved == 1 && totals.saturated == 1 && totals.other == 1, "the fold did not find one starved, one saturated and one other boundary: " + text);
    Require(totals.idleInDrainNs == static_cast<std::uint64_t>(9 * Ms + 2 * Ms) && totals.idleAfterDrainNs == static_cast<std::uint64_t>(2 * Ms + 6 * Ms), "the fold put the wrong committer idle in and after drains: " + text);
    Require(totals.busyInDrainNs == static_cast<std::uint64_t>(1 * Ms + 9 * Ms + 2 * Ms) && totals.busyNs == static_cast<std::uint64_t>(1 * Ms + 1 * Ms + 11 * Ms + 2 * Ms), "the fold put the wrong committer busy time in drains: " + text);
    Require(totals.busyNs + totals.idleNs == static_cast<std::uint64_t>(50 * Ms), "the committer's busy and idle time do not cover the folded span: " + text);
    Require(totals.idleNs - totals.idleInDrainNs - totals.idleAfterDrainNs == static_cast<std::uint64_t>(7 * Ms + 9 * Ms), "the fold put the wrong committer idle outside drains: " + text);
    Require(totals.roomWaitNs == static_cast<std::uint64_t>(7 * Ms) && totals.roomWaitInDrainNs == static_cast<std::uint64_t>(7 * Ms), "the fold put the wrong room waits in drains: " + text);
    Require(totals.heldAtDrain == 2 && totals.emptyAtDrain == 1 && totals.finisherJobsAtDrain == 4, "the fold counted the wrong queue state at drain starts: " + text);
    Require(totals.drainLengths[2] == 1 && totals.drainLengths[3] == 2, "the fold did not put the 4 ms drain under 5 ms and the 9 and 10 ms drains at 5 ms or more: " + text);

    BoundaryState split;
    BoundaryTotals first, second;
    std::vector<BoundaryRecord> early(timeline.begin(), timeline.begin() + 9), late(timeline.begin() + 9, timeline.end());
    BoundaryTrace::Fold(early, 25 * Ms, split, first);
    BoundaryTrace::Fold(late, 50 * Ms, split, second);
    Require(first.busyNs + second.busyNs == totals.busyNs && first.idleNs + second.idleNs == totals.idleNs && first.idleInDrainNs + second.idleInDrainNs == totals.idleInDrainNs && first.idleAfterDrainNs + second.idleAfterDrainNs == totals.idleAfterDrainNs && first.busyInDrainNs + second.busyInDrainNs == totals.busyInDrainNs && first.roomWaitInDrainNs + second.roomWaitInDrainNs == totals.roomWaitInDrainNs, "a drain split across two windows folded differently: " + BoundaryText(first) + " then " + BoundaryText(second));
    Require(first.drains + second.drains == 3 && second.saturated == 1, "a drain that started in one window and ended in the next was not counted once, as saturated: " + BoundaryText(first) + " then " + BoundaryText(second));
}

void AddBoundaries(AgcDriver::DriverDetail::BoundaryTotals& totals, const AgcDriver::DriverDetail::BoundaryTotals& more) {
    totals.windowNs += more.windowNs;
    totals.drains += more.drains;
    totals.commits += more.commits;
    totals.busyNs += more.busyNs;
    totals.idleNs += more.idleNs;
    totals.dropped += more.dropped;
}

void LiveDrainsReachTheTrace(bool pipelined) {
    using AgcDriver::DriverDetail::BoundaryTrace;
    static_cast<void>(BoundaryTrace::Take());
    auto commands = Draws(8, 24.0f);
    Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
    Require(sceAgcDriverSubmitDcb(&packet) == 0, "the boundary trace submission failed");
    AgcDriverWaitIdle_nid_postfix();
    auto totals = BoundaryTrace::Take();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (totals.drains == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        AddBoundaries(totals, BoundaryTrace::Take());
    }
    const auto text = BoundaryText(totals);
    std::cout << text << '\n';
    Require(totals.drains >= 1, "a queue 0 submission's finisher drain did not reach the boundary trace: " + text);
    Require(!pipelined || totals.commits >= 8, "the committer's items did not reach the boundary trace: " + text);
    Require(totals.busyNs + totals.idleNs == totals.windowNs, "the committer's busy and idle time do not cover the window: " + text);
    Require(totals.dropped == 0, "the boundary trace dropped events: " + text);
}

int main() {
    try {
        FoldSeparatesStarvationFromSaturation();
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
            const auto boundaries = AgcDriver::DriverDetail::BoundaryTrace::Take();
            Require(boundaries.drains == 0 && boundaries.commits == 0, "boundary events were traced without their gate: " + BoundaryText(boundaries));
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
        RewindTailsCountOnce();
        LiveDrainsReachTheTrace(AgcDriver::DriverDetail::DrawPipeline::Active());
        AgcDriverShutdown_nid_postfix();
        std::cout << "draw thread phases accounted " << (finisher ? "across the worker and the finisher, queued jobs included" : "on the worker") << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
