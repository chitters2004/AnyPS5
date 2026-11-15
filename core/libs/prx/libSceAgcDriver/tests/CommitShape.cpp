#include "prx/libSceAgcDriver/Graphics/include/CommitShape.hpp"

#include <array>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>

namespace {

using AgcDriver::CommitShape;
using AgcDriver::CommitStamps;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::array<double, CommitShape::Phases> NoPhases{};
constexpr std::array<const char*, CommitShape::Kinds> KindNames{"recipe hit", "template hit", "build", "BDA"};
constexpr std::array<const char*, CommitShape::Phases> PhaseNames{"validate", "vertex", "setup", "readTarget", "prepare", "lookup", "resources", "pipeline", "record", "keep", "sync", "writeBack", "describe"};

std::size_t DistanceIndex(std::size_t distance) {
    for (std::size_t index = 0; index < CommitShape::Distances.size(); ++index) {
        if (CommitShape::Distances[index] == distance) return index;
    }
    throw std::runtime_error("no such distance");
}

void UnchangedStampsSurviveEveryDistance() {
    CommitShape shape;
    const CommitStamps stamps{1, 2, 3, 4, 5, 6, 7, 8, 9};
    for (int draw = 0; draw < 20; ++draw) shape.Note(1, NoPhases, 0, stamps);
    const auto report = shape.Take();
    for (const auto distance : CommitShape::Distances) {
        const auto index = DistanceIndex(distance);
        Require(report.compared[index] == 20 - distance, "distance " + std::to_string(distance) + " compared " + std::to_string(report.compared[index]) + " draws instead of " + std::to_string(20 - distance));
        Require(report.survived[index] == report.compared[index], "unchanged stamps did not survive distance " + std::to_string(distance));
        for (const auto moved : report.moved[index]) Require(moved == 0, "an unchanged stamp was counted as moved");
    }
}

void AlternatingStampSurvivesEvenDistancesOnly() {
    CommitShape shape;
    for (int draw = 0; draw < 20; ++draw) {
        CommitStamps stamps{};
        stamps[AgcDriver::StampPublished] = static_cast<std::uint64_t>(draw % 2);
        shape.Note(1, NoPhases, 0, stamps);
    }
    const auto report = shape.Take();
    const auto one = DistanceIndex(1);
    Require(report.survived[one] == 0, "a stamp that changes every draw survived distance 1");
    Require(report.moved[one][AgcDriver::StampPublished] == 19, "the alternating stamp was not counted as moved at distance 1 for each of 19 draws");
    for (std::size_t stamp = 0; stamp < AgcDriver::StampCount; ++stamp) {
        if (stamp != AgcDriver::StampPublished) Require(report.moved[one][stamp] == 0, "a constant stamp was counted as moved");
    }
    for (const auto distance : {2u, 4u, 8u, 16u}) {
        const auto index = DistanceIndex(distance);
        Require(report.survived[index] == report.compared[index] && report.compared[index] == 20 - distance, "the alternating stamp did not survive even distance " + std::to_string(distance));
    }
}

void DistanceSixteenReachesTheOldestHeldDraw() {
    CommitShape shape;
    for (int draw = 0; draw < 40; ++draw) {
        CommitStamps stamps{};
        stamps[AgcDriver::StampEpoch] = draw >= 20 ? 1 : 0;
        shape.Note(1, NoPhases, 0, stamps);
    }
    const auto report = shape.Take();
    const auto sixteen = DistanceIndex(16);
    Require(report.compared[sixteen] == 24, "distance 16 compared " + std::to_string(report.compared[sixteen]) + " draws instead of 24");
    Require(report.moved[sixteen][AgcDriver::StampEpoch] == 16, "an epoch bump at draw 20 moved " + std::to_string(report.moved[sixteen][AgcDriver::StampEpoch]) + " draws at distance 16 instead of draws 20 to 35");
    Require(report.survived[sixteen] == 8, "distance 16 survived " + std::to_string(report.survived[sixteen]) + " draws instead of 8");
    const auto one = DistanceIndex(1);
    Require(report.moved[one][AgcDriver::StampEpoch] == 1, "one epoch bump moved more than one draw at distance 1");
}

void PhasesAndDependenciesSumPerKind() {
    CommitShape shape;
    std::array<double, CommitShape::Phases> phases{};
    phases[5] = 3.0;
    phases[8] = 1.5;
    shape.Note(1, phases, 0, {});
    shape.Note(1, phases, 2, {});
    shape.Note(2, phases, 1, {});
    shape.Note(CommitShape::Kinds, phases, 0, {});
    const auto report = shape.Take();
    Require(report.draws == 4, "the draw count missed a draw");
    Require(report.kindDraws[1] == 2 && report.kindDraws[2] == 1 && report.kindDraws[0] == 0 && report.kindDraws[3] == 0, "draws were counted under the wrong kind");
    Require(report.kindPhaseUs[1][5] == 6.0 && report.kindPhaseUs[1][8] == 3.0 && report.kindPhaseUs[2][5] == 3.0, "phase time was not summed per kind");
    Require(report.dependentDraws == 2 && report.dependencies == 3, "dependent draws or their hits were miscounted");
    const auto line = CommitShape::Line(report, KindNames, PhaseNames);
    Require(line.find("[commit-shape] 10 s: 4 draws") == 0, "the line does not start with the draw count: " + line);
    Require(line.find("template hit 2 x 4.50 us: lookup 3.00 record 1.50;") != std::string::npos, "the template kind's phases are missing from the line: " + line);
    Require(line.find("dependent 2 (50.0%), 3 hits;") != std::string::npos, "the dependencies are missing from the line: " + line);
}

void TakeResetsCountsButKeepsHistory() {
    CommitShape shape;
    const CommitStamps stamps{1, 1, 1, 1, 1, 1, 1, 1, 1};
    shape.Note(1, NoPhases, 1, stamps);
    shape.Note(1, NoPhases, 1, stamps);
    static_cast<void>(shape.Take());
    shape.Note(1, NoPhases, 0, stamps);
    const auto report = shape.Take();
    Require(report.draws == 1 && report.dependentDraws == 0, "Take did not reset the counts");
    Require(report.compared[DistanceIndex(1)] == 1 && report.compared[DistanceIndex(2)] == 1 && report.compared[DistanceIndex(4)] == 0, "a draw after Take did not compare against the draws before it");
}

}

int main() {
    try {
        UnchangedStampsSurviveEveryDistance();
        AlternatingStampSurvivesEvenDistancesOnly();
        DistanceSixteenReachesTheOldestHeldDraw();
        PhasesAndDependenciesSumPerKind();
        TakeResetsCountsButKeepsHistory();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "commit shape: %s\n", error.what());
        return 1;
    }
    std::puts("commit shape: ok");
    return 0;
}
