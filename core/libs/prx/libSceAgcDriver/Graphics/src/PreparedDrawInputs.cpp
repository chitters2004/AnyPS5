#include "prx/libSceAgcDriver/Graphics/include/PreparedDrawInputs.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTrace.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace AgcDriver::Graphics {

DrawVertexFetches PlanDrawVertexFetches(std::optional<std::pair<std::uint64_t, std::uint64_t>> colorTarget, const Pm4::DrawParameters& draw, std::span<const ShaderRecompiler::VertexAttribute> attributes, const VertexInputLayout& layout, std::uint32_t maxIndex) {
    const auto* args = draw.indirect ? &*draw.indirect : nullptr;
    DrawVertexFetches planned;
    auto& fetches = planned.fetches;
    auto& fetchValid = planned.fetchValid;
    fetches.reserve(attributes.size());
    fetchValid.reserve(attributes.size());
    planned.fetchOf.assign(attributes.size(), 0);
    planned.zeroBytes.assign(attributes.size(), std::nullopt);
    for (std::size_t i = 0; i < attributes.size(); ++i) {
        const auto& attribute = attributes[i];
        const auto described = std::find_if(layout.attributes.begin(), layout.attributes.end(), [&](const auto& entry) { return entry.location == attribute.location; });
        Require(described != layout.attributes.end(), "vertex attribute missing from its input layout");
        const auto narrow = DecodeVertexFormat(attribute);
        const std::size_t widened = described->format == narrow.format ? 0u : VertexReadBytes(attribute, described->format) - narrow.bytes;
        if (NullVertexDescriptor(attribute) || VertexFetchOutOfRange(attribute)) {
            planned.zeroBytes[i] = narrow.bytes + widened;
            continue;
        }
        const auto bytes = (args != nullptr ? VertexBufferExtent(attribute) : VertexBufferReadSize(attribute, maxIndex, draw.instanceCount, draw.firstInstance)) + widened;
        const auto& fields = attribute.resource.fields;
        const auto address = fields[0] | (static_cast<std::uint64_t>(fields[1] & 0xffffu) << 32u);
        const auto firstByte = args == nullptr ? VertexBufferReadOffset(attribute, draw.firstVertex, draw.firstInstance) : 0;
        Require(!colorTarget || address + bytes <= colorTarget->first || colorTarget->first + colorTarget->second <= address + firstByte, "vertex buffer aliases the render target");
        const auto stride = (fields[1] >> 16u) & 0x3fffu;
        planned.fetchOf[i] = fetches.size();
        fetches.push_back({address, address + bytes, stride, attribute.fetchIndex, DecodeVertexFormat(attribute).alignment});
        const auto recordBytes = static_cast<std::uint64_t>(fields[2]) * stride;
        fetchValid.push_back(args == nullptr && stride != 0 ? static_cast<std::size_t>(std::min(recordBytes, static_cast<std::uint64_t>(bytes))) : bytes);
    }
    planned.soloFetches = SoloZeroPaddedFetchIndices(fetches, fetchValid);
    planned.isSolo.assign(fetches.size(), false);
    for (const auto f : planned.soloFetches) planned.isSolo[f] = true;
    std::vector<VertexFetch> plannedFetches;
    plannedFetches.reserve(fetches.size());
    planned.plannedOf.assign(fetches.size(), 0);
    for (std::size_t f = 0; f < fetches.size(); ++f) {
        if (planned.isSolo[f]) continue;
        planned.plannedOf[f] = plannedFetches.size();
        plannedFetches.push_back(fetches[f]);
    }
    planned.plan = PlanVertexCopies(plannedFetches);
    return planned;
}

namespace {

constexpr std::uint32_t ZeroFetchSlack = 0x10000u;
constexpr std::size_t PreparedRangeLimit = Recorder::DrawInputBudget;

enum class JobState : std::uint32_t { Queued, Running, Done, Cancelled };

struct PreparedRange {
    std::uint64_t address = 0;
    std::size_t bytes = 0;
    std::size_t readable = 0;
    Recorder::SnapshotUse use = Recorder::SnapshotUse::Vertex;
    std::uint64_t generation = 0;
    std::uint64_t registry = 0;
    std::uint64_t forget = 0;
    std::uint32_t derived = 0;
    std::shared_ptr<Buffer> buffer;
    VkDeviceSize offset = 0;
    bool ring = false;
    VkBufferUsageFlags usage = 0;
    bool consumed = false;

