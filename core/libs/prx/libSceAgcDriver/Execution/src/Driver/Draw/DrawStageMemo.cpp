#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawStageMemo.hpp"
#include "prx/libSceAgcDriver/Execution/include/AggregateFields.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/KeyHash.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "Optimization/SrtWalker.hpp"
#include "ThreadOwned.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace AgcDriver::DriverDetail {

namespace {

class KeyWriter {
public:
    explicit KeyWriter(std::vector<std::uint64_t>& words) : words(words) {
        words.resize(std::max(words.capacity(), ReservedWords));
        data = words.data();
        limit = words.size();
    }

    void Finish() { words.resize(used); }

    [[gnu::always_inline]] void Word(std::uint64_t word) {
        if (used == limit) [[unlikely]] Grow();
        data[used++] = word;
    }

    template<typename TValue>
    void operator()(TValue value) requires (std::is_integral_v<TValue> || std::is_enum_v<TValue>) {
        Word(static_cast<std::uint64_t>(value));
    }

    void operator()(float value) { Word(std::bit_cast<std::uint32_t>(value)); }

    template<typename TValue, std::size_t TSize>
    void operator()(const std::array<TValue, TSize>& values) {
        for (const auto& value : values) (*this)(value);
    }

    template<typename TParent, typename TField>
    bool Member(const TParent&, const TField&) { return false; }

    bool Member(const ShaderRecompiler::ShaderPixelStageInfo& pixel, const decltype(ShaderRecompiler::ShaderPixelStageInfo::interpolatorSettings)& settings) {
        if (&settings != &pixel.interpolatorSettings) return false;
        for (std::uint32_t i = 0; i < pixel.interpolatorCount; ++i) (*this)(settings[i]);
        return true;
    }

    bool Member(const ShaderRecompiler::ShaderVertexStageInfo& vertex, const decltype(ShaderRecompiler::ShaderVertexStageInfo::resources)& resources) {
        if (&resources != &vertex.resources) return false;
        for (std::uint32_t r = 0; r < vertex.resourcesNum; ++r) AggregateFields::HashLeaves(resources[r], *this);
        return true;
    }

    bool Member(const ShaderRecompiler::ShaderVertexStageInfo& vertex, const decltype(ShaderRecompiler::ShaderVertexStageInfo::resourcesDst)& destinations) {
        if (&destinations != &vertex.resourcesDst) return false;
        for (std::uint32_t r = 0; r < vertex.resourcesNum; ++r) AggregateFields::HashLeaves(destinations[r], *this);
        return true;
    }

    void operator()(const ShaderRecompiler::ShaderPixelStageInfo& pixel) {
        Graphics::Require(pixel.interpolatorCount <= pixel.interpolatorSettings.size(), "draw stage memo: pixel stage info interpolator count exceeds its table");
        AggregateFields::HashLeaves(pixel, *this);
    }

    void operator()(const ShaderRecompiler::ShaderVertexStageInfo& vertex) {
        Graphics::Require(vertex.resourcesNum <= vertex.resources.size(), "draw stage memo: vertex stage info resource count exceeds its table");
        AggregateFields::HashLeaves(vertex, *this);
    }

    void operator()(const ShaderRecompiler::ShaderComputeStageInfo& compute) {
        (*this)(compute.numThreads);
        (*this)(compute.ldsSizeDwords);
        (*this)(compute.groupIdEnable);
        (*this)(compute.tgSizeEnable);
        (*this)(compute.threadIdComponentCount);
        (*this)(compute.partialThreads);
        (*this)(compute.scratchDwords);
        (*this)(compute.granulatedVgprCount);
        (*this)(compute.sharedVgprBlocks);
    }

    void operator()(const ShaderRecompiler::ShaderFloatMode& mode) {
        (*this)(mode.floatMode);
        for (const auto value : {mode.dx10Clamp, mode.ieeeMode, mode.fp16Overflow}) (*this)(value);
    }

