#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_COMMITPHASES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_COMMITPHASES_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace AgcDriver::Graphics {

enum class CommitPhase : std::uint8_t {
    Other,
    Plan,
    Inputs,
    Targets,
    Resolve,
    Revalidate,
    RevalidateEpoch,
    RevalidatePending,
    RevalidateFast,
    RevalidateWalk,
    Classify,
    Record,
    Bindings,
    PassAccess,
    Effects,
    Count
};

struct CommitPhaseTotals {
    std::uint64_t draws = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(CommitPhase::Count)> ns{};
};

bool CommitPhasesEnabled();
void BeginCommitPhase(CommitPhase phase);
CommitPhaseTotals ReadCommitPhases();
CommitPhaseTotals operator-(const CommitPhaseTotals& later, const CommitPhaseTotals& earlier);

class CommitPhaseDraw {
public:
    CommitPhaseDraw();
    ~CommitPhaseDraw();
    CommitPhaseDraw(const CommitPhaseDraw&) = delete;
    CommitPhaseDraw& operator=(const CommitPhaseDraw&) = delete;

private:
    bool owner = false;
};

class CommitPhaseScope {
public:
    explicit CommitPhaseScope(CommitPhase phase);
    ~CommitPhaseScope();
    CommitPhaseScope(const CommitPhaseScope&) = delete;
    CommitPhaseScope& operator=(const CommitPhaseScope&) = delete;

private:
    CommitPhase previous = CommitPhase::Other;
    bool active = false;
};

}

#endif
