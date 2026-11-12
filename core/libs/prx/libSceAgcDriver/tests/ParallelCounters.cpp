#include "prx/libSceAgcDriver/Graphics/include/CommitPhases.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using AgcDriver::Graphics::CommitCounter;
using AgcDriver::Graphics::CommitDrawKind;
using AgcDriver::Graphics::CommitPhase;
using AgcDriver::Graphics::CommitPhaseDraw;
using AgcDriver::Graphics::CommitPhaseTotals;
using AgcDriver::Graphics::Recorder;
using AgcDriver::Graphics::ShadowReason;

void Check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::size_t Index(ShadowReason reason) {
    return static_cast<std::size_t>(reason);
}

CommitPhaseTotals Draw(const std::function<void()>& body) {
    const auto before = AgcDriver::Graphics::ReadCommitPhases();
    {
        const CommitPhaseDraw draw;
        body();
    }
    return AgcDriver::Graphics::ReadCommitPhases() - before;
}

void RequireEligible(const CommitPhaseTotals& window, const std::string& what) {
    Check(window.draws == 1, what + ": the draw was not counted");
    Check(window.shadowEligible[static_cast<std::size_t>(CommitDrawKind::Full)] == 1, what + ": the draw was not counted as worker-eligible");
    for (std::size_t reason = 0; reason < window.shadowFirst.size(); ++reason) Check(window.shadowFirst[reason] == 0 && window.shadowAny[reason] == 0, what + ": an eligible draw counted a reason");
}

void RequireFirst(const CommitPhaseTotals& window, ShadowReason first, const std::string& what) {
    Check(window.draws == 1, what + ": the draw was not counted");
    std::uint64_t eligible = 0;
    for (const auto count : window.shadowEligible) eligible += count;
    Check(eligible == 0, what + ": a draw with a reason was counted as worker-eligible");
    for (std::size_t reason = 0; reason < window.shadowFirst.size(); ++reason) Check(window.shadowFirst[reason] == (reason == Index(first) ? 1u : 0u), what + ": wrong first reason");
    Check(window.shadowAny[Index(first)] == 1, what + ": the first reason is missing from the any-reason counts");
}

void TestEligibleDraw() {
    RequireEligible(Draw([] {}), "an empty draw");
    RequireEligible(Draw([] { AgcDriver::Graphics::CountCommit(CommitCounter::TargetRefreshEpoch); }), "a draw whose targets hit their epoch proof");
    RequireEligible(Draw([] { AgcDriver::Graphics::CountCommit(CommitCounter::TargetRefreshSlow, 0); }), "a draw counting zero slow refreshes");
}

void TestFirstAndAnyReasons() {
    const auto window = Draw([] {
        AgcDriver::Graphics::NoteShadowReason(ShadowReason::Waited);
        AgcDriver::Graphics::NoteShadowReason(ShadowReason::AddressBased);
        AgcDriver::Graphics::NoteShadowReason(ShadowReason::Waited);
    });
    RequireFirst(window, ShadowReason::AddressBased, "an address-based draw that waited");
    Check(window.shadowAny[Index(ShadowReason::Waited)] == 1, "a second reason is missing from the any-reason counts");
    Check(AgcDriver::Graphics::FirstShadowReason(0) == ShadowReason::Count, "no reason must give Count");
    Check(AgcDriver::Graphics::FirstShadowReason((1u << Index(ShadowReason::Waited)) | (1u << Index(ShadowReason::TemplateBuilt))) == ShadowReason::TemplateBuilt, "the first reason must be the lowest one");
}

void TestCommandsBeforeRecordingArePreOps() {
    RequireFirst(Draw([] { AgcDriver::Graphics::NoteShadowCommands(); }), ShadowReason::PreOp, "commands recorded during resolve");
    RequireEligible(Draw([] {
        AgcDriver::Graphics::NoteShadowRecording();
        AgcDriver::Graphics::NoteShadowCommands();
    }), "commands recorded by the draw's own recording");
    RequireEligible(Draw([] {}), "a draw after a pre-op draw");
}

