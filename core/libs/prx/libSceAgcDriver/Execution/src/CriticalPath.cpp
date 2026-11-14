#include "prx/libSceAgcDriver/Execution/include/CriticalPath.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <tuple>
#include <utility>
#ifdef _WIN32
#include <intrin.h>
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

namespace AgcDriver::CriticalPath {

namespace {

constexpr std::array<std::string_view, RoleCount> RoleNames{"other", "worker", "finisher", "committer", "presenter", "prepare", "title"};
constexpr std::array<std::string_view, MeasureCount> MeasureNames{"commit_busy", "commit_idle", "commits", "room_wait", "room_waits", "enqueues", "occupancy_sum", "occupancy_max", "finisher_room_wait", "finisher_busy", "finisher_idle", "finisher_jobs", "drain_wait", "worker_idle", "gpu_wait", "gpu_hold", "gpu_acquires", "tracker_wait", "tracker_hold", "collect", "collects"};
constexpr std::array<std::string_view, LockCount> LockNames{"gpu", "tracker"};

struct Slot {
    std::string name;
    Role role = Role::Other;
    std::array<std::atomic<std::uint64_t>, MeasureCount> values{};
    std::array<std::array<std::atomic<std::uint64_t>, RoleCount>, LockCount> behind{};
#ifdef _WIN32
    HANDLE handle = nullptr;
#else
    clockid_t clock{};
#endif
    bool timed = false;
    std::uint64_t lastCpu = 0;
};

struct QueuePosition {
    std::uint64_t serial = 0;
    std::uint32_t submission = 0;
    std::uint32_t index = 0;
    bool seen = false;
};

struct Census {
    std::mutex mutex;
    std::vector<RepeatDraw> current;
    std::vector<RepeatDraw> previous;
    std::array<QueuePosition, 16> positions{};
    bool hasPrevious = false;
};

Census& census() {
    static auto& value = *new Census();
    return value;
}

std::optional<RepeatStats> takeCensus() {
    if (!RepeatSwitched()) return std::nullopt;
    auto& c = census();
    std::lock_guard lock(c.mutex);
    std::optional<RepeatStats> stats;
    if (c.hasPrevious) stats = CompareFrames(c.previous, c.current);
    else stats = RepeatStats{.draws = c.current.size()};
    std::swap(c.previous, c.current);
    c.current.clear();
    c.positions = {};
    c.hasPrevious = true;
    return stats;
}

struct State {
    std::mutex mutex;
    std::vector<std::unique_ptr<Slot>> slots;
    Slot shared;
    std::vector<FrameRow> rows;
    std::uint64_t lastNs = 0;
    std::uint64_t present = 0;
    std::uint64_t ticksAtOpen = 0;
    std::uint64_t nsAtOpen = 0;
};

State& state() {
    static auto& value = *new State();
    return value;
}

std::array<std::atomic<std::uint8_t>, LockCount> owners{};
std::atomic<std::uint64_t> ringDepth{0};
thread_local Slot* local = nullptr;
thread_local std::array<std::uint64_t, LockCount> holdStart{};

Slot& slot() {
    return local != nullptr ? *local : state().shared;
}

std::uint64_t cpuTicks(Slot& slot) {
#ifdef _WIN32
    ULONG64 cycles = 0;
    if (slot.handle == nullptr || !QueryThreadCycleTime(slot.handle, &cycles)) return slot.lastCpu;
    return cycles;
#else
    timespec now{};
    if (clock_gettime(slot.clock, &now) != 0) return slot.lastCpu;
    return static_cast<std::uint64_t>(now.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(now.tv_nsec);
#endif
}

std::uint64_t referenceTicks() {
#ifdef _WIN32
    return __rdtsc();
#else
    return NowNs();
#endif
}

void appendNumber(std::string& text, std::uint64_t value) {
    text += std::to_string(value);
}

void appendName(std::string& text, std::string_view name) {
    text += '"';
    for (const char c : name) {
        if (c == '"' || c == '\\') text += '\\';
        if (static_cast<unsigned char>(c) < 0x20) continue;
        text += c;
    }
    text += '"';
}

ThreadRow take(Slot& slot, bool sample) {
    ThreadRow row;
    row.name = slot.name;
    row.role = slot.role;
    for (std::size_t m = 0; m < MeasureCount; ++m) row.values[m] = slot.values[m].exchange(0, std::memory_order_relaxed);
    for (std::size_t l = 0; l < LockCount; ++l) {
        for (std::size_t r = 0; r < RoleCount; ++r) row.waitedBehind[l][r] = slot.behind[l][r].exchange(0, std::memory_order_relaxed);
    }
    if (sample && slot.timed) {
        const auto now = cpuTicks(slot);
        row.cpuNs = now - slot.lastCpu;
        slot.lastCpu = now;
    }
    return row;
}

bool emptyRow(const ThreadRow& row) {
    if (row.cpuNs.has_value() && *row.cpuNs != 0) return false;
    for (const auto value : row.values) {
        if (value != 0) return false;
    }
    return true;
}

void collect(State& s, std::uint64_t present, std::uint64_t now) {
    FrameRow row;
    row.present = present;
    row.beginNs = s.lastNs;
    row.endNs = now;
    row.ringDepth = ringDepth.load(std::memory_order_relaxed);
    for (auto& entry : s.slots) {
        auto taken = take(*entry, true);
        if (!emptyRow(taken)) row.threads.push_back(std::move(taken));
    }
    auto shared = take(s.shared, false);
    if (!emptyRow(shared)) row.threads.push_back(std::move(shared));
    row.repeat = takeCensus();
    s.rows.push_back(std::move(row));
    s.lastNs = now;
}

}

std::string_view RoleName(Role role) {
    return RoleNames[static_cast<std::size_t>(role)];
}

std::string_view MeasureName(Measure measure) {
    return MeasureNames[static_cast<std::size_t>(measure)];
}

bool MeasureIsTime(Measure measure) {
    switch (measure) {
    case Measure::Commits:
    case Measure::RoomWaits:
    case Measure::Enqueues:
    case Measure::OccupancySum:
    case Measure::OccupancyMax:
    case Measure::FinisherJobs:
    case Measure::GpuAcquires:
    case Measure::Collects: return false;
    default: return true;
    }
}

Role RoleOf(std::string_view threadName) {
    if (threadName == "draw committer") return Role::Committer;
    if (threadName == "draw finisher") return Role::Finisher;
    if (threadName == "presenter") return Role::Presenter;
    if (threadName == "shader prepare") return Role::Prepare;
    if (threadName == "title") return Role::Title;
    if (threadName.find("worker") != std::string_view::npos) return Role::Worker;
    return Role::Other;
}

std::string FormatRow(const FrameRow& row) {
    std::string text = "{\"present\":";
    appendNumber(text, row.present);
    text += ",\"begin_ns\":";
    appendNumber(text, row.beginNs);
    text += ",\"end_ns\":";
    appendNumber(text, row.endNs);
    text += ",\"ring_depth\":";
    appendNumber(text, row.ringDepth);
    text += ",\"threads\":[";
    for (std::size_t t = 0; t < row.threads.size(); ++t) {
        const auto& thread = row.threads[t];
        if (t != 0) text += ',';
        text += "{\"name\":";
        appendName(text, thread.name);
        text += ",\"role\":";
        appendName(text, RoleName(thread.role));
        if (thread.cpuNs.has_value()) {
            text += ",\"cpu_ns\":";
            appendNumber(text, *thread.cpuNs);
        }
        for (std::size_t m = 0; m < MeasureCount; ++m) {
            if (thread.values[m] == 0) continue;
            text += ",\"";
            text += MeasureNames[m];
            text += MeasureIsTime(static_cast<Measure>(m)) ? "_ns\":" : "\":";
            appendNumber(text, thread.values[m]);
        }
        for (std::size_t l = 0; l < LockCount; ++l) {
            bool any = false;
            for (std::size_t r = 0; r < RoleCount; ++r) {
                if (thread.waitedBehind[l][r] == 0) continue;
                text += any ? "," : ",\"" + std::string(LockNames[l]) + "_wait_behind_ns\":{";
                any = true;
                appendName(text, RoleNames[r]);
                text += ':';
                appendNumber(text, thread.waitedBehind[l][r]);
            }
            if (any) text += '}';
        }
        text += '}';
    }
    text += ']';
    if (row.repeat.has_value()) {
        const auto& r = *row.repeat;
        text += ",\"repeat\":{\"draws\":";
        appendNumber(text, r.draws);
        text += ",\"positioned\":";
        appendNumber(text, r.positioned);
        constexpr std::array<std::string_view, RepeatLevels> levels{"exact", "masked", "masked_shaders"};
        for (std::size_t l = 0; l < RepeatLevels; ++l) {
            text += ",\"";
            text += levels[l];
            text += "\":{\"matched\":";
            appendNumber(text, r.matched[l]);
            text += ",\"runs\":";
            appendNumber(text, r.runs[l]);
            text += ",\"run_p50\":";
            appendNumber(text, r.runP50[l]);
            text += ",\"run_p90\":";
            appendNumber(text, r.runP90[l]);
            text += ",\"run_max\":";
            appendNumber(text, r.runMax[l]);
            text += ",\"in_runs_16\":";
            appendNumber(text, r.inLongRuns[l]);
            text += '}';
        }
        text += '}';
    }
    text += '}';
    return text;
}

RepeatStats CompareFrames(std::vector<RepeatDraw> previous, std::vector<RepeatDraw> current) {
    const auto position = [](const RepeatDraw& draw) { return std::tuple(draw.queue, draw.submission, draw.index); };
    const auto before = [&](const RepeatDraw& a, const RepeatDraw& b) { return position(a) < position(b); };
    std::ranges::sort(previous, before);
    std::ranges::sort(current, before);
    RepeatStats stats;
    stats.draws = current.size();
    std::array<std::vector<std::uint64_t>, RepeatLevels> lengths;
    std::array<std::uint64_t, RepeatLevels> run{};
    const auto close = [&](std::size_t level) {
        if (run[level] == 0) return;
        lengths[level].push_back(run[level]);
        if (run[level] >= LongRun) stats.inLongRuns[level] += run[level];
        run[level] = 0;
    };
    auto old = previous.begin();
    for (const auto& draw : current) {
        while (old != previous.end() && position(*old) < position(draw)) ++old;
        const bool placed = old != previous.end() && position(*old) == position(draw);
        if (placed) ++stats.positioned;
        bool lower = true;
        for (std::size_t level = 0; level < RepeatLevels; ++level) {
            const bool same = placed && old->hashes[level] == draw.hashes[level] && (level < 2 || lower);
            if (level == 1) lower = same;
            if (same) {
                ++stats.matched[level];
                ++run[level];
            } else {
                close(level);
            }
        }
    }
    for (std::size_t level = 0; level < RepeatLevels; ++level) {
        close(level);
        auto& values = lengths[level];
        stats.runs[level] = values.size();
        if (values.empty()) continue;
        std::ranges::sort(values);
        stats.runP50[level] = values[(values.size() - 1) / 2];
        stats.runP90[level] = values[(values.size() - 1) * 9 / 10];
        stats.runMax[level] = values.back();
    }
    return stats;
}

bool RepeatSwitched() {
    static const bool switched = [] {
        const char* value = std::getenv("APS5_TRACE_REPEAT");
        return value != nullptr && *value != '\0' && std::string_view(value) != "0";
    }();
    return switched;
}

void NoteDraw(std::uint32_t queue, std::uint64_t submissionSerial, const std::array<std::uint64_t, RepeatLevels>& hashes) {
    auto& c = census();
    std::lock_guard lock(c.mutex);
    auto& at = c.positions[queue % c.positions.size()];
    if (!at.seen || at.serial != submissionSerial) {
        if (at.seen) ++at.submission;
        at.serial = submissionSerial;
        at.index = 0;
        at.seen = true;
    }
    if (c.current.capacity() == 0) c.current.reserve(65536);
    c.current.push_back({queue, at.submission, at.index++, hashes});
}

bool Switched() {
    const char* value = std::getenv("APS5_TRACE_CRITICAL");
    return value != nullptr && *value != '\0' && std::string_view(value) != "0";
}

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void BindThread(const char* name) {
    static const bool switched = Switched();
    if (!switched || local != nullptr) return;
    auto bound = std::make_unique<Slot>();
    bound->name = name;
    bound->role = RoleOf(name);
#ifdef _WIN32
    HANDLE handle = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle, THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
        bound->handle = handle;
        bound->timed = true;
    }
#else
    bound->timed = pthread_getcpuclockid(pthread_self(), &bound->clock) == 0;
#endif
    if (bound->timed) bound->lastCpu = cpuTicks(*bound);
    auto& s = state();
    std::lock_guard lock(s.mutex);
    local = bound.get();
    s.slots.push_back(std::move(bound));
}

void Add(Measure measure, std::uint64_t value) {
    slot().values[static_cast<std::size_t>(measure)].fetch_add(value, std::memory_order_relaxed);
}

void Max(Measure measure, std::uint64_t value) {
    auto& entry = slot().values[static_cast<std::size_t>(measure)];
    auto current = entry.load(std::memory_order_relaxed);
    while (current < value && !entry.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

Role Owner(Lock lock) {
    return static_cast<Role>(owners[static_cast<std::size_t>(lock)].load(std::memory_order_relaxed));
}

void Waited(Lock lock, Role owner, std::uint64_t ns) {
    auto& target = slot();
    target.values[static_cast<std::size_t>(lock == Lock::Gpu ? Measure::GpuWait : Measure::TrackerWait)].fetch_add(ns, std::memory_order_relaxed);
    target.behind[static_cast<std::size_t>(lock)][static_cast<std::size_t>(owner)].fetch_add(ns, std::memory_order_relaxed);
}

void Acquired(Lock lock) {
    const auto index = static_cast<std::size_t>(lock);
    owners[index].store(static_cast<std::uint8_t>(local != nullptr ? local->role : Role::Other), std::memory_order_relaxed);
    holdStart[index] = NowNs();
    if (lock == Lock::Gpu) Add(Measure::GpuAcquires, 1);
}

void Released(Lock lock) {
    const auto index = static_cast<std::size_t>(lock);
    const auto start = std::exchange(holdStart[index], 0);
    if (start == 0) return;
    Add(lock == Lock::Gpu ? Measure::GpuHold : Measure::TrackerHold, NowNs() - start);
}

void SetRingDepth(std::uint64_t depth) {
    ringDepth.store(depth, std::memory_order_relaxed);
}

void OpenWindow(std::uint64_t present) {
    static const bool switched = Switched() || RepeatSwitched();
    if (!switched) return;
    if (RepeatSwitched()) {
        auto& c = census();
        std::lock_guard lock(c.mutex);
        c.current.clear();
        c.previous.clear();
        c.positions = {};
        c.hasPrevious = false;
    }
    auto& s = state();
    std::lock_guard lock(s.mutex);
    for (auto& entry : s.slots) take(*entry, true);
    take(s.shared, false);
    s.rows.clear();
    s.lastNs = NowNs();
    s.present = present;
    s.ticksAtOpen = referenceTicks();
    s.nsAtOpen = s.lastNs;
    Window.store(true, std::memory_order_relaxed);
}

void Present(std::uint64_t present) {
    if (!Active() && !RepeatActive()) return;
    auto& s = state();
    std::lock_guard lock(s.mutex);
    collect(s, present, NowNs());
}

void CloseWindow(std::uint64_t present, const std::filesystem::path& directory, const std::string& name) {
    if (!Active() && !RepeatActive()) return;
    Window.store(false, std::memory_order_relaxed);
    auto& s = state();
    std::vector<FrameRow> rows;
    double ticksPerNs = 1.0;
    {
        std::lock_guard lock(s.mutex);
        const auto now = NowNs();
        collect(s, present, now);
        rows = std::move(s.rows);
        s.rows.clear();
        const auto ticks = referenceTicks() - s.ticksAtOpen;
        if (now > s.nsAtOpen && ticks != 0) ticksPerNs = static_cast<double>(ticks) / static_cast<double>(now - s.nsAtOpen);
    }
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    std::ofstream file(directory / name, std::ios::binary | std::ios::trunc);
    if (!file) {
        std::fprintf(stderr, "[critical] writing %s failed\n", name.c_str());
        return;
    }
    file << "{\"format\":\"aps5-critical/1\",\"cpu_ticks_per_ns\":" << ticksPerNs << ",\"rows\":" << rows.size() << "}\n";
    for (auto& row : rows) {
        for (auto& thread : row.threads) {
            if (thread.cpuNs.has_value()) thread.cpuNs = static_cast<std::uint64_t>(static_cast<double>(*thread.cpuNs) / ticksPerNs);
        }
        file << FormatRow(row) << '\n';
    }
    std::fprintf(stderr, "[critical] wrote %zu frames to %s\n", rows.size(), name.c_str());
}

}
