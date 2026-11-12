#include "prx/libSceAgcDriver/Graphics/include/CommitPhases.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileCounter.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <algorithm>
#include <bit>
#include <string>
#include <string_view>

namespace AgcDriver::Graphics {

namespace {

constexpr std::array<const char*, static_cast<std::size_t>(CommitPhase::Count)> PhaseNames{"other", "plan", "inputs", "targets", "resolve", "revalidate", "revalidate epoch", "revalidate pending", "revalidate fast", "revalidate walk", "classify", "record", "bindings", "pass access", "effects", "plan.store", "plan.fb", "inputs.collect", "inputs.local", "targets.lru", "resolve.cache", "resolve.alias", "record.local", "pass access.local", "epilogue.writers", "epilogue.recipe", "plan.key", "targets.claims", "targets.lookup", "targets.refresh", "targets.clearcheck", "targets.view", "targets.settle", "targets.msaa", "targets.proved", "targets.cmask", "targets.depthprep", "targets.depth.claims", "targets.depth.clear", "targets.depth.find", "targets.depth.sync", "targets.depth.stale", "plan.hit.find", "plan.hit.words", "plan.hit.instance", "plan.hit.targets", "plan.hit.rebind", "resolve.setup", "resolve.space", "resolve.moved", "resolve.build", "resolve.evict", "targets.lookup.pure", "targets.lookup.guards", "targets.lookup.cache", "inputs.validate", "inputs.flush", "inputs.inplace", "inputs.reuse", "inputs.arm", "inputs.alloc", "inputs.keep", "inputs.indexrange", "inputs.expand", "inputs.layout", "record.setup", "record.pass", "record.begin", "record.bind", "record.push", "record.commands", "record.leave", "record.after", "record.recipe", "record.stores", "record.passkey", "record.begin.continue", "record.begin.proxies", "record.begin.barriers", "record.begin.lds", "record.begin.arguments", "record.begin.order", "record.begin.staged", "record.begin.pipeline", "record.begin.bind", "record.begin.dynamic", "record.begin.library", "revalidate certified", "targets.memo"};
static_assert(PhaseNames.back() != nullptr);

constexpr std::array<const char*, static_cast<std::size_t>(CommitPhase::Count)> PhaseGroups{"other", "plan", "inputs", "targets", "resolve", "revalidate", "revalidate", "revalidate", "revalidate", "revalidate", "classify", "record", "bindings", "pass access", "effects", "plan", "plan", "inputs", "inputs", "targets", "resolve", "resolve", "record", "pass access", "epilogue", "epilogue", "plan", "targets", "targets", "targets", "targets", "targets", "targets", "targets", "targets", "targets", "targets", "targets", "targets", "targets", "targets", "targets", "plan.hit", "plan.hit", "plan.hit", "plan.hit", "plan.hit", "resolve", "resolve", "resolve", "resolve", "resolve", "targets", "targets", "targets", "inputs", "inputs", "inputs", "inputs", "inputs", "inputs", "inputs", "inputs", "inputs", "inputs", "record", "record", "record", "record", "record", "record", "record", "record", "record", "record", "record", "record", "record", "record", "record", "record", "record", "record", "record", "record", "record", "record", "revalidate", "targets"};
static_assert(PhaseGroups.back() != nullptr);

constexpr std::array<const char*, 13> GroupOrder{"other", "plan", "plan.hit", "inputs", "targets", "resolve", "revalidate", "classify", "record", "bindings", "pass access", "effects", "epilogue"};

using Clock = std::chrono::steady_clock;

constexpr std::size_t PhaseCount = static_cast<std::size_t>(CommitPhase::Count);
constexpr std::size_t KindCount = static_cast<std::size_t>(CommitDrawKind::Count);
constexpr std::array<const char*, KindCount> KindNames{"full", "recipe hit", "plan hit"};

struct ThreadPhases {
    bool active = false;
    CommitPhase current = CommitPhase::Other;
    Clock::time_point mark{};
    CommitDrawKind kind = CommitDrawKind::Full;
    std::array<std::uint64_t, PhaseCount> drawNs{};
    unsigned shadow = 0;
    bool recording = false;
};

thread_local ThreadPhases threadPhases;
thread_local unsigned threadEpochCauses = 0;
ProfileCounter drawCount;
ProfileCounter keyCount;
ProfileCounter keyWordCount;
ProfileCounter keyMemoHitCount;
ProfileCounter keyMemoMissCount;
std::array<ProfileCounter, static_cast<std::size_t>(CommitCounter::Count)> counterValues{};
std::array<ProfileCounter, static_cast<std::size_t>(CommitPhase::Count)> phaseNs{};
std::array<ProfileCounter, KindCount> kindDrawCount{};
std::array<std::array<ProfileCounter, PhaseCount>, KindCount> kindPhaseNs{};
constexpr std::size_t ShadowCount = static_cast<std::size_t>(ShadowReason::Count);
constexpr std::array<const char*, ShadowCount> ShadowNames{"template built", "address-based", "CPU indirect", "pre-op in resolve", "template walk", "target memo miss", "slow target refresh", "input copied", "store flush", "special pass", "waited"};
std::array<ProfileCounter, KindCount> shadowEligibleCount{};
std::array<ProfileCounter, ShadowCount> shadowFirstCount{};
std::array<ProfileCounter, ShadowCount> shadowAnyCount{};

void markShadow(ThreadPhases& phases, ShadowReason reason) {
    phases.shadow |= 1u << static_cast<unsigned>(reason);
}
std::atomic<std::int64_t> reportedAt{0};
std::mutex reportMutex;
CommitPhaseTotals reported;

void charge(ThreadPhases& phases, Clock::time_point now) {
    const auto spent = std::chrono::duration_cast<std::chrono::nanoseconds>(now - phases.mark).count();
    if (spent > 0) {
        phaseNs[static_cast<std::size_t>(phases.current)].Add(static_cast<std::uint64_t>(spent));
        phases.drawNs[static_cast<std::size_t>(phases.current)] += static_cast<std::uint64_t>(spent);
    }
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
    std::string kinds;
    for (std::size_t kind = 0; kind < KindCount; ++kind) {
        const auto draws = totals.kindDraws[kind];
        if (draws == 0) continue;
        std::uint64_t kindAll = 0;
        for (const auto ns : totals.kindNs[kind]) kindAll += ns;
        char item[96];
        std::snprintf(item, sizeof(item), "%s%s: %llu draws, %.2f us each, by group:", kinds.empty() ? " " : "; ", KindNames[kind], static_cast<unsigned long long>(draws), static_cast<double>(kindAll) / 1e3 / static_cast<double>(draws));
        kinds += item;
        for (const auto* group : GroupOrder) {
            std::uint64_t ns = 0;
            for (std::size_t phase = 0; phase < PhaseCount; ++phase) {
                if (std::string_view(PhaseGroups[phase]) == group) ns += totals.kindNs[kind][phase];
            }
            if (ns == 0) continue;
            std::snprintf(item, sizeof(item), " %s %.2f", group, static_cast<double>(ns) / 1e3 / static_cast<double>(draws));
            kinds += item;
        }
    }
    std::fprintf(stderr, "[commit-phases-by-kind] (10 s) us per draw of each kind, by group (phases and their sub-phases, summing to that kind's total):%s\n", kinds.c_str());
    const auto count = [&](CommitCounter counter) { return static_cast<double>(totals.counters[static_cast<std::size_t>(counter)]); };
    const auto perDraw = [&](CommitCounter counter) { return count(counter) / static_cast<double>(totals.draws); };
    const auto targetDraws = std::max(1.0, count(CommitCounter::TargetDraws));
    std::fprintf(stderr, "[commit-phases] (10 s) %llu draws, %.2f us each; us per draw by phase:%s; by group (phases and their sub-phases, summing to the total):%s; %llu content keys of %.0f words on average, stage key memo %llu hits / %llu misses\n", static_cast<unsigned long long>(totals.draws), static_cast<double>(all) / 1e3 / static_cast<double>(totals.draws), text.c_str(), groups.c_str(), static_cast<unsigned long long>(totals.keys), totals.keys != 0 ? static_cast<double>(totals.keyWords) / static_cast<double>(totals.keys) : 0.0, static_cast<unsigned long long>(totals.keyMemoHits), static_cast<unsigned long long>(totals.keyMemoMisses));
    std::fprintf(stderr, "[commit-targets] (10 s) %.0f draws with targets, %.1f%% by recipe; per such draw %.2f color and %.2f depth attachments, %.1f%% on the previous draw's targets; target refreshes per draw: %.2f epoch hits, %.2f proved, %.2f slow; epoch misses per draw: %.2f no epoch, %.2f epoch moved, %.2f proof serial moved, %.2f pending serial moved; other refreshes per draw: %.2f epoch hits, %.2f proved, %.2f slow\n", count(CommitCounter::TargetDraws), 100.0 * count(CommitCounter::RecipeTargetDraws) / targetDraws, count(CommitCounter::ColorTargets) / targetDraws, count(CommitCounter::DepthTargets) / targetDraws, 100.0 * count(CommitCounter::RepeatedTargets) / targetDraws, perDraw(CommitCounter::TargetRefreshEpoch), perDraw(CommitCounter::TargetRefreshProved), perDraw(CommitCounter::TargetRefreshSlow), perDraw(CommitCounter::TargetEpochOff), perDraw(CommitCounter::TargetEpochMoved), perDraw(CommitCounter::TargetEpochProofMoved), perDraw(CommitCounter::TargetEpochPendingMoved), perDraw(CommitCounter::OtherRefreshEpoch), perDraw(CommitCounter::OtherRefreshProved), perDraw(CommitCounter::OtherRefreshSlow));
    const auto inputDraws = std::max(1.0, count(CommitCounter::InputDraws));
    const auto indexed = std::max(1.0, count(CommitCounter::InputIndexed));
    std::fprintf(stderr, "[commit-inputs] (10 s) %.0f draws prepared their inputs, %.1f%% indexed; per indexed draw: %.3f in place (%.3f with the highest index known), copies %.3f fresh, %.3f snapshot reused, %.3f owned reused, %.3f armed; %.3f expanded, %.3f restart compacted, %.3f clamped; per draw: %.2f vertex ranges, %.2f in place, copies %.2f fresh, %.2f snapshot reused, %.2f owned reused, %.2f armed; %.2f zero-padded fetches (%.2f reused), %.2f zeroed attributes\n", count(CommitCounter::InputDraws), 100.0 * count(CommitCounter::InputIndexed) / inputDraws, count(CommitCounter::InputIndexInPlace) / indexed, count(CommitCounter::InputIndexRangeKnown) / indexed, count(CommitCounter::InputIndexCopyFresh) / indexed, count(CommitCounter::InputIndexCopySnapshot) / indexed, count(CommitCounter::InputIndexCopyOwned) / indexed, count(CommitCounter::InputIndexCopyArmed) / indexed, count(CommitCounter::InputIndexExpanded) / indexed, count(CommitCounter::InputIndexCompacted) / indexed, count(CommitCounter::InputIndexClamped) / indexed, count(CommitCounter::InputVertexRanges) / inputDraws, count(CommitCounter::InputVertexInPlace) / inputDraws, count(CommitCounter::InputVertexCopyFresh) / inputDraws, count(CommitCounter::InputVertexCopySnapshot) / inputDraws, count(CommitCounter::InputVertexCopyOwned) / inputDraws, count(CommitCounter::InputVertexCopyArmed) / inputDraws, count(CommitCounter::InputVertexPadded) / inputDraws, count(CommitCounter::InputVertexPaddedReused) / inputDraws, count(CommitCounter::InputVertexZeroed) / inputDraws);
    const auto depthPreps = std::max(1.0, count(CommitCounter::DepthPreps));
    std::fprintf(stderr, "[commit-target-causes] (10 s) epoch moved per draw by the ordering points since the committing thread's previous epoch: %.3f submission start, %.3f WAIT_REG_MEM, %.3f drain, %.3f reap, %.3f packet, %.3f several kinds, %.3f unknown; target refresh proofs refused per draw: %.3f no proof, %.3f image generation moved, %.3f GPU lock not held, %.3f collect refused, %.3f DCC keys, %.3f memory changed, %.3f other pending results; depth preps %.0f: %.1f%% on the previous draw's surface, %.1f%% created, %.1f%% after a clear, %.1f%% mirrored, %.1f%% scanned other surfaces for stale mirrors, %.1f%% found as the previous draw's surface without a scan; committing threads followed the epoch token %.0f times (%.0f to an older token than the one followed before, %.0f older tokens kept the newer epoch; with a submission start %.0f, WAIT_REG_MEM %.0f, drain %.0f, reap %.0f, packet %.0f, no new ordering point %.0f); epoch-moved target misses per draw %.3f never stamped, %.3f stamped by another thread, %.3f stamped in an earlier epoch of this thread; target refresh exits stamped with the epoch per draw %.3f; target epoch misses per draw %.3f first for that image in the epoch; slow target refreshes per draw %.3f again for an image already slow-refreshed in the epoch\n", perDraw(CommitCounter::TargetEpochBySubmission), perDraw(CommitCounter::TargetEpochByWait), perDraw(CommitCounter::TargetEpochByDrain), perDraw(CommitCounter::TargetEpochByReap), perDraw(CommitCounter::TargetEpochByPacket), perDraw(CommitCounter::TargetEpochBySeveral), perDraw(CommitCounter::TargetEpochByUnknown), perDraw(CommitCounter::TargetProofNone), perDraw(CommitCounter::TargetProofGeneration), perDraw(CommitCounter::TargetProofUnlocked), perDraw(CommitCounter::TargetProofRefused), perDraw(CommitCounter::TargetProofKeys), perDraw(CommitCounter::TargetProofChanged), perDraw(CommitCounter::TargetProofPending), count(CommitCounter::DepthPreps), 100.0 * count(CommitCounter::DepthSameSurface) / depthPreps, 100.0 * count(CommitCounter::DepthCreated) / depthPreps, 100.0 * count(CommitCounter::DepthCleared) / depthPreps, 100.0 * count(CommitCounter::DepthMirrored) / depthPreps, 100.0 * count(CommitCounter::DepthStaleScans) / depthPreps, 100.0 * count(CommitCounter::DepthFoundRepeated) / depthPreps, count(CommitCounter::EpochFollows), count(CommitCounter::EpochFollowsBackward), count(CommitCounter::EpochFollowsSkipped), count(CommitCounter::EpochFollowsBySubmission), count(CommitCounter::EpochFollowsByWait), count(CommitCounter::EpochFollowsByDrain), count(CommitCounter::EpochFollowsByReap), count(CommitCounter::EpochFollowsByPacket), count(CommitCounter::EpochFollowsByNone), perDraw(CommitCounter::TargetEpochNeverStamped), perDraw(CommitCounter::TargetEpochOtherThread), perDraw(CommitCounter::TargetEpochThisThread), perDraw(CommitCounter::TargetEpochExitStamps), perDraw(CommitCounter::TargetMissFirstInEpoch), perDraw(CommitCounter::TargetSlowAgainInEpoch));
    const auto resolveDraws = std::max(1.0, count(CommitCounter::ResolveDraws));
    std::fprintf(stderr, "[commit-resolve] (10 s) %.0f draws resolved their resources (%.1f%% of all draws); of those %.1f%% keyed, %.1f%% found a template, %.1f%% checked its address space, %.1f%% revalidated it, %.1f%% hit (%.1f%% with moved buffers), %.1f%% built anew, %.1f%% invalidated a template, %.1f%% with address-based stages\n", count(CommitCounter::ResolveDraws), 100.0 * count(CommitCounter::ResolveDraws) / static_cast<double>(totals.draws), 100.0 * count(CommitCounter::ResolveCacheable) / resolveDraws, 100.0 * count(CommitCounter::ResolveFound) / resolveDraws, 100.0 * count(CommitCounter::ResolveSpaceChecks) / resolveDraws, 100.0 * count(CommitCounter::ResolveValid) / resolveDraws, 100.0 * count(CommitCounter::ResolveHits) / resolveDraws, 100.0 * count(CommitCounter::ResolveMovedHits) / resolveDraws, 100.0 * count(CommitCounter::ResolveBuilds) / resolveDraws, 100.0 * count(CommitCounter::ResolveInvalidated) / resolveDraws, 100.0 * count(CommitCounter::ResolveAddressStages) / resolveDraws);
    std::fprintf(stderr, "[commit-target-memo] (10 s) %.0f draws with targets; repeated attachment sets and fallbacks by cause: Accept %.0f; AcceptWithDepthMiss %.0f; Invalid %.0f; Descriptor %.0f; Resident %.0f; DepthOverlap %.0f; Epoch %.0f; Proofs %.0f; Pending %.0f; Unwatch %.0f; Allocations %.0f; Surfaces %.0f; Released %.0f; Claims %.0f; Recorder %.0f; Device %.0f; Metadata %.0f; PostDepthInvalidated %.0f; DepthFallback %.0f; AvoidedRefreshLookups %.0f;\n", count(CommitCounter::TargetDraws), count(CommitCounter::TargetMemoAccept), count(CommitCounter::TargetMemoAcceptWithDepthMiss), count(CommitCounter::TargetMemoInvalid), count(CommitCounter::TargetMemoDescriptor), count(CommitCounter::TargetMemoResident), count(CommitCounter::TargetMemoDepthOverlap), count(CommitCounter::TargetMemoEpoch), count(CommitCounter::TargetMemoProofs), count(CommitCounter::TargetMemoPending), count(CommitCounter::TargetMemoUnwatch), count(CommitCounter::TargetMemoAllocations), count(CommitCounter::TargetMemoSurfaces), count(CommitCounter::TargetMemoReleased), count(CommitCounter::TargetMemoClaims), count(CommitCounter::TargetMemoRecorder), count(CommitCounter::TargetMemoDevice), count(CommitCounter::TargetMemoMetadata), count(CommitCounter::TargetMemoPostDepthInvalidated), count(CommitCounter::TargetMemoDepthFallback), count(CommitCounter::TargetMemoAvoidedRefreshLookups));
    std::fprintf(stderr, "%s\n", FormatShadowReport(totals).c_str());
}

}

std::atomic<int> CommitPhasesState{0};

bool ResolveCommitPhases() {
    const bool enabled = std::getenv("APS5_PROFILE_COMMIT") != nullptr;
    CommitPhasesState.store(enabled ? 2 : 1, std::memory_order_relaxed);
    return enabled;
}

const char* CommitPhaseGroup(CommitPhase phase) {
    return PhaseGroups[static_cast<std::size_t>(phase)];
}

const char* CommitPhaseName(CommitPhase phase) {
    return PhaseNames[static_cast<std::size_t>(phase)];
}

namespace Active {

void BeginCommitPhase(CommitPhase phase) {
    auto& phases = threadPhases;
    if (!phases.active) return;
    charge(phases, Clock::now());
    phases.current = phase;
    if (phase == CommitPhase::ResolveBuild) markShadow(phases, ShadowReason::TemplateBuilt);
    else if (phase == CommitPhase::RevalidateWalk) markShadow(phases, ShadowReason::TemplateWalk);
}

void NoteShadowReason(ShadowReason reason) {
    auto& phases = threadPhases;
    if (phases.active) markShadow(phases, reason);
}

void NoteShadowRecording() {
    auto& phases = threadPhases;
    if (phases.active) phases.recording = true;
}

void NoteShadowCommands() {
    auto& phases = threadPhases;
    if (phases.active && !phases.recording) markShadow(phases, ShadowReason::PreOp);
}

}

ShadowReason FirstShadowReason(unsigned mask) {
    if (mask == 0) return ShadowReason::Count;
    return static_cast<ShadowReason>(std::countr_zero(mask));
}

const char* ShadowReasonName(ShadowReason reason) {
    return ShadowNames[static_cast<std::size_t>(reason)];
}

std::string FormatShadowReport(const CommitPhaseTotals& window) {
    std::uint64_t eligible = 0;
    for (const auto count : window.shadowEligible) eligible += count;
    const double draws = static_cast<double>(std::max<std::uint64_t>(window.draws, 1));
    char item[160];
    std::snprintf(item, sizeof(item), "[parallel-shadow] (10 s) %llu draws, %llu worker-eligible (%.1f%%); by kind:", static_cast<unsigned long long>(window.draws), static_cast<unsigned long long>(eligible), 100.0 * static_cast<double>(eligible) / draws);
    std::string report = item;
    for (std::size_t kind = 0; kind < KindCount; ++kind) {
        const auto kindDraws = window.kindDraws[kind];
        std::snprintf(item, sizeof(item), "%s %s %llu of %llu", kind == 0 ? "" : ",", KindNames[kind], static_cast<unsigned long long>(window.shadowEligible[kind]), static_cast<unsigned long long>(kindDraws));
        report += item;
    }
    report += "; first reason:";
    for (std::size_t reason = 0; reason < ShadowCount; ++reason) {
        std::snprintf(item, sizeof(item), "%s %s %llu", reason == 0 ? "" : ",", ShadowNames[reason], static_cast<unsigned long long>(window.shadowFirst[reason]));
        report += item;
    }
    report += "; any reason:";
    for (std::size_t reason = 0; reason < ShadowCount; ++reason) {
        std::snprintf(item, sizeof(item), "%s %s %llu", reason == 0 ? "" : ",", ShadowNames[reason], static_cast<unsigned long long>(window.shadowAny[reason]));
        report += item;
    }
    return report;
}

namespace Active {

void NoteCommitKey(std::size_t words) {
    if (!threadPhases.active) return;
    keyCount.Add(1);
    keyWordCount.Add(words);
}

void NoteCommitKeyMemo(bool hit) {
    if (!threadPhases.active) return;
    (hit ? keyMemoHitCount : keyMemoMissCount).Add(1);
}

void NoteCommitDrawKind(CommitDrawKind kind) {
    auto& phases = threadPhases;
    if (phases.active) phases.kind = kind;
}

}

void NoteEpochCauses(unsigned causes) {
    threadEpochCauses = causes;
}

void NoteEpochFollowSkipped() {
    counterValues[static_cast<std::size_t>(CommitCounter::EpochFollowsSkipped)].Add(1);
}

void NoteEpochFollow(bool backward, unsigned causes) {
    const auto add = [](CommitCounter counter) { counterValues[static_cast<std::size_t>(counter)].Add(1); };
    add(CommitCounter::EpochFollows);
    if (backward) add(CommitCounter::EpochFollowsBackward);
    if (causes == 0) add(CommitCounter::EpochFollowsByNone);
    for (unsigned cause = 0; cause < static_cast<unsigned>(EpochCause::Count); ++cause) {
        if ((causes & (1u << cause)) != 0) add(static_cast<CommitCounter>(static_cast<unsigned>(CommitCounter::EpochFollowsBySubmission) + cause));
    }
}

unsigned EpochCauses() {
    return threadEpochCauses;
}

CommitCounter EpochCauseCounter(unsigned causes) {
    if (causes == 0) return CommitCounter::TargetEpochByUnknown;
    if ((causes & (causes - 1)) != 0) return CommitCounter::TargetEpochBySeveral;
    return static_cast<CommitCounter>(static_cast<unsigned>(CommitCounter::TargetEpochBySubmission) + static_cast<unsigned>(std::countr_zero(causes)));
}

namespace Active {

void CountCommit(CommitCounter counter, std::uint64_t amount) {
    auto& phases = threadPhases;
    if (!phases.active) return;
    counterValues[static_cast<std::size_t>(counter)].Add(amount);
    if (amount == 0) return;
    if (counter == CommitCounter::TargetRefreshSlow || counter == CommitCounter::OtherRefreshSlow) markShadow(phases, ShadowReason::TargetSlow);
    else if (counter == CommitCounter::InputIndexCopyFresh || counter == CommitCounter::InputVertexCopyFresh) markShadow(phases, ShadowReason::InputFresh);
}

CommitPhase CurrentCommitPhase() {
    const auto& phases = threadPhases;
    return phases.active ? phases.current : CommitPhase::Other;
}

void RelabelCommitPhase(CommitPhase phase) {
    auto& phases = threadPhases;
    if (phases.active) phases.current = phase;
}

}

CommitPhaseTotals ReadCommitPhases() {
    CommitPhaseTotals totals;
    totals.draws = drawCount.Sum();
    totals.keys = keyCount.Sum();
    totals.keyWords = keyWordCount.Sum();
    totals.keyMemoHits = keyMemoHitCount.Sum();
    totals.keyMemoMisses = keyMemoMissCount.Sum();
    for (std::size_t counter = 0; counter < totals.counters.size(); ++counter) totals.counters[counter] = counterValues[counter].Sum();
    for (std::size_t phase = 0; phase < totals.ns.size(); ++phase) totals.ns[phase] = phaseNs[phase].Sum();
    for (std::size_t kind = 0; kind < KindCount; ++kind) {
        totals.kindDraws[kind] = kindDrawCount[kind].Sum();
        for (std::size_t phase = 0; phase < PhaseCount; ++phase) totals.kindNs[kind][phase] = kindPhaseNs[kind][phase].Sum();
        totals.shadowEligible[kind] = shadowEligibleCount[kind].Sum();
    }
    for (std::size_t reason = 0; reason < ShadowCount; ++reason) {
        totals.shadowFirst[reason] = shadowFirstCount[reason].Sum();
        totals.shadowAny[reason] = shadowAnyCount[reason].Sum();
    }
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
    for (std::size_t kind = 0; kind < KindCount; ++kind) {
        difference.kindDraws[kind] = later.kindDraws[kind] - earlier.kindDraws[kind];
        for (std::size_t phase = 0; phase < PhaseCount; ++phase) difference.kindNs[kind][phase] = later.kindNs[kind][phase] - earlier.kindNs[kind][phase];
        difference.shadowEligible[kind] = later.shadowEligible[kind] - earlier.shadowEligible[kind];
    }
    for (std::size_t reason = 0; reason < ShadowCount; ++reason) {
        difference.shadowFirst[reason] = later.shadowFirst[reason] - earlier.shadowFirst[reason];
        difference.shadowAny[reason] = later.shadowAny[reason] - earlier.shadowAny[reason];
    }
    return difference;
}

CommitPhaseDraw::CommitPhaseDraw() {
    if (!CommitPhasesEnabled()) return;
    auto& phases = threadPhases;
    if (phases.active) {
        nested = true;
        previous = phases.current;
        BeginCommitPhase(CommitPhase::Other);
        return;
    }
    owner = true;
    phases.active = true;
    phases.current = CommitPhase::Other;
    phases.kind = CommitDrawKind::Full;
    phases.drawNs.fill(0);
    phases.shadow = 0;
    phases.recording = false;
    phases.mark = Clock::now();
}

CommitPhaseDraw::~CommitPhaseDraw() {
    if (nested) BeginCommitPhase(previous);
    if (!owner) return;
    auto& phases = threadPhases;
    const auto now = Clock::now();
    charge(phases, now);
    phases.active = false;
    drawCount.Add(1);
    const auto kind = static_cast<std::size_t>(phases.kind);
    kindDrawCount[kind].Add(1);
    for (std::size_t phase = 0; phase < PhaseCount; ++phase) {
        if (phases.drawNs[phase] != 0) kindPhaseNs[kind][phase].Add(phases.drawNs[phase]);
    }
    if (phases.shadow == 0) {
        shadowEligibleCount[kind].Add(1);
    } else {
        shadowFirstCount[static_cast<std::size_t>(FirstShadowReason(phases.shadow))].Add(1);
        for (std::size_t reason = 0; reason < ShadowCount; ++reason) {
            if ((phases.shadow & (1u << reason)) != 0) shadowAnyCount[reason].Add(1);
        }
    }
    reportDue(now);
}

void CommitPhaseScope::enter(CommitPhase phase) {
    auto& phases = threadPhases;
    if (!phases.active) return;
    active = true;
    previous = phases.current;
    BeginCommitPhase(phase);
}

}