void TestPhaseAndCounterHooks() {
    RequireFirst(Draw([] { AgcDriver::Graphics::InCommitPhase(CommitPhase::ResolveBuild, [] {}); }), ShadowReason::TemplateBuilt, "a template build");
    RequireFirst(Draw([] { AgcDriver::Graphics::BeginCommitPhase(CommitPhase::RevalidateWalk); }), ShadowReason::TemplateWalk, "a revalidation walk");
    RequireFirst(Draw([] { AgcDriver::Graphics::NoteShadowReason(ShadowReason::TargetMiss); }), ShadowReason::TargetMiss, "a target miss");
    for (auto counter = static_cast<std::size_t>(CommitCounter::TargetMemoAccept); counter <= static_cast<std::size_t>(CommitCounter::TargetMemoAvoidedRefreshLookups); ++counter) {
        RequireEligible(Draw([counter] { AgcDriver::Graphics::CountCommit(static_cast<CommitCounter>(counter)); }), "target memo counter " + std::to_string(counter) + " counted as a target miss");
    }
    RequireFirst(Draw([] { AgcDriver::Graphics::CountCommit(CommitCounter::TargetRefreshSlow); }), ShadowReason::TargetSlow, "a slow target refresh");
    RequireFirst(Draw([] { AgcDriver::Graphics::CountCommit(CommitCounter::OtherRefreshSlow); }), ShadowReason::TargetSlow, "a slow texture refresh");
    RequireFirst(Draw([] { AgcDriver::Graphics::CountCommit(CommitCounter::InputIndexCopyFresh); }), ShadowReason::InputFresh, "a fresh index copy");
    RequireFirst(Draw([] { AgcDriver::Graphics::CountCommit(CommitCounter::InputVertexCopyFresh); }), ShadowReason::InputFresh, "a fresh vertex copy");
}

void TestNotesOutsideADrawAreIgnored() {
    const auto before = AgcDriver::Graphics::ReadCommitPhases();
    AgcDriver::Graphics::NoteShadowReason(ShadowReason::TemplateBuilt);
    AgcDriver::Graphics::NoteShadowCommands();
    AgcDriver::Graphics::CountCommit(CommitCounter::TargetRefreshSlow);
    const auto outside = AgcDriver::Graphics::ReadCommitPhases() - before;
    for (std::size_t reason = 0; reason < outside.shadowAny.size(); ++reason) Check(outside.shadowAny[reason] == 0, "a note outside a draw was counted");
    RequireEligible(Draw([] {}), "a draw after notes made outside any draw");
}

void TestShadowReport() {
    const auto window = Draw([] { AgcDriver::Graphics::NoteShadowReason(ShadowReason::StoreFlush); });
    const auto report = AgcDriver::Graphics::FormatShadowReport(window);
    Check(report.rfind("[parallel-shadow] (10 s) 1 draws, 0 worker-eligible (0.0%)", 0) == 0, "shadow report header: " + report);
    Check(report.find("first reason: template built 0,") != std::string::npos, "shadow report first reasons: " + report);
    Check(report.find("store flush 1") != std::string::npos, "shadow report store flush: " + report);
    Check(std::string(AgcDriver::Graphics::ShadowReasonName(ShadowReason::Waited)) == "waited", "the last reason name is out of order");
}

void TestBatchBuckets() {
    const std::pair<std::uint64_t, std::size_t> cases[] = {{0, 0}, {1, 1}, {2, 2}, {3, 2}, {4, 3}, {7, 3}, {8, 4}, {15, 4}, {16, 5}, {31, 5}, {32, 6}, {63, 6}, {64, 7}, {65, 7}, {100000, 7}};
    for (const auto& [count, bucket] : cases) Check(Recorder::BatchBucket(count) == bucket, "batch bucket of " + std::to_string(count));
}