    std::span<std::byte> Bytes() const {
        return buffer->Bytes().subspan(static_cast<std::size_t>(offset), bytes);
    }
};

template <typename Shard> class CounterShards {
public:
    static CounterShards& Get() {
        static auto* shards = new CounterShards;
        return *shards;
    }

    Shard* Lease() {
        std::lock_guard lock(mutex);
        if (!idle.empty()) {
            auto* shard = idle.back();
            idle.pop_back();
            return shard;
        }
        return &store.emplace_back();
    }

    void Return(Shard* shard) {
        std::lock_guard lock(mutex);
        idle.push_back(shard);
    }

    template <typename Visit> void ForEach(Visit&& visit) {
        std::lock_guard lock(mutex);
        for (const auto& shard : store) visit(shard);
    }

private:
    std::mutex mutex;
    std::deque<Shard> store;
    std::vector<Shard*> idle;
};

template <typename Shard> struct CounterLease {
    Shard* shard = nullptr;
    ~CounterLease() {
        if (shard != nullptr) CounterShards<Shard>::Get().Return(shard);
    }
};

template <typename Shard> Shard& LocalShard() {
    auto& lease = HostThreadLocal<CounterLease<Shard>, CounterLease<Shard>>();
    if (lease.shard == nullptr) lease.shard = CounterShards<Shard>::Get().Lease();
    return *lease.shard;
}

void add(std::atomic<std::uint64_t>& total, std::uint64_t amount) {
    total.store(total.load(std::memory_order_relaxed) + amount, std::memory_order_relaxed);
}

void sum(std::uint64_t& into, const std::atomic<std::uint64_t>& from) {
    into += from.load(std::memory_order_relaxed);
}

struct alignas(64) Counters {
    std::atomic<std::uint64_t> requests{0};
    std::atomic<std::uint64_t> ranges{0};
    std::atomic<std::uint64_t> prepared{0};
    std::atomic<std::uint64_t> refused{0};
    std::array<std::atomic<std::uint64_t>, PreparedRefusalCount> refusedBy{};
    std::atomic<std::uint64_t> adopted{0};
    std::atomic<std::uint64_t> reprepared{0};
    std::atomic<std::uint64_t> notReady{0};
    std::atomic<std::uint64_t> waited{0};
    std::atomic<std::uint64_t> unused{0};
    std::atomic<std::uint64_t> checks{0};
    std::atomic<std::uint64_t> checkNanoseconds{0};
    std::atomic<std::uint64_t> verifyDiffered{0};
    std::atomic<std::uint64_t> requestNanoseconds{0};
    std::atomic<std::uint64_t> epochFollows{0};
    std::atomic<std::uint64_t> collectNanoseconds{0};
    std::array<std::atomic<std::uint64_t>, 2> allocNanoseconds{};
    std::array<std::atomic<std::uint64_t>, 2> poolWaitNanoseconds{};
    std::array<std::atomic<std::uint64_t>, 2> stashHits{};
    std::atomic<std::uint64_t> stashReturned{0};
    std::atomic<std::uint64_t> spareReused{0};
    std::array<std::atomic<std::uint64_t>, DrawInputSourceCount> sources{};
    std::array<std::atomic<std::uint64_t>, DrawInputSourceCount> sourceBytes{};
    std::array<std::atomic<std::uint64_t>, DrawInputSourceCount> sourceNanoseconds{};
};

Counters& counters() {
    return LocalShard<Counters>();
}

void counted(std::atomic<std::uint64_t>& total, const char* scope, std::uint64_t amount = 1) {
    if (amount == 0) return;
    add(total, amount);
    PerformanceTrace::CountOwnedDraw(scope, amount);
}

constexpr std::array<const char*, PreparedRefusalCount> RefusalNames{"DrawInputPrep.refused_size", "DrawInputPrep.refused_inaccessible", "DrawInputPrep.refused_forget", "DrawInputPrep.refused_pending", "DrawInputPrep.refused_unwatched", "DrawInputPrep.refused_snapshot_changed", "DrawInputPrep.refused_failed"};
constexpr std::array<const char*, DrawInputSourceCount> SourceNames{"DrawInputPrep.source_owned_reused", "DrawInputPrep.source_snapshot_reused", "DrawInputPrep.source_adopted", "DrawInputPrep.source_copied"};
constexpr std::array<const char*, DrawInputSourceCount> SourceByteNames{"DrawInputPrep.bytes_owned_reused", "DrawInputPrep.bytes_snapshot_reused", "DrawInputPrep.bytes_adopted", "DrawInputPrep.bytes_copied"};
constexpr std::array<const char*, DrawInputSourceCount> SourceTimeNames{"DrawInputPrep.ns_owned_reused", "DrawInputPrep.ns_snapshot_reused", "DrawInputPrep.ns_adopted", "DrawInputPrep.ns_copied"};

std::atomic<int> configuredPreparers{-1};
std::atomic<bool> preparersHeld{false};
std::atomic<void (*)(PreparedDrawInputs&)> preparedHook{nullptr};

unsigned wantedPreparers() {
    const auto configured = configuredPreparers.load(std::memory_order_acquire);
    if (configured >= 0) return static_cast<unsigned>(configured);
    static const unsigned fromEnvironment = [] {
        const char* text = std::getenv("APS5_DRAW_INPUT_PREPARERS");
        return text != nullptr ? static_cast<unsigned>(std::clamp(std::atoi(text), 0, 8)) : 2u;
    }();
    return fromEnvironment;
}

std::uint64_t nanosecondsSince(std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

constexpr std::size_t StashLimit = 64;
constexpr std::size_t StashBytesLimit = std::size_t{8} << 20u;
constexpr std::array<const char*, 2> AllocNames{"DrawInputPrep.alloc_ns_preparer", "DrawInputPrep.alloc_ns_committer"};
constexpr std::array<const char*, 2> PoolWaitNames{"DrawInputPrep.pool_wait_ns_preparer", "DrawInputPrep.pool_wait_ns_committer"};
constexpr std::array<const char*, 2> StashHitNames{"DrawInputPrep.stash_hits_preparer", "DrawInputPrep.stash_hits_committer"};

struct StashedBuffer {
    VkDevice device = VK_NULL_HANDLE;
    std::size_t bytes = 0;
    VkBufferUsageFlags usage = 0;
    VkMemoryPropertyFlags properties = 0;
    std::shared_ptr<Buffer> buffer;
};

struct BufferStash {
    std::mutex mutex;
    std::vector<StashedBuffer> buffers;
    std::size_t bytes = 0;
    bool open = false;
};

BufferStash& bufferStash() {
    static BufferStash stash;
    return stash;
}

void openStash(bool open) {
    std::vector<StashedBuffer> dropped;
    auto& stash = bufferStash();
    std::lock_guard lock(stash.mutex);
    stash.open = open;
    if (open) return;
    dropped.swap(stash.buffers);
    stash.bytes = 0;
}

bool recycling() {
    return !RuntimeSwitches::Set(RuntimeSwitch::NoDrawInputRecycling);
}

std::shared_ptr<Buffer> takeStashed(VkDevice device, std::size_t bytes, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) {
    if (!recycling()) return nullptr;
    auto& stash = bufferStash();
    std::lock_guard lock(stash.mutex);
    for (auto it = stash.buffers.rbegin(); it != stash.buffers.rend(); ++it) {
        if (it->device != device || it->bytes != bytes || it->usage != usage || it->properties != properties) continue;
        auto buffer = std::move(it->buffer);
        stash.bytes -= bytes;
        stash.buffers.erase(std::next(it).base());
        return buffer;
    }
    return nullptr;
}

bool returnStashed(VkDevice device, std::size_t bytes, VkBufferUsageFlags usage, std::shared_ptr<Buffer>& buffer) {
    if (buffer == nullptr) return false;
    const auto properties = buffer->Properties();
    if (buffer.use_count() != 1 || !recycling() || bytes > StashBytesLimit) return false;
    auto& stash = bufferStash();
    std::shared_ptr<Buffer> evicted;
    std::lock_guard lock(stash.mutex);
    if (!stash.open) return false;
    while (!stash.buffers.empty() && (stash.buffers.size() >= StashLimit || stash.bytes + bytes > StashBytesLimit)) {
        stash.bytes -= stash.buffers.front().bytes;
        evicted = std::move(stash.buffers.front().buffer);
        stash.buffers.erase(stash.buffers.begin());
    }
    stash.buffers.push_back({device, bytes, usage, properties, std::move(buffer)});
    stash.bytes += bytes;
    return true;
}

struct alignas(64) UploadCounters {
    std::atomic<std::uint64_t> requests{0};
    std::atomic<std::uint64_t> notReady{0};
    std::atomic<std::uint64_t> waited{0};
    std::atomic<std::uint64_t> stageAPrepared{0};
    std::atomic<std::uint64_t> stageAAdopted{0};
    std::atomic<std::uint64_t> stageAStale{0};
    std::atomic<std::uint64_t> stageAStaleStaging{0};
    std::array<std::atomic<std::uint64_t>, PreparedStalenessCount> stageAStaleBy{};
    std::atomic<std::uint64_t> stageAStagingPredicted{0};
    std::atomic<std::uint64_t> stageAStagingFallback{0};
    std::atomic<std::uint64_t> stageAStagingMispredicted{0};
    std::atomic<std::uint64_t> stageAUnused{0};
    std::atomic<std::uint64_t> stageAPredictedHit{0};
    std::atomic<std::uint64_t> stageARefused{0};
    std::atomic<std::uint64_t> stageAFailed{0};
    std::atomic<std::uint64_t> stageAUnpinned{0};
    std::atomic<std::uint64_t> stageANanoseconds{0};
    std::atomic<std::uint64_t> stageAVerifyDiffered{0};
    std::atomic<std::uint64_t> regionsPrepared{0};
    std::atomic<std::uint64_t> regionsAdopted{0};
    std::atomic<std::uint64_t> regionsReprepared{0};
    std::atomic<std::uint64_t> regionsUnused{0};
    std::atomic<std::uint64_t> regionsRefused{0};
    std::array<std::atomic<std::uint64_t>, PreparedRefusalCount> regionsRefusedBy{};
    std::atomic<std::uint64_t> regionBytesAdopted{0};
    std::atomic<std::uint64_t> regionCheckNanoseconds{0};
    std::atomic<std::uint64_t> regionCollectNanoseconds{0};
    std::atomic<std::uint64_t> regionSpareReused{0};
    std::atomic<std::uint64_t> regionVerifyDiffered{0};
};

UploadCounters& uploadCounters() {
    return LocalShard<UploadCounters>();
}

void countedUpload(std::atomic<std::uint64_t>& total, const char* scope, std::uint64_t amount = 1) {
    if (amount == 0) return;
    add(total, amount);
    PerformanceTrace::CountOwnedDraw(scope, amount);
}

constexpr std::array<const char*, PreparedStalenessCount> StaleNames{"UploadPrep.stage_a_current", "UploadPrep.stage_a_stale_unusable", "UploadPrep.stage_a_stale_registry", "UploadPrep.stage_a_stale_forget", "UploadPrep.stage_a_stale_recorder", "UploadPrep.stage_a_stale_mirrors", "UploadPrep.stage_a_stale_image_views", "UploadPrep.stage_a_stale_staging_decisions"};
constexpr std::array<const char*, PreparedRefusalCount> RegionRefusalNames{"UploadPrep.regions_refused_size", "UploadPrep.regions_refused_inaccessible", "UploadPrep.regions_refused_forget", "UploadPrep.regions_refused_pending", "UploadPrep.regions_refused_unwatched", "UploadPrep.regions_refused_snapshot_changed", "UploadPrep.regions_refused_failed"};

enum class StagingRole : std::size_t { Preparer, Committer };

struct StagingCost {
    std::uint64_t allocNanoseconds = 0;
    std::uint64_t poolWaitNanoseconds = 0;
    std::uint64_t stashHits = 0;
};

std::shared_ptr<Buffer> stagingBuffer(const Context& context, std::size_t bytes, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, bool timed, StagingCost& cost) {
    const auto start = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto waitedBefore = BufferPool::ThreadLockWaitNanoseconds();
    auto buffer = takeStashed(context.device, bytes, usage, properties);
    cost.stashHits = buffer != nullptr ? 1u : 0u;
    if (buffer == nullptr) buffer = std::make_shared<Buffer>(context, bytes, usage, properties);
    if (timed) {
        cost.allocNanoseconds = nanosecondsSince(start);
        cost.poolWaitNanoseconds = BufferPool::ThreadLockWaitNanoseconds() - waitedBefore;
    }
    return buffer;
}

void countStaging(StagingRole role, const StagingCost& cost) {
    const auto index = static_cast<std::size_t>(role);
    auto& total = counters();
    if (cost.allocNanoseconds != 0) counted(total.allocNanoseconds[index], AllocNames[index], cost.allocNanoseconds);
    if (cost.poolWaitNanoseconds != 0) counted(total.poolWaitNanoseconds[index], PoolWaitNames[index], cost.poolWaitNanoseconds);
    if (cost.stashHits != 0) counted(total.stashHits[index], StashHitNames[index], cost.stashHits);
}

struct alignas(64) RingCounters {
    std::atomic<std::uint64_t> slices{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> chunks{0};
    std::atomic<std::uint64_t> recycled{0};
    std::atomic<std::uint64_t> returned{0};
    std::atomic<std::uint64_t> capped{0};
    std::atomic<std::uint64_t> oversize{0};
};

std::atomic<std::uint64_t> ringOutstanding{0};

RingCounters& ringCounters() {
    return LocalShard<RingCounters>();
}

struct RingLane {
    RingLane(VkDevice device, VkMemoryPropertyFlags properties) : device(device), properties(properties) {
        free.reserve(DrawInputRingFreeChunks);
    }
    const VkDevice device;
    const VkMemoryPropertyFlags properties;
    std::mutex mutex;
    std::atomic<bool> closed{false};
    std::shared_ptr<Buffer> current;
    VkDeviceSize used = 0;
    std::vector<std::shared_ptr<Buffer>> free;
};

struct ChunkReturn {
    std::shared_ptr<RingLane> lane;
    std::shared_ptr<Buffer> chunk;

    void operator()(Buffer*) {
        std::shared_ptr<Buffer> dropped;
        {
            std::lock_guard lock(lane->mutex);
            if (lane->closed.load(std::memory_order_relaxed) || lane->free.size() >= DrawInputRingFreeChunks) dropped = std::move(chunk);
            else lane->free.push_back(std::move(chunk));
        }
        ringOutstanding.fetch_sub(1, std::memory_order_relaxed);
        counted(ringCounters().returned, "DrawInputRing.returned");
    }
};

struct RingRegistry {
    std::mutex mutex;
    std::vector<std::weak_ptr<RingLane>> lanes;
};

RingRegistry& ringRegistry() {
    static RingRegistry registry;
    return registry;
}

void closeLane(RingLane& lane) {
    std::shared_ptr<Buffer> current;
    std::vector<std::shared_ptr<Buffer>> free;
    {
        std::lock_guard lock(lane.mutex);
        lane.closed.store(true, std::memory_order_relaxed);
        current = std::move(lane.current);
        free.swap(lane.free);
    }
}

struct ThreadLanes {
    std::vector<std::shared_ptr<RingLane>> lanes;

    ~ThreadLanes() {
        for (const auto& lane : lanes) closeLane(*lane);
    }

    std::shared_ptr<RingLane> Find(VkDevice device, VkMemoryPropertyFlags properties) {
        for (auto it = lanes.begin(); it != lanes.end(); ++it) {
            if ((*it)->device != device || (*it)->properties != properties) continue;
            if (!(*it)->closed.load(std::memory_order_relaxed)) return *it;
            lanes.erase(it);
            break;
        }
        auto lane = std::make_shared<RingLane>(device, properties);
        {
            auto& registry = ringRegistry();
            std::lock_guard lock(registry.mutex);
            std::erase_if(registry.lanes, [](const std::weak_ptr<RingLane>& held) { return held.expired(); });
            registry.lanes.push_back(lane);
        }
        lanes.push_back(lane);
        return lane;
    }
};

thread_local ThreadLanes threadLanes;

std::optional<DrawInputSlice> ringSlice(const Context& context, std::size_t bytes, VkMemoryPropertyFlags properties) {
    if (bytes == 0 || RuntimeSwitches::Set(RuntimeSwitch::NoDrawInputRing)) return std::nullopt;
    auto& total = ringCounters();
    if (bytes > DrawInputRingSliceLimit) {
        counted(total.oversize, "DrawInputRing.oversize");
        return std::nullopt;
    }
    const auto lane = threadLanes.Find(context.device, properties);
    bool recycled = false;
    bool made = false;
    std::unique_lock lock(lane->mutex);
    auto offset = (lane->used + DrawInputRingAlignment - 1) & ~(DrawInputRingAlignment - 1);
    if (lane->current == nullptr || offset + bytes > DrawInputRingChunk) {
        if (ringOutstanding.load(std::memory_order_relaxed) * DrawInputRingChunk >= DrawInputRingPinnedLimit) {
            lock.unlock();
            counted(total.capped, "DrawInputRing.capped");
            return std::nullopt;
        }
        auto retired = std::move(lane->current);
        lock.unlock();
        retired.reset();
        lock.lock();
        if (lane->closed.load(std::memory_order_relaxed)) {
            lock.unlock();
            return std::nullopt;
        }
        std::shared_ptr<Buffer> chunk;
        if (!lane->free.empty()) {
            chunk = std::move(lane->free.back());
            lane->free.pop_back();
            recycled = true;
        }
        lock.unlock();
        if (chunk == nullptr) {
            chunk = std::make_shared<Buffer>(context, DrawInputRingChunk, DrawInputUsage, properties);
            made = true;
        }
        auto* raw = chunk.get();
        ringOutstanding.fetch_add(1, std::memory_order_relaxed);
        std::shared_ptr<Buffer> handle(raw, ChunkReturn{lane, std::move(chunk)});
        lock.lock();
        if (lane->closed.load(std::memory_order_relaxed)) {
            lock.unlock();
            handle.reset();
            if (made) counted(total.chunks, "DrawInputRing.chunks");
            return std::nullopt;
        }
        lane->current = std::move(handle);
        offset = 0;
    }
    lane->used = offset + bytes;
    DrawInputSlice slice{lane->current, offset, bytes, true};
    lock.unlock();
    if (recycled) counted(total.recycled, "DrawInputRing.recycled");
    if (made) counted(total.chunks, "DrawInputRing.chunks");
    counted(total.slices, "DrawInputRing.slices");
    counted(total.bytes, "DrawInputRing.bytes", bytes);
    return slice;
}

}

class PreparedWork {
public:
    explicit PreparedWork(std::shared_ptr<const Context> shared) : shared(std::move(shared)), context(*this->shared) {}
    virtual ~PreparedWork() = default;
    PreparedWork(const PreparedWork&) = delete;
    PreparedWork& operator=(const PreparedWork&) = delete;

    std::shared_ptr<const Context> shared;
    const Context& context;
    std::uint64_t epoch = 0;
    bool followedEpoch = false;
    std::uint64_t collectNanoseconds = 0;
    StagingCost staging;
    std::atomic<JobState> state{JobState::Queued};
    std::vector<PreparedRange> ranges;
    std::uint64_t refusedRanges = 0;
    std::array<std::uint64_t, PreparedRefusalCount> refusedBy{};
    bool settled = false;
    bool cancelled = false;
    bool waited = false;

    virtual void Run() = 0;

    virtual void Fail() {
        ranges.clear();
        ++refusedRanges;
        ++refusedBy[static_cast<std::size_t>(PreparedRefusal::Failed)];
    }

    void Settle() {
        if (settled) return;
        settled = true;
        auto expected = JobState::Queued;
        if (state.compare_exchange_strong(expected, JobState::Cancelled, std::memory_order_acq_rel)) {
            cancelled = true;
            return;
        }
        if (expected == JobState::Running) {
            waited = true;
            state.wait(JobState::Running, std::memory_order_acquire);
        }
    }

    void Await() {
        auto current = state.load(std::memory_order_acquire);
        while (current == JobState::Queued || current == JobState::Running) {
            state.wait(current, std::memory_order_acquire);
            current = state.load(std::memory_order_acquire);
        }
    }

protected:
    std::optional<std::size_t> copyRange(std::uint64_t address, std::size_t bytes, Recorder::SnapshotUse use, VkBufferUsageFlags usage, std::size_t readable) {
        const auto refuse = [&](PreparedRefusal reason) -> std::optional<std::size_t> {
            ++refusedRanges;
            ++refusedBy[static_cast<std::size_t>(reason)];
            return std::nullopt;
        };
        if (bytes == 0 || readable == 0 || readable > bytes || bytes > PreparedRangeLimit || bytes > std::numeric_limits<std::uint64_t>::max() - address) return refuse(PreparedRefusal::Size);
        if (!GuestMemory::Accessible(reinterpret_cast<const void*>(address), readable)) return refuse(PreparedRefusal::Inaccessible);
        PreparedRange range;
        range.address = address;
        range.bytes = bytes;
        range.readable = readable;
        range.use = use;
        range.usage = usage;
        range.forget = GuestMemory::ForgetSerial();
        if ((range.forget & 1u) != 0) return refuse(PreparedRefusal::Forget);
        range.registry = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
        const auto pending = Recorder::PendingWriteSnapshot();
        if (Recorder::SnapshotOverlaps(pending.get(), address, readable)) return refuse(PreparedRefusal::Pending);
        const bool timed = DrawInputSourcesTimed();
        const auto collectStart = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        range.generation = GuestMemory::CollectWrites(address, readable);
        if (timed) collectNanoseconds += nanosecondsSince(collectStart);
        if (range.generation == 0) return refuse(PreparedRefusal::Unwatched);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const auto properties = use == Recorder::SnapshotUse::Storage ? HostBufferProperties : DrawInputMemoryProperties(context, use);
        const auto sliced = use == Recorder::SnapshotUse::Storage ? std::nullopt : ringSlice(context, bytes, properties);
        if (sliced) {
            range.buffer = sliced->buffer;
            range.offset = sliced->offset;
            range.ring = true;
        } else {
            StagingCost cost;
            range.buffer = stagingBuffer(context, bytes, usage, properties, timed, cost);
            staging.allocNanoseconds += cost.allocNanoseconds;
            staging.poolWaitNanoseconds += cost.poolWaitNanoseconds;
            staging.stashHits += cost.stashHits;
        }
        const auto target = range.Bytes();
        GuestMemory::CopyGuestBytes(target.data(), reinterpret_cast<const void*>(address), readable);
        std::fill(target.begin() + static_cast<std::ptrdiff_t>(readable), target.end(), std::byte{0});
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (Recorder::PendingWriteSnapshot() != pending) {
            if (!range.ring) static_cast<void>(returnStashed(context.device, bytes, usage, range.buffer));
            return refuse(PreparedRefusal::SnapshotChanged);
        }
        ranges.push_back(std::move(range));
        return ranges.size() - 1u;
    }

    std::array<std::uint64_t, 3> releaseRanges() {
        std::array<std::uint64_t, 3> counts{};
        if (cancelled) return counts;
        for (auto& range : ranges) {
            ++counts[0];
            if (!range.consumed) ++counts[1];
            if (!range.ring && returnStashed(context.device, range.bytes, range.usage, range.buffer)) ++counts[2];
        }
        return counts;
    }
};

enum class StageAOutcome : std::uint8_t { None, PredictedHit, Refused, Failed, Unpinned };

class PreparedDrawResources final : public PreparedWork {
public:
    using PreparedWork::PreparedWork;

    std::weak_ptr<const void> pin;
    const State* drawState = nullptr;
    Pm4::DrawParameters draw{};
    std::span<const CompiledShader> shaders;
    std::span<const GuestMemorySnapshot> snapshots;
    bool stageAWanted = false;
    bool regionsWanted = false;
    std::shared_ptr<ShaderResources> stageA;
    StageAOutcome outcome = StageAOutcome::None;
    bool stagingPredicted = false;
    std::uint64_t stageANanoseconds = 0;
    bool offered = false;

    void Run() override {
        const auto pinned = pin.lock();
        if (pinned == nullptr) {
            outcome = StageAOutcome::Unpinned;
            return;
        }
        const GuestMemory::DeviceLockForbiddenScope forbidden;
        const bool timed = DrawInputSourcesTimed();
        const auto start = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        try {
            bool predictedHit = false;
            stageA = PrepareDrawResourcesOffDevice(context, *drawState, draw, shaders, snapshots, predictedHit, stagingPredicted);
            if (predictedHit) outcome = StageAOutcome::PredictedHit;
        } catch (const OffDeviceRefusal&) {
            stageA.reset();
            outcome = StageAOutcome::Refused;
        }
        if (timed) stageANanoseconds = nanosecondsSince(start);
        if (!regionsWanted || stageA == nullptr) return;
        for (const auto& [begin, end] : stageA->OffDeviceCopies()) static_cast<void>(copyRange(begin, static_cast<std::size_t>(end - begin), Recorder::SnapshotUse::Storage, RegionCopyUsage(false), static_cast<std::size_t>(end - begin)));
        if (!stageAWanted) stageA.reset();
    }

    void Fail() override {
        stageA.reset();
        outcome = StageAOutcome::Failed;
        PreparedWork::Fail();
    }

    void Finish();
};

class PreparedDrawInputs final : public PreparedWork {
public:
    using PreparedWork::PreparedWork;

    Pm4::DrawParameters draw{};
    std::vector<ShaderRecompiler::VertexAttribute> attributes;
    std::shared_ptr<const VertexInputLayout> layout;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> colorTarget;
    std::optional<std::uint32_t> zeroFetchFrom;
    Recorder::SnapshotUse indexUse = Recorder::SnapshotUse::Index16;
    bool skipRestart = false;
    bool inputsRequested = false;
    std::shared_ptr<PreparedDrawResources> resources;

    void Run() override {
        ranges.reserve(attributes.size() + 1u);
        std::uint32_t maxIndex = 0;
        if (draw.indexed) {
            const auto bytes = static_cast<std::size_t>(draw.indexCount) * draw.indexSize;
            const auto index = countedCopy(draw.indexAddress, bytes, indexUse, DrawInputUsage, bytes);
            if (!index) return;
            auto& range = ranges[*index];
            const auto highest = HighestDrawIndex(range.Bytes(), draw.indexSize, skipRestart);
            range.derived = skipRestart ? (highest ? *highest + 1u : 0u) : highest.value_or(0u);
            if (!highest) return;
            if (zeroFetchFrom) {
                const auto limit = *zeroFetchFrom > draw.firstVertex ? *zeroFetchFrom - draw.firstVertex : 0u;
                if (*highest >= limit && *highest - limit >= ZeroFetchSlack) return;
            }
            if (draw.firstVertex > std::numeric_limits<std::uint32_t>::max() - *highest) return;
            maxIndex = *highest + draw.firstVertex;
        } else {
            if (draw.firstVertex > std::numeric_limits<std::uint32_t>::max() - (draw.indexCount - 1u)) return;
            maxIndex = draw.firstVertex + draw.indexCount - 1u;
        }
        if (attributes.empty() || layout == nullptr) return;
        const auto planned = PlanDrawVertexFetches(colorTarget, draw, attributes, *layout, maxIndex);
        for (const auto& [begin, end] : planned.plan.copies) static_cast<void>(countedCopy(begin, static_cast<std::size_t>(end - begin), Recorder::SnapshotUse::Vertex, DrawInputUsage, static_cast<std::size_t>(end - begin)));
        for (const auto solo : planned.soloFetches) {
            const auto& fetch = planned.fetches[solo];
            const auto bytes = static_cast<std::size_t>(fetch.end - fetch.begin);
            const auto readable = ZeroPaddedReadableBytes(fetch.begin, planned.fetchValid[solo], fetch.stride);
            if (readable == 0 || readable > std::numeric_limits<std::uint32_t>::max()) continue;
            if (const auto index = countedCopy(fetch.begin, bytes, Recorder::SnapshotUse::PaddedVertex, DrawInputUsage, readable)) ranges[*index].derived = static_cast<std::uint32_t>(readable);
        }
    }

    void Finish() {
        if (inputsRequested) {
            Settle();
            const auto [preparedRanges, unusedRanges, returned] = releaseRanges();
            auto& total = counters();
            counted(total.requests, "DrawInputPrep.requests");
            counted(total.prepared, "DrawInputPrep.prepared", preparedRanges);
            counted(total.refused, "DrawInputPrep.refused", cancelled ? 0 : refusedRanges);
            for (std::size_t reason = 0; reason < PreparedRefusalCount && !cancelled; ++reason) counted(total.refusedBy[reason], RefusalNames[reason], refusedBy[reason]);
            counted(total.unused, "DrawInputPrep.unused", unusedRanges);
            counted(total.notReady, "DrawInputPrep.not_ready", cancelled ? 1 : 0);
            counted(total.waited, "DrawInputPrep.waited", waited ? 1 : 0);
            counted(total.epochFollows, "DrawInputPrep.epoch_follows", !cancelled && followedEpoch ? 1 : 0);
            if (!cancelled && collectNanoseconds != 0) counted(total.collectNanoseconds, "DrawInputPrep.collect_ns", collectNanoseconds);
            if (!cancelled) countStaging(StagingRole::Preparer, staging);
            if (returned != 0) counted(total.stashReturned, "DrawInputPrep.stash_returned", returned);
        }
        if (resources != nullptr) resources->Finish();
    }

private:

    std::optional<std::size_t> countedCopy(std::uint64_t address, std::size_t bytes, Recorder::SnapshotUse use, VkBufferUsageFlags usage, std::size_t readable) {
        add(counters().ranges, 1);
        return copyRange(address, bytes, use, usage, readable);
    }
};

void PreparedDrawResources::Finish() {
    Settle();
    const auto [preparedRegions, unusedRegions, returned] = releaseRanges();
    auto& total = uploadCounters();
    countedUpload(total.requests, "UploadPrep.requests");
    if (cancelled) {
        countedUpload(total.notReady, "UploadPrep.not_ready");
        return;
    }
    countedUpload(total.waited, "UploadPrep.waited", waited ? 1 : 0);
    if (stageAWanted) {
        if (stageA != nullptr || offered) countedUpload(total.stageAPrepared, "UploadPrep.stage_a_prepared");
        if (stageA != nullptr && !offered) countedUpload(total.stageAUnused, "UploadPrep.stage_a_unused");
        if (stageA != nullptr || offered) {
            if (stagingPredicted) countedUpload(total.stageAStagingPredicted, "UploadPrep.stage_a_staging_predicted");
            else countedUpload(total.stageAStagingFallback, "UploadPrep.stage_a_staging_fallback");
        }
        countedUpload(total.stageAPredictedHit, "UploadPrep.stage_a_predicted_hit", outcome == StageAOutcome::PredictedHit ? 1 : 0);
        countedUpload(total.stageARefused, "UploadPrep.stage_a_refused", outcome == StageAOutcome::Refused ? 1 : 0);
        countedUpload(total.stageAFailed, "UploadPrep.stage_a_failed", outcome == StageAOutcome::Failed ? 1 : 0);
        countedUpload(total.stageAUnpinned, "UploadPrep.stage_a_unpinned", outcome == StageAOutcome::Unpinned ? 1 : 0);
        if (stageANanoseconds != 0) countedUpload(total.stageANanoseconds, "UploadPrep.stage_a_ns", stageANanoseconds);
    }
    countedUpload(total.regionsPrepared, "UploadPrep.regions_prepared", preparedRegions);
    countedUpload(total.regionsUnused, "UploadPrep.regions_unused", unusedRegions);
    countedUpload(total.regionsRefused, "UploadPrep.regions_refused", refusedRanges);
    for (std::size_t reason = 0; reason < PreparedRefusalCount; ++reason) countedUpload(total.regionsRefusedBy[reason], RegionRefusalNames[reason], refusedBy[reason]);
    if (collectNanoseconds != 0) countedUpload(total.regionCollectNanoseconds, "UploadPrep.region_collect_ns", collectNanoseconds);
    countStaging(StagingRole::Preparer, staging);
    if (returned != 0) counted(counters().stashReturned, "DrawInputPrep.stash_returned", returned);
    stageA.reset();
}

namespace {

class Preparers {
public:
    static Preparers* Get() {
        if (auto* existing = instance.load(std::memory_order_acquire); existing != nullptr) return existing;
        std::lock_guard lock(creation);
        if (auto* existing = instance.load(std::memory_order_acquire); existing != nullptr) return existing;
        const auto count = wantedPreparers();
        if (count == 0) return nullptr;
        auto* made = new Preparers(count);
        instance.store(made, std::memory_order_release);
        return made;
    }

    static void Release() {
        std::lock_guard lock(creation);
        if (auto* existing = instance.load(std::memory_order_acquire); existing != nullptr) {
            {
                std::lock_guard jobsLock(existing->mutex);
            }
            existing->wake.notify_all();
        }
    }

    static void Shutdown() {
        std::lock_guard lock(creation);
        delete instance.exchange(nullptr, std::memory_order_acq_rel);
        openStash(false);
    }

    void Submit(const std::shared_ptr<PreparedWork>& job) {
        {
            std::lock_guard lock(mutex);
            jobs.push_back(job);
        }
        wake.notify_one();
    }

    ~Preparers() {
        for (auto& thread : threads) thread.request_stop();
        wake.notify_all();
        threads.clear();
        for (auto& queued : jobs) {
            const auto job = queued.lock();
            if (job == nullptr) continue;
            auto expected = JobState::Queued;
            if (job->state.compare_exchange_strong(expected, JobState::Cancelled, std::memory_order_acq_rel)) job->state.notify_all();
        }
    }

private:
    explicit Preparers(unsigned count) {
        openStash(true);
        for (unsigned index = 0; index < count; ++index) threads.emplace_back([this](std::stop_token token) { run(token); });
    }

    void run(std::stop_token token) {
        std::uint64_t followed = 0;
        for (;;) {
            std::shared_ptr<PreparedWork> job;
            {
                std::unique_lock lock(mutex);
                if (!wake.wait(lock, token, [&] { return !jobs.empty() && !preparersHeld.load(std::memory_order_acquire); })) return;
                job = jobs.front().lock();
                jobs.pop_front();
            }
            if (job == nullptr) continue;
            auto expected = JobState::Queued;
            if (!job->state.compare_exchange_strong(expected, JobState::Running, std::memory_order_acq_rel)) continue;
            job->state.notify_all();
            try {
                if (job->epoch == 0) {
                    followed = 0;
                    GuestMemory::BumpCollectEpoch();
                } else if (job->epoch != followed) {
                    followed = job->epoch;
                    job->followedEpoch = true;
                    DriverDetail::DrawPipeline::FollowEpoch(job->epoch);
                }
                job->Run();
            } catch (...) {
                job->Fail();
            }
            job->state.store(JobState::Done, std::memory_order_release);
            job->state.notify_all();
        }
    }

    static inline std::atomic<Preparers*> instance{nullptr};
    static inline std::mutex creation;
    std::mutex mutex;
    std::condition_variable_any wake;
    std::deque<std::weak_ptr<PreparedWork>> jobs;
    std::vector<std::jthread> threads;
};

thread_local std::shared_ptr<PreparedDrawInputs> currentPrepared;

bool PreparedInputStillValid(const PreparedRange& range, const Recorder* recorder, std::uint64_t registryGeneration) {
    if (range.registry != registryGeneration || range.forget != GuestMemory::ForgetSerial()) return false;
    if (recorder != nullptr && recorder->PendingWriteOverlaps(range.address, range.readable)) return false;
    return GuestMemory::UnchangedSince(range.address, range.readable, range.generation);
}

}

std::shared_ptr<PreparedDrawInputs> PrepareDrawInputsAhead(const std::shared_ptr<const Context>& shared, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::uint64_t epoch) {
    const Context& context = *shared;
    if (context.drawInputsInPlace || (draw.indirect && !draw.indexed) || (!draw.indirect && draw.instanceCount == 0) || draw.indexCount == 0 || state.quadList || RuntimeSwitches::Set(RuntimeSwitch::NoEarlyDrawInputs)) return nullptr;
    if (shaders.empty() || shaders.front().program == nullptr || !GuestMemory::WriteWatched()) return nullptr;
    const bool listTopology = state.topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST || state.topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST || state.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    if (draw.indexed && ((draw.indexSize != 2 && draw.indexSize != 4) || (state.primitiveRestart && !context.primitiveListRestart && listTopology))) return nullptr;
    if (state.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP && BarycentricPatches(state, shaders)) return nullptr;
    const auto& attributes = shaders.front().program->vertexAttributes;
    if (!draw.indexed && attributes.empty()) return nullptr;
    auto* preparers = Preparers::Get();
    if (preparers == nullptr) return nullptr;
    const bool timed = DrawInputSourcesTimed();
    const auto started = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto job = std::make_shared<PreparedDrawInputs>(shared);
    job->draw = draw;
    job->epoch = epoch;
    job->attributes.assign(attributes.begin(), attributes.end());
    if (!attributes.empty()) {
        if (context.vertexInputs != nullptr) job->layout = context.vertexInputs->TryGet(shaders.front().program->variantId, attributes);
        if (job->layout == nullptr) {
            try {
                job->layout = std::make_shared<const VertexInputLayout>(BuildVertexInputLayout(context, attributes));
            } catch (const std::exception&) {
                return nullptr;
            }
        }
    }
    if (state.hasColorTarget) job->colorTarget = std::pair{state.color.MemoryAddress(), static_cast<std::uint64_t>(state.color.bytes)};
    if (draw.indexed) {
        job->skipRestart = state.primitiveRestart && (!listTopology || context.primitiveListRestart);
        job->indexUse = draw.indexSize == 2 ? (job->skipRestart ? Recorder::SnapshotUse::Index16Restart : Recorder::SnapshotUse::Index16) : (job->skipRestart ? Recorder::SnapshotUse::Index32Restart : Recorder::SnapshotUse::Index32);
        if (!state.stages.mesh && !attributes.empty()) job->zeroFetchFrom = ZeroFetchVertex(attributes);
    }
    job->inputsRequested = true;
    preparers->Submit(job);
    if (timed) {
        const auto spent = nanosecondsSince(started);
        counted(counters().requestNanoseconds, "DrawInputPrep.request_ns", spent);
    }
    return job;
}

void AwaitPreparedDrawInputs(PreparedDrawInputs& prepared) {
    prepared.Await();
    if (prepared.resources != nullptr) prepared.resources->Await();
}

const Context& PreparedDrawInputsContext(const PreparedDrawInputs& prepared) {
    return prepared.context;
}

const Context* PreparedDrawResourcesContext(const PreparedDrawInputs& prepared) {
    return prepared.resources != nullptr ? &prepared.resources->context : nullptr;
}

std::shared_ptr<PreparedDrawInputs> PrepareDrawResourcesAhead(std::shared_ptr<PreparedDrawInputs> job, const std::shared_ptr<const Context>& shared, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots, std::uint64_t epoch) {
    const Context& context = *shared;
    const bool stageAWanted = !RuntimeSwitches::Set(RuntimeSwitch::NoEarlyUploadPrepare);
    const bool regionsWanted = !RuntimeSwitches::Set(RuntimeSwitch::NoEarlyRegionCopies) && GuestMemory::WriteWatched();
    if ((!stageAWanted && !regionsWanted) || shaders.empty() || std::ranges::any_of(shaders, [](const CompiledShader& shader) { return shader.program == nullptr; })) return job;
    if (job != nullptr && job->context.device != context.device) return job;
    if (Preparers::Get() == nullptr) return job;
    if (job == nullptr) {
        job = std::make_shared<PreparedDrawInputs>(shared);
        job->epoch = epoch;
        job->state.store(JobState::Done, std::memory_order_release);
    }
    auto resources = std::make_shared<PreparedDrawResources>(shared);
    resources->epoch = epoch;
    resources->drawState = &state;
    resources->draw = draw;
    resources->shaders = shaders;
    resources->snapshots = snapshots;
    resources->stageAWanted = stageAWanted;
    resources->regionsWanted = regionsWanted;
    job->resources = std::move(resources);
    return job;
}

void SubmitPreparedDrawResources(const std::shared_ptr<PreparedDrawInputs>& job, std::shared_ptr<const void> pin) {
    if (job == nullptr || job->resources == nullptr || job->resources->state.load(std::memory_order_acquire) != JobState::Queued) return;
    auto* preparers = Preparers::Get();
    if (preparers == nullptr) return;
    job->resources->pin = pin;
    preparers->Submit(job->resources);
}

std::shared_ptr<ShaderResources> AdoptPreparedDrawResources(const Context& context, bool deviceStaging) {
    auto* job = currentPrepared.get();
    if (job == nullptr || job->resources == nullptr || job->context.device != context.device) return nullptr;
    auto& resources = *job->resources;
    resources.Settle();
    if (resources.cancelled || !resources.stageAWanted || resources.offered) return nullptr;
    resources.offered = true;
    if (resources.stageA == nullptr) return nullptr;
    auto& total = uploadCounters();
    if (resources.stageA->PreparedStaging() != deviceStaging) {
        resources.stageA.reset();
        countedUpload(total.stageAStaleStaging, "UploadPrep.stage_a_stale_staging");
        if (resources.stagingPredicted) countedUpload(total.stageAStagingMispredicted, "UploadPrep.stage_a_staging_mispredicted");
        return nullptr;
    }
    if (const auto staleness = resources.stageA->PreparedDrawStaleness(deviceStaging); staleness != PreparedStaleness::Current) {
        resources.stageA.reset();
        countedUpload(total.stageAStale, "UploadPrep.stage_a_stale");
        const auto reason = static_cast<std::size_t>(staleness);
        countedUpload(total.stageAStaleBy[reason], StaleNames[reason]);
        return nullptr;
    }
    countedUpload(total.stageAAdopted, "UploadPrep.stage_a_adopted");
    return std::move(resources.stageA);
}

void CountPreparedDrawResourcesVerified(bool differed) {
    if (differed) countedUpload(uploadCounters().stageAVerifyDiffered, "UploadPrep.verify_stage_a_differed");
}

std::shared_ptr<Buffer> AdoptPreparedRegion(const Context& context, const Recorder* recorder, std::uint64_t address, std::size_t bytes, VkBufferUsageFlags usage, std::shared_ptr<Buffer>* spare) {
    auto* job = currentPrepared.get();
    if (job == nullptr || job->resources == nullptr || job->context.device != context.device) return nullptr;
    auto& resources = *job->resources;
    resources.Settle();
    if (resources.cancelled) return nullptr;
    const auto found = std::find_if(resources.ranges.begin(), resources.ranges.end(), [&](const PreparedRange& range) { return !range.consumed && range.address == address && range.bytes == bytes && range.usage == usage; });
    if (found == resources.ranges.end()) return nullptr;
    found->consumed = true;
    const bool timed = DrawInputSourcesTimed();
    const auto start = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    GuestMemory::FlushGpuWrites(address, bytes);
    const auto registry = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    const bool valid = GuestMemory::CollectWrites(address, bytes) != 0 && PreparedInputStillValid(*found, recorder, registry);
    auto& total = uploadCounters();
    if (timed) countedUpload(total.regionCheckNanoseconds, "UploadPrep.region_check_ns", nanosecondsSince(start));
    if (!valid) {
        if (spare != nullptr && recycling()) {
            *spare = std::move(found->buffer);
            countedUpload(total.regionSpareReused, "UploadPrep.region_spare_reused");
        }
        found->buffer.reset();
        countedUpload(total.regionsReprepared, "UploadPrep.regions_reprepared");
        return nullptr;
    }
    countedUpload(total.regionsAdopted, "UploadPrep.regions_adopted");
    countedUpload(total.regionBytesAdopted, "UploadPrep.region_bytes_adopted", bytes);
    static const bool verify = std::getenv("APS5_VERIFY_EARLY_UPLOADS") != nullptr;
    if (verify && std::memcmp(found->buffer->Bytes().data(), reinterpret_cast<const void*>(address), bytes) != 0) countedUpload(total.regionVerifyDiffered, "UploadPrep.verify_regions_differed");
    return std::move(found->buffer);
}

PreparedUploadCounts PreparedUploadCounters() {
    PreparedUploadCounts counts;
    CounterShards<UploadCounters>::Get().ForEach([&](const UploadCounters& total) {
        sum(counts.requests, total.requests);
        sum(counts.notReady, total.notReady);
        sum(counts.waited, total.waited);
        sum(counts.stageAPrepared, total.stageAPrepared);
        sum(counts.stageAAdopted, total.stageAAdopted);
        sum(counts.stageAStale, total.stageAStale);
        sum(counts.stageAStaleStaging, total.stageAStaleStaging);
        for (std::size_t reason = 0; reason < PreparedStalenessCount; ++reason) sum(counts.stageAStaleBy[reason], total.stageAStaleBy[reason]);
        sum(counts.stageAStagingPredicted, total.stageAStagingPredicted);
        sum(counts.stageAStagingFallback, total.stageAStagingFallback);
        sum(counts.stageAStagingMispredicted, total.stageAStagingMispredicted);
        sum(counts.stageAUnused, total.stageAUnused);
        sum(counts.stageAPredictedHit, total.stageAPredictedHit);
        sum(counts.stageARefused, total.stageARefused);
        sum(counts.stageAFailed, total.stageAFailed);
        sum(counts.stageAUnpinned, total.stageAUnpinned);
        sum(counts.stageAVerifyDiffered, total.stageAVerifyDiffered);
        sum(counts.regionsPrepared, total.regionsPrepared);
        sum(counts.regionsAdopted, total.regionsAdopted);
        sum(counts.regionsReprepared, total.regionsReprepared);
        sum(counts.regionsUnused, total.regionsUnused);
        sum(counts.regionsRefused, total.regionsRefused);
        for (std::size_t reason = 0; reason < PreparedRefusalCount; ++reason) sum(counts.regionsRefusedBy[reason], total.regionsRefusedBy[reason]);
        sum(counts.regionSpareReused, total.regionSpareReused);
        sum(counts.regionVerifyDiffered, total.regionVerifyDiffered);
    });
    return counts;
}

PreparedDrawInputsScope::PreparedDrawInputsScope(std::shared_ptr<PreparedDrawInputs> prepared) : prepared(std::move(prepared)), previous(std::exchange(currentPrepared, this->prepared)) {
    if (const auto hook = preparedHook.load(std::memory_order_acquire); hook != nullptr && this->prepared != nullptr) hook(*this->prepared);
}

PreparedDrawInputsScope::~PreparedDrawInputsScope() {
    currentPrepared = std::move(previous);
    if (prepared != nullptr) prepared->Finish();
}

std::size_t ZeroPaddedReadableBytes(std::uint64_t address, std::size_t validBytes, std::size_t stride) {
    if (stride == 0 || GuestMemory::Accessible(reinterpret_cast<const void*>(address), validBytes)) return validBytes;
    const auto ranges = GuestMemory::CommittedRanges(address, validBytes);
    const std::size_t prefix = !ranges.empty() && ranges.front().first == address ? static_cast<std::size_t>(ranges.front().second - address) : 0;
    return validBytes - prefix < stride ? prefix : validBytes;
}

std::optional<AdoptedDrawInput> AdoptPreparedDrawInput(const Context& context, const Recorder* recorder, std::uint64_t address, std::size_t bytes, Recorder::SnapshotUse use, std::uint64_t registryGeneration, std::size_t readable, DrawInputSlice* spare) {
    auto* job = currentPrepared.get();
    if (job == nullptr || job->context.device != context.device) return std::nullopt;
    job->Settle();
    if (job->cancelled) return std::nullopt;
    const auto found = std::find_if(job->ranges.begin(), job->ranges.end(), [&](const PreparedRange& range) { return !range.consumed && range.address == address && range.bytes == bytes && range.readable == readable && range.use == use; });
    if (found == job->ranges.end()) return std::nullopt;
    found->consumed = true;
    const bool timed = DrawInputSourcesTimed();
    const auto start = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const bool valid = PreparedInputStillValid(*found, recorder, registryGeneration);
    auto& total = counters();
    add(total.checks, 1);
    if (timed) {
        const auto spent = nanosecondsSince(start);
        counted(total.checkNanoseconds, "DrawInputPrep.check_ns", spent);
    }
    if (!valid) {
        if (spare != nullptr && recycling()) {
            *spare = DrawInputSlice{std::move(found->buffer), found->offset, found->bytes, found->ring};
            counted(total.spareReused, "DrawInputPrep.spare_reused");
        }
        found->buffer.reset();
        counted(total.reprepared, "DrawInputPrep.reprepared");
        return std::nullopt;
    }
    counted(total.adopted, "DrawInputPrep.adopted");
    static const bool verify = std::getenv("APS5_VERIFY_EARLY_DRAW_INPUTS") != nullptr;
    if (verify && std::memcmp(found->Bytes().data(), reinterpret_cast<const void*>(address), bytes) != 0) {
        counted(total.verifyDiffered, "DrawInputPrep.verify_differed");
    }
    return AdoptedDrawInput{DrawInputSlice{std::move(found->buffer), found->offset, found->bytes, found->ring}, found->derived};
}

VkMemoryPropertyFlags DrawInputMemoryProperties(const Context& context, Recorder::SnapshotUse use) {
    return use == Recorder::SnapshotUse::Vertex || use == Recorder::SnapshotUse::PaddedVertex ? context.SnapshotProperties() : HostBufferProperties;
}

std::shared_ptr<Buffer> NewDrawInputBuffer(const Context& context, std::size_t bytes, Recorder::SnapshotUse use) {
    StagingCost cost;
    auto buffer = stagingBuffer(context, bytes, DrawInputUsage, DrawInputMemoryProperties(context, use), DrawInputSourcesTimed(), cost);
    countStaging(StagingRole::Committer, cost);
    return buffer;
}

DrawInputSlice NewDrawInputSlice(const Context& context, std::size_t bytes, Recorder::SnapshotUse use) {
    if (auto slice = ringSlice(context, bytes, DrawInputMemoryProperties(context, use))) return std::move(*slice);
    return DrawInputSlice{NewDrawInputBuffer(context, bytes, use), 0, bytes, false};
}

std::span<std::byte> DrawInputSlice::Bytes() const {
    return buffer->Bytes().subspan(static_cast<std::size_t>(offset), bytes);
}

DrawInputRingCounts DrawInputRingCounters() {
    DrawInputRingCounts counts;
    CounterShards<RingCounters>::Get().ForEach([&](const RingCounters& total) {
        sum(counts.slices, total.slices);
        sum(counts.bytes, total.bytes);
        sum(counts.chunks, total.chunks);
        sum(counts.recycled, total.recycled);
        sum(counts.returned, total.returned);
        sum(counts.capped, total.capped);
        sum(counts.oversize, total.oversize);
    });
    counts.outstanding = ringOutstanding.load(std::memory_order_relaxed);
    return counts;
}

bool DrawInputSourcesTimed() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile || PerformanceTrace::Recording();
}

void CountDrawInputSource(DrawInputSource source, std::size_t bytes, std::uint64_t nanoseconds) {
    const auto index = static_cast<std::size_t>(source);
    auto& total = counters();
    counted(total.sources[index], SourceNames[index]);
    counted(total.sourceBytes[index], SourceByteNames[index], bytes);
    counted(total.sourceNanoseconds[index], SourceTimeNames[index], nanoseconds);
}

PreparedDrawInputCounts PreparedDrawInputCounters() {
    PreparedDrawInputCounts counts;
    CounterShards<Counters>::Get().ForEach([&](const Counters& total) {
        sum(counts.requests, total.requests);
        sum(counts.ranges, total.ranges);
        sum(counts.prepared, total.prepared);
        sum(counts.refused, total.refused);
        for (std::size_t reason = 0; reason < PreparedRefusalCount; ++reason) sum(counts.refusedBy[reason], total.refusedBy[reason]);
        sum(counts.adopted, total.adopted);
        sum(counts.reprepared, total.reprepared);
        sum(counts.notReady, total.notReady);
        sum(counts.waited, total.waited);
        sum(counts.unused, total.unused);
        sum(counts.checks, total.checks);
        sum(counts.checkNanoseconds, total.checkNanoseconds);
        sum(counts.verifyDiffered, total.verifyDiffered);
        sum(counts.requestNanoseconds, total.requestNanoseconds);
        sum(counts.epochFollows, total.epochFollows);
        sum(counts.collectNanoseconds, total.collectNanoseconds);
        for (std::size_t role = 0; role < 2; ++role) {
            sum(counts.allocNanoseconds[role], total.allocNanoseconds[role]);
            sum(counts.poolWaitNanoseconds[role], total.poolWaitNanoseconds[role]);
            sum(counts.stashHits[role], total.stashHits[role]);
        }
        sum(counts.stashReturned, total.stashReturned);
        sum(counts.spareReused, total.spareReused);
        for (std::size_t index = 0; index < DrawInputSourceCount; ++index) {
            sum(counts.sources[index], total.sources[index]);
            sum(counts.sourceBytes[index], total.sourceBytes[index]);
            sum(counts.sourceNanoseconds[index], total.sourceNanoseconds[index]);
        }
    });
    return counts;
}

void SetDrawInputPreparers(unsigned count) {
    Preparers::Shutdown();
    configuredPreparers.store(static_cast<int>(count), std::memory_order_release);
}

void SetPreparedDrawInputsHook(void (*hook)(PreparedDrawInputs& prepared)) {
    preparedHook.store(hook, std::memory_order_release);
}

void HoldDrawInputPreparers(bool held) {
    preparersHeld.store(held, std::memory_order_release);
    if (!held) Preparers::Release();
}

void ShutdownDrawInputPreparers() {
    Preparers::Shutdown();
}

void ClearDrawInputStash(VkDevice device) {
    std::vector<StashedBuffer> dropped;
    {
        auto& stash = bufferStash();
        std::lock_guard lock(stash.mutex);
        for (auto it = stash.buffers.begin(); it != stash.buffers.end();) {
            if (it->device != device) {
                ++it;
                continue;
            }
            stash.bytes -= it->bytes;
            dropped.push_back(std::move(*it));
            it = stash.buffers.erase(it);
        }
    }
    std::vector<std::shared_ptr<RingLane>> lanes;
    {
        auto& registry = ringRegistry();
        std::lock_guard lock(registry.mutex);
        for (const auto& held : registry.lanes) {
            if (auto lane = held.lock(); lane != nullptr && lane->device == device) lanes.push_back(std::move(lane));
        }
    }
    for (const auto& lane : lanes) closeLane(*lane);
}

}
