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
    TargetsClaims,
    TargetsLookup,
    TargetsRefresh,
    TargetsClearCheck,
    TargetsView,
    TargetsSettle,
    TargetsMultisample,
    TargetsProved,
    TargetsCmask,
    TargetsDepthPrep,
    TargetsDepthClaims,
    TargetsDepthClear,
    TargetsDepthFind,
    TargetsDepthSync,
    TargetsDepthStale,
    PlanHitFind,
    PlanHitWords,
    PlanHitInstance,
    PlanHitTargets,
    PlanHitRebind,
    ResolveSetup,
    ResolveSpace,
    ResolveMoved,
    ResolveBuild,
    ResolveEvict,
    TargetsLookupPure,
    TargetsLookupGuards,
    TargetsLookupCache,
    InputsValidate,
    InputsFlush,
    InputsInPlace,
    InputsReuse,
    InputsArm,
    InputsAlloc,
    InputsKeep,
    InputsIndexRange,
    InputsExpand,
    InputsLayout,
    RecordSetup,
    RecordPass,
    RecordBegin,
    RecordBind,
    RecordPush,
    RecordCommands,
    RecordLeave,
    RecordAfter,
    RecordRecipe,
    RecordStores,
    RecordPassKey,
    RecordBeginContinue,
    RecordBeginProxies,
    RecordBeginBarriers,
    RecordBeginLds,
    RecordBeginArguments,
    RecordBeginOrder,
    RecordBeginStaged,
    RecordBeginPipeline,
    RecordBeginBind,
    RecordBeginDynamic,
    RecordBeginLibrary,
    RevalidateCertified,
    Count
};

enum class CommitCounter : std::uint8_t {
    TargetDraws,
    RecipeTargetDraws,
    ColorTargets,
    DepthTargets,
    RepeatedTargets,
    TargetRefreshEpoch,
    TargetRefreshProved,
    TargetRefreshSlow,
    OtherRefreshEpoch,
    OtherRefreshProved,
    OtherRefreshSlow,
    TargetEpochOff,
    TargetEpochMoved,
    TargetEpochProofMoved,
    TargetEpochPendingMoved,
    TargetEpochBySubmission,
    TargetEpochByWait,
    TargetEpochByDrain,
    TargetEpochByReap,
    TargetEpochByPacket,
    TargetEpochBySeveral,
    TargetEpochByUnknown,
    TargetProofNone,
    TargetProofGeneration,
    TargetProofUnlocked,
    TargetProofRefused,
    TargetProofKeys,
    TargetProofChanged,
    TargetProofPending,
    DepthPreps,
    DepthSameSurface,
    DepthCreated,
    DepthCleared,
    DepthMirrored,
    DepthStaleScans,
    EpochFollows,
    EpochFollowsBackward,
    EpochFollowsBySubmission,
    EpochFollowsByWait,
    EpochFollowsByDrain,
    EpochFollowsByReap,
    EpochFollowsByPacket,
    EpochFollowsByNone,
    TargetMissFirstInEpoch,
    TargetSlowAgainInEpoch,
    EpochFollowsSkipped,
    DepthFoundRepeated,
    ResolveDraws,
    ResolveCacheable,
    ResolveFound,
    ResolveSpaceChecks,
    ResolveValid,
    ResolveHits,
    ResolveMovedHits,
    ResolveBuilds,
    ResolveInvalidated,
    ResolveAddressStages,
    PreparedSurfaceHits,
    PreparedSurfaceMisses,
    TargetEpochNeverStamped,
    TargetEpochOtherThread,
    TargetEpochThisThread,
    TargetEpochExitStamps,
    InputDraws,
    InputIndexed,
    InputIndexInPlace,
    InputIndexRangeKnown,
    InputIndexExpanded,
    InputIndexCompacted,
    InputIndexClamped,
    InputVertexRanges,
    InputVertexInPlace,
    InputVertexPadded,
    InputVertexPaddedReused,
    InputVertexZeroed,
    InputIndexCopyFresh,
    InputIndexCopySnapshot,
    InputIndexCopyOwned,
    InputIndexCopyArmed,
    InputVertexCopyFresh,
    InputVertexCopySnapshot,
    InputVertexCopyOwned,
    InputVertexCopyArmed,
    Count
};

enum class EpochCause : std::uint8_t {
    Submission,
    Wait,
    Drain,
    Reap,
    Packet,
    Count
};

enum class CommitDrawKind : std::uint8_t {
    Full,
    RecipeHit,
    PlanHit,
    Count
};

struct CommitPhaseTotals {
    std::uint64_t draws = 0;
    std::uint64_t keys = 0;
    std::uint64_t keyWords = 0;
    std::uint64_t keyMemoHits = 0;
    std::uint64_t keyMemoMisses = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(CommitCounter::Count)> counters{};
    std::array<std::uint64_t, static_cast<std::size_t>(CommitPhase::Count)> ns{};
    std::array<std::uint64_t, static_cast<std::size_t>(CommitDrawKind::Count)> kindDraws{};
    std::array<std::array<std::uint64_t, static_cast<std::size_t>(CommitPhase::Count)>, static_cast<std::size_t>(CommitDrawKind::Count)> kindNs{};
};

bool CommitPhasesEnabled();
const char* CommitPhaseName(CommitPhase phase);
const char* CommitPhaseGroup(CommitPhase phase);
void BeginCommitPhase(CommitPhase phase);
void NoteCommitKey(std::size_t words);
void NoteCommitKeyMemo(bool hit);
void NoteCommitDrawKind(CommitDrawKind kind);
void CountCommit(CommitCounter counter, std::uint64_t amount = 1);
void NoteEpochCauses(unsigned causes);
void NoteEpochFollow(bool backward, unsigned causes);
void NoteEpochFollowSkipped();
unsigned EpochCauses();
CommitCounter EpochCauseCounter(unsigned causes);
CommitPhase CurrentCommitPhase();
void RelabelCommitPhase(CommitPhase phase);
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
