#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAWSETCOUNTERS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAWSETCOUNTERS_HPP

#include "prx/libSceAgcDriver/Execution/include/ProfileCounter.hpp"

#include <cstdint>

namespace AgcDriver::Graphics {

struct DrawSetCounters {
    ProfileCounter draws;
    ProfileCounter dynamicElements;
    ProfileCounter overEight;
    ProfileCounter overSixteen;
    ProfileCounter patched;
    ProfileCounter rangeCompares;
    ProfileCounter rangeChanges;
    ProfileCounter rangeChangeDraws;
    ProfileCounter ringSnapshots;
    ProfileCounter otherSnapshots;
    ProfileCounter otherDraws;
    ProfileCounter ringBuffers;
    ProfileCounter multiRingDraws;
    ProfileCounter reboundDraws;
    ProfileCounter faultDraws;
    ProfileCounter uploadChunks;
};

struct DrawSetCounts {
    std::uint64_t draws = 0;
    std::uint64_t dynamicElements = 0;
    std::uint64_t overEight = 0;
    std::uint64_t overSixteen = 0;
    std::uint64_t patched = 0;
    std::uint64_t rangeCompares = 0;
    std::uint64_t rangeChanges = 0;
    std::uint64_t rangeChangeDraws = 0;
    std::uint64_t ringSnapshots = 0;
    std::uint64_t otherSnapshots = 0;
    std::uint64_t otherDraws = 0;
    std::uint64_t ringBuffers = 0;
    std::uint64_t multiRingDraws = 0;
    std::uint64_t reboundDraws = 0;
    std::uint64_t faultDraws = 0;
    std::uint64_t uploadChunks = 0;
};

bool DrawSetCountsProfiled();
DrawSetCounters& DrawSetCountTallies();
DrawSetCounts TakeDrawSetCounts();
void ReportDrawSetCounts();

}

#endif
