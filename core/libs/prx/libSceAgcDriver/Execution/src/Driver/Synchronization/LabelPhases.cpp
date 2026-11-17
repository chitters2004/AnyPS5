#include "prx/libSceAgcDriver/Execution/include/LabelPhases.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileCounter.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace AgcDriver::LabelPhases {

namespace {

std::atomic<bool>& EnabledFlag() {
    static std::atomic<bool> enabled{std::getenv("APS5_PROFILE_DRAW") != nullptr};
    return enabled;
}

struct Counters {
    ProfileCounter labels;
    ProfileCounter holdNs;
    std::array<ProfileCounter, PhaseCount> ns;
    std::array<ProfileCounter, PhaseCount> counts;
    std::array<ProfileCounter, static_cast<std::size_t>(ReapTry::Count)> reapTries;
    std::array<ProfileCounter, static_cast<std::size_t>(ReapTry::Count)> reapTryNs;
    std::array<ProfileCounter, PreSlotCount> preNs;
    std::array<ProfileCounter, PreSlotCount> preCalls;
    ProfileCounter drawnStored;
    ProfileCounter readbackScanned;
    ProfileCounter readbackFlushed;
    ProfileCounter readbackSkipped;
    ProfileCounter labelReaps;
    ProfileCounter labelReapsWithWork;
    ProfileCounter labelReapRetired;
    ProfileCounter labelReapInFlight;
};

Counters& Shared() {
    static Counters counters;
    return counters;
}

struct Open {
    bool active = false;
    std::chrono::steady_clock::time_point begun{};
    std::chrono::steady_clock::time_point last{};
};

thread_local Open open;

std::uint64_t Nanoseconds(std::chrono::steady_clock::duration duration) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}

std::uint64_t SteadyNs() {
    return Nanoseconds(std::chrono::steady_clock::now().time_since_epoch());
}

void Add(Phase phase, std::chrono::steady_clock::time_point now) {
    const auto index = static_cast<std::size_t>(phase);
    Shared().ns[index].Add(Nanoseconds(now - open.last));
    Shared().counts[index].Add();
    open.last = now;
}

}

bool Enabled() {
    return EnabledFlag().load(std::memory_order_relaxed);
}

void SetEnabled(bool enabled) {
    EnabledFlag().store(enabled, std::memory_order_relaxed);
}

void Begin() {
    if (!Enabled()) return;
    open.active = true;
    open.begun = open.last = std::chrono::steady_clock::now();
}

void Mark(Phase phase) {
    if (!open.active) return;
    Add(phase, std::chrono::steady_clock::now());
}

void End() {
    if (!open.active) return;
    const auto now = std::chrono::steady_clock::now();
    Add(Phase::Tail, now);
    Shared().labels.Add();
    Shared().holdNs.Add(Nanoseconds(now - open.begun));
    open.active = false;
}

void CountReapTry(ReapTry outcome, std::uint64_t ns) {
    const auto index = static_cast<std::size_t>(outcome);
    Shared().reapTries[index].Add();
    Shared().reapTryNs[index].Add(ns);
}

bool Active() {
    return open.active;
}

void CountDrawnStored(std::uint64_t stored) {
    if (open.active) Shared().drawnStored.Add(stored);
}

void CountReadback(std::uint64_t scanned, std::uint64_t flushed) {
    if (!open.active) return;
    Shared().readbackScanned.Add(scanned);
    Shared().readbackFlushed.Add(flushed);
}

void CountReadbackSkipped() {
    if (open.active) Shared().readbackSkipped.Add();
}

void CountLabelReap(std::uint64_t inFlightBefore, std::uint64_t inFlightAfter) {
    if (!open.active) return;
    const auto retired = inFlightBefore > inFlightAfter ? inFlightBefore - inFlightAfter : 0;
    Shared().labelReaps.Add();
    if (retired != 0) Shared().labelReapsWithWork.Add();
    Shared().labelReapRetired.Add(retired);
    Shared().labelReapInFlight.Add(inFlightBefore);
}

PreTimer::PreTimer(PreSlot slot) : slot(slot), on(open.active) {
    if (on) started = SteadyNs();
}

PreTimer::~PreTimer() {
    if (!on) return;
    const auto index = static_cast<std::size_t>(slot);
    Shared().preNs[index].Add(SteadyNs() - started);
    Shared().preCalls[index].Add();
}

