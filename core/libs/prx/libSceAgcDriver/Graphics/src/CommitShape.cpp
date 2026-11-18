#include "prx/libSceAgcDriver/Graphics/include/CommitShape.hpp"

#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace AgcDriver {

bool CommitShapeProfiled() {
    static const bool profiled = std::getenv("APS5_PROFILE_COMMIT_SHAPE") != nullptr;
    return profiled;
}

void CommitShape::Note(std::size_t kind, std::span<const double, Phases> phaseUs, std::uint64_t dependencies, const CommitStamps& stamps, std::uint64_t pipelines, std::size_t buildReason, const ProofLeadNote& lead) {
    std::lock_guard lock(mutex);
    ++counts.draws;
    if (lead.looked) {
        ++counts.leadLooked;
        if (lead.ending < ProofEndingCount) {
            ++counts.endings[lead.ending];
            counts.endingUs[lead.ending] += lead.revalidateUs;
            if (lead.decline < ProofDeclineCount) {
                ++counts.declineEndings[lead.decline][lead.ending];
                counts.declineEndingUs[lead.decline][lead.ending] += lead.revalidateUs;
                if (lead.declineAlone) ++counts.declineAlone[lead.decline];
            }
            if (lead.ending == EndingFull) {
                const auto fastFail = lead.fastFail < FastFailSlots ? lead.fastFail : FastFailSlots;
                ++counts.fullFastFails[fastFail];
                counts.fullFastFailUs[fastFail] += lead.revalidateUs;
                if (lead.served) ++counts.fullFastFailServed[fastFail];
                if (lead.collectSource < CollectSourceCount) {
                    ++counts.fullCollects[lead.collectSource];
                    counts.fullCollectUs[lead.collectSource] += lead.revalidateUs;
                    if (lead.collectSource == CollectUnwatched && lead.unwatchCause < UnwatchCauseCount) {
                        ++counts.fullUnwatchCauses[lead.unwatchCause];
                        counts.fullUnwatchCauseUs[lead.unwatchCause] += lead.revalidateUs;
                    }
                }
            }
        }
        if (lead.stamped) {
            ++counts.leadStamped;
            std::size_t bucket = 0;
            while (bucket < Report::LeadBounds.size() && lead.lead > Report::LeadBounds[bucket]) ++bucket;
            ++counts.leadBuckets[bucket];
            if (lead.survived) ++counts.leadSurvived;
            if (lead.epochAdvanced) ++counts.leadEpochAdvanced;
            if (lead.collectAdvanced) ++counts.leadCollectAdvanced;
            if (lead.walked) {
                ++counts.walkedStamped;
                counts.walkedStampedUs += lead.revalidateUs;
                if (lead.collectAdvanced) ++counts.walkedCollectAdvanced;
                if (lead.survived) {
                    ++counts.walkedUnchanged;
                    counts.walkedUnchangedUs += lead.revalidateUs;
                    if (lead.collectSource == CollectUnwatched) {
                        ++counts.walkedUnchangedUnwatched;
                        counts.walkedUnchangedUnwatchedUs += lead.revalidateUs;
                    }
                }
                if (lead.survived && !lead.collectAdvanced) {
                    ++counts.walkedKept;
                    counts.walkedKeptUs += lead.revalidateUs;
                }
            }
        }
        if (lead.walked) {
            ++counts.walked;
            if (lead.lookups) ++counts.walkedLookups;
            if (lead.served) {
                ++counts.walkedServed;
                const bool pure = lead.effects == 0;
                const bool survived = lead.stamped && lead.survived;
                if (lead.stamped) ++counts.servedStamped;
                if (survived) ++counts.servedSurvived;
                if (pure) ++counts.servedPure;
                if (pure && survived) ++counts.servedPureSurvived;
                for (std::size_t effect = 0; effect < WalkEffectCount; ++effect) {
                    if ((lead.effects >> effect) & 1u) ++counts.servedEffects[effect];
                }
            }
        }
    }
    if (kind < Kinds) {
        ++counts.kindDraws[kind];
        for (std::size_t phase = 0; phase < Phases; ++phase) counts.kindPhaseUs[kind][phase] += phaseUs[phase];
        counts.kindPipelines[kind] += pipelines;
        if (pipelines != 0) ++counts.kindPipelineDraws[kind];
    }
    if (buildReason < BuildReasonCount) {
        ++counts.reasonBuilds[buildReason];
        for (std::size_t phase = 0; phase < Phases; ++phase) counts.reasonPhaseUs[buildReason][phase] += phaseUs[phase];
        counts.reasonPipelines[buildReason] += pipelines;
        if (pipelines != 0) ++counts.reasonPipelineDraws[buildReason];
    }
    if (dependencies != 0) {
        ++counts.dependentDraws;
        counts.dependencies += dependencies;
    }
    for (std::size_t index = 0; index < Distances.size(); ++index) {
        const auto distance = Distances[index];
        if (noted < distance) continue;
        const auto& earlier = history[(noted - distance) % History];
        ++counts.compared[index];
        bool same = true;
        for (std::size_t stamp = 0; stamp < StampCount; ++stamp) {
            if (earlier[stamp] == stamps[stamp]) continue;
            ++counts.moved[index][stamp];
            same = false;
        }
        if (same) ++counts.survived[index];
    }
    history[noted % History] = stamps;
    ++noted;
}

