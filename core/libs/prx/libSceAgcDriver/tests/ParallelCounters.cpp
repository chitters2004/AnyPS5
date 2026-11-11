#include "prx/libSceAgcDriver/Graphics/include/CommitPhases.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using AgcDriver::Graphics::Recorder;

void Check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
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
