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
    const CommitStamps stamps{1, 2, 3, 4, 5, 6, 7, 8};
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
    const CommitStamps stamps{1, 1, 1, 1, 1, 1, 1, 1};
    shape.Note(1, NoPhases, 1, stamps);
    shape.Note(1, NoPhases, 1, stamps);
    static_cast<void>(shape.Take());
    shape.Note(1, NoPhases, 0, stamps);
    const auto report = shape.Take();
    Require(report.draws == 1 && report.dependentDraws == 0, "Take did not reset the counts");
    Require(report.compared[DistanceIndex(1)] == 1 && report.compared[DistanceIndex(2)] == 1 && report.compared[DistanceIndex(4)] == 0, "a draw after Take did not compare against the draws before it");
}

void BuildsSumPerReasonAndPipelinesPerKind() {
    CommitShape shape;
    std::array<double, CommitShape::Phases> phases{};
    phases[6] = 100.0;
    phases[7] = 300.0;
    shape.Note(2, phases, 0, {}, 1, AgcDriver::BuildReturnedKey);
    shape.Note(2, phases, 0, {}, 0, AgcDriver::BuildReturnedKey);
    shape.Note(3, phases, 0, {}, 2, AgcDriver::BuildNewKey);
    shape.Note(1, phases, 0, {}, 1);
    shape.Note(1, phases, 0, {});
    const auto report = shape.Take();
    Require(report.reasonBuilds[AgcDriver::BuildReturnedKey] == 2 && report.reasonBuilds[AgcDriver::BuildNewKey] == 1, "builds were counted under the wrong reason");
    Require(report.reasonBuilds[AgcDriver::BuildUncacheable] == 0 && report.reasonBuilds[AgcDriver::BuildInvalidated] == 0, "a draw without a build reason was counted as a build");
    Require(report.reasonPhaseUs[AgcDriver::BuildReturnedKey][7] == 600.0 && report.reasonPhaseUs[AgcDriver::BuildNewKey][6] == 100.0, "phase time was not summed per build reason");
    Require(report.reasonPipelines[AgcDriver::BuildReturnedKey] == 1 && report.reasonPipelineDraws[AgcDriver::BuildReturnedKey] == 1 && report.reasonPipelines[AgcDriver::BuildNewKey] == 2 && report.reasonPipelineDraws[AgcDriver::BuildNewKey] == 1, "pipelines created were not counted per build reason");
    Require(report.kindPipelines[1] == 1 && report.kindPipelineDraws[1] == 1 && report.kindPipelines[2] == 1 && report.kindPipelines[3] == 2, "pipelines created were not counted per kind");
    const auto line = CommitShape::BuildLine(report, KindNames, 6, 7);
    Require(line.find("[draw-builds] 10 s: 3 builds;") == 0, "the build line does not start with the build count: " + line);
    Require(line.find(" new key 1 x 400.00 us (resources 100.00, pipeline 300.00), 2 pipelines created in 1;") != std::string::npos, "the new-key builds are missing from the line: " + line);
    Require(line.find(" evicted key returned 2 x 400.00 us (resources 100.00, pipeline 300.00), 1 pipelines created in 1;") != std::string::npos, "the returned-key builds are missing from the line: " + line);
    Require(line.find("uncacheable") == std::string::npos, "a reason without builds is in the line: " + line);
    Require(line.find("template hit 1 in 1 of 2 draws") != std::string::npos, "the template hits' pipelines are missing from the line: " + line);
}

}