CommitShape::Report CommitShape::Take() {
    std::lock_guard lock(mutex);
    auto taken = counts;
    counts = Report{};
    return taken;
}

std::string CommitShape::Line(const Report& report, std::span<const char* const, Kinds> kindNames, std::span<const char* const, Phases> phaseNames) {
    constexpr std::array<const char*, StampCount> stampNames{"epoch", "proofs", "pending", "unwatch", "allocations", "surfaces", "released", "published"};
    const auto share = [](std::uint64_t part, std::uint64_t whole) { return whole != 0 ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0; };
    std::string text;
    char item[256];
    double totalUs = 0;
    for (const auto& kind : report.kindPhaseUs) {
        for (const auto us : kind) totalUs += us;
    }
    std::snprintf(item, sizeof(item), "[commit-shape] 10 s: %llu draws, %.2f us/draw;", static_cast<unsigned long long>(report.draws), report.draws != 0 ? totalUs / static_cast<double>(report.draws) : 0.0);
    text += item;
    for (std::size_t kind = 0; kind < Kinds; ++kind) {
        const auto draws = report.kindDraws[kind];
        if (draws == 0) continue;
        double kindUs = 0;
        for (const auto us : report.kindPhaseUs[kind]) kindUs += us;
        std::snprintf(item, sizeof(item), " %s %llu x %.2f us:", kindNames[kind], static_cast<unsigned long long>(draws), kindUs / static_cast<double>(draws));
        text += item;
        for (std::size_t phase = 0; phase < Phases; ++phase) {
            const auto us = report.kindPhaseUs[kind][phase];
            if (us <= 0) continue;
            std::snprintf(item, sizeof(item), " %s %.2f", phaseNames[phase], us / static_cast<double>(draws));
            text += item;
        }
        text += ';';
    }
    std::snprintf(item, sizeof(item), " dependent %llu (%.1f%%), %llu hits;", static_cast<unsigned long long>(report.dependentDraws), share(report.dependentDraws, report.draws), static_cast<unsigned long long>(report.dependencies));
    text += item;
    text += " stamps survive";
    for (std::size_t index = 0; index < Distances.size(); ++index) {
        std::snprintf(item, sizeof(item), " d%zu %.1f%%", Distances[index], share(report.survived[index], report.compared[index]));
        text += item;
    }
    text += "; moved";
    for (std::size_t index = 0; index < Distances.size(); ++index) {
        std::snprintf(item, sizeof(item), " d%zu:", Distances[index]);
        text += item;
        for (std::size_t stamp = 0; stamp < StampCount; ++stamp) {
            if (report.moved[index][stamp] == 0) continue;
            std::snprintf(item, sizeof(item), " %s %.1f%%", stampNames[stamp], share(report.moved[index][stamp], report.compared[index]));
            text += item;
        }
        text += index + 1 < Distances.size() ? "," : "";
    }
    return text;
}

std::string CommitShape::BuildLine(const Report& report, std::span<const char* const, Kinds> kindNames, std::size_t resourcesPhase, std::size_t pipelinePhase) {
    std::string text;
    char item[256];
    std::uint64_t builds = 0;
    for (const auto count : report.reasonBuilds) builds += count;
    std::snprintf(item, sizeof(item), "[draw-builds] 10 s: %llu builds;", static_cast<unsigned long long>(builds));
    text += item;
    for (std::size_t reason = 0; reason < BuildReasonCount; ++reason) {
        const auto count = report.reasonBuilds[reason];
        if (count == 0) continue;
        double drawUs = 0;
        for (const auto us : report.reasonPhaseUs[reason]) drawUs += us;
        const auto per = static_cast<double>(count);
        std::snprintf(item, sizeof(item), " %s %llu x %.2f us (resources %.2f, pipeline %.2f), %llu pipelines created in %llu;", BuildReasonNames[reason], static_cast<unsigned long long>(count), drawUs / per, report.reasonPhaseUs[reason][resourcesPhase] / per, report.reasonPhaseUs[reason][pipelinePhase] / per, static_cast<unsigned long long>(report.reasonPipelines[reason]), static_cast<unsigned long long>(report.reasonPipelineDraws[reason]));
        text += item;
    }
    text += " pipelines created by kind:";
    for (std::size_t kind = 0; kind < Kinds; ++kind) {
        std::snprintf(item, sizeof(item), " %s %llu in %llu of %llu draws%s", kindNames[kind], static_cast<unsigned long long>(report.kindPipelines[kind]), static_cast<unsigned long long>(report.kindPipelineDraws[kind]), static_cast<unsigned long long>(report.kindDraws[kind]), kind + 1 < Kinds ? "," : "");
        text += item;
    }
    return text;
}

