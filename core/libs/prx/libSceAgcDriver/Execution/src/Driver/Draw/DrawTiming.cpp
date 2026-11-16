#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawTiming.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace AgcDriver::DriverDetail {

PendingDrawPhases& Driver::pendingDrawPhases() {
    static thread_local PendingDrawPhases pending;
    return pending;
}

void Driver::addDrawPhases(const std::array<double, DrawDriverPhaseCount>& ms, bool drawn, std::uint64_t captures) {
    std::lock_guard lock(drawPhasesMutex);
    auto& totals = drawPhaseTotals;
    for (std::size_t i = 0; i < DrawDriverPhaseCount; ++i) totals.ms[i] += ms[i];
    ++totals.packets;
    if (drawn) ++totals.drawn;
    totals.captures += captures;
    const auto now = std::chrono::steady_clock::now();
    if (now - totals.lastReport < std::chrono::seconds(10)) return;
    totals.lastReport = now;
    std::string report;
    double total = 0;
    for (std::size_t i = 0; i < DrawDriverPhaseCount; ++i) {
        char text[64];
        std::snprintf(text, sizeof(text), " %s %.1f", DrawDriverPhaseNames[i], totals.ms[i] * 1000 / static_cast<double>(totals.packets));
        report += text;
        total += totals.ms[i];
    }
    AgcDriver::ProfilePrint_nid_no_patch( "[draw] driver phases (10 s, %llu draw packets, %llu drawn, %llu captures), us per packet:%s, total %.1f (%.1f ms; Graphics::Draw %.1f ms, skipped %.1f ms)\n", static_cast<unsigned long long>(totals.packets), static_cast<unsigned long long>(totals.drawn), static_cast<unsigned long long>(totals.captures), report.c_str(), total * 1000 / static_cast<double>(totals.packets), total, totals.ms[DrawRowGraphics], totals.ms[DrawRowSkipped]);
    totals.ms = {};
    totals.packets = totals.drawn = totals.captures = 0;
}

void DrawPhaseTiming::Phase(DrawDriverPhase which) {
    if (!profile) return;
    const auto now = std::chrono::steady_clock::now();
    phaseMs[which] += std::chrono::duration<double, std::milli>(now - phaseLap).count();
    if (ThreadPhases::Enabled()) ThreadPhases::AddPhase(which, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - phaseLap).count()));
    phaseLap = now;
}

namespace {

thread_local PhaseThread phaseRole = PhaseThread::Other;
std::array<std::array<std::atomic<std::uint64_t>, ThreadPhaseTotals::Groups>, ThreadPhaseTotals::Threads> phaseGroupNs{};
std::array<std::atomic<std::uint64_t>, ThreadPhaseTotals::Threads> phaseDrawNs{}, phaseDraws{};
std::atomic<std::uint64_t> phaseBusyNs{0}, phaseSubmissions{0};
std::atomic<std::int64_t> phaseWindowStart{std::chrono::steady_clock::now().time_since_epoch().count()};

std::size_t groupOf(DrawDriverPhase which) {
    switch (which) {
    case DrawRowDecode:
    case DrawRowProgramPrepare:
    case DrawRowCapture:
    case DrawRowCaptureHookWaits:
    case DrawRowRecompile:
        return static_cast<std::size_t>(PhaseGroup::Prepare);
    case DrawRowKeyLookupValidate:
    case DrawRowValidateWait:
        return static_cast<std::size_t>(PhaseGroup::Lookup);
    case DrawRowLockWait:
        return static_cast<std::size_t>(PhaseGroup::Handoff);
    case DrawRowGraphics:
        return static_cast<std::size_t>(PhaseGroup::Graphics);
    default:
        return ThreadPhaseTotals::Groups;
    }
}

}

bool ThreadPhases::Enabled() {
    static const bool enabled = std::getenv("APS5_DRAW_THREAD_TIMES") != nullptr || std::getenv("APS5_PIPELINE_STATS") != nullptr || std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return enabled;
}

PhaseThread ThreadPhases::Current() {
    return phaseRole;
}