void ProofLeadSplitsSurvivalPurityAndLead() {
    CommitShape shape;
    using Lead = AgcDriver::ProofLeadNote;
    const auto note = [&](const Lead& lead) { shape.Note(1, NoPhases, 0, {}, 0, AgcDriver::BuildReasonCount, lead); };
    note({.looked = true, .stamped = true, .lead = 0, .survived = true, .walked = true, .lookups = true, .served = true, .effects = 0});
    note({.looked = true, .stamped = true, .lead = 3, .survived = false, .epochAdvanced = true, .walked = true, .lookups = false, .served = true, .effects = 1u << AgcDriver::EffectUpload});
    note({.looked = true, .stamped = true, .lead = 40, .survived = true, .walked = true, .lookups = false, .served = true, .effects = (1u << AgcDriver::EffectStaging) | (1u << AgcDriver::EffectUpload)});
    note({.looked = true, .stamped = false, .walked = true, .served = true, .effects = 0});
    note({.looked = true, .stamped = true, .lead = 8, .survived = true, .walked = true, .served = false, .ending = AgcDriver::EndingFull, .revalidateUs = 30.0});
    note({.looked = true, .stamped = true, .lead = 9, .survived = true, .walked = false, .served = true, .ending = AgcDriver::EndingEpoch, .revalidateUs = 2.0});
    note({.looked = false, .ending = AgcDriver::EndingFast, .revalidateUs = 100.0});
    note({});
    const auto report = shape.Take();
    Require(report.leadLooked == 6 && report.leadStamped == 5 && report.leadSurvived == 4 && report.leadEpochAdvanced == 1, "the lookups, stamps and survivals were miscounted");
    Require(report.leadBuckets[0] == 1 && report.leadBuckets[1] == 1 && report.leadBuckets[2] == 1 && report.leadBuckets[3] == 1 && report.leadBuckets[4] == 0 && report.leadBuckets[5] == 1, "the leads fell in the wrong buckets");
    Require(report.walked == 5 && report.walkedLookups == 1 && report.walkedServed == 4 && report.servedStamped == 3, "the walks were miscounted");
    Require(report.servedSurvived == 2 && report.servedPure == 2 && report.servedPureSurvived == 1, "the served walks' survival and purity were miscounted");
    Require(report.servedEffects[AgcDriver::EffectUpload] == 2 && report.servedEffects[AgcDriver::EffectStaging] == 1 && report.servedEffects[AgcDriver::EffectDepthPlanes] == 0, "the effects were miscounted");
    const auto line = CommitShape::ProofLeadLine(report);
    Require(line.find("[proof-lead] 10 s: 6 template lookups, 5 stamped at prepare (83.3%), proof, pending and retired serials unchanged since prepare 80.0%, epoch token advanced since prepare 20.0%;") == 0, "the line does not start with the lookups: " + line);
    Require(line.find("lead in draws 0: 20.0%, 1-4: 20.0%, 5-8: 20.0%, 9-16: 20.0%, 17-32: 0.0%, >32: 20.0%;") != std::string::npos, "the lead distribution is wrong: " + line);
    Require(line.find("full walks 5 (1 with lookups), served 4 (3 stamped): serials unchanged since prepare 66.7% of stamped, pure 50.0%, pure and unchanged 33.3% of stamped;") != std::string::npos, "the served walks are wrong: " + line);
    Require(line.find(" upload 50.0%, pending flush 0.0%, staging 25.0%,") != std::string::npos, "the effects are wrong: " + line);
    Require(report.endings[AgcDriver::EndingFull] == 1 && report.endings[AgcDriver::EndingEpoch] == 1 && report.endings[AgcDriver::EndingFast] == 0, "the revalidate endings were miscounted");
    Require(line.find("; revalidate epoch exit 1 x 2.00 us (6.2% of the time), fast-ending 0 x 0.00 us (0.0% of the time), full-walk-ending 1 x 30.00 us (93.8% of the time), refused before the walk 0 x 0.00 us (0.0% of the time)") != std::string::npos, "the revalidate split is wrong: " + line);
}

void ProofDeclineCrossesDeclineCausesWithEndingsAndFastFails() {
    CommitShape shape;
    using Lead = AgcDriver::ProofLeadNote;
    const auto note = [&](const Lead& lead) { shape.Note(1, NoPhases, 0, {}, 0, AgcDriver::BuildReasonCount, lead); };
    note({.looked = true, .walked = false, .ending = AgcDriver::EndingEpoch, .revalidateUs = 0.5, .decline = AgcDriver::DeclineTaken});
    note({.looked = true, .walked = true, .ending = AgcDriver::EndingFast, .revalidateUs = 4.0, .decline = AgcDriver::DeclinePending, .declineAlone = true, .fastFail = 2});
    note({.looked = true, .walked = true, .served = true, .ending = AgcDriver::EndingFull, .revalidateUs = 50.0, .decline = AgcDriver::DeclinePending, .declineAlone = true, .fastFail = 1, .collectSource = AgcDriver::CollectUnwatched, .unwatchCause = static_cast<std::size_t>(AgcDriver::GuestMemory::UnwatchCause::ImportUnwatched)});
    note({.looked = true, .walked = true, .ending = AgcDriver::EndingFull, .revalidateUs = 30.0, .decline = AgcDriver::DeclineProof, .fastFail = 2});
    note({.looked = true, .walked = true, .ending = AgcDriver::EndingFull, .revalidateUs = 20.0, .decline = AgcDriver::DeclineNeverProved});
    note({.looked = true, .walked = true, .ending = AgcDriver::EndingFull, .revalidateUs = 7.0});
    const auto report = shape.Take();
    Require(report.declineEndings[AgcDriver::DeclinePending][AgcDriver::EndingFast] == 1 && report.declineEndings[AgcDriver::DeclinePending][AgcDriver::EndingFull] == 1 && report.declineAlone[AgcDriver::DeclinePending] == 2, "the pending serial row was miscounted");
    Require(report.fullFastFails[1] == 1 && report.fullFastFails[2] == 1 && report.fullFastFails[AgcDriver::FastFailSlots] == 2 && report.fullFastFailServed[1] == 1, "a fast-ending proof's reason or a full walk's reason was miscounted");
    const std::array<const char*, 3> names{"no record", "collect", "pending image"};
    const auto line = CommitShape::ProofDeclineLine(report, names);
    const std::string expected = "[proof-decline] 10 s: revalidations by why the epoch exit declined, as epoch exit / fast-ending / full-walk-ending / refused before the walk (count x us): exit taken 1 x 0.50 / 0 x 0.00 / 0 x 0.00 / 0 x 0.00 (0.0% of full-walk time); never proved 0 x 0.00 / 0 x 0.00 / 1 x 20.00 / 0 x 0.00 (18.7% of full-walk time); proof serial moved (alone 0) 0 x 0.00 / 0 x 0.00 / 1 x 30.00 / 0 x 0.00 (28.0% of full-walk time); pending serial moved (alone 2) 0 x 0.00 / 1 x 4.00 / 1 x 50.00 / 0 x 0.00 (46.7% of full-walk time); full walks by the first failing fast check: collect 1 x 50.00 us, 1 served (46.7% of full-walk time), pending image 1 x 30.00 us, 0 served (28.0% of full-walk time), no fast proof ran 2 x 13.50 us, 0 served (25.2% of full-walk time); collect fails behind full walks by source: uncommitted pages 0 x 0.00 us, unwatched 1 x 50.00 us, other 0 x 0.00 us; unwatched collects behind full walks by cause: import that stays unwatched 1 x 50.00 us";
    Require(line == expected, "the decline line is wrong:\n" + line + "\nexpected:\n" + expected);
}