std::string CommitShape::ProofLeadLine(const Report& report) {
    const auto share = [](std::uint64_t part, std::uint64_t whole) { return whole != 0 ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0; };
    std::string text;
    char item[256];
    std::snprintf(item, sizeof(item), "[proof-lead] 10 s: %llu template lookups, %llu stamped at prepare (%.1f%%), proof, pending and retired serials unchanged since prepare %.1f%%, epoch token advanced since prepare %.1f%%; lead in draws", static_cast<unsigned long long>(report.leadLooked), static_cast<unsigned long long>(report.leadStamped), share(report.leadStamped, report.leadLooked), share(report.leadSurvived, report.leadStamped), share(report.leadEpochAdvanced, report.leadStamped));
    text += item;
    for (std::size_t bucket = 0; bucket < report.leadBuckets.size(); ++bucket) {
        if (bucket == 0) std::snprintf(item, sizeof(item), " 0: %.1f%%", share(report.leadBuckets[bucket], report.leadStamped));
        else if (bucket < Report::LeadBounds.size()) std::snprintf(item, sizeof(item), ", %llu-%llu: %.1f%%", static_cast<unsigned long long>(Report::LeadBounds[bucket - 1] + 1), static_cast<unsigned long long>(Report::LeadBounds[bucket]), share(report.leadBuckets[bucket], report.leadStamped));
        else std::snprintf(item, sizeof(item), ", >%llu: %.1f%%", static_cast<unsigned long long>(Report::LeadBounds.back()), share(report.leadBuckets[bucket], report.leadStamped));
        text += item;
    }
    std::snprintf(item, sizeof(item), "; full walks %llu (%llu with lookups), served %llu (%llu stamped): serials unchanged since prepare %.1f%% of stamped, pure %.1f%%, pure and unchanged %.1f%% of stamped; effects", static_cast<unsigned long long>(report.walked), static_cast<unsigned long long>(report.walkedLookups), static_cast<unsigned long long>(report.walkedServed), static_cast<unsigned long long>(report.servedStamped), share(report.servedSurvived, report.servedStamped), share(report.servedPure, report.walkedServed), share(report.servedPureSurvived, report.servedStamped));
    text += item;
    for (std::size_t effect = 0; effect < WalkEffectCount; ++effect) {
        std::snprintf(item, sizeof(item), " %s %.1f%%%s", WalkEffectNames[effect], share(report.servedEffects[effect], report.walkedServed), effect + 1 < WalkEffectCount ? "," : "");
        text += item;
    }
    text += "; revalidate";
    double totalUs = 0;
    for (const auto us : report.endingUs) totalUs += us;
    for (std::size_t ending = 0; ending < ProofEndingCount; ++ending) {
        const auto count = report.endings[ending];
        std::snprintf(item, sizeof(item), " %s %llu x %.2f us (%.1f%% of the time)%s", ProofEndingNames[ending], static_cast<unsigned long long>(count), count != 0 ? report.endingUs[ending] / static_cast<double>(count) : 0.0, totalUs > 0 ? 100.0 * report.endingUs[ending] / totalUs : 0.0, ending + 1 < ProofEndingCount ? "," : "");
        text += item;
    }
    std::snprintf(item, sizeof(item), "; stamped draws whose collect epoch advanced since prepare %.1f%%; walked and stamped %llu: collect epoch advanced %.1f%%,", share(report.leadCollectAdvanced, report.leadStamped), static_cast<unsigned long long>(report.walkedStamped), share(report.walkedCollectAdvanced, report.walkedStamped));
    text += item;
    std::snprintf(item, sizeof(item), " token kept (serials unchanged and collect epoch not advanced) %.1f%%, walk time %.1f ms, kept %.1f ms (%.1f%%)", share(report.walkedKept, report.walkedStamped), report.walkedStampedUs / 1000.0, report.walkedKeptUs / 1000.0, report.walkedStampedUs > 0 ? 100.0 * report.walkedKeptUs / report.walkedStampedUs : 0.0);
    text += item;
    std::snprintf(item, sizeof(item), "; walked, stamped and serials unchanged %llu (%.1f ms): %llu over unwatched ranges (%.1f ms, %.1f%% of their walk time)", static_cast<unsigned long long>(report.walkedUnchanged), report.walkedUnchangedUs / 1000.0, static_cast<unsigned long long>(report.walkedUnchangedUnwatched), report.walkedUnchangedUnwatchedUs / 1000.0, report.walkedUnchangedUs > 0 ? 100.0 * report.walkedUnchangedUnwatchedUs / report.walkedUnchangedUs : 0.0);
    text += item;
    return text;
}

