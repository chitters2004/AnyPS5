#ifndef APS5_AGC_CRITICAL_PATH_HPP
#define APS5_AGC_CRITICAL_PATH_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AgcDriver::CriticalPath {

enum class Role : std::uint8_t { Other, Worker, Finisher, Committer, Presenter, Prepare, Title, Count };

enum class Measure : std::uint8_t {
    CommitBusy,
    CommitIdle,
    Commits,
    RoomWait,
    RoomWaits,
    Enqueues,
    OccupancySum,
    OccupancyMax,
    FinisherRoomWait,
    FinisherBusy,
    FinisherIdle,
    FinisherJobs,
    DrainWait,
    WorkerIdle,
    GpuWait,
    GpuHold,
    GpuAcquires,
    TrackerWait,
    TrackerHold,
    Collect,
    Collects,
    Count
};

enum class Lock : std::uint8_t { Gpu, Tracker, Count };

constexpr std::size_t RoleCount = static_cast<std::size_t>(Role::Count);
constexpr std::size_t MeasureCount = static_cast<std::size_t>(Measure::Count);
constexpr std::size_t LockCount = static_cast<std::size_t>(Lock::Count);

std::string_view RoleName(Role role);
std::string_view MeasureName(Measure measure);
bool MeasureIsTime(Measure measure);
Role RoleOf(std::string_view threadName);

struct ThreadRow {
    std::string name;
    Role role = Role::Other;
    std::optional<std::uint64_t> cpuNs;
    std::array<std::uint64_t, MeasureCount> values{};
    std::array<std::array<std::uint64_t, RoleCount>, LockCount> waitedBehind{};
};

constexpr std::size_t RepeatLevels = 3;

struct RepeatDraw {
    std::uint32_t queue = 0;
    std::uint32_t submission = 0;
    std::uint32_t index = 0;
    std::array<std::uint64_t, RepeatLevels> hashes{};
};

struct RepeatStats {
    std::uint64_t draws = 0;
    std::uint64_t positioned = 0;
    std::array<std::uint64_t, RepeatLevels> matched{};
    std::array<std::uint64_t, RepeatLevels> runs{};
    std::array<std::uint64_t, RepeatLevels> runP50{};
    std::array<std::uint64_t, RepeatLevels> runP90{};
    std::array<std::uint64_t, RepeatLevels> runMax{};
    std::array<std::uint64_t, RepeatLevels> inLongRuns{};
};

constexpr std::uint64_t LongRun = 16;

RepeatStats CompareFrames(std::vector<RepeatDraw> previous, std::vector<RepeatDraw> current);

struct FrameRow {
    std::uint64_t present = 0;
    std::uint64_t beginNs = 0;
    std::uint64_t endNs = 0;
    std::uint64_t ringDepth = 0;
    std::vector<ThreadRow> threads;
    std::optional<RepeatStats> repeat;
};

std::string FormatRow(const FrameRow& row);

bool Switched();

inline std::atomic<bool> Window{false};

inline bool Active() {
    static const bool switched = Switched();
    return switched && Window.load(std::memory_order_relaxed);
}

bool RepeatSwitched();

inline bool RepeatActive() {
    static const bool switched = RepeatSwitched();
    return switched && Window.load(std::memory_order_relaxed);
}

void NoteDraw(std::uint32_t queue, std::uint64_t submissionSerial, const std::array<std::uint64_t, RepeatLevels>& hashes);

std::uint64_t NowNs();
void BindThread(const char* name);
void Add(Measure measure, std::uint64_t value);
void Max(Measure measure, std::uint64_t value);
Role Owner(Lock lock);
void Waited(Lock lock, Role owner, std::uint64_t ns);
void Acquired(Lock lock);
void Released(Lock lock);
void SetRingDepth(std::uint64_t depth);

class Span {
public:
    explicit Span(Measure measure) : measure(measure), start(Active() ? NowNs() : 0) {}
    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;
    ~Span() {
        if (start != 0 && Active()) Add(measure, NowNs() - start);
    }

private:
    Measure measure;
    std::uint64_t start;
};

void OpenWindow(std::uint64_t present);
void Present(std::uint64_t present);
void CloseWindow(std::uint64_t present, const std::filesystem::path& directory, const std::string& name);

}

#endif