void ProofLeadKeepsTokensOnlyWithinTheCollectEpoch() {
    CommitShape shape;
    using Lead = AgcDriver::ProofLeadNote;
    const auto note = [&](const Lead& lead) { shape.Note(1, NoPhases, 0, {}, 0, AgcDriver::BuildReasonCount, lead); };
    note({.looked = true, .stamped = true, .survived = true, .walked = true, .ending = AgcDriver::EndingFull, .revalidateUs = 50.0, .collectSource = AgcDriver::CollectUnwatched});
    note({.looked = true, .stamped = true, .survived = true, .walked = true, .ending = AgcDriver::EndingFast, .revalidateUs = 4.0, .collectAdvanced = true, .collectSource = AgcDriver::CollectOther});
    note({.looked = true, .stamped = true, .survived = false, .walked = true, .ending = AgcDriver::EndingFull, .revalidateUs = 0.0, .collectSource = AgcDriver::CollectUnwatched});
    note({.looked = true, .stamped = false, .walked = true, .ending = AgcDriver::EndingFull, .revalidateUs = 0.0, .collectSource = AgcDriver::CollectUnwatched});
    note({.looked = true, .stamped = true, .survived = false, .walked = true, .ending = AgcDriver::EndingFast, .revalidateUs = 6.0});
    note({.looked = true, .stamped = true, .survived = true, .walked = false, .ending = AgcDriver::EndingEpoch, .revalidateUs = 1.0, .collectAdvanced = true});
    note({.looked = true, .stamped = false, .walked = true, .ending = AgcDriver::EndingFull, .revalidateUs = 40.0, .collectAdvanced = true});
    const auto report = shape.Take();
    Require(report.leadCollectAdvanced == 2, "stamped draws whose collect epoch advanced were miscounted");
    Require(report.walkedStamped == 4 && report.walkedCollectAdvanced == 1 && report.walkedKept == 1, "the stamped walks and their kept tokens were miscounted");
    Require(report.walkedUnchanged == 2 && report.walkedUnchangedUs == 54.0 && report.walkedUnchangedUnwatched == 1 && report.walkedUnchangedUnwatchedUs == 50.0, "the unchanged stamped walks over unwatched ranges were miscounted");
    Require(report.walkedStampedUs == 60.0 && report.walkedKeptUs == 50.0, "the stamped and kept walk time was miscounted");
    const auto line = CommitShape::ProofLeadLine(report);
    Require(line.find("; stamped draws whose collect epoch advanced since prepare 40.0%; walked and stamped 4: collect epoch advanced 25.0%, token kept (serials unchanged and collect epoch not advanced) 25.0%, walk time 0.1 ms, kept 0.1 ms (83.3%); walked, stamped and serials unchanged 2 (0.1 ms): 1 over unwatched ranges (0.1 ms, 92.6% of their walk time)") != std::string::npos, "the collect epoch clause is wrong: " + line);
}

int main() {
    try {
        UnchangedStampsSurviveEveryDistance();
        AlternatingStampSurvivesEvenDistancesOnly();
        DistanceSixteenReachesTheOldestHeldDraw();
        PhasesAndDependenciesSumPerKind();
        TakeResetsCountsButKeepsHistory();
        BuildsSumPerReasonAndPipelinesPerKind();
        ProofLeadSplitsSurvivalPurityAndLead();
        ProofLeadKeepsTokensOnlyWithinTheCollectEpoch();
        ProofDeclineCrossesDeclineCausesWithEndingsAndFastFails();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "commit shape: %s\n", error.what());
        return 1;
    }
    std::puts("commit shape: ok");
    return 0;
}
