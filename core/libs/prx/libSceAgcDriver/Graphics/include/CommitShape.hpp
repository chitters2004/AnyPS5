#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_COMMITSHAPE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_COMMITSHAPE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"

namespace AgcDriver {

enum CommitStamp : std::size_t { StampEpoch, StampProofs, StampPending, StampUnwatch, StampAllocations, StampSurfaces, StampReleased, StampPublished, StampCount };

using CommitStamps = std::array<std::uint64_t, StampCount>;

enum BuildReason : std::size_t { BuildUncacheable, BuildNewKey, BuildReturnedKey, BuildSpaceChanged, BuildInvalidated, BuildInUse, BuildNotReusable, BuildReasonCount };

inline constexpr std::array<const char*, BuildReasonCount> BuildReasonNames{"uncacheable", "new key", "evicted key returned", "space changed", "invalidated", "in use", "not reusable"};

enum WalkEffect : std::size_t { EffectDepthPlanes, EffectCpuWrites, EffectMultisample, EffectOwnRefresh, EffectUpload, EffectPendingFlush, EffectStaging, EffectSubscribe, WalkEffectCount };

inline constexpr std::array<const char*, WalkEffectCount> WalkEffectNames{"depth planes", "CPU writes", "multisample claim", "own refresh", "upload", "pending flush", "staging", "subscribe"};

enum ProofEnding : std::size_t { EndingEpoch, EndingFast, EndingFull, EndingRefused, ProofEndingCount };
inline constexpr std::array<const char*, ProofEndingCount> ProofEndingNames{"epoch exit", "fast-ending", "full-walk-ending", "refused before the walk"};
struct ProofLeadNote {
    bool looked = false;
    bool stamped = false;
    std::uint64_t lead = 0;
    bool survived = false;
    bool epochAdvanced = false;
    bool walked = false;
    bool lookups = false;
    bool served = false;
    std::uint32_t effects = 0;
    std::size_t ending = ProofEndingCount;
    double revalidateUs = 0;
    bool collectAdvanced = false;
};

bool CommitShapeProfiled();

class CommitShape {
public:
    static constexpr std::size_t Kinds = 4;
    static constexpr std::size_t Phases = 13;
    static constexpr std::array<std::size_t, 5> Distances{1, 2, 4, 8, 16};
    static constexpr std::size_t History = 16;

    struct Report {
        std::uint64_t draws = 0;
        std::array<std::uint64_t, Kinds> kindDraws{};
        std::array<std::array<double, Phases>, Kinds> kindPhaseUs{};
        std::uint64_t dependentDraws = 0;
        std::uint64_t dependencies = 0;
        std::array<std::uint64_t, Distances.size()> compared{};
        std::array<std::uint64_t, Distances.size()> survived{};
        std::array<std::array<std::uint64_t, StampCount>, Distances.size()> moved{};
        std::array<std::uint64_t, Kinds> kindPipelines{};
        std::array<std::uint64_t, Kinds> kindPipelineDraws{};
        std::array<std::uint64_t, BuildReasonCount> reasonBuilds{};
        std::array<std::array<double, Phases>, BuildReasonCount> reasonPhaseUs{};
        std::array<std::uint64_t, BuildReasonCount> reasonPipelines{};
        std::array<std::uint64_t, BuildReasonCount> reasonPipelineDraws{};
        static constexpr std::array<std::uint64_t, 5> LeadBounds{0, 4, 8, 16, 32};
        std::uint64_t leadLooked = 0, leadStamped = 0, leadSurvived = 0, leadEpochAdvanced = 0, walked = 0, walkedLookups = 0, walkedServed = 0, servedStamped = 0, servedSurvived = 0, servedPure = 0, servedPureSurvived = 0;
        std::uint64_t leadCollectAdvanced = 0, walkedStamped = 0, walkedCollectAdvanced = 0, walkedKept = 0;
        double walkedStampedUs = 0, walkedKeptUs = 0;
        std::array<std::uint64_t, LeadBounds.size() + 1> leadBuckets{};
        std::array<std::uint64_t, WalkEffectCount> servedEffects{};
        std::array<std::uint64_t, ProofEndingCount> endings{};
        std::array<double, ProofEndingCount> endingUs{};
    };

    void Note(std::size_t kind, std::span<const double, Phases> phaseUs, std::uint64_t dependencies, const CommitStamps& stamps, std::uint64_t pipelines = 0, std::size_t buildReason = BuildReasonCount, const ProofLeadNote& lead = {});
    Report Take();

    static std::string Line(const Report& report, std::span<const char* const, Kinds> kindNames, std::span<const char* const, Phases> phaseNames);
    static std::string BuildLine(const Report& report, std::span<const char* const, Kinds> kindNames, std::size_t resourcesPhase, std::size_t pipelinePhase);
    static std::string ProofLeadLine(const Report& report);

private:
    Mutex mutex;
    Report counts;
    std::array<CommitStamps, History> history{};
    std::uint64_t noted = 0;
};

}

#endif
