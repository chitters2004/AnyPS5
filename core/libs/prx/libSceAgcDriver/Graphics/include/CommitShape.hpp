#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_COMMITSHAPE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_COMMITSHAPE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"

namespace AgcDriver {

enum CommitStamp : std::size_t { StampEpoch, StampProofs, StampPending, StampUnwatch, StampAllocations, StampSurfaces, StampReleased, StampClaims, StampPublished, StampCount };

using CommitStamps = std::array<std::uint64_t, StampCount>;

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
    };

    void Note(std::size_t kind, std::span<const double, Phases> phaseUs, std::uint64_t dependencies, const CommitStamps& stamps);
    Report Take();

    static std::string Line(const Report& report, std::span<const char* const, Kinds> kindNames, std::span<const char* const, Phases> phaseNames);

private:
    Mutex mutex;
    Report counts;
    std::array<CommitStamps, History> history{};
    std::uint64_t noted = 0;
};

}

#endif