Totals Take() {
    Totals totals;
    auto& shared = Shared();
    totals.labels = shared.labels.Take();
    totals.holdNs = shared.holdNs.Take();
    for (std::size_t i = 0; i < PhaseCount; ++i) {
        totals.ns[i] = shared.ns[i].Take();
        totals.counts[i] = shared.counts[i].Take();
    }
    for (std::size_t i = 0; i < totals.reapTries.size(); ++i) {
        totals.reapTries[i] = shared.reapTries[i].Take();
        totals.reapTryNs[i] = shared.reapTryNs[i].Take();
    }
    for (std::size_t i = 0; i < PreSlotCount; ++i) {
        totals.preNs[i] = shared.preNs[i].Take();
        totals.preCalls[i] = shared.preCalls[i].Take();
    }
    totals.drawnStored = shared.drawnStored.Take();
    totals.readbackScanned = shared.readbackScanned.Take();
    totals.readbackFlushed = shared.readbackFlushed.Take();
    totals.readbackSkipped = shared.readbackSkipped.Take();
    totals.labelReaps = shared.labelReaps.Take();
    totals.labelReapsWithWork = shared.labelReapsWithWork.Take();
    totals.labelReapRetired = shared.labelReapRetired.Take();
    totals.labelReapInFlight = shared.labelReapInFlight.Take();
    return totals;
}

std::string Line(const Totals& totals) {
    constexpr std::array<const char*, PhaseCount> names{"pre", "reap", "lookup", "record", "notes", "tail"};
    const double labels = totals.labels == 0 ? 1.0 : static_cast<double>(totals.labels);
    std::array<char, 256> part{};
    std::snprintf(part.data(), part.size(), "[labels] commit phases (10 s): %llu labels, held %.2f us/label =", static_cast<unsigned long long>(totals.labels), static_cast<double>(totals.holdNs) / 1000.0 / labels);
    std::string line = part.data();
    for (std::size_t i = 0; i < PhaseCount; ++i) {
        std::snprintf(part.data(), part.size(), "%s %s %.2f (%llu)", i == 0 ? "" : " +", names[i], static_cast<double>(totals.ns[i]) / 1000.0 / labels, static_cast<unsigned long long>(totals.counts[i]));
        line += part.data();
    }
    const auto reaped = static_cast<std::size_t>(ReapTry::Reaped);
    const auto failed = static_cast<std::size_t>(ReapTry::Failed);
    const auto drawn = static_cast<std::size_t>(PreSlot::DrawnLinear);
    const auto readback = static_cast<std::size_t>(PreSlot::Readback);
    std::snprintf(part.data(), part.size(), "; pre detail: drawn-linear %.2f us/label (%llu calls, %llu stored), readback %.2f us/label (%llu calls, %llu textures scanned, %llu flushed)", static_cast<double>(totals.preNs[drawn]) / 1000.0 / labels, static_cast<unsigned long long>(totals.preCalls[drawn]), static_cast<unsigned long long>(totals.drawnStored), static_cast<double>(totals.preNs[readback]) / 1000.0 / labels, static_cast<unsigned long long>(totals.preCalls[readback]), static_cast<unsigned long long>(totals.readbackScanned), static_cast<unsigned long long>(totals.readbackFlushed));
    line += part.data();
    std::snprintf(part.data(), part.size(), ", readback skipped %llu", static_cast<unsigned long long>(totals.readbackSkipped));
    line += part.data();
    std::snprintf(part.data(), part.size(), "; in-label reaps: %llu, %llu retired a batch (%llu batches), %llu batches in flight before them", static_cast<unsigned long long>(totals.labelReaps), static_cast<unsigned long long>(totals.labelReapsWithWork), static_cast<unsigned long long>(totals.labelReapRetired), static_cast<unsigned long long>(totals.labelReapInFlight));
    line += part.data();
    std::snprintf(part.data(), part.size(), "; boundary reap tries: %llu reaped (%.2f ms), %llu failed (%.2f ms)\n", static_cast<unsigned long long>(totals.reapTries[reaped]), static_cast<double>(totals.reapTryNs[reaped]) / 1e6, static_cast<unsigned long long>(totals.reapTries[failed]), static_cast<double>(totals.reapTryNs[failed]) / 1e6);
    line += part.data();
    return line;
}

}
