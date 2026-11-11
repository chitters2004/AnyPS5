#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWSTAGEMEMO_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWSTAGEMEMO_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "Recompiler.hpp"
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

struct DrawStageMemoCounts {
    std::uint64_t lookups = 0, refused = 0, absent = 0, proved = 0, compared = 0, stale = 0, inserts = 0, evictions = 0, skipped = 0, verified = 0, hitNanoseconds = 0, missNanoseconds = 0, validatedReused = 0;
};

struct DrawStageMemoCounters {
    std::atomic<std::uint64_t> lookups{0}, refused{0}, absent{0}, proved{0}, compared{0}, stale{0}, inserts{0}, evictions{0}, skipped{0}, verified{0}, hitNanoseconds{0}, missNanoseconds{0}, validatedReused{0};
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
