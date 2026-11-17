#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINECREATES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINECREATES_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace AgcDriver {

enum class PipelineCreateKind : std::size_t { VertexInputPart, PreRasterizationPart, FragmentShaderPart, FragmentOutputPart, Link, OptimizedRelink, Monolithic, SecondChannel, BackFaces, Compute, Count };

inline constexpr std::array<const char*, static_cast<std::size_t>(PipelineCreateKind::Count)> PipelineCreateKindNames{"vertex-input part", "pre-raster part", "fragment part", "output part", "link", "optimized relink", "monolithic", "second channel", "back faces", "compute"};

enum class MonolithicReason : std::size_t { NoLibraries, Statistics, Tessellation, DepthBiasPerFace, RectList, BarycentricPatches, AlternateAlpha, FunctionLds, MeshWindow, Interlock, StageOrIdentity, Count };

inline constexpr std::array<const char*, static_cast<std::size_t>(MonolithicReason::Count)> MonolithicReasonNames{"no pipeline libraries", "pipeline statistics", "tessellation", "per-face depth bias", "rect list", "barycentric patches", "alternate alpha", "function LDS", "mesh window", "fragment interlock", "geometry stage or no identity"};

enum class PipelineCacheOutcome : std::uint8_t { Hit, Miss, Unknown };

class PipelineCreates {
public:
    static constexpr std::size_t Kinds = static_cast<std::size_t>(PipelineCreateKind::Count);
    static constexpr std::size_t Reasons = static_cast<std::size_t>(MonolithicReason::Count);

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
    };

    void Note(PipelineCreateKind kind, bool inDraw, PipelineCacheOutcome outcome, double ms, MonolithicReason reason = MonolithicReason::Count);
    Report Take();

    static std::string Line(const Report& report);

private:
    std::mutex mutex;
    Report counts;
};

bool PipelineCreatesProfiled();
PipelineCreates& DevicePipelineCreates();

}

#endif