    void operator()(const ShaderRecompiler::MeshConfiguration& mesh) {
        for (const auto value : {mesh.inputPrimitive, mesh.primitivesPerGroup, mesh.verticesPerGroup, mesh.maxVertices, mesh.maxPrimitives, mesh.threadsPerGroup, mesh.ldsSizeDwords, mesh.provokingVertex, mesh.esgsItemSize, mesh.outputPrimitiveSize}) (*this)(value);
    }

    void operator()(const ShaderRecompiler::TessellationConfiguration& tessellation) {
        for (const auto value : {tessellation.inputControlPoints, tessellation.outputControlPoints, tessellation.domain, tessellation.partitioning, tessellation.outputTopology, tessellation.patchesPerGroup, tessellation.primitiveGroupSize}) (*this)(value);
        (*this)(tessellation.breakAtInstance);
        (*this)(tessellation.mergedHull);
    }

    template<typename TValue>
    void operator()(const std::optional<TValue>& value) {
        (*this)(value.has_value());
        if (value) (*this)(*value);
    }

    void operator()(std::span<const std::uint32_t> values) {
        (*this)(values.size());
        for (const auto value : values) (*this)(value);
    }

private:
    [[gnu::noinline]] void Grow() {
        words.resize(words.size() * 2u);
        data = words.data();
        limit = words.size();
    }

    static constexpr std::size_t ReservedWords = 256;
    std::vector<std::uint64_t>& words;
    std::uint64_t* data = nullptr;
    std::size_t limit = 0;
    std::size_t used = 0;
};

}

ShaderRecompiler::RecompileRequest DrawStageRequest(const DrawProgram& program, std::uint32_t pushOffset, const Graphics::State& graphics, std::span<const ShaderRecompiler::MemoryRegion> memory, std::span<const ShaderRecompiler::LinkedProgram> linked, const Pm4::DrawParameters& drawParameters, const ShaderRecompiler::SpirvTarget& target, const std::optional<ShaderRecompiler::MeshBarycentricLayout>& meshBarycentrics) {
    using Stage = ShaderRecompiler::ShaderStage;
    const auto waveSize = Graphics::StageWaveSize(graphics.stages, program.binary.stage);
    ShaderRecompiler::RecompileRequest request{
        program.binary,
        {waveSize, program.firstUserSgpr, program.userData, std::nullopt, std::nullopt, std::nullopt, memory, RegisteredFloatMode(*program.snapshot)},
        target,
        {0, 0, pushOffset, Graphics::StagePushLimit(graphics.stages, pushOffset, graphics.depthRemap.mode != 0u) - pushOffset % Graphics::PipelinePushSlotBytes},
        ShaderRecompiler::GraphicsCompileContext{program.firstUserSgpr, linked, Graphics::StageMeshConfiguration(graphics.stages, program.binary.stage), graphics.stages.tessellation, {drawParameters.indexAddress, drawParameters.indexCount, drawParameters.indexSize, drawParameters.instanceCount}, program.binary.stage == Stage::Mesh ? meshBarycentrics : std::nullopt}
    };
    if (graphics.depthRemap.mode != 0u && pushOffset < Graphics::PipelinePushSlotBytes) {
        request.layout.pushConstantSizeBytes = std::min(request.layout.pushConstantSizeBytes, ShaderRecompiler::DepthRemapPushOffsetBytes - pushOffset);
    }
    return request;
}

bool BuildDrawStageKey(const ShaderRecompiler::RecompileRequest& request, const ShaderSnapshot& snapshot, std::size_t codeOffset, std::uint64_t deviceSerial, DrawStageKey& key) {
    return BuildDrawStageKey(request, request.context.pixel ? &*request.context.pixel : nullptr, request.context.vertex ? &*request.context.vertex : nullptr, snapshot, codeOffset, deviceSerial, key);
}

