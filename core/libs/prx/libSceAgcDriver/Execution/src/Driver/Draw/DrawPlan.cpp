#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPlan.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <mutex>

namespace AgcDriver::DriverDetail {

namespace {

class StateWriter {
public:
    explicit StateWriter(std::vector<std::uint64_t>& words) : words(words) {}

    template<typename TValue>
    void operator()(TValue value) requires (std::is_integral_v<TValue> || std::is_enum_v<TValue>) {
        words.push_back(static_cast<std::uint64_t>(value));
    }

    void operator()(float value) { words.push_back(std::bit_cast<std::uint32_t>(value)); }

    template<typename TValue, std::size_t TSize>
    void operator()(const std::array<TValue, TSize>& values) {
        for (const auto& value : values) (*this)(value);
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

    void operator()(const VkStencilOpState& stencil) {
        for (const auto value : {stencil.failOp, stencil.passOp, stencil.depthFailOp}) (*this)(value);
        (*this)(stencil.compareOp);
        (*this)(stencil.compareMask);
        (*this)(stencil.writeMask);
    }

    void operator()(const VkPipelineColorBlendAttachmentState& blend) {
        (*this)(blend.blendEnable);
        for (const auto value : {blend.srcColorBlendFactor, blend.dstColorBlendFactor, blend.srcAlphaBlendFactor, blend.dstAlphaBlendFactor}) (*this)(value);
        (*this)(blend.colorBlendOp);
        (*this)(blend.alphaBlendOp);
        (*this)(blend.colorWriteMask);
    }

    void operator()(const Graphics::ShaderStages& stages) {
        (*this)(stages.path);
        (*this)(stages.registerValue);
        (*this)(stages.vertexWaveSize);
        (*this)(stages.fragmentWaveSize);
        (*this)(stages.mesh);
        (*this)(stages.tessellation);
        (*this)(stages.minTessellationLevel);
        (*this)(stages.maxTessellationLevel);
        (*this)(stages.hullWaveSize);
        (*this)(stages.tessellationGeometry);
        (*this)(stages.legacyVertex);
    }

    void operator()(const Graphics::DepthTarget& depth) {
        (*this)(depth.format);
        (*this)(depth.samples);
        (*this)(depth.stencilAddress != 0);
        (*this)(depth.htileAddress != 0);
        (*this)(depth.htileStencil);
        (*this)(depth.depthSwizzle);
        (*this)(depth.iterate256);
        (*this)(depth.htilePipeAligned);
        (*this)(depth.htileVrsEncoding);
        (*this)(depth.htileDepthOnly);
        (*this)(depth.gcn);
        (*this)(depth.depthPartiallyResident);
        (*this)(depth.stencilPartiallyResident);
        (*this)(depth.lastSlice != depth.firstSlice);
    }

    void operator()(const Graphics::ColorTarget& color) {
        (*this)(color.format);
        (*this)(color.componentMapping);
        (*this)(color.tileMode);
        (*this)(color.elementBytes);
        (*this)(color.packing);
        (*this)(color.dccAddress != 0);
        (*this)(color.chainDccAddress != 0);
        (*this)(color.dccAlphaOnMsb);
        (*this)(color.dccPipeAligned);
        (*this)(color.cmaskAddress != 0);
        (*this)(color.cmaskFastClear);
        (*this)(color.eqaaFragments);
        (*this)(color.mipTail);
        (*this)(color.slot);
        (*this)(color.arrayLastSlice != 0);
        (*this)(color.layers);
        (*this)(color.exportIndex);
        (*this)(color.uintExport);
        (*this)(color.volume);
        (*this)(color.samples);
        (*this)(color.coverageSamples);
        (*this)(color.dccControl);
        (*this)(color.fmaskAddress != 0);
        (*this)(color.partialWriteMask);
        (*this)(color.mergeBlend);
        (*this)(color.staged);
        (*this)(color.stagedMask);
        (*this)(color.stagedFormat);
    }

private:
    std::vector<std::uint64_t>& words;
};

std::uint64_t HashPlanWords(std::span<const std::uint64_t> words) {
    std::uint64_t hash = 0x6a09e667f3bcc909ull ^ words.size();
    for (const auto word : words) {
        hash = (hash ^ word) * 0xff51afd7ed558ccdull;
        hash ^= hash >> 32u;
    }
    return hash;
}

bool SameVertexInput(const Graphics::VertexInputLayout& a, const Graphics::VertexInputLayout& b) {
    const auto sameBinding = [](const VkVertexInputBindingDescription& x, const VkVertexInputBindingDescription& y) { return x.binding == y.binding && x.stride == y.stride && x.inputRate == y.inputRate; };
    const auto sameAttribute = [](const VkVertexInputAttributeDescription& x, const VkVertexInputAttributeDescription& y) { return x.location == y.location && x.binding == y.binding && x.format == y.format && x.offset == y.offset; };
    return std::ranges::equal(a.bindings, b.bindings, sameBinding) && std::ranges::equal(a.attributes, b.attributes, sameAttribute);
}

}

void AppendDrawPlanStateWords(const Graphics::State& state, std::vector<std::uint64_t>& words) {
    StateWriter write(words);
    write(state.stages);
    write(state.depth.has_value());
    if (state.depth) write(*state.depth);
    for (const bool flag : {state.depthTest, state.depthWrite, state.depthCopySource, state.depthBoundsTest, state.depthBias, state.depthBiasPerFace, state.stencilTest}) write(flag);
    write(state.depthCompare);
    write(state.stencilFront);
    write(state.stencilBack);
    write(state.stencilReplayReference.has_value());
    write(state.hasColorTarget);
    if (state.hasColorTarget) write(state.color);
    write(state.colors.size());
    for (const auto& color : state.colors) write(color);
    write(state.blends.size());
    for (const auto& blend : state.blends) write(blend);
    write(state.blend);
    for (const bool flag : {state.rectList, state.quadList, state.rasterizerDiscard, state.dualSourceBlend, state.primitiveRestart, state.negativeOneToOne, state.depthClamp, state.alphaToCoverage, state.ditheredAlphaToCoverage, state.perSampleShading, state.mergedCoverage, state.truncatedCoverage, state.customSampleLocations, state.sampleLocationGrid, state.vertexPoints, state.pixelEarlyZ, state.pixelKill}) write(flag);
    write(state.tessellationCapture != nullptr);
    write(state.reversedBlend);
    write(state.renderLayers);
    write(state.meshLayerBase);
    write(state.meshLayerCount);
    write(state.layerExports);
    write(state.topology);
    write(state.depthRemap.mode);
    write(state.ditheredAlphaSamples);
    write(state.conservativeRasterization);
    write(state.cullMode);
    write(state.polygonMode);
    write(state.lineWidth);
    write(state.frontFace);
    write(state.provokingVertexMode);
    write(state.colorRop);
    write(state.samples);
    write(state.sampleLocations);
    write(state.paClVsOutCntl);
    write(state.pointSize);
    write(state.pointSizeMin);
    write(state.pointSizeMax);
    write(state.quadSampleMask);
}

std::shared_ptr<const DrawPlanKey> BuildDrawPlanKey(std::span<const std::shared_ptr<DispatchVariant>> stages, const Graphics::State& state, const Pm4::DrawParameters& draw) {
    if (stages.empty() || std::any_of(stages.begin(), stages.end(), [](const std::shared_ptr<DispatchVariant>& stage) { return stage == nullptr; })) return nullptr;
    auto key = std::make_shared<DrawPlanKey>();
    key->words.reserve(160);
    key->words.push_back(stages.size());
    for (const auto& stage : stages) {
        key->words.push_back(reinterpret_cast<std::uintptr_t>(stage.get()));
        key->stages.emplace_back(stage);
    }
    AppendDrawPlanStateWords(state, key->words);
    key->words.push_back(draw.indexed);
    key->words.push_back(draw.indexed ? draw.indexSize : 0u);
    key->words.push_back(draw.indirect.has_value());
    key->hash = HashPlanWords(key->words);
    return key;
}

bool DrawPlanKey::Matches(const DrawPlanKey& other) const {
    if (hash != other.hash || words != other.words || stages.size() != other.stages.size()) return false;
    for (std::size_t i = 0; i < stages.size(); ++i) {
        if (stages[i].owner_before(other.stages[i]) || other.stages[i].owner_before(stages[i])) return false;
    }
    return true;
}

bool DrawPlanKey::Expired() const {
    return std::any_of(stages.begin(), stages.end(), [](const std::weak_ptr<const DispatchVariant>& stage) { return stage.expired(); });
}

DrawPlanCounts DrawPlanCounters::Load() const {
    DrawPlanCounts counts;
    for (std::size_t kind = 0; kind < DrawPlanCountKinds; ++kind) counts.values[kind] = values[kind].load(std::memory_order_relaxed);
    return counts;
}

DrawPlanDecisionDifferences CompareDrawPlanDecisions(const DrawRecipe& plan, const DrawRecipe& drawn) {
    DrawPlanDecisionDifferences differences;
    differences.pipeline = plan.pipeline.owner_before(drawn.pipeline) || drawn.pipeline.owner_before(plan.pipeline);
    differences.fragmentOutputs = plan.fragmentOutputs != drawn.fragmentOutputs;
    differences.shaderStages = plan.shaderStages != drawn.shaderStages;
    differences.pushStages = plan.pushStages != drawn.pushStages;
    differences.hasDepth = plan.hasDepth != drawn.hasDepth;
    differences.vertexInput = !SameVertexInput(plan.vertexInput, drawn.vertexInput);
    differences.maskedState = plan.masked.has_value() != drawn.masked.has_value();
    const auto sameTargets = plan.targets.size() == drawn.targets.size() && std::ranges::equal(plan.targets, drawn.targets, [](const auto& a, const auto& b) { return !a.owner_before(b) && !b.owner_before(a); });
    differences.sameInstance = sameTargets && plan.targetViews == drawn.targetViews;
    differences.passDiffers = differences.sameInstance && plan.passKey != drawn.passKey;
    return differences;
}

DrawPlanIndex::DrawPlanIndex(std::size_t capacity) : capacity(std::max<std::size_t>(capacity, 1u)) {}

DrawPlanIndex::Order::iterator DrawPlanIndex::locate(const DrawPlanKey& key) {
    const auto [first, last] = index.equal_range(key.hash);
    for (auto it = first; it != last; ++it) {
        if (it->second->key->Matches(key)) return it->second;
    }
    return order.end();
}

std::shared_ptr<const DrawRecipe> DrawPlanIndex::Find(const DrawPlanKey& key) {
    std::lock_guard lock(mutex);
    const auto found = locate(key);
    if (found == order.end()) return nullptr;
    order.splice(order.begin(), order, found);
    return found->decisions;
}

std::size_t DrawPlanIndex::Insert(const std::shared_ptr<const DrawPlanKey>& key, std::shared_ptr<const DrawRecipe> decisions) {
    std::lock_guard lock(mutex);
    const auto found = locate(*key);
    if (found != order.end()) {
        found->decisions = std::move(decisions);
        order.splice(order.begin(), order, found);
        return 0;
    }
    order.push_front({key, std::move(decisions)});
    index.emplace(key->hash, order.begin());
    std::size_t evicted = 0;
    while (order.size() > capacity || (!order.empty() && order.back().key->Expired())) {
        const auto victim = std::prev(order.end());
        const auto [first, last] = index.equal_range(victim->key->hash);
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

std::size_t DrawPlanIndex::Size() {
    std::lock_guard lock(mutex);
    return order.size();
}

std::size_t Driver::drawPlanEntries() {
    static const std::size_t entries = [] {
        const char* text = std::getenv("APS5_DRAW_PLAN_ENTRIES");
        const auto parsed = text != nullptr ? std::strtoull(text, nullptr, 10) : 0ull;
        return parsed != 0 ? static_cast<std::size_t>(parsed) : std::size_t{4096};
    }();
    return entries;
}

DrawPlanCounts Driver::DrawPlanShadowCounts() const {
    return drawPlanCounters.Load();
}

void Driver::shadowDrawPlan(const std::shared_ptr<const DrawPlanKey>& key, const std::shared_ptr<const DrawRecipe>& drawn) {
    drawPlanCounters.Add(DrawPlanCount::Lookups);
    if (drawn == nullptr) {
        drawPlanCounters.Add(DrawPlanCount::NoDecision);
        return;
    }
    if (!drawn->servable) drawPlanCounters.Add(DrawPlanCount::Unservable);
    drawPlanCounters.Add(DrawPlanCount::MovedBases, drawn->movedBases);
    const auto plan = drawPlans.Find(*key);
    if (plan == nullptr || plan->pipeline.expired()) {
        drawPlanCounters.Add(DrawPlanCount::Absent);
        const auto evicted = drawPlans.Insert(key, drawn);
        if (evicted != 0) drawPlanCounters.Add(DrawPlanCount::Evictions, evicted);
        return;
    }
    drawPlanCounters.Add(DrawPlanCount::Present);
    const auto differences = CompareDrawPlanDecisions(*plan, *drawn);
    if (differences.Agree()) drawPlanCounters.Add(DrawPlanCount::Agreed);
    if (differences.pipeline) drawPlanCounters.Add(DrawPlanCount::Pipeline);
    if (differences.fragmentOutputs) drawPlanCounters.Add(DrawPlanCount::FragmentOutputs);
    if (differences.shaderStages) drawPlanCounters.Add(DrawPlanCount::ShaderStages);
    if (differences.pushStages) drawPlanCounters.Add(DrawPlanCount::PushStages);
    if (differences.hasDepth) drawPlanCounters.Add(DrawPlanCount::HasDepth);
    if (differences.vertexInput) drawPlanCounters.Add(DrawPlanCount::VertexInput);
    if (differences.maskedState) drawPlanCounters.Add(DrawPlanCount::MaskedState);
    drawPlanCounters.Add(differences.sameInstance ? DrawPlanCount::SameInstance : DrawPlanCount::InstanceDiffers);
    if (differences.passDiffers) drawPlanCounters.Add(DrawPlanCount::PassDiffersSameInstance);
}

}
