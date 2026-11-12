#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPlan.hpp"
#include "prx/libSceAgcDriver/Execution/include/AggregateFields.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/KeyHash.hpp"
#include "prx/libSceAgcDriver/Graphics/include/CommitPhases.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include "ThreadOwned.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <mutex>

namespace AgcDriver::DriverDetail {

namespace {

class HandwrittenStateWriter {
public:
    explicit HandwrittenStateWriter(std::vector<std::uint64_t>& words) : words(words) {}

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

template<typename TMember>
struct MemberOf;

template<typename TClass, typename TField>
struct MemberOf<TField TClass::*> {
    using Class = TClass;
    using Field = TField;
};

template<auto TMember, typename TParent, typename TField>
constexpr bool Typed = std::is_same_v<TParent, typename MemberOf<decltype(TMember)>::Class> && std::is_same_v<TField, typename MemberOf<decltype(TMember)>::Field>;

template<auto TMember, typename TParent, typename TField>
bool Is(const TParent& parent, const TField& field) {
    if constexpr (Typed<TMember, TParent, TField>) return &field == &(parent.*TMember);
    else return false;
}

template<auto... TMembers>
struct Members {
    template<typename TParent, typename TField>
    static constexpr bool Typed = (DriverDetail::Typed<TMembers, TParent, TField> || ...);

    template<typename TParent, typename TField>
    static bool Has(const TParent& parent, const TField& field) {
        return (Is<TMembers>(parent, field) || ...);
    }
};

using SkippedMembers = Members<&Graphics::State::minDepthBounds, &Graphics::State::maxDepthBounds, &Graphics::State::depthBiasConstant, &Graphics::State::depthBiasSlope, &Graphics::State::backDepthBiasConstant, &Graphics::State::backDepthBiasSlope, &Graphics::State::depthScale, &Graphics::State::depthOffset, &Graphics::State::depthClampMin, &Graphics::State::depthClampMax, &Graphics::State::viewport, &Graphics::State::scissor, &Graphics::State::renderExtent, &Graphics::State::DepthRemap::scale, &Graphics::State::DepthRemap::offset, &Graphics::DepthTarget::address, &Graphics::DepthTarget::extent, &Graphics::DepthTarget::clearDepth, &Graphics::DepthTarget::clearStencil, &Graphics::DepthTarget::pipeBankXor, &Graphics::DepthTarget::firstSlice, &Graphics::ColorTarget::address, &Graphics::ColorTarget::extent, &Graphics::ColorTarget::bytes, &Graphics::ColorTarget::cmaskBytes, &Graphics::ColorTarget::surfaceAddress, &Graphics::ColorTarget::surfaceExtent, &Graphics::ColorTarget::mipCount, &Graphics::ColorTarget::mip, &Graphics::ColorTarget::clearWords, &Graphics::ColorTarget::depth, &Graphics::ColorTarget::depthSlice, &Graphics::ColorTarget::arrayAddress, &Graphics::ColorTarget::arraySlice, &Graphics::ColorTarget::pipeBankXor, &Graphics::ColorTarget::fmaskPipeBankXor>;

using PresentMembers = Members<&Graphics::State::depthBiasClamp, &Graphics::State::stencilReplayReference, &Graphics::DepthTarget::stencilAddress, &Graphics::DepthTarget::htileAddress, &Graphics::ColorTarget::dccAddress, &Graphics::ColorTarget::chainDccAddress, &Graphics::ColorTarget::cmaskAddress, &Graphics::ColorTarget::arrayLastSlice, &Graphics::ColorTarget::fmaskAddress>;

using LayeredDepthMembers = Members<&Graphics::DepthTarget::lastSlice>;

using ColorTargetMembers = Members<&Graphics::State::color>;

class PlanStateWriter {
public:
    PlanStateWriter(std::vector<std::uint64_t>& words, std::vector<DrawPlanStateRuledField>* ruled) : words(words), ruled(ruled), scratch(Scratch()) {
        data = scratch.data();
        limit = scratch.size();
    }

    void Finish() { words.insert(words.end(), data, data + used); }

    [[gnu::always_inline]] void Word(std::uint64_t word) {
        if (used == limit) [[unlikely]] Grow();
        data[used++] = word;
    }

