#include "prx/libSceAgcDriver/Graphics/include/PreparedDrawInputs.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTrace.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
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
    bool consumed = false;
};

struct Counters {
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
    std::array<std::atomic<std::uint64_t>, DrawInputSourceCount> sources{};
    std::array<std::atomic<std::uint64_t>, DrawInputSourceCount> sourceBytes{};
    std::array<std::atomic<std::uint64_t>, DrawInputSourceCount> sourceNanoseconds{};
};

Counters& counters() {
    static Counters values;
    return values;
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

}

class PreparedDrawInputs {
public:
    Context context;
    Pm4::DrawParameters draw{};
    std::vector<ShaderRecompiler::VertexAttribute> attributes;
    std::shared_ptr<const VertexInputLayout> layout;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> colorTarget;
    std::optional<std::uint32_t> zeroFetchFrom;
    Recorder::SnapshotUse indexUse = Recorder::SnapshotUse::Index16;
    bool skipRestart = false;
    std::atomic<JobState> state{JobState::Queued};
    std::vector<PreparedRange> ranges;
    std::uint64_t refusedRanges = 0;
    std::array<std::uint64_t, PreparedRefusalCount> refusedBy{};
    bool resolved = false;
    bool cancelled = false;
    bool waited = false;

    explicit PreparedDrawInputs(const Context& context) : context(context) {}

