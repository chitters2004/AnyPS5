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
    if (total == 0) text += " (no pipelines created)";
    return text;
}

}
