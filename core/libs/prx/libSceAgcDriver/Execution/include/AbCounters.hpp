#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ABCOUNTERS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_ABCOUNTERS_HPP

#include "prx/libSceAgcDriver/Execution/include/PerformanceTrace.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AgcDriver {

struct AbCommitterTotals {
    std::uint64_t commits = 0, commitNs = 0, drawCommits = 0, drawNs = 0, enqueued = 0, fullWaitNs = 0, wakes = 0, drains = 0, drainWaitNs = 0, finisherDrains = 0, finisherDrainWaitNs = 0;
};

struct AbSample {
    double seconds = 0;
    std::uint64_t present = 0;
    double gpuBusyMs = 0, gpuGapMs = 0;
    AbCommitterTotals committer;
    std::vector<TraceThreadTotals> threads;
};

class AbCounters {
public:
    static std::string Line(char arm, std::uint64_t phase, const AbSample& from, const AbSample& to, std::span<const double> flipSeconds) {
        std::string line = "[ab-counters] phase ";
        line += arm;
        line += ' ' + std::to_string(phase);
        const auto number = [&](std::string_view key, double value) {
            char text[64];
            std::snprintf(text, sizeof text, " %.*s=%.3f", static_cast<int>(key.size()), key.data(), value);
            line += text;
        };
        const auto count = [&](std::string_view key, std::uint64_t value) { line += ' ' + std::string(key) + '=' + std::to_string(value); };
        const auto ms = [](std::uint64_t ns) { return static_cast<double>(ns) / 1e6; };
        number("wall_ms", (to.seconds - from.seconds) * 1e3);
        count("presents", to.present - from.present);
        std::vector<double> frames;
        for (std::size_t index = 1; index < flipSeconds.size(); ++index) frames.push_back((flipSeconds[index] - flipSeconds[index - 1]) * 1e3);
        std::sort(frames.begin(), frames.end());
        const auto percentile = [&](double fraction) { return frames.empty() ? 0.0 : frames[std::min(frames.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(frames.size())))]; };
        number("flip_p50_ms", percentile(0.5));
        number("flip_p90_ms", percentile(0.9));
        number("gpu_busy_ms", to.gpuBusyMs - from.gpuBusyMs);
        number("gpu_gap_ms", to.gpuGapMs - from.gpuGapMs);
        const auto& a = from.committer;
        const auto& b = to.committer;
        count("commits", b.commits - a.commits);
        number("commit_ms", ms(b.commitNs - a.commitNs));
        count("draws", b.drawCommits - a.drawCommits);
        number("draw_commit_ms", ms(b.drawNs - a.drawNs));
        count("enqueued", b.enqueued - a.enqueued);
        number("room_wait_ms", ms(b.fullWaitNs - a.fullWaitNs));
        count("wakes", b.wakes - a.wakes);
        count("drains", b.drains - a.drains);
        number("drain_wait_ms", ms(b.drainWaitNs - a.drainWaitNs));
        count("finisher_drains", b.finisherDrains - a.finisherDrains);
        number("finisher_drain_wait_ms", ms(b.finisherDrainWaitNs - a.finisherDrainWaitNs));
        bool recorded = false;
        for (std::size_t index = 0; index < to.threads.size(); ++index) {
            const auto before = index < from.threads.size() && from.threads[index].name == to.threads[index].name ? std::span<const TraceThread::Total>(from.threads[index].totals) : std::span<const TraceThread::Total>();
            const auto totals = TraceFormat::Since(to.threads[index].totals, before);
            Clock::duration busy{}, idle{}, memory{}, lock{}, gpu{};
            std::uint64_t jobs = 0;
            for (const auto& total : totals) {
                const std::string_view scope = total.scope;
                const std::string_view stage = total.stage != nullptr ? total.stage : "";
                if (total.stage == nullptr && std::find(TraceFormat::WorkerBusyScopes.begin(), TraceFormat::WorkerBusyScopes.end(), scope) != TraceFormat::WorkerBusyScopes.end()) busy += total.length;
                if (total.stage == nullptr && scope == FinisherScope) {
                    busy += total.length;
                    jobs += total.count;
                }
                if (scope == TraceFormat::WorkerWaitScope && total.stage == nullptr) idle += total.length;
                if (scope == "Driver.Packet" && (stage == "memory_wait" || stage == "rendering_wait")) memory += total.length;
                else if (scope == "GpuMutex.wait") lock += total.length;
                else if ((total.stage == nullptr && (scope == "Recorder.Sync" || scope == "Recorder.WaitSerial")) || (scope == "Graphics.Wait" && stage == "fence_wait")) gpu += total.length;
            }
            if (busy == Clock::duration{} && idle == Clock::duration{} && memory == Clock::duration{} && lock == Clock::duration{} && gpu == Clock::duration{}) continue;
            recorded = true;
            std::string name = to.threads[index].name;
            std::replace(name.begin(), name.end(), ' ', '_');
            const auto millis = [](Clock::duration length) { return std::chrono::duration<double, std::milli>(length).count(); };
            if (busy != Clock::duration{}) number("t." + name + ".busy_ms", millis(busy));
            if (jobs != 0) count("t." + name + ".jobs", jobs);
            if (idle != Clock::duration{}) number("t." + name + ".idle_ms", millis(idle));
            if (memory != Clock::duration{}) number("t." + name + ".memory_wait_ms", millis(memory));
            if (lock != Clock::duration{}) number("t." + name + ".lock_wait_ms", millis(lock));
            if (gpu != Clock::duration{}) number("t." + name + ".gpu_wait_ms", millis(gpu));
        }
        if (!recorded) line += " threads=none";
        line += '\n';
        return line;
    }

private:
    using Clock = TraceThread::Clock;
    static constexpr std::string_view FinisherScope = "Driver.DrawFinish";
};

void InstallAbCounters();

}

#endif