bool BuildDrawStageKey(const ShaderRecompiler::RecompileRequest& request, const ShaderRecompiler::ShaderPixelStageInfo* pixel, const ShaderRecompiler::ShaderVertexStageInfo* vertex, const ShaderSnapshot& snapshot, std::size_t codeOffset, std::uint64_t deviceSerial, DrawStageKey& key) {
    key.words.clear();
    key.hash = 0;
    if (!request.useCache || !request.graphics || request.graphics->meshBarycentrics || !request.context.linkedCallFallbacks.empty()) return false;
    KeyWriter write(key.words);
    write(reinterpret_cast<std::uintptr_t>(&snapshot));
    write(codeOffset);
    write(deviceSerial);
    write(request.shader.stage);
    write(request.shader.codeAddress);
    write(request.shader.code.size());
    write(request.shader.headerAddress);
    write(request.shader.header.size());
    const auto& context = request.context;
    write(context.waveSize);
    write(context.userDataBaseRegister);
    write(context.userData);
    write(context.compute);
    write(pixel != nullptr);
    if (pixel != nullptr) write(*pixel);
    write(vertex != nullptr);
    if (vertex != nullptr) write(*vertex);
    write(context.floatMode);
    write(context.convertedSamples);
    write(context.layerRowSamples);
    write(request.layout.descriptorSet);
    write(request.layout.firstBinding);
    write(request.layout.pushConstantOffsetBytes);
    write(request.layout.pushConstantSizeBytes);
    const auto& graphics = *request.graphics;
    write(graphics.firstUserSgpr);
    write(graphics.mesh);
    write(graphics.tessellation);
    if (graphics.tessellation) {
        write(graphics.draw.indexCount);
        write(graphics.draw.instanceCount);
    }
    write(ShaderRecompiler::DebugProbeActive());
    write(ShaderRecompiler::RayTracingStrict());
    write(ShaderRecompiler::RayTracingMiss());
    write.Finish();
    key.hash = HashKeyWords(key.words, 0x9e3779b97f4a7c15ull);
    return true;
}

std::vector<std::pair<std::uint64_t, std::size_t>> DrawStageSpans(std::span<const std::pair<std::uint64_t, std::uint64_t>> runs) {
    constexpr std::uint64_t BlockBytes = 65536;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> sorted(runs.begin(), runs.end());
    std::sort(sorted.begin(), sorted.end());
    std::vector<std::pair<std::uint64_t, std::size_t>> spans;
    std::uint64_t begin = 0, end = 0;
    for (const auto& [low, high] : sorted) {
        if (low >= high) continue;
        if (begin != end && low <= end + BlockBytes) {
            end = std::max(end, high);
            continue;
        }
        if (begin != end) spans.emplace_back(begin, static_cast<std::size_t>(end - begin));
        begin = low;
        end = high;
    }
    if (begin != end) spans.emplace_back(begin, static_cast<std::size_t>(end - begin));
    return spans;
}

const char* DrawStageMismatch(const DispatchVariant& kept, std::span<const std::pair<std::uint64_t, std::uint64_t>> runs, std::span<const std::uint32_t> words, const ShaderRecompiler::RecompileResult& compiled) {
    const auto flatten = [](std::span<const std::pair<std::uint64_t, std::uint64_t>> source, std::span<const std::uint32_t> values) {
        std::vector<std::pair<std::uint64_t, std::uint32_t>> flat;
        std::size_t word = 0;
        for (const auto& [begin, end] : source) {
            for (auto address = begin; address < end && word < values.size(); address += sizeof(std::uint32_t)) flat.emplace_back(address, values[word++]);
        }
        std::sort(flat.begin(), flat.end());
        flat.erase(std::unique(flat.begin(), flat.end()), flat.end());
        return flat;
    };
    const auto& memoized = *kept.compiled;
    const bool sameBindings = memoized.bindings.size() == compiled.bindings.size() && std::equal(memoized.bindings.begin(), memoized.bindings.end(), compiled.bindings.begin(), [](const auto& a, const auto& b) { return a.kind == b.kind && a.role == b.role && a.binding == b.binding && a.count == b.count && a.guestDescriptor == b.guestDescriptor; });
    if (flatten(kept.runs, kept.words) != flatten(runs, words)) return "captured words";
    if (memoized.variantId != compiled.variantId) return "variant";
    if (memoized.pushConstants != compiled.pushConstants) return "push constants";
    if (!sameBindings) return "bindings";
    if (memoized.parameterExports != compiled.parameterExports) return "parameter exports";
    return nullptr;
}