ThreadPhases::Role::Role(PhaseThread role) : previous(phaseRole) {
    phaseRole = role;
}

ThreadPhases::Role::~Role() {
    phaseRole = previous;
}

void ThreadPhases::AddPhase(DrawDriverPhase which, std::uint64_t ns) {
    const auto group = groupOf(which);
    if (group == ThreadPhaseTotals::Groups) return;
    phaseGroupNs[static_cast<std::size_t>(phaseRole)][group].fetch_add(ns, std::memory_order_relaxed);
}

void ThreadPhases::AddDraw(std::uint64_t ns) {
    const auto role = static_cast<std::size_t>(phaseRole);
    phaseDrawNs[role].fetch_add(ns, std::memory_order_relaxed);
    phaseDraws[role].fetch_add(1, std::memory_order_relaxed);
}

void ThreadPhases::AddSubmission(std::uint64_t ns) {
    phaseBusyNs.fetch_add(ns, std::memory_order_relaxed);
    phaseSubmissions.fetch_add(1, std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    if (std::chrono::steady_clock::duration(now - phaseWindowStart.load(std::memory_order_relaxed)) < std::chrono::seconds(10)) return;
    AgcDriver::ProfilePrint_nid_no_patch("%s\n", Text(Take()).c_str());
}

ThreadPhaseTotals ThreadPhases::Take() {
    ThreadPhaseTotals totals;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    totals.windowNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::duration(now - phaseWindowStart.exchange(now, std::memory_order_relaxed))).count());
    for (std::size_t thread = 0; thread < ThreadPhaseTotals::Threads; ++thread) {
        for (std::size_t group = 0; group < ThreadPhaseTotals::Groups; ++group) totals.groupNs[thread][group] = phaseGroupNs[thread][group].exchange(0, std::memory_order_relaxed);
        totals.drawNs[thread] = phaseDrawNs[thread].exchange(0, std::memory_order_relaxed);
        totals.draws[thread] = phaseDraws[thread].exchange(0, std::memory_order_relaxed);
    }
    totals.busyNs = phaseBusyNs.exchange(0, std::memory_order_relaxed);
    totals.submissions = phaseSubmissions.exchange(0, std::memory_order_relaxed);
    return totals;
}

std::string ThreadPhases::Text(const ThreadPhaseTotals& totals) {
    const auto ms = [](std::uint64_t ns) { return static_cast<double>(ns) / 1e6; };
    const auto signedMs = [](std::uint64_t whole, std::uint64_t part) { return (static_cast<double>(whole) - static_cast<double>(part)) / 1e6; };
    const auto draws = [&](PhaseThread thread, const char* noun) {
        const auto index = static_cast<std::size_t>(thread);
        const auto& groups = totals.groupNs[index];
        const auto grouped = groups[0] + groups[1] + groups[2] + groups[3];
        char text[224];
        std::snprintf(text, sizeof(text), "%llu %s in %.1f ms (prepare %.1f, lookup %.1f, lock and commit hand-off %.1f, Graphics::Draw %.1f, rest %.1f)", static_cast<unsigned long long>(totals.draws[index]), noun, ms(totals.drawNs[index]), ms(groups[0]), ms(groups[1]), ms(groups[2]), ms(groups[3]), signedMs(totals.drawNs[index], grouped));
        return std::string(text);
    };
    const auto worker = static_cast<std::size_t>(PhaseThread::Worker);
    char head[160];
    std::snprintf(head, sizeof(head), "[draw-threads] (%.1f s): queue 0 worker busy %.1f ms in %llu submissions, idle %.1f ms; ", static_cast<double>(totals.windowNs) / 1e9, ms(totals.busyNs), static_cast<unsigned long long>(totals.submissions), signedMs(totals.windowNs, totals.busyNs));
    char packets[64];
    std::snprintf(packets, sizeof(packets), ", other packets %.1f ms", signedMs(totals.busyNs, totals.drawNs[worker]));
    return head + draws(PhaseThread::Worker, "draws") + packets + "; finisher " + draws(PhaseThread::Finisher, "jobs") + "; other queues " + draws(PhaseThread::Other, "draws");
}

}