    void Prepare() {
        ranges.reserve(attributes.size() + 1u);
        std::uint32_t maxIndex = 0;
        if (draw.indexed) {
            const auto bytes = static_cast<std::size_t>(draw.indexCount) * draw.indexSize;
            const auto index = copyRange(draw.indexAddress, bytes, indexUse, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, bytes);
            if (!index) return;
            auto& range = ranges[*index];
            const auto highest = HighestDrawIndex(range.buffer->Bytes().first(bytes), draw.indexSize, skipRestart);
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
        for (const auto& [begin, end] : planned.plan.copies) static_cast<void>(copyRange(begin, static_cast<std::size_t>(end - begin), Recorder::SnapshotUse::Vertex, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, static_cast<std::size_t>(end - begin)));
        for (const auto solo : planned.soloFetches) {
            const auto& fetch = planned.fetches[solo];
            const auto bytes = static_cast<std::size_t>(fetch.end - fetch.begin);
            const auto readable = ZeroPaddedReadableBytes(fetch.begin, planned.fetchValid[solo], fetch.stride);
            if (readable == 0 || readable > std::numeric_limits<std::uint32_t>::max()) continue;
            if (const auto index = copyRange(fetch.begin, bytes, Recorder::SnapshotUse::PaddedVertex, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, readable)) ranges[*index].derived = static_cast<std::uint32_t>(readable);
        }
    }

    void Resolve() {
        if (resolved) return;
        resolved = true;
        auto expected = JobState::Queued;
        if (state.compare_exchange_strong(expected, JobState::Cancelled, std::memory_order_acq_rel)) {
            cancelled = true;
            return;
        }
        if (expected == JobState::Running) {
            waited = true;
            state.wait(JobState::Running, std::memory_order_acquire);
        }
        static_cast<void>(state.load(std::memory_order_acquire));
    }

    void Await() {
        auto current = state.load(std::memory_order_acquire);
        while (current == JobState::Queued || current == JobState::Running) {
            state.wait(current, std::memory_order_acquire);
            current = state.load(std::memory_order_acquire);
        }
    }

    void Finish() {
        auto expected = JobState::Queued;
        if (state.compare_exchange_strong(expected, JobState::Cancelled, std::memory_order_acq_rel)) cancelled = true;
        else if (expected == JobState::Running) {
            waited = true;
            state.wait(JobState::Running, std::memory_order_acquire);
        }
        std::uint64_t unusedRanges = 0;
        std::uint64_t preparedRanges = 0;
        if (!cancelled) {
            for (const auto& range : ranges) {
                ++preparedRanges;
                if (!range.consumed) ++unusedRanges;
            }
        }
        auto& total = counters();
        total.requests.fetch_add(1, std::memory_order_relaxed);
        total.prepared.fetch_add(preparedRanges, std::memory_order_relaxed);
        total.refused.fetch_add(cancelled ? 0 : refusedRanges, std::memory_order_relaxed);
        total.unused.fetch_add(unusedRanges, std::memory_order_relaxed);
        if (cancelled) total.notReady.fetch_add(1, std::memory_order_relaxed);
        if (waited) total.waited.fetch_add(1, std::memory_order_relaxed);
        PerformanceTrace::CountOwnedDraw("DrawInputPrep.requests");
        PerformanceTrace::CountOwnedDraw("DrawInputPrep.prepared", preparedRanges);
        PerformanceTrace::CountOwnedDraw("DrawInputPrep.refused", cancelled ? 0 : refusedRanges);
        for (std::size_t reason = 0; reason < PreparedRefusalCount && !cancelled; ++reason) {
            total.refusedBy[reason].fetch_add(refusedBy[reason], std::memory_order_relaxed);
            PerformanceTrace::CountOwnedDraw(RefusalNames[reason], refusedBy[reason]);
        }
        PerformanceTrace::CountOwnedDraw("DrawInputPrep.unused", unusedRanges);
        PerformanceTrace::CountOwnedDraw("DrawInputPrep.not_ready", cancelled ? 1 : 0);
        PerformanceTrace::CountOwnedDraw("DrawInputPrep.waited", waited ? 1 : 0);
    }

private:
    std::optional<std::size_t> copyRange(std::uint64_t address, std::size_t bytes, Recorder::SnapshotUse use, VkBufferUsageFlags usage, std::size_t readable) {
        counters().ranges.fetch_add(1, std::memory_order_relaxed);
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
        range.forget = GuestMemory::ForgetSerial();
        if ((range.forget & 1u) != 0) return refuse(PreparedRefusal::Forget);
        range.registry = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
        const auto pending = Recorder::PendingWriteSnapshot();
        if (Recorder::SnapshotOverlaps(pending.get(), address, readable)) return refuse(PreparedRefusal::Pending);
        range.generation = GuestMemory::CollectWrites(address, readable);
        if (range.generation == 0) return refuse(PreparedRefusal::Unwatched);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        range.buffer = std::make_shared<Buffer>(context, bytes, usage);
        const auto target = range.buffer->Bytes();
        GuestMemory::CopyGuestBytes(target.data(), reinterpret_cast<const void*>(address), readable);
        std::fill(target.begin() + static_cast<std::ptrdiff_t>(readable), target.begin() + static_cast<std::ptrdiff_t>(bytes), std::byte{0});
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (Recorder::PendingWriteSnapshot() != pending) return refuse(PreparedRefusal::SnapshotChanged);
        ranges.push_back(std::move(range));
        return ranges.size() - 1u;
    }
};

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
    }

    void Submit(const std::shared_ptr<PreparedDrawInputs>& job) {
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
        for (unsigned index = 0; index < count; ++index) threads.emplace_back([this](std::stop_token token) { run(token); });
    }

    void run(std::stop_token token) {
        for (;;) {
            std::shared_ptr<PreparedDrawInputs> job;
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
                job->Prepare();
            } catch (...) {
                job->ranges.clear();
                ++job->refusedRanges;
                ++job->refusedBy[static_cast<std::size_t>(PreparedRefusal::Failed)];
            }
            job->state.store(JobState::Done, std::memory_order_release);
            job->state.notify_all();
        }
    }

    static inline std::atomic<Preparers*> instance{nullptr};
    static inline std::mutex creation;
    std::mutex mutex;
    std::condition_variable_any wake;
    std::deque<std::weak_ptr<PreparedDrawInputs>> jobs;
    std::vector<std::jthread> threads;
};

thread_local std::shared_ptr<PreparedDrawInputs> currentPrepared;

bool PreparedInputStillValid(const PreparedRange& range, const Recorder* recorder, std::uint64_t registryGeneration) {
    if (range.registry != registryGeneration || range.forget != GuestMemory::ForgetSerial()) return false;
    if (recorder != nullptr && recorder->PendingWriteOverlaps(range.address, range.readable)) return false;
    return GuestMemory::UnchangedSince(range.address, range.readable, range.generation);
}

}

std::shared_ptr<PreparedDrawInputs> PrepareDrawInputsAhead(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders) {
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
    auto job = std::make_shared<PreparedDrawInputs>(context);
    job->draw = draw;
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
    preparers->Submit(job);
    if (timed) {
        const auto spent = nanosecondsSince(started);
        counters().requestNanoseconds.fetch_add(spent, std::memory_order_relaxed);
        PerformanceTrace::CountOwnedDraw("DrawInputPrep.request_ns", spent);
    }
    return job;
}

