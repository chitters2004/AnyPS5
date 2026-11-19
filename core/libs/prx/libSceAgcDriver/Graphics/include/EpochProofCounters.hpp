#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_EPOCHPROOFCOUNTERS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_EPOCHPROOFCOUNTERS_HPP

#include "prx/libSceAgcDriver/Execution/include/ProfileCounter.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

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
    class ObjectScope {
    public:
        explicit ObjectScope(const void* object);
        ~ObjectScope();
        ObjectScope(const ObjectScope&) = delete;
        ObjectScope& operator=(const ObjectScope&) = delete;

    private:
        const void* previous;
    };
    template <typename Referenced> void KeepReferenced(Referenced&& referenced) {
        for (const auto& [object, work] : objectWork) {
            if (referenced(object)) continue;
            --counts[static_cast<std::size_t>(work)];
            ++unreferenced[static_cast<std::size_t>(work)];
        }
        objectWork.clear();
    }
    std::uint64_t Unreferenced(ProofWork work) const { return unreferenced[static_cast<std::size_t>(work)]; }
    static void NoteUpload(std::uint64_t address, std::uint64_t bytes);
    std::uint64_t Count(ProofWork work) const { return counts[static_cast<std::size_t>(work)]; }
    std::uint64_t Total() const;
    std::size_t UploadCount() const { return uploadCount; }
    std::pair<std::uint64_t, std::uint64_t> Upload(std::size_t index) const { return uploads[index]; }

private:
    std::array<std::uint64_t, static_cast<std::size_t>(ProofWork::Count)> counts{};
    std::array<std::uint64_t, static_cast<std::size_t>(ProofWork::Count)> unreferenced{};
    std::vector<std::pair<const void*, ProofWork>> objectWork;
    std::array<std::pair<std::uint64_t, std::uint64_t>, 4> uploads{};
    std::size_t uploadCount = 0;
    ProofWorkTrace* previous = nullptr;
};

EpochProofCounters& EpochProofCounts();
bool EpochProofCountsProfiled();
void ReportEpochProofCounts();

}

#endif