void TestBatchStatistics() {
    Recorder::BatchStatistics statistics;
    statistics.Add(Recorder::SubmitCause::WorkCap, 64, 3, 64);
    const auto earlier = statistics;
    statistics.maxima = {};
    statistics.Add(Recorder::SubmitCause::LabelDeadline, 5, 2, 7);
    statistics.Add(Recorder::SubmitCause::SyncPendingWrite, 0, 0, 1);
    const auto window = Recorder::BatchWindow(statistics, earlier);
    Check(window.batches == 2, "batch window count");
    Check(window.causes[static_cast<std::size_t>(Recorder::SubmitCause::WorkCap)] == 0, "batch window kept an earlier cause");
    Check(window.causes[static_cast<std::size_t>(Recorder::SubmitCause::LabelDeadline)] == 1 && window.causes[static_cast<std::size_t>(Recorder::SubmitCause::SyncPendingWrite)] == 1, "batch window causes");
    Check(window.totals[0] == 5 && window.totals[1] == 2 && window.totals[2] == 8, "batch window totals");
    Check(window.maxima[0] == 5 && window.maxima[1] == 2 && window.maxima[2] == 7, "batch window maxima");
    Check(window.histograms[0][0] == 1 && window.histograms[0][3] == 1 && window.histograms[0][7] == 0, "batch window draw histogram");
    const auto report = Recorder::FormatBatchReport(window);
    Check(report.rfind("[parallel-batches] (10 s) 2 batches; draws per batch: mean 2.50, max 5, histogram 0:1 1:0 2-3:0 4-7:1", 0) == 0, "batch report draws: " + report);
    Check(report.find("closed by: other 0, label deadline 1, work cap 0,") != std::string::npos, "batch report causes: " + report);
    Check(report.find("sync pending write 1") != std::string::npos, "batch report sync cause: " + report);
}

void TestSubmitCauseScope() {
    Check(Recorder::AnnouncedSubmitCause() == Recorder::SubmitCause::Other, "the default submit cause");
    {
        const Recorder::SubmitCauseScope outer(Recorder::SubmitCause::Wait);
        Check(Recorder::AnnouncedSubmitCause() == Recorder::SubmitCause::Wait, "an announced submit cause");
        {
            const Recorder::SubmitCauseScope inner(Recorder::SubmitCause::SyncIdle);
            Check(Recorder::AnnouncedSubmitCause() == Recorder::SubmitCause::SyncIdle, "a nested submit cause");
        }
        Check(Recorder::AnnouncedSubmitCause() == Recorder::SubmitCause::Wait, "a nested submit cause was not restored");
    }
    Check(Recorder::AnnouncedSubmitCause() == Recorder::SubmitCause::Other, "the submit cause was not restored");
    Check(std::string(Recorder::SubmitCauseName(Recorder::SubmitCause::SyncOther)) == "sync other", "the last submit cause name is out of order");
    const Recorder::SubmitCause syncs[] = {Recorder::SubmitCause::SyncIdle, Recorder::SubmitCause::SyncPendingWrite, Recorder::SubmitCause::SyncRecordedStore, Recorder::SubmitCause::SyncAddressBased, Recorder::SubmitCause::SyncOther};
    for (int source = 0; source < 5; ++source) Check(Recorder::SyncSubmitCause(source) == syncs[source], "the submit cause of sync source " + std::to_string(source));
    Check(Recorder::SyncSubmitCause(-1) == Recorder::SubmitCause::SyncOther && Recorder::SyncSubmitCause(9) == Recorder::SubmitCause::SyncOther, "an unknown sync source must count as other");
}

}

int main() {
    try {
        Check(AgcDriver::Graphics::CommitPhasesEnabled(), "APS5_PROFILE_COMMIT must be set for this test");
        TestEligibleDraw();
        TestFirstAndAnyReasons();
        TestCommandsBeforeRecordingArePreOps();
        TestPhaseAndCounterHooks();
        TestNotesOutsideADrawAreIgnored();
        TestShadowReport();
        TestBatchBuckets();
        TestBatchStatistics();
        TestSubmitCauseScope();
        std::cout << "Parallel counters passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
