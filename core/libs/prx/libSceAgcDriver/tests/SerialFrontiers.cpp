#include "prx/libSceAgcDriver/Graphics/include/SerialFrontiers.hpp"
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using AgcDriver::Graphics::BatchState;
using AgcDriver::Graphics::SerialFrontiers;

void Check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool Refused(const std::function<void()>& step) {
    try {
        step();
    } catch (const std::logic_error&) {
        return true;
    }
    return false;
}

void TestOneBatchAtATime() {
    SerialFrontiers frontiers;
    Check(frontiers.Newest() == 0 && frontiers.Through(BatchState::Completed) == 0 && frontiers.Terminal() == 0, "a new frontier set is not empty");
    for (std::uint64_t serial = 1; serial <= 3; ++serial) {
        Check(frontiers.Reserve() == serial, "a reservation skipped a serial");
        frontiers.Mark(serial, BatchState::Recorded);
        frontiers.Mark(serial, BatchState::Submitted);
        Check(frontiers.Through(BatchState::Submitted) == serial && frontiers.Through(BatchState::Completed) == serial - 1, "submitting a batch moved the wrong frontier");
        frontiers.Mark(serial, BatchState::Completed);
        Check(frontiers.Through(BatchState::Completed) == serial && frontiers.Terminal() == serial, "completing a batch did not move the completed frontier");
        Check(frontiers.State(serial) == BatchState::Completed, "a completed batch reads as another state");
    }
}

void TestFrontiersStayContiguous() {
    SerialFrontiers frontiers;
    for (int index = 0; index < 3; ++index) frontiers.Reserve();
    for (std::uint64_t serial = 1; serial <= 3; ++serial) frontiers.Mark(serial, BatchState::Recorded);
    frontiers.Mark(2, BatchState::Submitted);
    Check(frontiers.Through(BatchState::Submitted) == 0, "a submitted later batch moved the submitted frontier past an unsubmitted earlier one");
    Check(frontiers.Through(BatchState::Recorded) == 3, "the recorded frontier did not cover every recorded batch");
    frontiers.Mark(1, BatchState::Submitted);
    frontiers.Mark(3, BatchState::Submitted);
    frontiers.Mark(3, BatchState::Completed);
    Check(frontiers.Through(BatchState::Completed) == 0 && frontiers.Terminal() == 0, "a completed later batch moved the completed frontier past an earlier one");
    frontiers.Mark(1, BatchState::Completed);
    Check(frontiers.Through(BatchState::Completed) == 1, "the completed frontier did not reach the first completed batch");
    frontiers.Mark(2, BatchState::Completed);
    Check(frontiers.Through(BatchState::Completed) == 3 && frontiers.Terminal() == 3, "the completed frontier did not close the gap");
}

void TestFailureStopsTheCompletedFrontier() {
    SerialFrontiers frontiers;
    for (int index = 0; index < 4; ++index) frontiers.Reserve();
    for (std::uint64_t serial = 1; serial <= 2; ++serial) {
        frontiers.Mark(serial, BatchState::Recorded);
        frontiers.Mark(serial, BatchState::Submitted);
    }
    frontiers.Mark(1, BatchState::Completed);
    frontiers.Mark(2, BatchState::Failed);
    frontiers.Mark(3, BatchState::Cancelled);
    Check(frontiers.Through(BatchState::Completed) == 1, "a failed batch counted as completed");
    Check(frontiers.Terminal() == 3, "the terminal frontier did not pass the failed and cancelled batches");
    Check(frontiers.State(2) == BatchState::Failed && frontiers.State(3) == BatchState::Cancelled, "a pruned failed or cancelled batch reads as another state");
    frontiers.Mark(4, BatchState::Recorded);
    frontiers.Mark(4, BatchState::Submitted);
    frontiers.Mark(4, BatchState::Completed);
    Check(frontiers.Through(BatchState::Completed) == 1, "a batch completed after a failure moved the completed frontier past the failure");
    Check(frontiers.Through(BatchState::Submitted) == 1, "the submitted frontier passed a failed batch");
}

void TestRetractAndRefusals() {
    SerialFrontiers frontiers;
    frontiers.Reserve();
    frontiers.Mark(1, BatchState::Recorded);
    frontiers.Retract(1);
    Check(frontiers.Newest() == 0 && frontiers.Reserve() == 1, "a retracted serial was not reused");
    frontiers.Reserve();
    Check(Refused([&] { frontiers.Retract(1); }), "a serial that is not the newest was retracted");
    frontiers.Mark(2, BatchState::Recorded);
    frontiers.Mark(2, BatchState::Submitted);
    Check(Refused([&] { frontiers.Retract(2); }), "a submitted serial was retracted");
    Check(Refused([&] { frontiers.Mark(2, BatchState::Cancelled); }), "a submitted batch was cancelled");
    Check(Refused([&] { frontiers.Mark(1, BatchState::Submitted); }), "a batch skipped the recorded state");
    Check(Refused([&] { frontiers.Mark(3, BatchState::Recorded); }), "a serial that was never reserved was marked");
    Check(Refused([&] { static_cast<void>(frontiers.State(3)); }), "a serial that was never reserved was read");
    Check(Refused([&] { static_cast<void>(frontiers.Through(BatchState::Failed)); }), "a success frontier was given for a failure state");
    frontiers.Mark(1, BatchState::Failed);
    Check(Refused([&] { frontiers.Mark(1, BatchState::Completed); }), "a failed batch moved again");
}

}

int main() {
    try {
        TestOneBatchAtATime();
        TestFrontiersStayContiguous();
        TestFailureStopsTheCompletedFrontier();
        TestRetractAndRefusals();
        std::cout << "serial frontier tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
