#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAWTIMING_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAWTIMING_HPP

#include <array>
#include <chrono>
#include <cstdint>
#include <list>
#include <string>
#include <vector>

namespace AgcDriver::DriverDetail {

enum DrawDriverPhase { DrawRowPrologue, DrawRowPrecheck, DrawRowDecode, DrawRowProgramPrepare, DrawRowCapture, DrawRowCaptureHookWaits, DrawRowRecompile, DrawRowRectList, DrawRowVectors, DrawRowKeyLookupValidate, DrawRowValidateWait, DrawRowLockWait, DrawRowLabels, DrawRowGraphics, DrawRowSkipped, DrawRowEpilogue, DrawDriverPhaseCount };

inline constexpr const char* DrawDriverPhaseNames[DrawDriverPhaseCount] = {"prologue", "precheck", "decode", "program prepare", "capture", "capture hook waits", "recompile", "rect-list", "vectors", "key/lookup/validate", "validate GPU wait", "lock wait", "labels", "Graphics::Draw", "skipped", "epilogue"};

struct DrawPhaseTotals {
    std::array<double, DrawDriverPhaseCount> ms{};
    std::uint64_t packets = 0, drawn = 0, captures = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

struct PendingDrawPhases {
    bool phases = false;
    std::uint64_t captures = 0;
    std::array<double, DrawDriverPhaseCount> ms{};
    std::chrono::steady_clock::time_point tailAt{};
};

enum class PhaseThread : std::uint8_t { Worker, Finisher, Other, Count };
enum class PhaseGroup : std::uint8_t { Prepare, Lookup, Handoff, Graphics, Count };

struct ThreadPhaseTotals {
    static constexpr std::size_t Threads = static_cast<std::size_t>(PhaseThread::Count);
    static constexpr std::size_t Groups = static_cast<std::size_t>(PhaseGroup::Count);
    std::array<std::array<std::uint64_t, Groups>, Threads> groupNs{};
    std::array<std::uint64_t, Threads> lapNs{}, drawNs{}, draws{};
    std::uint64_t busyNs = 0, submissions = 0, windowNs = 0;
};

enum class BoundaryEvent : std::uint8_t { DrainBegin, DrainEnd, CommitBegin, CommitEnd, RoomWaitBegin, RoomWaitEnd };

struct BoundaryRecord {
    std::int64_t ns = 0;
    BoundaryEvent event = BoundaryEvent::DrainBegin;
    std::uint32_t finisherJobs = 0, committerItems = 0;
};

struct BoundaryTotals {
    std::uint64_t windowNs = 0, drains = 0, drainNs = 0, commits = 0, busyNs = 0, idleNs = 0, idleInDrainNs = 0, idleAfterDrainNs = 0, busyInDrainNs = 0, roomWaitNs = 0, roomWaitInDrainNs = 0;
    std::uint64_t emptyAtDrain = 0, heldAtDrain = 0, finisherJobsAtDrain = 0, starved = 0, saturated = 0, other = 0, dropped = 0;
    std::array<std::uint64_t, 4> drainLengths{};
};

struct BoundaryState {
    std::int64_t cursor = 0;
    bool started = false, committerBusy = false, draining = false, afterDrain = false;
    std::uint32_t roomWaiters = 0;
    std::int64_t drainStart = 0;
    std::uint64_t drainIdleNs = 0, drainBusyNs = 0, drainRoomWaitNs = 0;
};

class BoundaryTrace {
public:
    static bool Enabled();
    static void Note(BoundaryEvent event, std::uint32_t finisherJobs = 0, std::uint32_t committerItems = 0);
    static BoundaryTotals Take();
    static void Fold(std::vector<BoundaryRecord>& records, std::int64_t until, BoundaryState& state, BoundaryTotals& totals);
    static std::string Text(const BoundaryTotals& totals);
};

class ThreadPhases {
public:
    static bool Enabled();
    static PhaseThread Current();
    static void AddPhase(DrawDriverPhase which, std::uint64_t ns);
    static void AddDraw(std::uint64_t ns);
    static void AddSubmission(std::uint64_t ns);
    static ThreadPhaseTotals Take();
    static std::string Text(const ThreadPhaseTotals& totals);

    class Role {
    public:
        explicit Role(PhaseThread role);
        ~Role();
        Role(const Role&) = delete;
        Role& operator=(const Role&) = delete;

    private:
        PhaseThread previous;
    };
};

struct DrawPhaseTiming {
    bool profile;
    std::array<double, DrawDriverPhaseCount>& phaseMs;
    std::chrono::steady_clock::time_point& phaseLap;
    void Phase(DrawDriverPhase which);
};

}

#endif