    template<typename TParent, typename TField>
    bool Member(const TParent& parent, const TField& field) {
        if constexpr (SkippedMembers::Typed<TParent, TField>) {
            if (SkippedMembers::Has(parent, field)) return Note(field);
        }
        if constexpr (PresentMembers::Typed<TParent, TField>) {
            if (PresentMembers::Has(parent, field)) return Presence(field);
        }
        if constexpr (LayeredDepthMembers::Typed<TParent, TField>) {
            if (LayeredDepthMembers::Has(parent, field)) {
                Word(parent.lastSlice != parent.firstSlice);
                return Note(field);
            }
        }
        if constexpr (ColorTargetMembers::Typed<TParent, TField>) {
            if (ColorTargetMembers::Has(parent, field)) {
                if (!parent.hasColorTarget) return Note(field);
                AggregateFields::HashLeaves(field, *this);
                return true;
            }
        }
        return false;
    }

private:
    template<typename TField>
    bool Note(const TField& field) {
        if (ruled != nullptr) ruled->push_back({&field, sizeof(TField)});
        return true;
    }

    template<typename TField>
    bool Presence(const TField& field) {
        if constexpr (AggregateFields::IsOptional<TField>::value) Word(field.has_value());
        else Word(field != TField{});
        return Note(field);
    }

    static std::vector<std::uint64_t>& Scratch() {
        thread_local std::vector<std::uint64_t>* slot = nullptr;
        auto& words = ShaderRecompiler::ThreadOwned(slot);
        if (words.size() < ReservedWords) words.resize(ReservedWords);
        return words;
    }

    [[gnu::noinline]] void Grow() {
        scratch.resize(scratch.size() * 2u);
        data = scratch.data();
        limit = scratch.size();
    }

