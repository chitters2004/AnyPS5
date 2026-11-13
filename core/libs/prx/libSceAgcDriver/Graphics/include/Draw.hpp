#include <string>
#include <optional>
#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAW_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DRAW_HPP

#include "prx/libSceAgcDriver/Graphics/include/Pipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/Recipe.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include <array>
#include <functional>
#include <cstdint>
#include <memory>
#include <vector>

namespace AgcDriver::Graphics {

struct VkDrawCounts {
    std::uint64_t direct = 0;
    std::uint64_t indexed = 0;
    std::uint64_t indirect = 0;
    std::uint64_t mesh = 0;
};
VkDrawCounts TakeVkDrawCounts();

struct InertFragmentCounts {
    std::uint64_t omitted = 0;
    std::uint64_t kept = 0;
};
InertFragmentCounts TakeInertFragmentCounts();

struct HostImport;

struct DrawTraceContext {
    std::uint64_t token = 0;
    std::uint64_t submission = 0;
    std::uint32_t queue = 0;
    std::size_t offset = 0;
    std::uint64_t target = 0;
};

// `recipe`, when given, receives the DrawRecipe a recorded, cacheable, reusable, non-indirect draw
// built for its draw-cache entry (design_cpu_final M8; null otherwise, and always under
// APS5_NO_DRAW_RECIPE=1).
void Draw(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots = {}, std::shared_ptr<const DrawRecipe>* recipe = nullptr, const DrawTraceContext* trace = nullptr, std::uint64_t shape = 0);

struct PixelFunctionLdsLayouts {
    VkDescriptorSetLayout empty = VK_NULL_HANDLE;
    VkDescriptorSetLayout buffer = VK_NULL_HANDLE;
};

PixelFunctionLdsLayouts PixelFunctionLdsSetLayouts(const Context& context);
void PreparePixelFunctionLds(const Context& context, VkCommandBuffer commands, std::uint32_t subgroupSize);
void BindPixelFunctionLds(const Context& context, VkCommandBuffer commands, VkPipelineLayout layout);
void CheckPixelFunctionLds(const Context& context);
void ReleasePixelFunctionLds(VkDevice device);
bool UsesMeshWindow(std::span<const CompiledShader> shaders);
bool ValidationKey(const Context& context, std::span<const CompiledShader> shaders, const State& state, std::vector<std::uint64_t>& key);
VkDescriptorSetLayout MeshWindowSetLayout(const Context& context);
void PrepareMeshWindow(const Context& context);
void BindMeshWindow(const Context& context, VkCommandBuffer commands, VkPipelineLayout layout);
void ReleaseMeshWindow(VkDevice device);
std::optional<std::string> KnownValidationFailure(const Context& context, std::span<const CompiledShader> shaders, const State& state);
std::uint64_t DrawRenderPassKey(const Context& context, const State& state, std::span<const VkImageView> targetViews);

struct DrawInputCopy {
    std::shared_ptr<Buffer> buffer;
    VkDeviceSize offset = 0;
    std::size_t bytes = 0;
    bool reused = false;
    bool prepared = false;
    std::uint32_t derived = 0;
    std::uint64_t generation = 0;
    std::uint64_t forgetSerial = 0;
    Recorder::OwnedDrawInputProof owned;
    std::span<std::byte> Bytes() const;
    std::optional<RangeCertificate> certified;
};
enum class TargetResidency : std::uint8_t { Multisampled, Resident, Copied, LinearImport };
bool ResidentTargetsEnabled();
TargetResidency ColorTargetResidency(const Context& context, const ColorTarget& color, bool residentTargets, bool residentLinear);
bool LinearTargetImported(const Context& context, const ColorTarget& color, bool import);
std::optional<bool> PredictDrawDeviceStaging(const Context& context, const State& state);
DrawInputCopy CopyDrawInput(const Context& context, Recorder* recorder, std::uint64_t address, std::size_t bytes, std::size_t alignment, Recorder::SnapshotUse use);
DrawInputCopy CopyZeroPaddedDrawInput(const Context& context, std::uint64_t address, std::size_t bytes, std::size_t validBytes, std::size_t stride = 0);
void KeepDrawInput(Recorder* recorder, std::uint64_t address, const DrawInputCopy& copy, Recorder::SnapshotUse use, std::uint32_t derived);
const HostImport* InPlaceDrawInput(const Context& context, std::uint64_t address, std::size_t bytes, std::size_t alignment, bool* behindGpuWrite = nullptr);
struct InPlaceIndices {
    std::optional<std::uint32_t> highest;
    bool reused = false;
};
InPlaceIndices InPlaceIndexRange(Recorder& recorder, std::uint64_t address, std::size_t bytes, std::uint32_t indexSize, bool skipRestart);

std::array<std::uint32_t, 4> MeshIndexBufferDescriptor(const Pm4::DrawParameters& draw);

struct MeshArguments {
    std::uint32_t groups;
    std::uint32_t instances;
    std::uint32_t layers;
    std::uint32_t indexCount;
    std::uint32_t firstIndex;
};
static_assert(sizeof(MeshArguments) == ShaderRecompiler::MeshArgumentBytes);
struct MeshArgumentRules {
    std::uint32_t indexCount;
    std::uint32_t inputSize;
    std::uint32_t step;
    std::uint32_t primitivesPerGroup;
    std::uint32_t maxGroups;
    std::uint32_t maxInstances;
    std::uint32_t maxTotal;
};
MeshArgumentRules MeshArgumentRulesFor(const Context& context, const ShaderRecompiler::MeshConfiguration& mesh, std::uint32_t indexCount);
struct MeshGrid {
    std::uint32_t groups;
    std::uint32_t instances;
    std::uint32_t layers;
    std::uint32_t layerStride;
};
std::optional<MeshGrid> MeshGridFor(const Context& context, std::uint32_t groups, std::uint32_t instances);
MeshArguments ResolveMeshArguments(const Pm4::DrawArguments& record, const MeshArgumentRules& rules);

// Why DrawWithRecipe did not record from the recipe (the caller then runs Draw): the draw is not
// recordable (no recorder, APS5_SYNC_DRAWS, APS5_DUMP_TARGETS), a resident target is gone from the
// storage cache, the template is gone, the pipeline is gone from the pipeline store, or the
// template's proof failed (ShaderResources::ProveCurrent; the template is removed from the
// resource cache and kept by the batch).
enum class DrawRecipeMiss : std::uint8_t { None, NotRecordable, TargetGone, TemplateGone, ObjectsGone, Proof, Instance, Moved, Count };
enum class DrawRecipeUse : std::uint8_t { Exact, Plan };
enum class DrawPlanInstance : std::uint8_t { None, Shape, Template, Target, View, Depth };
const char* DrawRecipeMissName(DrawRecipeMiss miss);
struct DrawRecipeOutcome {
    bool recorded = false;
    DrawRecipeMiss miss = DrawRecipeMiss::None;
    ShaderResources::ProofReport proof;
    // APS5_PROFILE_DRAW: the proof's and the record's time.
    double proofUs = 0;
    double recordUs = 0;
    std::uint32_t sharedBindingStages = 0;
    std::uint32_t comparedBindingStages = 0;
    std::uint32_t movedBuffers = 0;
    bool patchedData = false;
    DrawPlanInstance instance = DrawPlanInstance::None;
    std::uint8_t instanceMismatches = 0;
    bool templateInstance = false;
    bool ownAttachments = false;
    bool handedTargets = false;
    std::shared_ptr<const DrawRecipe> ownRecipe;
    std::uint64_t ownTargetsNs = 0;
    std::uint64_t ownFramebufferNs = 0;
};

inline constexpr std::uint8_t PlanMismatchShape = 1u;
inline constexpr std::uint8_t PlanMismatchTemplate = 2u;
inline constexpr std::uint8_t PlanMismatchTarget = 4u;
inline constexpr std::uint8_t PlanMismatchView = 8u;
inline constexpr std::uint8_t PlanMismatchDepth = 16u;
// A draw-cache hit recorded from its recipe, under GuestMemory::GpuMutex after the packet's labels
// (design_cpu_final M8, step 8b): the validation with the recipe's stored outputs, the index and
// vertex work as Draw's, the resident-target proof on the stored images (Refresh while Cached()),
// the template's proof (ProveCurrent) with the alias checks repeated, the pipeline and framebuffer
// from the recipe, then the record as Draw's (pass continued or begun, Kept, MarkGpuWrites). A
// miss records nothing.
DrawRecipeOutcome DrawWithRecipe(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots, const DrawRecipe& recipe, const DrawTraceContext* trace = nullptr, DrawRecipeUse use = DrawRecipeUse::Exact);
// Whether recorded draws build recipes and draw-cache hits use them (APS5_NO_DRAW_RECIPE unset).
bool DrawRecipes();
bool DrawPlanShadow();
bool DrawPlans();
bool DrawPlanTemplates();
bool VerifyDrawPlans();
void SetDrawPlansForTest(int enabled);
void SetDrawPlanSetKeySaltForTest(std::uint32_t salt);
struct StageKeyMemoCounts {
    std::uint64_t staticHits = 0;
    std::uint64_t staticFills = 0;
    std::uint64_t staticLosses = 0;
    std::uint64_t patchedSegments = 0;
    std::uint64_t layoutSplitMisses = 0;
    std::uint64_t verified = 0;
    std::uint64_t resultHits = 0;
    std::uint64_t resultWalks = 0;
};
StageKeyMemoCounts StageKeyMemoCounters();
std::shared_ptr<const ShaderRecompiler::ResultKeyShape::Static> StageStaticKeys(const CompiledShader& shader);
std::shared_ptr<const DrawRecipe> TakeDrawPlanShadowDecision();

// How an indirect draw's records were read, for the [draws] line (APS5_PROFILE_DRAW): Gpu is
// vkCmdDrawIndirect from the host import; every other value names why the CPU read them instead,
// either decided by the driver before the draw (its patched-SGPR fold rules, the shader path, the
// draw index, the vertex ranges, the device features, APS5_NO_GPU_INDIRECT_DRAW) or under the device
// lock in Draw (the memory state of the records). `readMs` is the CPU read's time (its sync);
// `rewritten` counts a GPU-side draw whose records were copied and patched with a constant.
enum class IndirectDrawPath : std::uint8_t { Gpu, NotFolded, FetchUnknown, NonVertexPath, IndxOffset, DrawIndex, VertexRange, FeatureGap, PendingImage, PendingLabelOrCopy, NotImported, Disabled, IndexRange, IndexLimitUnknown, Count };
void CountIndirectDraw(IndirectDrawPath path, double readMs, bool rewritten = false);
struct DrawOpsCounts {
    static constexpr std::size_t SizeBuckets = 6;
    std::uint64_t recorded = 0;
    std::uint64_t fallbacks = 0;
    std::uint64_t sets = 0;
    std::uint64_t stateOps = 0;
    std::array<std::uint64_t, SizeBuckets> sizes{};
    std::array<std::uint64_t, DrawStateOp::KindCount> kinds{};
};
void CountDrawOps(bool recorded, std::uint32_t sets = 0, std::span<const DrawStateOp> state = {});
DrawOpsCounts DrawOpsTotals();
std::array<std::uint64_t, static_cast<std::size_t>(IndirectDrawPath::Count)> IndirectDrawCounts();
void CountEmptyIndirectDraw(double readMs);
std::uint64_t EmptyIndirectDrawCount();

struct DrawPassCounts {
    std::uint64_t draws = 0;
    std::uint64_t passesBegun = 0;
    std::uint64_t passesContinued = 0;
    std::uint64_t keyStoreFlushes = 0;
    std::uint64_t storeFlushes = 0;
};
std::array<DrawPassCounts, 2> DrawPassTotals();
std::array<std::uint64_t, PassBreakCount> DrawPassBreaks();
const char* IndirectDrawPathName(IndirectDrawPath path);

// Draw packets that drew nothing, for the [draws] line: Nothing (an empty count, or no color writes
// and no pixel shader), Prechecked (State.hpp's DrawRejection refused it before the decode) and
// Thrown (the decode or the build threw); `us` is the packet's time in the driver.
enum class DrawSkip : std::uint8_t { Nothing, Prechecked, Thrown, Count };
void CountDrawSkip(DrawSkip kind, double us);

// Recorded draws whose written guest buffers were copied (their results reach guest memory by a CPU
// write-back when their batch completes), listed until that write-back ran. It is the draw
// counterpart of VulkanDevice's State::copiedWriters for dispatches, which VulkanDevice.cpp consults
// in two places: DispatchIndirect (an indirect dispatch whose arguments such a writer produced reads
// them on the CPU, behind the write-back, instead of from the host import on the GPU) and the fill
// HLE (a vkCmdFillBuffer over a range a listed write-back will land on must wait for it, or the
// write-back overwrites the fill). Draws are listed here only under APS5_RECORD_COPIED_DRAWS=1,
// which is safe only once BOTH of those checks also test this list, i.e. each extends its
// `std::any_of(state->copiedWriters ...)` with
// `|| std::any_of(Graphics::DrawCopiedWriters()->begin(), Graphics::DrawCopiedWriters()->end(), [&](const auto& writer) { return writer->WritesOverlap(<its range>); })`;
// cleaner still, State::copiedWriters can become this list (one registry for both producers), after
// which the switch can turn into APS5_NO_RECORD_COPIED_DRAWS. Until then such draws are recorded and
// waited for at once. Read and written under GuestMemory::GpuMutex, like the device's list.
std::shared_ptr<std::vector<std::shared_ptr<ShaderResources>>> DrawCopiedWriters();
void ListCopiedWriter(Recorder& recorder, const std::shared_ptr<std::vector<std::shared_ptr<ShaderResources>>>& writers, const std::shared_ptr<ShaderResources>& resources, std::function<void()> completion, bool listed);

void RunColorMetadataPass(const Context& context, const ColorMetadataPass& pass);
std::vector<std::pair<std::uint64_t, std::uint64_t>> PipelinedColorMetadataPassWrites(const ColorMetadataPass& pass);
bool DrawPipelineEnabled();
bool CommitTrims();
enum class AddressDrawReuse : std::uint8_t { Reused, Built, NotReadOnly, NotSpaceOnly, SnapshotOutside, PartialOverlap, SnapshotChanged, MovedBuffer, PatchedData, SpaceChanged, InUse, Revalidation, Count };
using AddressDrawReuseCounts = std::array<std::uint64_t, static_cast<std::size_t>(AddressDrawReuse::Count)>;
AddressDrawReuseCounts AddressDrawReuseCounters();
const char* AddressDrawReuseName(AddressDrawReuse outcome);
std::uint64_t AddressDrawReuseVerifications();
enum class DrawStopForTest { None = 0, ReturnAfterResolve = 1, ThrowAfterResolve = 2 };
void SetSpaceKeepFailuresForTest(std::uint32_t count);
void SetDrawStopAfterResolveForTest(DrawStopForTest stop);
std::uint64_t TargetMemoBuildsForTest();
std::uint64_t TargetMissDrawsForTest();
std::uint64_t CopiedTargetResolutionsForTest();
std::uint64_t TargetMemoHitNanosecondsForTest(const Context& context, const State& state, std::size_t repeats, std::size_t& hits);

}

#endif
