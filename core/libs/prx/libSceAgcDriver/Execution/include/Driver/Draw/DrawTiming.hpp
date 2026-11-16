#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAWTIMING_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAWTIMING_HPP

#include <array>
#include <chrono>
#include <cstdint>
#include <list>
#include <string>

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
    std::array<std::uint64_t, Threads> drawNs{}, draws{};
    std::uint64_t busyNs = 0, submissions = 0, windowNs = 0;
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
