#include "prx/libSceAgcDriver/Graphics/include/CommitPhases.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <algorithm>
#include <string>
#include <string_view>

namespace AgcDriver::Graphics {

namespace {

constexpr std::array<const char*, static_cast<std::size_t>(CommitPhase::Count)> PhaseNames{"other", "plan", "inputs", "targets", "resolve", "revalidate", "revalidate epoch", "revalidate pending", "revalidate fast", "revalidate walk", "classify", "record", "bindings", "pass access", "effects", "plan.store", "plan.fb", "inputs.collect", "inputs.local", "targets.lru", "resolve.cache", "resolve.alias", "record.local", "pass access.local", "epilogue.writers", "epilogue.recipe", "plan.key", "targets.claims", "targets.lookup", "targets.refresh", "targets.clearcheck", "targets.view", "targets.settle", "targets.msaa", "targets.proved", "targets.cmask", "targets.depthprep", "resolve.setup", "resolve.space", "resolve.moved", "resolve.build", "resolve.evict"};
static_assert(PhaseNames.back() != nullptr);

constexpr std::array<const char*, static_cast<std::size_t>(CommitPhase::Count)> PhaseGroups{"other", "plan", "inputs", "targets", "resolve", "revalidate", "revalidate", "revalidate", "revalidate", "revalidate", "classify", "record", "bindings", "pass access", "effects", "plan", "plan", "inputs", "inputs", "targets", "resolve", "resolve", "record", "pass access", "epilogue", "epilogue", "plan", "targets", "targets", "targets", "targets", "targets", "targets", "targets", "targets", "targets", "targets", "resolve", "resolve", "resolve", "resolve", "resolve"};
static_assert(PhaseGroups.back() != nullptr);

constexpr std::array<const char*, 12> GroupOrder{"other", "plan", "inputs", "targets", "resolve", "revalidate", "classify", "record", "bindings", "pass access", "effects", "epilogue"};

using Clock = std::chrono::steady_clock;

struct ThreadPhases {
    bool active = false;
    CommitPhase current = CommitPhase::Other;
    Clock::time_point mark{};
};

thread_local ThreadPhases threadPhases;
std::atomic<std::uint64_t> drawCount{0};
std::atomic<std::uint64_t> keyCount{0};
std::atomic<std::uint64_t> keyWordCount{0};
std::atomic<std::uint64_t> keyMemoHitCount{0};
std::atomic<std::uint64_t> keyMemoMissCount{0};
std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(CommitCounter::Count)> counterValues{};
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
    std::string groups;
    for (const auto* group : GroupOrder) {
        std::uint64_t ns = 0;
        for (std::size_t phase = 0; phase < totals.ns.size(); ++phase) {
            if (std::string_view(PhaseGroups[phase]) == group) ns += totals.ns[phase];
        }
        char item[80];
        std::snprintf(item, sizeof(item), " %s %.2f", group, static_cast<double>(ns) / 1e3 / static_cast<double>(totals.draws));
        groups += item;
    }
    const auto count = [&](CommitCounter counter) { return static_cast<double>(totals.counters[static_cast<std::size_t>(counter)]); };
    const auto perDraw = [&](CommitCounter counter) { return count(counter) / static_cast<double>(totals.draws); };
    const auto targetDraws = std::max(1.0, count(CommitCounter::TargetDraws));
    std::fprintf(stderr, "[commit-phases] (10 s) %llu draws, %.2f us each; us per draw by phase:%s; by group (phases and their sub-phases, summing to the total):%s; %llu content keys of %.0f words on average, stage key memo %llu hits / %llu misses\n", static_cast<unsigned long long>(totals.draws), static_cast<double>(all) / 1e3 / static_cast<double>(totals.draws), text.c_str(), groups.c_str(), static_cast<unsigned long long>(totals.keys), totals.keys != 0 ? static_cast<double>(totals.keyWords) / static_cast<double>(totals.keys) : 0.0, static_cast<unsigned long long>(totals.keyMemoHits), static_cast<unsigned long long>(totals.keyMemoMisses));
    std::fprintf(stderr, "[commit-targets] (10 s) %.0f draws with targets, %.1f%% by recipe; per such draw %.2f color and %.2f depth attachments, %.1f%% on the previous draw's targets; target refreshes per draw: %.2f epoch hits, %.2f proved, %.2f slow; epoch misses per draw: %.2f no epoch, %.2f epoch moved, %.2f proof serial moved, %.2f pending serial moved; other refreshes per draw: %.2f epoch hits, %.2f proved, %.2f slow\n", count(CommitCounter::TargetDraws), 100.0 * count(CommitCounter::RecipeTargetDraws) / targetDraws, count(CommitCounter::ColorTargets) / targetDraws, count(CommitCounter::DepthTargets) / targetDraws, 100.0 * count(CommitCounter::RepeatedTargets) / targetDraws, perDraw(CommitCounter::TargetRefreshEpoch), perDraw(CommitCounter::TargetRefreshProved), perDraw(CommitCounter::TargetRefreshSlow), perDraw(CommitCounter::TargetEpochOff), perDraw(CommitCounter::TargetEpochMoved), perDraw(CommitCounter::TargetEpochProofMoved), perDraw(CommitCounter::TargetEpochPendingMoved), perDraw(CommitCounter::OtherRefreshEpoch), perDraw(CommitCounter::OtherRefreshProved), perDraw(CommitCounter::OtherRefreshSlow));
    const auto resolveDraws = std::max(1.0, count(CommitCounter::ResolveDraws));
    std::fprintf(stderr, "[commit-resolve] (10 s) %.0f draws resolved their resources (%.1f%% of all draws); of those %.1f%% keyed, %.1f%% found a template, %.1f%% checked its address space, %.1f%% revalidated it, %.1f%% hit (%.1f%% with moved buffers), %.1f%% built anew, %.1f%% invalidated a template, %.1f%% with address-based stages\n", count(CommitCounter::ResolveDraws), 100.0 * count(CommitCounter::ResolveDraws) / static_cast<double>(totals.draws), 100.0 * count(CommitCounter::ResolveCacheable) / resolveDraws, 100.0 * count(CommitCounter::ResolveFound) / resolveDraws, 100.0 * count(CommitCounter::ResolveSpaceChecks) / resolveDraws, 100.0 * count(CommitCounter::ResolveValid) / resolveDraws, 100.0 * count(CommitCounter::ResolveHits) / resolveDraws, 100.0 * count(CommitCounter::ResolveMovedHits) / resolveDraws, 100.0 * count(CommitCounter::ResolveBuilds) / resolveDraws, 100.0 * count(CommitCounter::ResolveInvalidated) / resolveDraws, 100.0 * count(CommitCounter::ResolveAddressStages) / resolveDraws);
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

void NoteCommitKey(std::size_t words) {
    if (!threadPhases.active) return;
    keyCount.fetch_add(1, std::memory_order_relaxed);
    keyWordCount.fetch_add(words, std::memory_order_relaxed);
}

void NoteCommitKeyMemo(bool hit) {
    if (!threadPhases.active) return;
    (hit ? keyMemoHitCount : keyMemoMissCount).fetch_add(1, std::memory_order_relaxed);
}

void CountCommit(CommitCounter counter, std::uint64_t amount) {
    if (!threadPhases.active) return;
    counterValues[static_cast<std::size_t>(counter)].fetch_add(amount, std::memory_order_relaxed);
}

CommitPhase CurrentCommitPhase() {
    const auto& phases = threadPhases;
    return phases.active ? phases.current : CommitPhase::Other;
}

void RelabelCommitPhase(CommitPhase phase) {
    auto& phases = threadPhases;
    if (phases.active) phases.current = phase;
}

CommitPhaseTotals ReadCommitPhases() {
    CommitPhaseTotals totals;
    totals.draws = drawCount.load(std::memory_order_relaxed);
    totals.keys = keyCount.load(std::memory_order_relaxed);
    totals.keyWords = keyWordCount.load(std::memory_order_relaxed);
    totals.keyMemoHits = keyMemoHitCount.load(std::memory_order_relaxed);
    totals.keyMemoMisses = keyMemoMissCount.load(std::memory_order_relaxed);
    for (std::size_t counter = 0; counter < totals.counters.size(); ++counter) totals.counters[counter] = counterValues[counter].load(std::memory_order_relaxed);
    for (std::size_t phase = 0; phase < totals.ns.size(); ++phase) totals.ns[phase] = phaseNs[phase].load(std::memory_order_relaxed);
    return totals;
}

CommitPhaseTotals operator-(const CommitPhaseTotals& later, const CommitPhaseTotals& earlier) {
    CommitPhaseTotals difference;
    difference.draws = later.draws - earlier.draws;
    difference.keys = later.keys - earlier.keys;
    difference.keyWords = later.keyWords - earlier.keyWords;
    difference.keyMemoHits = later.keyMemoHits - earlier.keyMemoHits;
    difference.keyMemoMisses = later.keyMemoMisses - earlier.keyMemoMisses;
    for (std::size_t counter = 0; counter < difference.counters.size(); ++counter) difference.counters[counter] = later.counters[counter] - earlier.counters[counter];
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
