#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceOverlay.hpp"
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <utility>

namespace AgcDriver::DriverDetail {

bool Driver::traceDrawCache() {
    static const bool trace = std::getenv("APS5_TRACE_DRAW_CACHE") != nullptr;
    return trace;
}

void Driver::traceInsert(std::unordered_set<std::uint64_t>& set, std::uint64_t key) {
    if (set.size() >= (1u << 16u)) set.clear();
    set.insert(key);
}

void Driver::maybeReportDrawCache(bool profile) {
    auto& counters = drawEntryCounters;
    if (!profile && !traceDrawCache()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - counters.lastReport < std::chrono::seconds(10)) return;
    reportDrawCache(counters, profile);
    counters.lastReport = now;
}

void Driver::reportDrawCache(DrawEntryCounters& counters, bool profile) {
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto miss = [&](DrawMiss reason) { return count(counters.misses[static_cast<std::size_t>(reason)]); };
    std::string text;
    if (traceDrawCache()) {
        char line[1536];
        std::uint64_t misses = 0;
        for (const auto value : counters.misses) misses += value;
        std::snprintf(line, sizeof(line), "[draw-cache] 10 s: %llu lookups, %llu hits (%.1f%%), %llu stage misses, %llu without an entry, %llu data-only hits (stages: %llu reused, %llu compiled; %llu verified); no entry: new registers %llu, user words/pointers only %llu, evicted %llu, seen but never inserted %llu; stage misses: front differing %llu, fragment differing %llu, other differing %llu, recaptured over queued stores %llu, layout %llu, gate %llu, stage count %llu; differing captures: same runs %llu (%.1f words changed each), other runs %llu; %zu entries, %llu evicted (%llu over the byte budget)\n", count(counters.lookups), count(counters.hits), counters.lookups != 0 ? 100.0 * static_cast<double>(counters.hits) / static_cast<double>(counters.lookups) : 0.0, count(misses), count(counters.absent), count(counters.dataHits), count(counters.dataStagesReused), count(counters.dataStagesCompiled), count(counters.dataVerified), count(counters.absentNewRegisters), count(counters.absentUserWords), count(counters.absentEvicted), count(counters.absentNeverInserted), miss(DrawMiss::FrontDiffering), miss(DrawMiss::FragmentDiffering), miss(DrawMiss::OtherDiffering), miss(DrawMiss::QueuedStores), miss(DrawMiss::Layout), miss(DrawMiss::Gate), miss(DrawMiss::Stages), count(counters.differingSameRuns), counters.differingSameRuns != 0 ? static_cast<double>(counters.differingWords) / static_cast<double>(counters.differingSameRuns) : 0.0, count(counters.differingRunsChanged), drawCache.size(), count(counters.evictions), count(counters.byteEvictions));
        text += line;
        std::snprintf(line, sizeof(line), "[draw-cache] register-key miss component changes (10 s): stable registers %llu, user data %llu, auxiliary registers %llu, device %llu, first shader set %llu, unchanged components %llu\n", count(counters.registerKeyMissChanges[0]), count(counters.registerKeyMissChanges[1]), count(counters.registerKeyMissChanges[2]), count(counters.registerKeyMissChanges[3]), count(counters.registerKeyMissChanges[4]), count(counters.registerKeyMissChanges[5]));
        text += line;
    }
    if (profile) text += profileDrawCacheLines(counters);
    AgcDriver::ProfilePrint_nid_no_patch("%s", text.c_str());
    counters = DrawEntryCounters{};
}

DrawEntryCounters Driver::DrawCacheCounters() {
    std::lock_guard cacheLock(drawCacheMutex);
    return drawEntryCounters;
}

Driver::DispatchCacheFootprint Driver::DrawCacheSize() {
    std::lock_guard cacheLock(drawCacheMutex);
    return {drawCache.size(), drawCacheVariantBytes};
}

OverlayDriverCounters Driver::OverlayCounters() {
    OverlayDriverCounters counters{flipsCounted.load(std::memory_order_relaxed), std::nullopt};
    std::unique_lock cacheLock(drawCacheMutex, std::try_to_lock);
    if (cacheLock.owns_lock()) counters.drawCache = OverlayHits{drawEntryCounters.lookups, drawEntryCounters.hits};
    return counters;
}

std::string Driver::profileDrawCacheLines(const DrawEntryCounters& counters) {
    std::string text;
    const auto append = [&](const char* format, auto... values) {
        char line[2048];
        std::snprintf(line, sizeof(line), format, values...);
        text += line;
    };
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto validated = counters.lookups > counters.absent ? counters.lookups - counters.absent : 0;
    const auto miss = [&](DrawMiss reason) { return count(counters.misses[static_cast<std::size_t>(reason)]); };
    std::string ranks;
    for (std::size_t rank = 0; rank < dispatchVariants(); ++rank) {
        char item[32];
        std::snprintf(item, sizeof(item), "%s%llu", rank == 0 ? "" : " / ", count(counters.variantHitsByRank[rank]));
        ranks += item;
    }
    append("[draw-cache] %llu lookups (10 s): %llu no entry, %llu validated in %.1f us each (key, lookup and every stage): %llu hits (every stage equal; stage hits by variant rank 1..k %s), misses by reason: front stage differing %llu, fragment differing %llu, other stage differing %llu, recaptured over queued stores %llu, layout (push offset) %llu, gate %llu, stage count %llu; %llu stage validations (%llu equal, %.2f variants compared each); %llu inserts (%llu shape updates, %llu variants inserted, %llu evicted beyond k, %llu already present, %llu unstable), %zu entries (%llu variants, ~%.1f MiB of a %.1f MiB budget), %llu evictions in total (%llu over the byte budget), %llu LRU moves; verify: %llu hits captured again, %llu stages differed\n", count(counters.lookups), count(counters.absent), count(validated), validated != 0 ? counters.validateUs / static_cast<double>(validated) : 0.0, count(counters.hits), ranks.c_str(), miss(DrawMiss::FrontDiffering), miss(DrawMiss::FragmentDiffering), miss(DrawMiss::OtherDiffering), miss(DrawMiss::QueuedStores), miss(DrawMiss::Layout), miss(DrawMiss::Gate), miss(DrawMiss::Stages), count(counters.stageValidations), count(counters.stageEqual), counters.stageValidations != 0 ? static_cast<double>(counters.variantsCompared) / static_cast<double>(counters.stageValidations) : 0.0, count(counters.inserts), count(counters.shapeUpdates), count(counters.variantsInserted), count(counters.variantsEvicted), count(counters.present), count(counters.unstable), drawCache.size(), count(drawCacheVariants), static_cast<double>(drawCacheVariantBytes) / (1024.0 * 1024.0), static_cast<double>(DrawCacheBytes()) / (1024.0 * 1024.0), count(drawCacheEvictions), count(drawCacheByteEvictions), count(counters.touches), count(counters.verifyHits), count(counters.verifyMismatches));
    append("[draw-cache] register key (10 s): %llu lookups, %llu hits, key build %.1f us each; decode skipped %llu, partial (state/pixel/programs from the entry) %llu; facade log != table %llu; verify: %llu decodes compared, %llu differed\n", count(counters.registerKeyLookups), count(counters.registerKeyHits), counters.registerKeyLookups != 0 ? counters.keyUs / static_cast<double>(counters.registerKeyLookups) : 0.0, count(counters.decodeSkipped), count(counters.decodePartial), count(counters.facadeMismatches), count(counters.verifyDecodes), count(counters.verifyDecodeMismatches));
    append("[draw-cache] stage data hits (10 s): %llu stages (%llu words refreshed), verified %llu; stage inserts with data positions %llu (%.1f positions each)\n", count(counters.stageDataHits), count(counters.stageDataWords), count(counters.stageDataVerified), count(counters.stageDataInserts), counters.stageDataInserts != 0 ? static_cast<double>(counters.stageDataPositions) / static_cast<double>(counters.stageDataInserts) : 0.0);
    const auto memo = drawStageMemoCounters.Load();
    using Memo = DrawStageMemoCount;
    const auto ratio = [](std::uint64_t part, std::uint64_t whole) { return whole != 0 ? static_cast<double>(part) / static_cast<double>(whole) : 0.0; };
    const auto missed = memo[Memo::Lookups] > memo.Served() ? memo[Memo::Lookups] - memo.Served() : 0;
    append("[draw-cache] draw stage memo (since start): %llu lookups (%llu refused a key), %llu absent (%llu admitted for the next sighting), %llu served (%llu proved by write stamps, %llu compared, %.0f bytes each), %llu stale, %llu inserts (%llu skipped), %llu evicted, %zu kept; fragment stages %llu looked up, %llu served; %.2f us per served stage, %.2f us per lookup that missed; %llu stages a partial draw-cache miss had validated, reused without a build; draw-cache misses %llu, %llu with every stage reused; verify: %llu served stages built again; %llu prepared invocation keys rebuilt for a sample mode recorded between their lock scopes; prepared handle hints: %llu served, refused %llu for a moved table, %llu for sample modes, %llu otherwise\n", count(memo[Memo::Lookups]), count(memo[Memo::Refused]), count(memo[Memo::Absent]), count(memo[Memo::AbsentAdmitted]), count(memo.Served()), count(memo[Memo::Proved]), count(memo[Memo::Compared]), ratio(memo[Memo::ComparedBytes], memo[Memo::Compared]), count(memo[Memo::Stale]), count(memo[Memo::Inserts]), count(memo[Memo::Skipped]), count(memo[Memo::Evictions]), drawStageMemo.Size(), count(memo[Memo::FragmentLookups]), count(memo[Memo::FragmentServed]), ratio(memo[Memo::HitNanoseconds], memo.Served()) / 1000.0, ratio(memo[Memo::MissNanoseconds], missed) / 1000.0, count(memo[Memo::ValidatedReused]), count(memo[Memo::DrawMisses]), count(memo[Memo::ReusedMisses]), count(memo[Memo::Verified]), count(InvocationKeyRebuilds()), count(PreparedHandleHints().served), count(PreparedHandleHints().refusedVersion), count(PreparedHandleHints().refusedModes), count(PreparedHandleHints().refusedOther));
    const auto keyMemo = Graphics::StageKeyMemoCounters();
    append("[draw-cache] stage key memo (since start): %llu static hits, %llu static fills (%llu lost the publish race), %llu patched segments appended, %llu layout split misses (APS5_PROFILE_DRAW), %llu verified; unshaped stages: %llu per-result memo hits, %llu full walks\n", count(keyMemo.staticHits), count(keyMemo.staticFills), count(keyMemo.staticLosses), count(keyMemo.patchedSegments), count(keyMemo.layoutSplitMisses), count(keyMemo.verified), count(keyMemo.resultHits), count(keyMemo.resultWalks));
    if (Graphics::DrawPlanShadow() || Graphics::DrawPlans()) {
        const auto plans = drawPlanCounters.Load();
        using Plan = DrawPlanCount;
        DrawPlanCounts window;
        for (std::size_t kind = 0; kind < DrawPlanCountKinds; ++kind) window.values[kind] = plans.values[kind] - drawPlanReported.values[kind];
        drawPlanReported = plans;
        append("[draw-plan] served (10 s): %llu lookups, %llu hits (%.1f%%), %llu full-path draws with a plan key; %.2f us per hit, %.2f us per full-path draw with a plan key (APS5_PROFILE_DRAW); missed: absent %llu, unservable %llu, another instance %llu, refused a move %llu, proof %llu, gone or not recordable %llu; another instance by cause: template %llu, target %llu, view %llu, depth %llu, shape %llu; %llu hits rebound buffers, %llu served another template from the resource cache\n", count(window[Plan::PlanLookups]), count(window[Plan::Hits]), 100.0 * ratio(window[Plan::Hits], window[Plan::PlanLookups]), count(window[Plan::FullDraws]), ratio(window[Plan::HitNanoseconds], window[Plan::Hits]) / 1000.0, ratio(window[Plan::FullNanoseconds], window[Plan::FullDraws]) / 1000.0, count(window[Plan::MissAbsent]), count(window[Plan::MissUnservable]), count(window[Plan::MissInstance]), count(window[Plan::MissMoved] + window[Plan::MissPatchedData]), count(window[Plan::MissProof]), count(window[Plan::MissTargetGone] + window[Plan::MissTemplateGone] + window[Plan::MissObjectsGone] + window[Plan::MissNotRecordable]), count(window[Plan::InstanceTemplate]), count(window[Plan::InstanceTarget]), count(window[Plan::InstanceView]), count(window[Plan::InstanceDepth]), count(window[Plan::InstanceShape]), count(window[Plan::RebasedHits]), count(window[Plan::TemplateInstanceHits]));
        {
            const auto at = [&](DrawPlanCount base, std::size_t offset) { return static_cast<unsigned long long>(window.values[static_cast<std::size_t>(base) + offset]); };
            std::string sets;
            for (std::size_t mask = 1; mask < 32; ++mask) {
                if (at(Plan::InstanceMismatchSets, mask) == 0) continue;
                std::string name;
                for (const auto& [bit, letter] : std::initializer_list<std::pair<std::uint8_t, char>>{{Graphics::PlanMismatchShape, 'S'}, {Graphics::PlanMismatchTemplate, 'T'}, {Graphics::PlanMismatchTarget, 'G'}, {Graphics::PlanMismatchView, 'V'}, {Graphics::PlanMismatchDepth, 'D'}}) {
                    if ((mask & bit) != 0) name += letter;
                }
                sets += " " + name + " " + std::to_string(at(Plan::InstanceMismatchSets, mask));
            }
            std::string distances;
            for (std::size_t bucket = 0; bucket < PlanLookupDistanceBuckets; ++bucket) {
                if (at(Plan::LookupDistances, bucket) == 0) continue;
                distances += " " + std::to_string(1ull << bucket) + (bucket + 1 == PlanLookupDistanceBuckets ? "+" : "") + ":" + std::to_string(at(Plan::LookupDistances, bucket));
            }
            std::string replacements;
            for (std::size_t size = 0; size <= PlanSignatureSetLimit; ++size) {
                if (at(Plan::ReplacementSignatureSets, size) == 0) continue;
                replacements += " " + std::to_string(size + 1) + (size == PlanSignatureSetLimit ? "+" : "") + ":" + std::to_string(at(Plan::ReplacementSignatureSets, size));
            }
            append("[draw-plan] instance detail (10 s, APS5_PROFILE_DRAW): another-instance misses by every check that differs (S shape, T template, G target surface, V target view, D depth view):%s; lookups by lookups since the key's previous lookup (bucket floor):%s; replacements by distinct instance signatures seen for the key (target and depth views):%s, %llu returning to a signature seen before; absent lookups: %llu for keys the index evicted, %llu for keys the full path saw without a decision, %llu for keys not seen before\n", sets.empty() ? " none" : sets.c_str(), distances.empty() ? " none" : distances.c_str(), replacements.empty() ? " none" : replacements.c_str(), at(Plan::ReplacementReturns, 0), at(Plan::AbsentEvicted, 0), at(Plan::AbsentUndecided, 0), at(Plan::AbsentNew, 0));
        }
        if (Graphics::RuntimeSwitches::Set(Graphics::RuntimeSwitch::PlanAttachments)) append("[draw-plan] attachments (10 s, APS5_PLAN_ATTACHMENTS, APS5_PROFILE_DRAW): %llu plan hits drew into attachments the full path's target preparation resolved for them, not the plan's own, and offered %llu exact recipes with those attachments for their draw keys; per such hit %.2f us resolving its targets and %.2f us acquiring its framebuffer (the plan.fb phase of those hits); %llu resolutions needed copies and were handed to the full path\n", static_cast<unsigned long long>(window[Plan::OwnAttachmentHits]), static_cast<unsigned long long>(window[Plan::OwnRecipes]), window[Plan::OwnAttachmentHits] == 0 ? 0.0 : static_cast<double>(window[Plan::OwnTargetsNanoseconds]) / 1e3 / static_cast<double>(window[Plan::OwnAttachmentHits]), window[Plan::OwnAttachmentHits] == 0 ? 0.0 : static_cast<double>(window[Plan::OwnFramebufferNanoseconds]) / 1e3 / static_cast<double>(window[Plan::OwnAttachmentHits]), static_cast<unsigned long long>(window[Plan::HandedTargets]));
        append("[draw-plan] served (since start): %llu lookups, %llu hits; missed: absent %llu, unservable %llu, another instance %llu, target gone %llu, template gone %llu, objects gone %llu, proof %llu, not recordable %llu; %llu plans replaced by a newer instance or a servable decision; binding words proved by the program's memo %llu stages, compared %llu; %llu hits rebound %llu moved buffers, %llu refused a move (%llu over patched data); verify: %llu hits checked; %.2f us per hit, %.2f us per full-path draw with a plan key (APS5_PROFILE_DRAW)\n", count(plans[Plan::PlanLookups]), count(plans[Plan::Hits]), count(plans[Plan::MissAbsent]), count(plans[Plan::MissUnservable]), count(plans[Plan::MissInstance]), count(plans[Plan::MissTargetGone]), count(plans[Plan::MissTemplateGone]), count(plans[Plan::MissObjectsGone]), count(plans[Plan::MissProof]), count(plans[Plan::MissNotRecordable]), count(plans[Plan::Replaced]), count(plans[Plan::SharedBindingStages]), count(plans[Plan::ComparedBindingStages]), count(plans[Plan::RebasedHits]), count(plans[Plan::RebasedBuffers]), count(plans[Plan::MissMoved]) + count(plans[Plan::MissPatchedData]), count(plans[Plan::MissPatchedData]), count(plans[Plan::Verified]), ratio(plans[Plan::HitNanoseconds], plans[Plan::Hits]) / 1000.0, ratio(plans[Plan::FullNanoseconds], plans[Plan::FullDraws]) / 1000.0);
        append("[draw-plan] shadow (since start): %llu lookups, %llu without a decision, %llu absent (inserted), %llu present: %llu agreed, differing pipeline %llu, fragment outputs %llu, shader stages %llu, push stages %llu, depth %llu, vertex input %llu, masked state %llu; instance same %llu, differs %llu, pass key differing on the same instance %llu; %llu moved bases, %llu unservable decisions, %zu plans, %llu evicted\n", count(plans[Plan::Lookups]), count(plans[Plan::NoDecision]), count(plans[Plan::Absent]), count(plans[Plan::Present]), count(plans[Plan::Agreed]), count(plans[Plan::Pipeline]), count(plans[Plan::FragmentOutputs]), count(plans[Plan::ShaderStages]), count(plans[Plan::PushStages]), count(plans[Plan::HasDepth]), count(plans[Plan::VertexInput]), count(plans[Plan::MaskedState]), count(plans[Plan::SameInstance]), count(plans[Plan::InstanceDiffers]), count(plans[Plan::PassDiffersSameInstance]), count(plans[Plan::MovedBases]), count(plans[Plan::Unservable]), drawPlans.Size(), count(plans[Plan::Evictions]));
    }
    append("[draw-cache] indirect records (10 s): %llu patched words forwarded as push data, %llu stages re-materialized, %llu words refused (walked, not forwarded, or vertex info changed), verified %llu\n", count(counters.recordWordsForwarded), count(counters.recordStagesRecompiled), count(counters.recordWordsRefused), count(counters.recordWordsVerified));
    return text;
}

}
