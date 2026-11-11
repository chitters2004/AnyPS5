#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/CommitPhases.hpp"
#include <cstdlib>

namespace AgcDriver::DriverDetail {

PollStats& Driver::pollStats() {
    static thread_local PollStats stats;
    return stats;
}

WaitOutcomes& Driver::waitOutcomes() {
    static thread_local WaitOutcomes outcomes;
    return outcomes;
}

Graphics::Recorder::LateStatistics& Driver::lateCountsSeen() {
    static thread_local Graphics::Recorder::LateStatistics seen{};
    return seen;
}

EpochBumps& Driver::epochBumps() {
    static thread_local EpochBumps bumps;
    return bumps;
}

std::chrono::steady_clock::time_point& Driver::lastEpochBump() {
    static thread_local std::chrono::steady_clock::time_point at{};
    return at;
}

void Driver::bumpEpoch(std::uint64_t EpochBumps::*counter) {
    lastEpochBump() = std::chrono::steady_clock::now();
    GuestMemory::BumpCollectEpoch();
    const bool profiled = Graphics::CommitPhasesEnabled();
    const auto cause = static_cast<std::size_t>(counter == &EpochBumps::submissions ? Graphics::EpochCause::Submission : counter == &EpochBumps::waits ? Graphics::EpochCause::Wait : counter == &EpochBumps::drains ? Graphics::EpochCause::Drain : counter == &EpochBumps::reaps ? Graphics::EpochCause::Reap : Graphics::EpochCause::Packet);
    if (profiled) Graphics::NoteEpochCauses(1u << cause);
    if (DrawPipeline::Active()) {
        if (profiled) DrawPipeline::EpochTokenCauses()[cause].fetch_add(1, std::memory_order_relaxed);
        DrawPipeline::EpochToken().fetch_add(1, std::memory_order_relaxed);
    }
    ++(epochBumps().*counter);
}

bool Driver::packetEpoch() {
    static const bool packet = std::getenv("APS5_PACKET_EPOCH") != nullptr;
    return packet;
}

}
