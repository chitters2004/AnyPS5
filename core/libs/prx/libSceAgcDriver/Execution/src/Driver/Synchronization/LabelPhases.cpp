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
    return totals;
}

std::string Line(const Totals& totals) {
    constexpr std::array<const char*, PhaseCount> names{"pre", "reap", "lookup", "record", "notes", "tail"};
    const double labels = totals.labels == 0 ? 1.0 : static_cast<double>(totals.labels);
    std::array<char, 160> part{};
    std::snprintf(part.data(), part.size(), "[labels] commit phases (10 s): %llu labels, held %.2f us/label =", static_cast<unsigned long long>(totals.labels), static_cast<double>(totals.holdNs) / 1000.0 / labels);
    std::string line = part.data();
    for (std::size_t i = 0; i < PhaseCount; ++i) {
        std::snprintf(part.data(), part.size(), "%s %s %.2f (%llu)", i == 0 ? "" : " +", names[i], static_cast<double>(totals.ns[i]) / 1000.0 / labels, static_cast<unsigned long long>(totals.counts[i]));
        line += part.data();
    }
    const auto reaped = static_cast<std::size_t>(ReapTry::Reaped);
    const auto failed = static_cast<std::size_t>(ReapTry::Failed);
    std::snprintf(part.data(), part.size(), "; boundary reap tries: %llu reaped (%.2f ms), %llu failed (%.2f ms)\n", static_cast<unsigned long long>(totals.reapTries[reaped]), static_cast<double>(totals.reapTryNs[reaped]) / 1e6, static_cast<unsigned long long>(totals.reapTries[failed]), static_cast<double>(totals.reapTryNs[failed]) / 1e6);
    line += part.data();
    return line;
}

}
