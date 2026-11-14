#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileCounter.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <cstdlib>
#include <unordered_map>

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

namespace {
struct EpochBumpCounters {
    ProfileCounter submissions, waits, drains, reaps, packets, waitsOrdered;
};

EpochBumpCounters& epochBumpCounters() {
    static auto* counters = new EpochBumpCounters;
    return *counters;
}
}

void CountEpochBump(std::uint64_t EpochBumps::*counter) {
    if (!GuestMemory::ScopedProofsProfiled()) return;
    auto& counters = epochBumpCounters();
    if (counter == &EpochBumps::submissions) counters.submissions.Add();
    else if (counter == &EpochBumps::waits) counters.waits.Add();
    else if (counter == &EpochBumps::drains) counters.drains.Add();
    else if (counter == &EpochBumps::reaps) counters.reaps.Add();
    else if (counter == &EpochBumps::packets) counters.packets.Add();
    else if (counter == &EpochBumps::waitsOrdered) counters.waitsOrdered.Add();
}

EpochBumps TakeEpochBumpKinds() {
    auto& counters = epochBumpCounters();
    EpochBumps taken;
    taken.submissions = counters.submissions.Take();
    taken.waits = counters.waits.Take();
    taken.drains = counters.drains.Take();
    taken.reaps = counters.reaps.Take();
    taken.packets = counters.packets.Take();
    taken.waitsOrdered = counters.waitsOrdered.Take();
    return taken;
}

void ReportEpochBumpKinds() {
    const auto taken = TakeEpochBumpKinds();
    AgcDriver::ProfilePrint_nid_no_patch("[scoped-proofs] (10 s) collect epochs by kind: submissions %llu, waits %llu (%llu more ordered by an earlier wait, no epoch), drains %llu, reaps %llu, packets %llu\n", static_cast<unsigned long long>(taken.submissions), static_cast<unsigned long long>(taken.waits), static_cast<unsigned long long>(taken.waitsOrdered), static_cast<unsigned long long>(taken.drains), static_cast<unsigned long long>(taken.reaps), static_cast<unsigned long long>(taken.packets));
}

void Driver::bumpEpoch(std::uint64_t EpochBumps::*counter) {
    lastEpochBump() = std::chrono::steady_clock::now();
    GuestMemory::BumpCollectEpoch();
    if (DrawPipeline::Active()) DrawPipeline::EpochToken().fetch_add(1, std::memory_order_relaxed);
    ++(epochBumps().*counter);
    CountEpochBump(counter);
}

void Driver::bumpWaitEpoch(std::uint64_t address, std::size_t bytes) {
    struct Ordered {
        std::uint64_t value = 0;
        std::uint64_t generation = 0;
    };
    struct OrderedStorage {};
    auto& ordered = HostThreadLocal<std::unordered_map<std::uint64_t, Ordered>, OrderedStorage>();
    static const bool memo = std::getenv("APS5_NO_ORDERED_WAITS") == nullptr;
    if (!GuestMemory::Accessible(reinterpret_cast<const void*>(address), bytes)) {
        bumpEpoch(&EpochBumps::waits);
        return;
    }
    const auto generation = memo ? GuestMemory::CollectWritesUncached(address, bytes) : 0;
    const std::uint64_t value = bytes == 8 ? *reinterpret_cast<const volatile std::uint64_t*>(address) : *reinterpret_cast<const volatile std::uint32_t*>(address);
    if (generation != 0) {
        const auto found = ordered.find(address);
        if (found != ordered.end() && found->second.value == value && GuestMemory::UnchangedSince(address, bytes, found->second.generation)) {
            ++epochBumps().waitsOrdered;
            CountEpochBump(&EpochBumps::waitsOrdered);
            return;
        }
    }
    bumpEpoch(&EpochBumps::waits);
    if (generation == 0) return;
    if (ordered.size() >= 4096) ordered.clear();
    ordered[address] = {value, generation};
}

bool Driver::packetEpoch() {
    static const bool packet = std::getenv("APS5_PACKET_EPOCH") != nullptr;
    return packet;
}

}
