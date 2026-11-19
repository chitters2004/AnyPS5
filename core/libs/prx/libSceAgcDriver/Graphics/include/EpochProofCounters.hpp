#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_EPOCHPROOFCOUNTERS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_EPOCHPROOFCOUNTERS_HPP

#include "prx/libSceAgcDriver/Execution/include/ProfileCounter.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace AgcDriver::Graphics {

struct EpochProofCounters {
    ProfileCounter resourceReuses;
    ProfileCounter resourceProofs;
    ProfileCounter storageReuses;
    ProfileCounter mirrorReuses;
    ProfileCounter mirrorRefreshes;
};

enum class ProofWork : std::uint8_t { CpuWriteSync, StorageUpload, Count };

class ProofWorkTrace {
public:
    ProofWorkTrace();
    ~ProofWorkTrace();
    ProofWorkTrace(const ProofWorkTrace&) = delete;
    ProofWorkTrace& operator=(const ProofWorkTrace&) = delete;
    static bool Active();
    static void Note(ProofWork work);
    static void NoteUpload(std::uint64_t address, std::uint64_t bytes);
    std::uint64_t Count(ProofWork work) const { return counts[static_cast<std::size_t>(work)]; }
    std::uint64_t Total() const;
    std::size_t UploadCount() const { return uploadCount; }
    std::pair<std::uint64_t, std::uint64_t> Upload(std::size_t index) const { return uploads[index]; }

private:
    std::array<std::uint64_t, static_cast<std::size_t>(ProofWork::Count)> counts{};
    std::array<std::pair<std::uint64_t, std::uint64_t>, 4> uploads{};
    std::size_t uploadCount = 0;
    ProofWorkTrace* previous = nullptr;
};

EpochProofCounters& EpochProofCounts();
bool EpochProofCountsProfiled();
void ReportEpochProofCounts();

}

#endif
