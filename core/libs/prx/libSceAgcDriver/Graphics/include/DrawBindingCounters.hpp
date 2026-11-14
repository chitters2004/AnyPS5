#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAWBINDINGCOUNTERS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAWBINDINGCOUNTERS_HPP

#include "prx/libSceAgcDriver/Execution/include/ProfileCounter.hpp"

#include <cstdint>

namespace AgcDriver::Graphics {

struct DrawBindingCounters {
    ProfileCounter calls;
    ProfileCounter allocations;
    ProfileCounter moved;
    ProfileCounter walkPairs;
    ProfileCounter walkScans;
    ProfileCounter overrideUploads;
    ProfileCounter overrideBytes;
    ProfileCounter rangeUploads;
    ProfileCounter rangeBytes;
    ProfileCounter unwrittenLookups;
    ProfileCounter unwrittenHits;
    ProfileCounter unwrittenNotes;
    ProfileCounter unwrittenInserts;
    ProfileCounter unwrittenReplaced;
    ProfileCounter unwrittenRefused;
    ProfileCounter unwrittenRetires;
    ProfileCounter unwrittenRetired;
    ProfileCounter keepProbes;
    ProfileCounter keepOwners;
};

struct DrawBindingCounts {
    std::uint64_t calls = 0;
    std::uint64_t allocations = 0;
    std::uint64_t moved = 0;
    std::uint64_t walkPairs = 0;
    std::uint64_t walkScans = 0;
    std::uint64_t overrideUploads = 0;
    std::uint64_t overrideBytes = 0;
    std::uint64_t rangeUploads = 0;
    std::uint64_t rangeBytes = 0;
    std::uint64_t unwrittenLookups = 0;
    std::uint64_t unwrittenHits = 0;
    std::uint64_t unwrittenNotes = 0;
    std::uint64_t unwrittenInserts = 0;
    std::uint64_t unwrittenReplaced = 0;
    std::uint64_t unwrittenRefused = 0;
    std::uint64_t unwrittenRetires = 0;
    std::uint64_t unwrittenRetired = 0;
    std::uint64_t keepProbes = 0;
    std::uint64_t keepOwners = 0;
};

bool DrawBindingCountsProfiled();
DrawBindingCounters& DrawBindingCountTallies();
DrawBindingCounts TakeDrawBindingCounts();
void ReportDrawBindingCounts();

}

#endif
