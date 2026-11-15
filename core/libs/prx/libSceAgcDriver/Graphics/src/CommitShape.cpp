#include "prx/libSceAgcDriver/Graphics/include/CommitShape.hpp"

#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace AgcDriver {

bool CommitShapeProfiled() {
    static const bool profiled = std::getenv("APS5_PROFILE_COMMIT_SHAPE") != nullptr;
    return profiled;
}

void CommitShape::Note(std::size_t kind, std::span<const double, Phases> phaseUs, std::uint64_t dependencies, const CommitStamps& stamps) {
    std::lock_guard lock(mutex);
    ++counts.draws;
    if (kind < Kinds) {
        ++counts.kindDraws[kind];
        for (std::size_t phase = 0; phase < Phases; ++phase) counts.kindPhaseUs[kind][phase] += phaseUs[phase];
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
    constexpr std::array<const char*, StampCount> stampNames{"epoch", "proofs", "pending", "unwatch", "allocations", "surfaces", "released", "claims", "published"};
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

}