std::string CommitShape::ProofDeclineLine(const Report& report, std::span<const char* const> fastFailNames) {
    std::string text = "[proof-decline] 10 s: revalidations by why the epoch exit declined, as epoch exit / fast-ending / full-walk-ending / refused before the walk (count x us):";
    char item[256];
    const double fullUs = report.endingUs[EndingFull];
    bool first = true;
    for (std::size_t decline = 0; decline < ProofDeclineCount; ++decline) {
        const auto& counts = report.declineEndings[decline];
        if (counts[EndingEpoch] + counts[EndingFast] + counts[EndingFull] + counts[EndingRefused] == 0) continue;
        std::snprintf(item, sizeof(item), "%s %s", first ? "" : ";", ProofDeclineNames[decline]);
        text += item;
        first = false;
        if (decline >= DeclineEpoch && decline <= DeclineRetired) {
            std::snprintf(item, sizeof(item), " (alone %llu)", static_cast<unsigned long long>(report.declineAlone[decline]));
            text += item;
        }
        for (std::size_t ending = 0; ending < ProofEndingCount; ++ending) {
            const auto count = counts[ending];
            std::snprintf(item, sizeof(item), "%s %llu x %.2f", ending == 0 ? "" : " /", static_cast<unsigned long long>(count), count != 0 ? report.declineEndingUs[decline][ending] / static_cast<double>(count) : 0.0);
            text += item;
        }
        std::snprintf(item, sizeof(item), " (%.1f%% of full-walk time)", fullUs > 0 ? 100.0 * report.declineEndingUs[decline][EndingFull] / fullUs : 0.0);
        text += item;
    }
    text += "; full walks by the first failing fast check:";
    first = true;
    for (std::size_t reason = 0; reason <= FastFailSlots; ++reason) {
        const auto count = report.fullFastFails[reason];
        if (count == 0) continue;
        const char* name = reason < fastFailNames.size() ? fastFailNames[reason] : reason == FastFailSlots ? "no fast proof ran" : "unnamed";
        std::snprintf(item, sizeof(item), "%s %s %llu x %.2f us, %llu served (%.1f%% of full-walk time)", first ? "" : ",", name, static_cast<unsigned long long>(count), report.fullFastFailUs[reason] / static_cast<double>(count), static_cast<unsigned long long>(report.fullFastFailServed[reason]), fullUs > 0 ? 100.0 * report.fullFastFailUs[reason] / fullUs : 0.0);
        text += item;
        first = false;
    }
    if (first) text += " none";
    text += "; collect fails behind full walks by source:";
    for (std::size_t source = 0; source < CollectSourceCount; ++source) {
        const auto count = report.fullCollects[source];
        std::snprintf(item, sizeof(item), " %s %llu x %.2f us%s", CollectSourceNames[source], static_cast<unsigned long long>(count), count != 0 ? report.fullCollectUs[source] / static_cast<double>(count) : 0.0, source + 1 < CollectSourceCount ? "," : "");
        text += item;
    }
    text += "; unwatched collects behind full walks by cause:";
    first = true;
    for (std::size_t cause = 0; cause < UnwatchCauseCount; ++cause) {
        const auto count = report.fullUnwatchCauses[cause];
        if (count == 0) continue;
        std::snprintf(item, sizeof(item), "%s %s %llu x %.2f us", first ? "" : ",", GuestMemory::UnwatchCauseName(static_cast<GuestMemory::UnwatchCause>(cause)), static_cast<unsigned long long>(count), report.fullUnwatchCauseUs[cause] / static_cast<double>(count));
        text += item;
        first = false;
    }
    if (first) text += " none";
    return text;
}

}
