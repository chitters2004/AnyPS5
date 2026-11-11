#include "prx/libSceAgcDriver/Graphics/include/CommitPhases.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace AgcDriver::Graphics {

namespace {

constexpr std::array<const char*, static_cast<std::size_t>(CommitPhase::Count)> PhaseNames{"other", "plan", "inputs", "targets", "resolve", "revalidate", "revalidate epoch", "revalidate pending", "revalidate fast", "revalidate walk", "classify", "record", "bindings", "pass access", "effects", "plan.store", "plan.fb", "inputs.collect", "inputs.local", "targets.lru", "resolve.cache", "resolve.alias", "record.local", "pass access.local", "epilogue.writers", "epilogue.recipe"};
static_assert(PhaseNames.back() != nullptr);

using Clock = std::chrono::steady_clock;

struct ThreadPhases {
    bool active = false;
    CommitPhase current = CommitPhase::Other;
    Clock::time_point mark{};
};

thread_local ThreadPhases threadPhases;
std::atomic<std::uint64_t> drawCount{0};
std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(CommitPhase::Count)> phaseNs{};
std::atomic<std::int64_t> reportedAt{0};
std::mutex reportMutex;
CommitPhaseTotals reported;

void charge(ThreadPhases& phases, Clock::time_point now) {
    const auto spent = std::chrono::duration_cast<std::chrono::nanoseconds>(now - phases.mark).count();
    if (spent > 0) phaseNs[static_cast<std::size_t>(phases.current)].fetch_add(static_cast<std::uint64_t>(spent), std::memory_order_relaxed);
    phases.mark = now;
}

void reportDue(Clock::time_point now) {
    const auto window = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count() / 10;
    auto previous = reportedAt.load(std::memory_order_relaxed);
    if (previous == window) return;
    if (previous == 0) {
        reportedAt.compare_exchange_strong(previous, window, std::memory_order_relaxed);
        return;
    }
    if (!reportedAt.compare_exchange_strong(previous, window, std::memory_order_relaxed)) return;
    CommitPhaseTotals totals;
    {
        std::lock_guard lock(reportMutex);
        const auto now = ReadCommitPhases();
        totals = now - reported;
        reported = now;
    }
    if (totals.draws == 0) return;
    std::uint64_t all = 0;
    for (const auto ns : totals.ns) all += ns;
    std::string text;
    for (std::size_t phase = 0; phase < totals.ns.size(); ++phase) {
        if (totals.ns[phase] == 0) continue;
        char item[80];
        std::snprintf(item, sizeof(item), " %s %.2f", PhaseNames[phase], static_cast<double>(totals.ns[phase]) / 1e3 / static_cast<double>(totals.draws));
        text += item;
    }
    std::fprintf(stderr, "[commit-phases] (10 s) %llu draws, %.2f us each; us per draw by phase:%s\n", static_cast<unsigned long long>(totals.draws), static_cast<double>(all) / 1e3 / static_cast<double>(totals.draws), text.c_str());
}

}

bool CommitPhasesEnabled() {
    static const bool enabled = std::getenv("APS5_PROFILE_COMMIT") != nullptr;
    return enabled;
}

const char* CommitPhaseName(CommitPhase phase) {
    return PhaseNames[static_cast<std::size_t>(phase)];
}

void BeginCommitPhase(CommitPhase phase) {
    auto& phases = threadPhases;
    if (!phases.active) return;
    charge(phases, Clock::now());
    phases.current = phase;
}

CommitPhaseTotals ReadCommitPhases() {
    CommitPhaseTotals totals;
    totals.draws = drawCount.load(std::memory_order_relaxed);
    for (std::size_t phase = 0; phase < totals.ns.size(); ++phase) totals.ns[phase] = phaseNs[phase].load(std::memory_order_relaxed);
    return totals;
}

CommitPhaseTotals operator-(const CommitPhaseTotals& later, const CommitPhaseTotals& earlier) {
    CommitPhaseTotals difference;
    difference.draws = later.draws - earlier.draws;
    for (std::size_t phase = 0; phase < difference.ns.size(); ++phase) difference.ns[phase] = later.ns[phase] - earlier.ns[phase];
    return difference;
}

CommitPhaseDraw::CommitPhaseDraw() {
    auto& phases = threadPhases;
    if (phases.active) {
        nested = true;
        previous = phases.current;
        BeginCommitPhase(CommitPhase::Other);
        return;
    }
    if (!CommitPhasesEnabled()) return;
    owner = true;
    phases.active = true;
    phases.current = CommitPhase::Other;
    phases.mark = Clock::now();
}

CommitPhaseDraw::~CommitPhaseDraw() {
    if (nested) BeginCommitPhase(previous);
    if (!owner) return;
    auto& phases = threadPhases;
    const auto now = Clock::now();
    charge(phases, now);
    phases.active = false;
    drawCount.fetch_add(1, std::memory_order_relaxed);
    reportDue(now);
}

CommitPhaseScope::CommitPhaseScope(CommitPhase phase) {
    auto& phases = threadPhases;
    if (!phases.active) return;
    active = true;
    previous = phases.current;
    BeginCommitPhase(phase);
}

CommitPhaseScope::~CommitPhaseScope() {
    if (active) BeginCommitPhase(previous);
}

}
