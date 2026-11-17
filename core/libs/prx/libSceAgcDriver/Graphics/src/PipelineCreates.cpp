#include "prx/libSceAgcDriver/Graphics/include/PipelineCreates.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace AgcDriver {

bool PipelineCreatesProfiled() {
    static const bool profiled = std::getenv("APS5_PROFILE_COMMIT_SHAPE") != nullptr;
    return profiled;
}

PipelineCreates& DevicePipelineCreates() {
    static auto* const creates = new PipelineCreates();
    return *creates;
}

void PipelineCreates::Note(PipelineCreateKind kind, bool inDraw, PipelineCacheOutcome outcome, double ms, MonolithicReason reason) {
    const auto index = static_cast<std::size_t>(kind);
    if (index >= Kinds) return;
    std::lock_guard lock(mutex);
    auto& cell = counts.bySite[inDraw ? 1 : 0][index];
    ++cell.count;
    if (outcome == PipelineCacheOutcome::Hit) ++cell.hits;
    else if (outcome == PipelineCacheOutcome::Miss) ++cell.misses;
    else ++cell.unknown;
    cell.ms += ms;
    cell.longestMs = std::max(cell.longestMs, ms);
    if (const auto slot = static_cast<std::size_t>(reason); slot < Reasons) {
        ++counts.reasons[slot];
        counts.reasonMs[slot] += ms;
    }
}

void PipelineCreates::NoteWait(std::size_t part, bool inDraw, double ms) {
    if (part >= 4) return;
    std::lock_guard lock(mutex);
    ++counts.waited[inDraw ? 1 : 0][part];
    counts.waitedMs[inDraw ? 1 : 0][part] += ms;
}

void PipelineCreates::NoteDuplicate() {
    std::lock_guard lock(mutex);
    ++counts.duplicates;
}

void PipelineCreates::NotePartMiss(std::size_t part, const PartOrigin& origin, bool inDraw) {
    if (!inDraw || part < 1 || part > 2) return;
    std::lock_guard lock(mutex);
    auto cause = PartMissCause::NewModule;
    if (const auto found = origins[part - 1].find(origin.module); found != origins[part - 1].end()) {
        const auto& seen = found->second;
        const auto any = [&](auto&& match) { return std::ranges::any_of(seen, match); };
        if (!any([&](const PartOrigin& other) { return other.bindings == origin.bindings; })) cause = PartMissCause::Bindings;
        else if (!any([&](const PartOrigin& other) { return other.bindings == origin.bindings && other.specialization == origin.specialization; })) cause = PartMissCause::Specialization;
        else if (!any([&](const PartOrigin& other) { return other.bindings == origin.bindings && other.specialization == origin.specialization && other.context == origin.context; })) cause = PartMissCause::Context;
        else cause = PartMissCause::State;
    }
    ++counts.missCauses[part - 1][static_cast<std::size_t>(cause)];
}

void PipelineCreates::NotePartBuilt(std::size_t part, const PartOrigin& origin, bool inDraw) {
    if (part < 1 || part > 2) return;
    std::lock_guard lock(mutex);
    origins[part - 1][origin.module].push_back(origin);
    if (!inDraw) unusedOutside[part - 1].insert(origin.key);
}

void PipelineCreates::NotePartHit(std::size_t part, const PartOrigin& origin, bool inDraw) {
    if (!inDraw || part < 1 || part > 2) return;
    std::lock_guard lock(mutex);
    if (unusedOutside[part - 1].erase(origin.key) != 0) ++counts.prebuiltUsed[part - 1];
}

PipelineCreates::Report PipelineCreates::Take() {
    std::lock_guard lock(mutex);
    const auto report = counts;
    counts = Report{};
    return report;
}

std::string PipelineCreates::Line(const Report& report) {
    std::string text = "[pipeline-creates] 10 s:";
    char item[256];
    std::uint64_t total = 0;
    for (std::size_t side = 2; side-- > 0;) {
        std::uint64_t sideCount = 0;
        for (const auto& cell : report.bySite[side]) sideCount += cell.count;
        total += sideCount;
        std::snprintf(item, sizeof(item), " %s %llu", side == 1 ? "in committed draws" : "outside draws", static_cast<unsigned long long>(sideCount));
        text += item;
        for (std::size_t kind = 0; kind < Kinds; ++kind) {
            const auto& cell = report.bySite[side][kind];
            if (cell.count == 0) continue;
            std::snprintf(item, sizeof(item), ", %s %llu (cache hit %llu, miss %llu, no feedback %llu) %.1f ms, longest %.1f ms", PipelineCreateKindNames[kind], static_cast<unsigned long long>(cell.count), static_cast<unsigned long long>(cell.hits), static_cast<unsigned long long>(cell.misses), static_cast<unsigned long long>(cell.unknown), cell.ms, cell.longestMs);
            text += item;
        }
        text += ";";
    }
    text += " monolithic because:";
    bool any = false;
    for (std::size_t reason = 0; reason < Reasons; ++reason) {
        if (report.reasons[reason] == 0) continue;
        std::snprintf(item, sizeof(item), "%s %s %llu %.1f ms", any ? "," : "", MonolithicReasonNames[reason], static_cast<unsigned long long>(report.reasons[reason]), report.reasonMs[reason]);
        text += item;
        any = true;
    }
    if (!any) text += " none";
    text += "; in-flight parts waited for:";
    bool waited = false;
    for (std::size_t side = 2; side-- > 0;) {
        for (std::size_t part = 0; part < 4; ++part) {
            if (report.waited[side][part] == 0) continue;
            std::snprintf(item, sizeof(item), "%s %s %s %llu %.1f ms", waited ? "," : "", side == 1 ? "in committed draws" : "outside draws", PipelineCreateKindNames[part], static_cast<unsigned long long>(report.waited[side][part]), report.waitedMs[side][part]);
            text += item;
            waited = true;
        }
    }
    if (!waited) text += " none";
    std::snprintf(item, sizeof(item), "; duplicate parts built %llu", static_cast<unsigned long long>(report.duplicates));
    text += item;
    text += "; part misses in committed draws by cause:";
    for (std::size_t part = 0; part < 2; ++part) {
        text += part == 0 ? " " : "; ";
        text += PipelineCreateKindNames[part + 1];
        for (std::size_t cause = 0; cause < Causes; ++cause) {
            std::snprintf(item, sizeof(item), "%s %s %llu", cause == 0 ? "" : ",", PartMissCauseNames[cause], static_cast<unsigned long long>(report.missCauses[part][cause]));
            text += item;
        }
    }
    std::snprintf(item, sizeof(item), "; parts built outside draws first used in a committed draw: pre-raster part %llu, fragment part %llu", static_cast<unsigned long long>(report.prebuiltUsed[0]), static_cast<unsigned long long>(report.prebuiltUsed[1]));
    text += item;
    if (total == 0) text += " (no pipelines created)";
    return text;
}

}
