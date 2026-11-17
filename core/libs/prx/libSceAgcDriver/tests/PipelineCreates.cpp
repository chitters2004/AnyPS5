#include "prx/libSceAgcDriver/Graphics/include/PipelineCreates.hpp"

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>

namespace {

using AgcDriver::MonolithicReason;
using AgcDriver::PipelineCacheOutcome;
using AgcDriver::PipelineCreateKind;
using AgcDriver::PipelineCreates;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::size_t Index(PipelineCreateKind kind) {
    return static_cast<std::size_t>(kind);
}

void CountsBySideKindAndOutcome() {
    PipelineCreates creates;
    creates.Note(PipelineCreateKind::FragmentShaderPart, true, PipelineCacheOutcome::Miss, 120.0);
    creates.Note(PipelineCreateKind::FragmentShaderPart, true, PipelineCacheOutcome::Hit, 2.0);
    creates.Note(PipelineCreateKind::Link, true, PipelineCacheOutcome::Unknown, 0.5);
    creates.Note(PipelineCreateKind::OptimizedRelink, false, PipelineCacheOutcome::Miss, 40.0);
    creates.Note(PipelineCreateKind::Monolithic, true, PipelineCacheOutcome::Miss, 200.0, MonolithicReason::RectList);
    creates.Note(PipelineCreateKind::Monolithic, true, PipelineCacheOutcome::Hit, 1.0, MonolithicReason::RectList);
    creates.Note(PipelineCreateKind::Count, true, PipelineCacheOutcome::Hit, 1.0);
    const auto report = creates.Take();
    const auto& fragment = report.bySite[1][Index(PipelineCreateKind::FragmentShaderPart)];
    Require(fragment.count == 2 && fragment.hits == 1 && fragment.misses == 1 && fragment.unknown == 0, "fragment part counts are wrong");
    Require(fragment.ms == 122.0 && fragment.longestMs == 120.0, "fragment part times are wrong");
    Require(report.bySite[1][Index(PipelineCreateKind::Link)].unknown == 1, "a create without feedback is not counted as unknown");
    Require(report.bySite[0][Index(PipelineCreateKind::OptimizedRelink)].count == 1 && report.bySite[1][Index(PipelineCreateKind::OptimizedRelink)].count == 0, "a create outside draws is counted in them");
    const auto rect = static_cast<std::size_t>(MonolithicReason::RectList);
    Require(report.reasons[rect] == 2 && report.reasonMs[rect] == 201.0, "monolithic reasons are not counted");
    creates.NoteWait(Index(PipelineCreateKind::FragmentShaderPart), true, 30.0);
    creates.NoteWait(Index(PipelineCreateKind::FragmentShaderPart), true, 12.0);
    creates.NoteWait(Index(PipelineCreateKind::VertexInputPart), false, 1.5);
    creates.NoteWait(4, true, 1.0);
    creates.NoteDuplicate();
    const auto waits = creates.Take();
    Require(waits.waited[1][Index(PipelineCreateKind::FragmentShaderPart)] == 2 && waits.waitedMs[1][Index(PipelineCreateKind::FragmentShaderPart)] == 42.0 && waits.waited[0][Index(PipelineCreateKind::VertexInputPart)] == 1 && waits.duplicates == 1, "in-flight waits or duplicates are not counted by side and part");
    const auto waitLine = PipelineCreates::Line(waits);
    Require(waitLine.find("; in-flight parts waited for: in committed draws fragment part 2 42.0 ms, outside draws vertex-input part 1 1.5 ms; duplicate parts built 1") != std::string::npos, "the line does not give the in-flight waits and duplicates: " + waitLine);
    const auto again = creates.Take();
    Require(again.bySite[1][Index(PipelineCreateKind::FragmentShaderPart)].count == 0 && again.reasons[rect] == 0, "Take does not reset the window");
    const auto line = PipelineCreates::Line(report);
    Require(line.rfind("[pipeline-creates] 10 s: in committed draws 5, ", 0) == 0, "the line does not start with the creates in committed draws: " + line);
    Require(line.find("fragment part 2 (cache hit 1, miss 1, no feedback 0) 122.0 ms, longest 120.0 ms") != std::string::npos, "the line does not give the fragment part cell: " + line);
    Require(line.find("outside draws 1, optimized relink 1 (cache hit 0, miss 1, no feedback 0) 40.0 ms") != std::string::npos, "the line does not give the creates outside draws: " + line);
    Require(line.find("monolithic because: rect list 2 201.0 ms") != std::string::npos, "the line does not give the monolithic reasons: " + line);
    Require(line.find("no pipelines created") == std::string::npos, "a window with creates says none were created");
}

void EmptyWindowSaysSo() {
    PipelineCreates creates;
    const auto line = PipelineCreates::Line(creates.Take());
    Require(line == "[pipeline-creates] 10 s: in committed draws 0; outside draws 0; monolithic because: none; in-flight parts waited for: none; duplicate parts built 0 (no pipelines created)", "an empty window prints: " + line);
}

}

int main() {
    try {
        CountsBySideKindAndOutcome();
        EmptyWindowSaysSo();
        std::puts("pipeline creates tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
