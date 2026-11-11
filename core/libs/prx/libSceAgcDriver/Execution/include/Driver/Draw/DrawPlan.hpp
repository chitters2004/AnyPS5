#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWPLAN_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWPLAN_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Dispatch/DispatchCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/Recipe.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace AgcDriver::DriverDetail {

struct DrawPlanKey {
    std::vector<std::uint64_t> words;
    std::uint64_t hash = 0;
    bool Matches(const DrawPlanKey& other) const;
};

std::shared_ptr<const DrawPlanKey> BuildDrawPlanKey(std::span<const Graphics::CompiledShader> stages, const Graphics::State& state, const Pm4::DrawParameters& draw);
void AppendDrawPlanStateWords(const Graphics::State& state, std::vector<std::uint64_t>& words);

enum class DrawPlanCount : std::size_t {
    Lookups,
    Absent,
    Present,
    NoDecision,
    Agreed,
    Pipeline,
    FragmentOutputs,
    ShaderStages,
    PushStages,
    HasDepth,
    VertexInput,
    MaskedState,
    SameInstance,
    InstanceDiffers,
    PassDiffersSameInstance,
    MovedBases,
    Unservable,
    Evictions,
    Replaced,
    PlanLookups,
    Hits,
    MissAbsent,
    MissUnservable,
    MissNotRecordable,
    MissTargetGone,
    MissTemplateGone,
    MissObjectsGone,
    MissProof,
    MissInstance,
    Verified,
    HitNanoseconds,
    FullDraws,
    FullNanoseconds,
    SharedBindingStages,
    ComparedBindingStages,
    Count
};

inline constexpr std::size_t DrawPlanCountKinds = static_cast<std::size_t>(DrawPlanCount::Count);

struct DrawPlanCounts {
    std::array<std::uint64_t, DrawPlanCountKinds> values{};
    std::uint64_t operator[](DrawPlanCount count) const { return values[static_cast<std::size_t>(count)]; }
};

struct DrawPlanCounters {
    std::array<std::atomic<std::uint64_t>, DrawPlanCountKinds> values{};
    void Add(DrawPlanCount count, std::uint64_t amount = 1) { values[static_cast<std::size_t>(count)].fetch_add(amount, std::memory_order_relaxed); }
    DrawPlanCounts Load() const;
};

struct DrawPlanDecisionDifferences {
    bool pipeline = false;
    bool fragmentOutputs = false;
    bool shaderStages = false;
    bool pushStages = false;
    bool hasDepth = false;
    bool vertexInput = false;
    bool maskedState = false;
    bool sameInstance = false;
    bool passDiffers = false;
    bool Agree() const { return !pipeline && !fragmentOutputs && !shaderStages && !pushStages && !hasDepth && !vertexInput && !maskedState; }
};

DrawPlanDecisionDifferences CompareDrawPlanDecisions(const DrawRecipe& plan, const DrawRecipe& drawn);

class DrawPlanIndex {
public:
    explicit DrawPlanIndex(std::size_t capacity);
    DrawPlanIndex(const DrawPlanIndex&) = delete;
    DrawPlanIndex& operator=(const DrawPlanIndex&) = delete;

    std::shared_ptr<const DrawRecipe> Find(const DrawPlanKey& key);
    std::size_t Insert(const std::shared_ptr<const DrawPlanKey>& key, std::shared_ptr<const DrawRecipe> decisions);
    std::size_t Size();

private:
    struct Entry {
        std::shared_ptr<const DrawPlanKey> key;
        std::shared_ptr<const DrawRecipe> decisions;
    };
    using Order = std::list<Entry>;
    Order::iterator locate(const DrawPlanKey& key);

    AgcDriver::Mutex mutex;
    std::size_t capacity;
    Order order;
    std::unordered_multimap<std::uint64_t, Order::iterator> index;
};

}

#endif