    static constexpr std::size_t ReservedWords = 512;
    std::vector<std::uint64_t>& words;
    std::vector<DrawPlanStateRuledField>* ruled;
    std::vector<std::uint64_t>& scratch;
    std::size_t used = 0;
    std::uint64_t* data = nullptr;
    std::size_t limit = 0;
};

bool SameVertexInput(const Graphics::VertexInputLayout& a, const Graphics::VertexInputLayout& b) {
    const auto sameBinding = [](const VkVertexInputBindingDescription& x, const VkVertexInputBindingDescription& y) { return x.binding == y.binding && x.stride == y.stride && x.inputRate == y.inputRate; };
    const auto sameAttribute = [](const VkVertexInputAttributeDescription& x, const VkVertexInputAttributeDescription& y) { return x.location == y.location && x.binding == y.binding && x.format == y.format && x.offset == y.offset; };
    return std::ranges::equal(a.bindings, b.bindings, sameBinding) && std::ranges::equal(a.attributes, b.attributes, sameAttribute);
}

}

void AppendHandwrittenDrawPlanStateWords(const Graphics::State& state, std::vector<std::uint64_t>& words) {
    HandwrittenStateWriter write(words);
    write(state.stages);
    write(state.depth.has_value());
    if (state.depth) write(*state.depth);
    for (const bool flag : {state.depthTest, state.depthWrite, state.depthCopySource, state.depthBoundsTest, state.depthBias, state.depthBiasPerFace, state.stencilTest}) write(flag);
    write(state.depthCompare);
    write(state.stencilFront);
    write(state.stencilBack);
    write(state.stencilFront.reference);
    write(state.stencilBack.reference);
    write(state.depthBiasClamp != 0.0f);
    write(state.blendConstants);
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

namespace {

[[gnu::flatten]] void AppendWalkedDrawPlanStateWords(const Graphics::State& state, std::vector<std::uint64_t>& words) {
    PlanStateWriter write(words, nullptr);
    AggregateFields::HashLeaves(state, write);
    write.Finish();
}

}

void AppendDrawPlanStateWords(const Graphics::State& state, std::vector<std::uint64_t>& words) {
    if (Graphics::RuntimeSwitches::Set(Graphics::RuntimeSwitch::HandwrittenPlanState)) {
        AppendHandwrittenDrawPlanStateWords(state, words);
        return;
    }
    AppendWalkedDrawPlanStateWords(state, words);
}

std::vector<DrawPlanStateRuledField> DrawPlanStateRuledFields(const Graphics::State& state) {
    std::vector<std::uint64_t> words;
    std::vector<DrawPlanStateRuledField> ruled;
    PlanStateWriter write(words, &ruled);
    AggregateFields::HashLeaves(state, write);
    write.Finish();
    return ruled;
}

std::shared_ptr<const DrawPlanKey> BuildDrawPlanKey(std::span<const Graphics::CompiledShader> stages, const Graphics::State& state, const Pm4::DrawParameters& draw) {
    if (stages.empty() || std::any_of(stages.begin(), stages.end(), [](const Graphics::CompiledShader& stage) { return stage.program == nullptr || stage.program->PipelineVariantId() == 0; })) return nullptr;
    auto key = std::make_shared<DrawPlanKey>();
    constexpr std::size_t StateWords = 256;
    std::size_t stageWords = 1;
    for (const auto& stage : stages) stageWords += 10 + stage.program->bindings.size() + 7 * stage.program->vertexAttributes.size();
    key->words.reserve(stageWords + StateWords + 3);
    key->words.push_back(stages.size());
    for (const auto& stage : stages) {
        const auto& program = *stage.program;
        const auto identity = program.Identity();
        for (const std::uint64_t word : {static_cast<std::uint64_t>(stage.stage), program.variantId, program.specializationId, std::uint64_t{stage.pushConstantOffset}, std::uint64_t{program.pushConstants.size()}, std::uint64_t{program.bdaAbiVersion}, std::uint64_t{program.vertexAttributes.size()}, identity.low, identity.high, std::uint64_t{program.bindings.size()}}) key->words.push_back(word);
        for (const auto& binding : program.bindings) key->words.push_back(static_cast<std::uint64_t>(binding.role) | (std::uint64_t{binding.readOnly} << 32u));
        for (const auto& attribute : program.vertexAttributes) {
            const auto& fields = attribute.resource.fields;
            for (const std::uint64_t word : {std::uint64_t{attribute.location}, std::uint64_t{attribute.components}, std::uint64_t{attribute.fetchIndex}, std::uint64_t{attribute.formatComponents}, std::uint64_t{fields[1] >> 16u}, std::uint64_t{fields[3]}, std::uint64_t{Graphics::NullVertexDescriptor(attribute)}}) key->words.push_back(word);
        }
    }
    AppendDrawPlanStateWords(state, key->words);
    key->words.push_back(draw.indexed);
    key->words.push_back(draw.indexed ? draw.indexSize : 0u);
    key->words.push_back(draw.indirect.has_value());
    key->hash = HashKeyWords(key->words, 0x6a09e667f3bcc909ull);
    return key;
}

bool DrawPlanKey::Matches(const DrawPlanKey& other) const {
    return hash == other.hash && words == other.words;
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

namespace {

std::uint64_t PlanSignature(const DrawRecipe& decisions) {
    std::uint64_t hash = 0x243f6a8885a308d3ull ^ decisions.targetViews.size() ^ (static_cast<std::uint64_t>(decisions.hasDepth) << 63u);
    for (const auto view : decisions.targetViews) {
        hash = (hash ^ reinterpret_cast<std::uint64_t>(view)) * 0xff51afd7ed558ccdull;
        hash ^= hash >> 32u;
    }
    return hash;
}

std::size_t LookupDistanceBucket(std::uint64_t distance) {
    return std::min<std::size_t>(static_cast<std::size_t>(std::bit_width(distance)) - 1u, PlanLookupDistanceBuckets - 1u);
}

bool ProfileDrawPlans() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return profile;
}

void AddCount(DrawPlanCounters* counters, DrawPlanCount base, std::size_t offset) {
    counters->values[static_cast<std::size_t>(base) + offset].fetch_add(1, std::memory_order_relaxed);
}

}

std::shared_ptr<const DrawRecipe> DrawPlanIndex::Find(const DrawPlanKey& key, DrawPlanCounters* counters) {
    std::lock_guard lock(mutex);
    const auto found = locate(key);
    if (counters != nullptr) ++lookups;
    if (found == order.end()) return nullptr;
    if (counters != nullptr) {
        if (found->lastLookup != 0) AddCount(counters, DrawPlanCount::LookupDistances, LookupDistanceBucket(lookups - found->lastLookup));
        found->lastLookup = lookups;
    }
    order.splice(order.begin(), order, found);
    return found->decisions;
}

std::size_t DrawPlanIndex::Insert(const std::shared_ptr<const DrawPlanKey>& key, std::shared_ptr<const DrawRecipe> decisions, DrawPlanCounters* counters) {
    std::lock_guard lock(mutex);
    const auto signature = counters != nullptr ? PlanSignature(*decisions) : 0u;
    const auto found = locate(*key);
    if (found != order.end() && counters != nullptr) {
        const auto known = std::min(found->signatureCount, PlanSignatureSetLimit);
        const bool seen = std::find(found->signatures.begin(), found->signatures.begin() + static_cast<std::ptrdiff_t>(known), signature) != found->signatures.begin() + static_cast<std::ptrdiff_t>(known);
        if (!seen) found->signatures[found->signatureCount++ % PlanSignatureSetLimit] = signature;
        AddCount(counters, DrawPlanCount::ReplacementSignatureSets, std::min(found->signatureCount, PlanSignatureSetLimit + 1u) - 1u);
        if (seen) AddCount(counters, DrawPlanCount::ReplacementReturns, 0);
    }
    if (found != order.end()) {
        found->decisions = std::move(decisions);
        order.splice(order.begin(), order, found);
        return 0;
    }
    order.push_front({key, std::move(decisions), lookups, {signature}, counters != nullptr ? 1u : 0u});
    index.emplace(key->hash, order.begin());
    std::size_t evicted = 0;
    while (order.size() > capacity || (!order.empty() && order.back().decisions->pipeline.expired())) {
        const auto victim = std::prev(order.end());
        const auto [first, last] = index.equal_range(victim->key->hash);
        for (auto it = first; it != last; ++it) {
            if (it->second != victim) continue;
            index.erase(it);
            break;
        }
        if (counters != nullptr) this->evicted.Remember(victim->key->hash, capacity);
        order.erase(victim);
        ++evicted;
    }
    return evicted;
}

void DrawPlanIndex::HashRing::Remember(std::uint64_t hash, std::size_t limit) {
    if (!hashes.insert(hash).second) return;
    order.push_back(hash);
    while (order.size() > limit) {
        hashes.erase(order.front());
        order.pop_front();
    }
}

DrawPlanAbsence DrawPlanIndex::ClassifyAbsent(const DrawPlanKey& key) {
    std::lock_guard lock(mutex);
    if (evicted.hashes.contains(key.hash)) return DrawPlanAbsence::Evicted;
    return undecided.hashes.contains(key.hash) ? DrawPlanAbsence::Undecided : DrawPlanAbsence::New;
}

void DrawPlanIndex::NoteUndecided(const DrawPlanKey& key) {
    std::lock_guard lock(mutex);
    undecided.Remember(key.hash, capacity);
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
        if (ProfileDrawPlans()) drawPlans.NoteUndecided(*key);
        return;
    }
    if (!drawn->servable) drawPlanCounters.Add(DrawPlanCount::Unservable);
    drawPlanCounters.Add(DrawPlanCount::MovedBases, drawn->movedBases);
    const auto plan = drawPlans.Find(*key);
    const auto insert = [&] {
        const auto evicted = drawPlans.Insert(key, drawn, ProfileDrawPlans() ? &drawPlanCounters : nullptr);
        if (evicted != 0) drawPlanCounters.Add(DrawPlanCount::Evictions, evicted);
    };
    if (plan == nullptr || plan->pipeline.expired()) {
        drawPlanCounters.Add(DrawPlanCount::Absent);
        insert();
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
    if (drawn->plannable && (!plan->plannable || !differences.sameInstance || (drawn->servable && !plan->servable))) {
        drawPlanCounters.Add(DrawPlanCount::Replaced);
        insert();
    }
}

bool Driver::serveDrawPlan(const std::shared_ptr<VulkanDevice>& device, const DrawPlanKey& key, const Graphics::State& graphics, const Pm4::DrawParameters& draw, std::span<const Graphics::CompiledShader> stages, std::span<const Graphics::GuestMemorySnapshot> snapshots, const VulkanDevice::DrawTraceContext* trace, std::shared_ptr<const DrawRecipe>& attachable) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    drawPlanCounters.Add(DrawPlanCount::PlanLookups);
    const auto plan = Graphics::InCommitPhase(Graphics::CommitPhase::PlanHitFind, [&] { return drawPlans.Find(key, profile ? &drawPlanCounters : nullptr); });
    if (plan == nullptr) {
        drawPlanCounters.Add(DrawPlanCount::MissAbsent);
        if (profile) {
            switch (drawPlans.ClassifyAbsent(key)) {
                case DrawPlanAbsence::Evicted: drawPlanCounters.Add(DrawPlanCount::AbsentEvicted); break;
                case DrawPlanAbsence::Undecided: drawPlanCounters.Add(DrawPlanCount::AbsentUndecided); break;
                case DrawPlanAbsence::New: drawPlanCounters.Add(DrawPlanCount::AbsentNew); break;
            }
        }
        return false;
    }
    if (!plan->plannable) {
        drawPlanCounters.Add(DrawPlanCount::MissUnservable);
        return false;
    }
    const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto outcome = device->DrawFromPlan(graphics, draw, stages, snapshots, *plan, trace);
    if (outcome.recorded) {
        drawPlanCounters.Add(DrawPlanCount::Hits);
        drawPlanCounters.Add(DrawPlanCount::SharedBindingStages, outcome.sharedBindingStages);
        drawPlanCounters.Add(DrawPlanCount::ComparedBindingStages, outcome.comparedBindingStages);
        if (outcome.movedBuffers != 0) {
            drawPlanCounters.Add(DrawPlanCount::RebasedHits);
            drawPlanCounters.Add(DrawPlanCount::RebasedBuffers, outcome.movedBuffers);
        }
        if (outcome.templateInstance) drawPlanCounters.Add(DrawPlanCount::TemplateInstanceHits);
        if (outcome.ownAttachments) {
            drawPlanCounters.Add(DrawPlanCount::OwnAttachmentHits);
            drawPlanCounters.Add(DrawPlanCount::OwnTargetsNanoseconds, outcome.ownTargetsNs);
            drawPlanCounters.Add(DrawPlanCount::OwnFramebufferNanoseconds, outcome.ownFramebufferNs);
        }
        if (outcome.ownRecipe != nullptr) {
            drawPlanCounters.Add(DrawPlanCount::OwnRecipes);
            attachable = std::move(outcome.ownRecipe);
        } else if (plan->servable && outcome.movedBuffers == 0 && !outcome.templateInstance && !outcome.ownAttachments && plan->renderExtent.width == graphics.renderExtent.width && plan->renderExtent.height == graphics.renderExtent.height) {
            attachable = plan;
        }
        if (Graphics::VerifyDrawPlans()) drawPlanCounters.Add(DrawPlanCount::Verified);
        if (profile) drawPlanCounters.Add(DrawPlanCount::HitNanoseconds, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()));
        return true;
    }
    switch (outcome.miss) {
        case Graphics::DrawRecipeMiss::NotRecordable: drawPlanCounters.Add(DrawPlanCount::MissNotRecordable); break;
        case Graphics::DrawRecipeMiss::TargetGone: drawPlanCounters.Add(DrawPlanCount::MissTargetGone); break;
        case Graphics::DrawRecipeMiss::TemplateGone: drawPlanCounters.Add(DrawPlanCount::MissTemplateGone); break;
        case Graphics::DrawRecipeMiss::Proof: drawPlanCounters.Add(DrawPlanCount::MissProof); break;
        case Graphics::DrawRecipeMiss::Instance:
            drawPlanCounters.Add(DrawPlanCount::MissInstance);
            if (outcome.handedTargets) drawPlanCounters.Add(DrawPlanCount::HandedTargets);
            if (outcome.instanceMismatches != 0) AddCount(&drawPlanCounters, DrawPlanCount::InstanceMismatchSets, outcome.instanceMismatches);
            switch (outcome.instance) {
                case Graphics::DrawPlanInstance::Shape: drawPlanCounters.Add(DrawPlanCount::InstanceShape); break;
                case Graphics::DrawPlanInstance::Template: drawPlanCounters.Add(DrawPlanCount::InstanceTemplate); break;
                case Graphics::DrawPlanInstance::Target: drawPlanCounters.Add(DrawPlanCount::InstanceTarget); break;
                case Graphics::DrawPlanInstance::View: drawPlanCounters.Add(DrawPlanCount::InstanceView); break;
                case Graphics::DrawPlanInstance::Depth: drawPlanCounters.Add(DrawPlanCount::InstanceDepth); break;
                default: break;
            }
            break;
        case Graphics::DrawRecipeMiss::Moved: drawPlanCounters.Add(outcome.patchedData ? DrawPlanCount::MissPatchedData : DrawPlanCount::MissMoved); break;
        default: drawPlanCounters.Add(DrawPlanCount::MissObjectsGone); break;
    }
    return false;
}

std::chrono::steady_clock::time_point Driver::fullPlanDrawStart(const DrawPlanKey* key) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return key != nullptr && profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
}

void Driver::countFullPlanDraw(std::chrono::steady_clock::time_point start) {
    if (start == std::chrono::steady_clock::time_point{}) return;
    drawPlanCounters.Add(DrawPlanCount::FullDraws);
    drawPlanCounters.Add(DrawPlanCount::FullNanoseconds, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()));
}

}
