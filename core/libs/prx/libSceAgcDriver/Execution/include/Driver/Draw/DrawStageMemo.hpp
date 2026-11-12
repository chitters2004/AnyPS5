#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWSTAGEMEMO_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWSTAGEMEMO_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "Recompiler.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

struct DrawStageKey {
    std::vector<std::uint64_t> words;
    std::uint64_t hash = 0;
    bool operator==(const DrawStageKey& other) const { return hash == other.hash && words == other.words; }
};

ShaderRecompiler::RecompileRequest DrawStageRequest(const DrawProgram& program, std::uint32_t pushOffset, const Graphics::State& graphics, std::span<const ShaderRecompiler::MemoryRegion> memory, std::span<const ShaderRecompiler::LinkedProgram> linked, const Pm4::DrawParameters& drawParameters, const ShaderRecompiler::SpirvTarget& target, const std::optional<ShaderRecompiler::MeshBarycentricLayout>& meshBarycentrics);

void CompleteDrawStageRequest(ShaderRecompiler::RecompileRequest& request, const DrawProgram& program, const ShaderRecompiler::ShaderPixelStageInfo& pixel, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertex, std::span<const ShaderRecompiler::MemoryRegion> memory, std::span<const ShaderRecompiler::LinkedProgram> linked, const std::optional<ShaderRecompiler::MeshBarycentricLayout>& meshBarycentrics);

void VerifySharedDrawStageRequest(const ShaderRecompiler::RecompileRequest& shared, const DrawProgram& program, std::uint32_t pushOffset, const Graphics::State& graphics, const ShaderRecompiler::ShaderPixelStageInfo& pixel, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertex, std::span<const ShaderRecompiler::MemoryRegion> memory, std::span<const ShaderRecompiler::LinkedProgram> linked, const Pm4::DrawParameters& drawParameters, const ShaderRecompiler::SpirvTarget& target, std::uint64_t deviceSerial, const std::optional<ShaderRecompiler::MeshBarycentricLayout>& meshBarycentrics);

bool BuildDrawStageKey(const ShaderRecompiler::RecompileRequest& request, const ShaderSnapshot& snapshot, std::size_t codeOffset, std::uint64_t deviceSerial, DrawStageKey& key);
bool BuildDrawStageKey(const ShaderRecompiler::RecompileRequest& request, const ShaderRecompiler::ShaderPixelStageInfo* pixel, const ShaderRecompiler::ShaderVertexStageInfo* vertex, const ShaderSnapshot& snapshot, std::size_t codeOffset, std::uint64_t deviceSerial, DrawStageKey& key);

std::vector<std::pair<std::uint64_t, std::size_t>> DrawStageSpans(std::span<const std::pair<std::uint64_t, std::uint64_t>> runs);

const char* DrawStageMismatch(const DispatchVariant& kept, std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::span<const std::uint32_t> words, const ShaderRecompiler::RecompileResult& compiled);

struct DrawStageStamp {
    std::uint64_t generation = 0;
    std::uint64_t layout = 0;
};

struct DrawStageRecord {
    DrawStageKey key;
    bool convertedSamples = false;
    bool layerRowSamples = false;
    std::shared_ptr<DispatchVariant> variant;
    std::vector<std::pair<std::uint64_t, std::size_t>> spans;
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::uint64_t> layout{0};

    DrawStageStamp Stamp() const;
    void Restamp(const DrawStageStamp& stamp);
};

enum class DrawStageProof : std::uint8_t { Proved, Compared, Stale };

enum class DrawStageMemoCount : std::size_t { Lookups, Refused, Absent, AbsentAdmitted, Proved, Compared, ComparedBytes, Stale, Inserts, Evictions, Skipped, Verified, HitNanoseconds, MissNanoseconds, ValidatedReused, FragmentLookups, FragmentServed, DrawMisses, ReusedMisses, Count };

inline constexpr std::size_t DrawStageMemoCountKinds = static_cast<std::size_t>(DrawStageMemoCount::Count);

struct DrawStageMemoCounts {
    std::array<std::uint64_t, DrawStageMemoCountKinds> values{};
    std::uint64_t operator[](DrawStageMemoCount count) const { return values[static_cast<std::size_t>(count)]; }
    std::uint64_t Served() const { return (*this)[DrawStageMemoCount::Proved] + (*this)[DrawStageMemoCount::Compared]; }
};

struct DrawStageMemoCounters {
    std::array<std::atomic<std::uint64_t>, DrawStageMemoCountKinds> values{};
    void Add(DrawStageMemoCount count, std::uint64_t amount = 1) { values[static_cast<std::size_t>(count)].fetch_add(amount, std::memory_order_relaxed); }
    DrawStageMemoCounts Load() const;
};

class DrawStageMemo {
public:
    explicit DrawStageMemo(std::size_t capacity);
    DrawStageMemo(const DrawStageMemo&) = delete;
    DrawStageMemo& operator=(const DrawStageMemo&) = delete;

    std::shared_ptr<DrawStageRecord> Find(const DrawStageKey& key, bool& admit);
    std::size_t Insert(const std::shared_ptr<DrawStageRecord>& record);
    bool Erase(const std::shared_ptr<DrawStageRecord>& record);
    std::size_t Size();
    void Clear();

private:
    using Order = std::list<std::shared_ptr<DrawStageRecord>>;
    Order::iterator locate(const DrawStageKey& key);

    AgcDriver::Mutex mutex;
    std::size_t capacity;
    Order order;
    std::unordered_multimap<std::uint64_t, Order::iterator> index;
    DrawKeySightings sightings;
};

}

#endif
