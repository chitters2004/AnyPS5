#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINECREATES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINECREATES_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace AgcDriver {

enum class PipelineCreateKind : std::size_t { VertexInputPart, PreRasterizationPart, FragmentShaderPart, FragmentOutputPart, Link, OptimizedRelink, Monolithic, SecondChannel, BackFaces, Compute, Count };

inline constexpr std::array<const char*, static_cast<std::size_t>(PipelineCreateKind::Count)> PipelineCreateKindNames{"vertex-input part", "pre-raster part", "fragment part", "output part", "link", "optimized relink", "monolithic", "second channel", "back faces", "compute"};

enum class MonolithicReason : std::size_t { NoLibraries, Statistics, Tessellation, DepthBiasPerFace, RectList, BarycentricPatches, AlternateAlpha, FunctionLds, MeshWindow, Interlock, StageOrIdentity, Count };

inline constexpr std::array<const char*, static_cast<std::size_t>(MonolithicReason::Count)> MonolithicReasonNames{"no pipeline libraries", "pipeline statistics", "tessellation", "per-face depth bias", "rect list", "barycentric patches", "alternate alpha", "function LDS", "mesh window", "fragment interlock", "geometry stage or no identity"};

enum class PipelineCacheOutcome : std::uint8_t { Hit, Miss, Unknown };

enum class PartMissCause : std::size_t { NewModule, Bindings, Specialization, Context, State, Count };

inline constexpr std::array<const char*, static_cast<std::size_t>(PartMissCause::Count)> PartMissCauseNames{"new module", "other bindings", "other specialization values", "other layout or attachments", "other fixed-function state"};

struct PartOrigin {
    std::uint64_t module = 0;
    std::uint64_t bindings = 0;
    std::uint64_t specialization = 0;
    std::uint64_t context = 0;
    std::uint64_t key = 0;
};

class PipelineCreates {
public:
    static constexpr std::size_t Kinds = static_cast<std::size_t>(PipelineCreateKind::Count);
    static constexpr std::size_t Reasons = static_cast<std::size_t>(MonolithicReason::Count);
    static constexpr std::size_t Causes = static_cast<std::size_t>(PartMissCause::Count);

    struct Cell {
        std::uint64_t count = 0;
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t unknown = 0;
        double ms = 0;
        double longestMs = 0;
    };

    struct Report {
        std::array<std::array<Cell, Kinds>, 2> bySite{};
        std::array<std::uint64_t, Reasons> reasons{};
        std::array<double, Reasons> reasonMs{};
        std::array<std::array<std::uint64_t, 4>, 2> waited{};
        std::array<std::array<double, 4>, 2> waitedMs{};
        std::uint64_t duplicates = 0;
        std::array<std::array<std::uint64_t, Causes>, 2> missCauses{};
        std::array<std::uint64_t, 2> prebuiltUsed{};
    };

    void Note(PipelineCreateKind kind, bool inDraw, PipelineCacheOutcome outcome, double ms, MonolithicReason reason = MonolithicReason::Count);
    void NoteWait(std::size_t part, bool inDraw, double ms);
    void NoteDuplicate();
    void NotePartMiss(std::size_t part, const PartOrigin& origin, bool inDraw);
    void NotePartBuilt(std::size_t part, const PartOrigin& origin, bool inDraw);
    void NotePartHit(std::size_t part, const PartOrigin& origin, bool inDraw);
    Report Take();

    static std::string Line(const Report& report);

private:
    std::mutex mutex;
    Report counts;
    std::array<std::map<std::uint64_t, std::vector<PartOrigin>>, 2> origins;
    std::array<std::set<std::uint64_t>, 2> unusedOutside;
};

bool PipelineCreatesProfiled();
PipelineCreates& DevicePipelineCreates();

}

#endif
