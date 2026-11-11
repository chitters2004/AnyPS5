#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_COMMITPHASES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_COMMITPHASES_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

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
    PlanStore,
    PlanFramebuffer,
    InputsCollect,
    InputsLocal,
    TargetsLru,
    ResolveCache,
    ResolveAlias,
    RecordLocal,
    PassAccessLocal,
    EpilogueWriters,
    EpilogueRecipe,
    PlanKey,
    Count
};

struct CommitPhaseTotals {
    std::uint64_t draws = 0;
    std::uint64_t keys = 0;
    std::uint64_t keyWords = 0;
    std::uint64_t keyMemoHits = 0;
    std::uint64_t keyMemoMisses = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(CommitPhase::Count)> ns{};
};

bool CommitPhasesEnabled();
const char* CommitPhaseName(CommitPhase phase);
void BeginCommitPhase(CommitPhase phase);
void NoteCommitKey(std::size_t words);
void NoteCommitKeyMemo(bool hit);
CommitPhaseTotals ReadCommitPhases();
CommitPhaseTotals operator-(const CommitPhaseTotals& later, const CommitPhaseTotals& earlier);

class CommitPhaseDraw {
public:
    CommitPhaseDraw();
    ~CommitPhaseDraw();
    CommitPhaseDraw(const CommitPhaseDraw&) = delete;
    CommitPhaseDraw& operator=(const CommitPhaseDraw&) = delete;

private:
    CommitPhase previous = CommitPhase::Other;
    bool owner = false;
    bool nested = false;
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

template <class Work>
decltype(auto) InCommitPhase(CommitPhase phase, Work&& work) {
    const CommitPhaseScope scope(phase);
    return std::forward<Work>(work)();
}

}

#endif