void AwaitPreparedDrawInputs(PreparedDrawInputs& prepared) {
    prepared.Await();
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

std::optional<AdoptedDrawInput> AdoptPreparedDrawInput(const Context& context, const Recorder* recorder, std::uint64_t address, std::size_t bytes, Recorder::SnapshotUse use, std::uint64_t registryGeneration, std::size_t readable) {
    auto* job = currentPrepared.get();
    if (job == nullptr || job->context.device != context.device) return std::nullopt;
    job->Resolve();
    if (job->cancelled) return std::nullopt;
    const auto found = std::find_if(job->ranges.begin(), job->ranges.end(), [&](const PreparedRange& range) { return !range.consumed && range.address == address && range.bytes == bytes && range.readable == readable && range.use == use; });
    if (found == job->ranges.end()) return std::nullopt;
    found->consumed = true;
    const bool timed = DrawInputSourcesTimed();
    const auto start = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const bool valid = PreparedInputStillValid(*found, recorder, registryGeneration);
    auto& total = counters();
    total.checks.fetch_add(1, std::memory_order_relaxed);
    if (timed) {
        const auto spent = nanosecondsSince(start);
        total.checkNanoseconds.fetch_add(spent, std::memory_order_relaxed);
        PerformanceTrace::CountOwnedDraw("DrawInputPrep.check_ns", spent);
    }
    if (!valid) {
        found->buffer.reset();
        total.reprepared.fetch_add(1, std::memory_order_relaxed);
        PerformanceTrace::CountOwnedDraw("DrawInputPrep.reprepared");
        return std::nullopt;
    }
    total.adopted.fetch_add(1, std::memory_order_relaxed);
    PerformanceTrace::CountOwnedDraw("DrawInputPrep.adopted");
    static const bool verify = std::getenv("APS5_VERIFY_EARLY_DRAW_INPUTS") != nullptr;
    if (verify && std::memcmp(found->buffer->Bytes().data(), reinterpret_cast<const void*>(address), bytes) != 0) {
        total.verifyDiffered.fetch_add(1, std::memory_order_relaxed);
        PerformanceTrace::CountOwnedDraw("DrawInputPrep.verify_differed");
    }
    return AdoptedDrawInput{std::move(found->buffer), found->derived};
}

bool DrawInputSourcesTimed() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile || PerformanceTrace::Recording();
}

void CountDrawInputSource(DrawInputSource source, std::size_t bytes, std::uint64_t nanoseconds) {
    const auto index = static_cast<std::size_t>(source);
    auto& total = counters();
    total.sources[index].fetch_add(1, std::memory_order_relaxed);
    total.sourceBytes[index].fetch_add(bytes, std::memory_order_relaxed);
    total.sourceNanoseconds[index].fetch_add(nanoseconds, std::memory_order_relaxed);
    PerformanceTrace::CountOwnedDraw(SourceNames[index]);
    PerformanceTrace::CountOwnedDraw(SourceByteNames[index], bytes);
    PerformanceTrace::CountOwnedDraw(SourceTimeNames[index], nanoseconds);
}

PreparedDrawInputCounts PreparedDrawInputCounters() {
    auto& total = counters();
    PreparedDrawInputCounts counts;
    counts.requests = total.requests.load(std::memory_order_relaxed);
    counts.ranges = total.ranges.load(std::memory_order_relaxed);
    counts.prepared = total.prepared.load(std::memory_order_relaxed);
    counts.refused = total.refused.load(std::memory_order_relaxed);
    for (std::size_t reason = 0; reason < PreparedRefusalCount; ++reason) counts.refusedBy[reason] = total.refusedBy[reason].load(std::memory_order_relaxed);
    counts.adopted = total.adopted.load(std::memory_order_relaxed);
    counts.reprepared = total.reprepared.load(std::memory_order_relaxed);
    counts.notReady = total.notReady.load(std::memory_order_relaxed);
    counts.waited = total.waited.load(std::memory_order_relaxed);
    counts.unused = total.unused.load(std::memory_order_relaxed);
    counts.checks = total.checks.load(std::memory_order_relaxed);
    counts.checkNanoseconds = total.checkNanoseconds.load(std::memory_order_relaxed);
    counts.verifyDiffered = total.verifyDiffered.load(std::memory_order_relaxed);
    counts.requestNanoseconds = total.requestNanoseconds.load(std::memory_order_relaxed);
    for (std::size_t index = 0; index < DrawInputSourceCount; ++index) {
        counts.sources[index] = total.sources[index].load(std::memory_order_relaxed);
        counts.sourceBytes[index] = total.sourceBytes[index].load(std::memory_order_relaxed);
        counts.sourceNanoseconds[index] = total.sourceNanoseconds[index].load(std::memory_order_relaxed);
    }
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

}
