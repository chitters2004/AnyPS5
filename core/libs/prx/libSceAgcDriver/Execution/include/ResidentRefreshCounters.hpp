#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_RESIDENTREFRESHCOUNTERS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_RESIDENTREFRESHCOUNTERS_HPP

#include "prx/libSceAgcDriver/Execution/include/ProfileCounter.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace AgcDriver {

enum class ProofBreak : std::size_t {
    NonDraw,
    DrawWrites,
    CopiedWriters,
    Labels,
    OwnStores,
    MarkedWrites,
    WriteBacks,
    GpuMutex,
    Count,
};

constexpr std::size_t ProofBreakKinds = static_cast<std::size_t>(ProofBreak::Count);

using ProofBreakSerials = std::array<std::uint64_t, ProofBreakKinds>;

struct ResidentRefreshCounters {
    ProfileCounter submissions;
    ProfileCounter epochs;
    ProfileCounter exits;
    ProfileCounter proved;
    ProfileCounter written;
    ProfileCounter refused;
    ProfileCounter collects;
    ProfileCounter collectBytes;
    ProfileCounter walkedBytes;
    ProfileCounter sampledNs;
    ProfileCounter samples;
    std::array<ProfileCounter, ProofBreakKinds> brokeBefore;
    std::array<ProfileCounter, ProofBreakKinds> brokeBeforeProved;
};

ResidentRefreshCounters& ResidentRefreshCounts();
bool ResidentRefreshProfiled();
ProofBreakSerials ProofBreakCounts();
void ReportResidentRefreshCounts();

}

#endif