DrawStageStamp DrawStageRecord::Stamp() const {
    const auto layoutValue = layout.load(std::memory_order_acquire);
    const auto generationValue = generation.load(std::memory_order_acquire);
    return layout.load(std::memory_order_acquire) == layoutValue ? DrawStageStamp{generationValue, layoutValue} : DrawStageStamp{};
}

void DrawStageRecord::Restamp(const DrawStageStamp& stamp) {
    generation.store(0, std::memory_order_release);
    layout.store(stamp.layout, std::memory_order_release);
    generation.store(stamp.generation, std::memory_order_release);
}

DrawStageMemo::DrawStageMemo(std::size_t capacity) : capacity(std::max<std::size_t>(capacity, DrawKeySightings::Ways)), sightings(std::max<std::size_t>(capacity, DrawKeySightings::Ways) * 4u) {}

DrawStageMemo::Order::iterator DrawStageMemo::locate(const DrawStageKey& key) {
    const auto [first, last] = index.equal_range(key.hash);
    for (auto it = first; it != last; ++it) {
        if ((*it->second)->key == key) return it->second;
    }
    return order.end();
}

std::shared_ptr<DrawStageRecord> DrawStageMemo::Find(const DrawStageKey& key, bool& admit) {
    std::lock_guard lock(mutex);
    const auto found = locate(key);
    if (found != order.end()) {
        admit = true;
        order.splice(order.begin(), order, found);
        return *found;
    }
    admit = sightings.SeenBefore(key.hash);
    return nullptr;
}

std::size_t DrawStageMemo::Insert(const std::shared_ptr<DrawStageRecord>& record) {
    std::lock_guard lock(mutex);
    const auto found = locate(record->key);
    if (found != order.end()) {
        *found = record;
        order.splice(order.begin(), order, found);
        return 0;
    }
    order.push_front(record);
    index.emplace(record->key.hash, order.begin());
    std::size_t evicted = 0;
    while (order.size() > capacity) {
        const auto victim = std::prev(order.end());
        const auto [first, last] = index.equal_range((*victim)->key.hash);
        for (auto it = first; it != last; ++it) {
            if (it->second != victim) continue;
            index.erase(it);
            break;
        }
        order.erase(victim);
        ++evicted;
    }
    return evicted;
}

bool DrawStageMemo::Erase(const std::shared_ptr<DrawStageRecord>& record) {
    std::lock_guard lock(mutex);
    const auto [first, last] = index.equal_range(record->key.hash);
    for (auto it = first; it != last; ++it) {
        if (*it->second != record) continue;
        order.erase(it->second);
        index.erase(it);
        return true;
    }
    return false;
}

std::size_t DrawStageMemo::Size() {
    std::lock_guard lock(mutex);
    return order.size();
}

void DrawStageMemo::Clear() {
    std::lock_guard lock(mutex);
    index.clear();
    order.clear();
}

DrawStageMemoCounts DrawStageMemoCounters::Load() const {
    DrawStageMemoCounts counts;
    for (std::size_t kind = 0; kind < DrawStageMemoCountKinds; ++kind) counts.values[kind] = values[kind].load(std::memory_order_relaxed);
    return counts;
}

bool Driver::drawStageMemoEnabled() {
    static const bool enabled = std::getenv("APS5_NO_DRAW_STAGE_MEMO") == nullptr;
    return enabled;
}

bool Driver::verifyDrawStageMemo() {
    static const bool verify = std::getenv("APS5_VERIFY_DRAW_STAGE_MEMO") != nullptr;
    return verify;
}

std::size_t Driver::drawStageMemoEntries() {
    static const std::size_t entries = [] {
        const char* text = std::getenv("APS5_DRAW_STAGE_MEMO_ENTRIES");
        const auto parsed = text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
        return parsed != 0 ? static_cast<std::size_t>(parsed) : std::size_t{8192};
    }();
    return entries;
}

DrawStageMemoCounts Driver::StageMemoCounts() const {
    return drawStageMemoCounters.Load();
}

std::size_t Driver::DrawStageMemoSize() {
    return drawStageMemo.Size();
}

std::atomic<void (*)()>& Driver::drawStageCompareHook() {
    static std::atomic<void (*)()> hook{nullptr};
    return hook;
}

void Driver::SetDrawStageCompareHook(void (*hook)()) {
    drawStageCompareHook().store(hook, std::memory_order_release);
}

bool Driver::drawStageUnchanged(const DrawStageRecord& record) {
    const auto& variant = *record.variant;
    if (record.spans.empty()) return true;
    const auto stamp = record.Stamp();
    if (stamp.generation == 0) return false;
    const auto forget = GuestMemory::ForgetSerial();
    if ((forget & 1) != 0) return false;
    if (DrawPipeline::Active()) {
        auto& pipeline = DrawPipeline::Queue0();
        if (pipeline.Busy() && pipeline.OverlapsAny(variant.runs)) return false;
    }
    for (const auto& label : deferredLabels().labels) {
        for (const auto& [begin, end] : variant.runs) {
            if (label.address < end && begin < label.address + label.size) return false;
        }
    }
    if (Graphics::StorageTexture::AnyPendingOverlaps(variant.runs) || Graphics::AnyShadowedOverlaps(variant.runs)) return false;
    for (const auto& [address, bytes] : record.spans) {
        if (!ShaderMemory::Vouched(&QueryPendingSpan, address, bytes, {stamp.generation, stamp.layout})) return false;
    }
    return GuestMemory::ForgetSerial() == forget;
}

DrawStageProof Driver::proveDrawStage(DrawStageRecord& record, std::uint64_t program, std::uint32_t queue) {
    const auto& variant = *record.variant;
    if (variant.registeredViews != nullptr && !CaptureRegisteredViewsCurrent(*variant.registeredViews)) return DrawStageProof::Stale;
    const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DrawCache);
    if (drawStageUnchanged(record)) return DrawStageProof::Proved;
    DrawStageStamp stamp{0, GuestAllocations::GuestAllocationsGeneration_nid_postfix()};
    for (const auto& [address, bytes] : record.spans) {
        const auto collected = GuestMemory::CollectWrites(address, bytes);
        if (collected == 0) {
            stamp.generation = 0;
            break;
        }
        stamp.generation = std::max(stamp.generation, collected);
    }
    thread_local std::vector<ShaderRecompiler::MemoryRegion> regions;
    regions.clear();
    appendEntryRegions(variant, regions);
    std::uint64_t imagesFlushed = 0, runsSynced = 0;
    std::optional<SampledReadScope> sampling;
    auto outcome = validateVariant(program, queue, variant, regions, imagesFlushed, runsSynced, sampling);
    if (gateRetry() && (outcome == EntryOutcome::PublishMoved || outcome == EntryOutcome::PendingMoved)) outcome = validateVariant(program, queue, variant, regions, imagesFlushed, runsSynced, sampling);
    if (outcome != EntryOutcome::Equal) return DrawStageProof::Stale;
    if (const auto hook = drawStageCompareHook().load(std::memory_order_acquire)) hook();
    if (stamp.generation != 0 && stamp.layout == GuestAllocations::GuestAllocationsGeneration_nid_postfix()) record.Restamp(stamp);
    drawStageMemoCounters.Add(DrawStageMemoCount::ComparedBytes, variant.words.size() * sizeof(std::uint32_t));
    return DrawStageProof::Compared;
}

std::shared_ptr<DrawStageRecord> Driver::lookupDrawStage(std::size_t i, std::uint32_t pushOffset, std::uint32_t queue, const std::vector<DrawProgram>& programs, const Graphics::State& graphics, const ShaderRecompiler::ShaderPixelStageInfo& pixel, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, const Pm4::DrawParameters& drawParameters, const std::shared_ptr<VulkanDevice>& localDevice, std::shared_ptr<DrawStageRecord>& pending) {
    pending.reset();
    if (!drawStageMemoEnabled() || drawParameters.indirect) return nullptr;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    const auto started = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto charge = [&](DrawStageMemoCount nanoseconds) {
        if (profile) drawStageMemoCounters.Add(nanoseconds, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count()));
    };
    const auto& program = programs[i];
    auto request = DrawStageRequest(program, pushOffset, graphics, {}, {}, drawParameters, localDevice->Target(), std::nullopt);
    ApplyPreparedSampleModes(*program.snapshot, program.codeOffset, request.context);
    const auto* stagePixel = program.binary.stage == ShaderRecompiler::ShaderStage::Fragment ? &pixel : nullptr;
    const auto* stageVertex = vertexInfos[i] ? &*vertexInfos[i] : nullptr;
    thread_local DrawStageKey* keySlot = nullptr;
    auto& key = ShaderRecompiler::ThreadOwned(keySlot);
    const bool fragment = program.binary.stage == ShaderRecompiler::ShaderStage::Fragment;
    drawStageMemoCounters.Add(DrawStageMemoCount::Lookups);
    if (fragment) drawStageMemoCounters.Add(DrawStageMemoCount::FragmentLookups);
    if (!BuildDrawStageKey(request, stagePixel, stageVertex, *program.snapshot, program.codeOffset, localDevice->Serial(), key)) {
        drawStageMemoCounters.Add(DrawStageMemoCount::Refused);
        charge(DrawStageMemoCount::MissNanoseconds);
        return nullptr;
    }
    const auto pend = [&] {
        auto record = std::make_shared<DrawStageRecord>();
        record->key = key;
        record->convertedSamples = request.context.convertedSamples;
        record->layerRowSamples = request.context.layerRowSamples;
        pending = std::move(record);
    };
    bool admit = false;
    auto found = drawStageMemo.Find(key, admit);
    if (found == nullptr) {
        drawStageMemoCounters.Add(DrawStageMemoCount::Absent);
        if (admit) drawStageMemoCounters.Add(DrawStageMemoCount::AbsentAdmitted);
        if (admit) pend();
        charge(DrawStageMemoCount::MissNanoseconds);
        return nullptr;
    }
    switch (proveDrawStage(*found, program.binary.codeAddress, queue)) {
    case DrawStageProof::Proved:
        drawStageMemoCounters.Add(DrawStageMemoCount::Proved);
        charge(DrawStageMemoCount::HitNanoseconds);
        if (fragment) drawStageMemoCounters.Add(DrawStageMemoCount::FragmentServed);
        return found;
    case DrawStageProof::Compared:
        drawStageMemoCounters.Add(DrawStageMemoCount::Compared);
        charge(DrawStageMemoCount::HitNanoseconds);
        if (fragment) drawStageMemoCounters.Add(DrawStageMemoCount::FragmentServed);
        return found;
    case DrawStageProof::Stale:
        break;
    }
    drawStageMemoCounters.Add(DrawStageMemoCount::Stale);
    drawStageMemo.Erase(found);
    pend();
    charge(DrawStageMemoCount::MissNanoseconds);
    return nullptr;
}

void Driver::memoizeDrawStages(const std::vector<std::shared_ptr<DrawStageRecord>>& pending, const std::vector<std::shared_ptr<DispatchVariant>>& fresh, const std::vector<bool>& reused, const std::vector<StageCapture>& stageCaptures) {
    for (std::size_t i = 0; i < pending.size() && i < fresh.size(); ++i) {
        const auto& record = pending[i];
        if (record == nullptr) continue;
        const auto& capture = stageCaptures[i];
        if (reused[i] || fresh[i] == nullptr || capture.linkedCode || capture.convertedSamples != record->convertedSamples || capture.layerRowSamples != record->layerRowSamples) {
            drawStageMemoCounters.Add(DrawStageMemoCount::Skipped);
            continue;
        }
        record->variant = fresh[i];
        record->spans = DrawStageSpans(fresh[i]->runs);
        drawStageMemoCounters.Add(DrawStageMemoCount::Inserts);
        const auto evicted = drawStageMemo.Insert(record);
        if (evicted != 0) drawStageMemoCounters.Add(DrawStageMemoCount::Evictions, evicted);
    }
}

}
