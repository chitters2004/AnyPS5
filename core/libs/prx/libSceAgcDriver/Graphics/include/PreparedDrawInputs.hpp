#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PREPAREDDRAWINPUTS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PREPAREDDRAWINPUTS_HPP

#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

struct DrawVertexFetches {
    std::vector<VertexFetch> fetches;
    std::vector<std::size_t> fetchValid;
    std::vector<std::size_t> fetchOf;
    std::vector<std::optional<std::size_t>> zeroBytes;
    std::vector<std::size_t> soloFetches;
    std::vector<bool> isSolo;
    std::vector<std::size_t> plannedOf;
    VertexCopyPlan plan;
};

DrawVertexFetches PlanDrawVertexFetches(std::optional<std::pair<std::uint64_t, std::uint64_t>> colorTarget, const Pm4::DrawParameters& draw, std::span<const ShaderRecompiler::VertexAttribute> attributes, const VertexInputLayout& layout, std::uint32_t maxIndex);

class PreparedDrawInputs;
class ShaderResources;

std::shared_ptr<PreparedDrawInputs> PrepareDrawInputsAhead(const std::shared_ptr<const Context>& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::uint64_t epoch = 0);
void AwaitPreparedDrawInputs(PreparedDrawInputs& prepared);
const Context& PreparedDrawInputsContext(const PreparedDrawInputs& prepared);
const Context* PreparedDrawResourcesContext(const PreparedDrawInputs& prepared);
std::shared_ptr<PreparedDrawInputs> PrepareDrawResourcesAhead(std::shared_ptr<PreparedDrawInputs> job, const std::shared_ptr<const Context>& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots, std::uint64_t epoch = 0);
void SubmitPreparedDrawResources(const std::shared_ptr<PreparedDrawInputs>& job, std::shared_ptr<const void> pin);
std::vector<std::uint32_t> DrawResourceCacheKey(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders);
std::shared_ptr<ShaderResources> PrepareDrawResourcesOffDevice(const Context& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders, std::span<const GuestMemorySnapshot> snapshots, bool& predictedHit, bool& stagingPredicted);
std::shared_ptr<ShaderResources> AdoptPreparedDrawResources(const Context& context, bool deviceStaging);
void CountPreparedDrawResourcesVerified(bool differed);
std::shared_ptr<Buffer> AdoptPreparedRegion(const Context& context, const Recorder* recorder, std::uint64_t address, std::size_t bytes, VkBufferUsageFlags usage, std::shared_ptr<Buffer>* spare);

class PreparedDrawInputsScope {
public:
    explicit PreparedDrawInputsScope(std::shared_ptr<PreparedDrawInputs> prepared);
    ~PreparedDrawInputsScope();
    PreparedDrawInputsScope(const PreparedDrawInputsScope&) = delete;
    PreparedDrawInputsScope& operator=(const PreparedDrawInputsScope&) = delete;

private:
    std::shared_ptr<PreparedDrawInputs> prepared;
    std::shared_ptr<PreparedDrawInputs> previous;
};

struct DrawInputSlice {
    std::shared_ptr<Buffer> buffer;
    VkDeviceSize offset = 0;
    std::size_t bytes = 0;
    bool ring = false;
    std::span<std::byte> Bytes() const;
};

struct AdoptedDrawInput {
    DrawInputSlice slice;
    std::uint32_t derived = 0;
};

std::size_t ZeroPaddedReadableBytes(std::uint64_t address, std::size_t validBytes, std::size_t stride);
std::optional<AdoptedDrawInput> AdoptPreparedDrawInput(const Context& context, const Recorder* recorder, std::uint64_t address, std::size_t bytes, Recorder::SnapshotUse use, std::uint64_t registryGeneration, std::size_t readable, DrawInputSlice* spare = nullptr);
inline constexpr VkBufferUsageFlags DrawInputUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
VkMemoryPropertyFlags DrawInputMemoryProperties(const Context& context, Recorder::SnapshotUse use);
std::shared_ptr<Buffer> NewDrawInputBuffer(const Context& context, std::size_t bytes, Recorder::SnapshotUse use);
DrawInputSlice NewDrawInputSlice(const Context& context, std::size_t bytes, Recorder::SnapshotUse use);
inline constexpr std::size_t DrawInputRingChunk = std::size_t{1} << 20u;
inline constexpr std::size_t DrawInputRingSliceLimit = std::size_t{64} << 10u;
inline constexpr VkDeviceSize DrawInputRingAlignment = 16;
inline constexpr std::size_t DrawInputRingFreeChunks = 4;
inline constexpr std::size_t DrawInputRingPinnedLimit = std::size_t{128} << 20u;

struct DrawInputRingCounts {
    std::uint64_t slices = 0;
    std::uint64_t bytes = 0;
    std::uint64_t chunks = 0;
    std::uint64_t recycled = 0;
    std::uint64_t returned = 0;
    std::uint64_t capped = 0;
    std::uint64_t oversize = 0;
    std::uint64_t outstanding = 0;
};

DrawInputRingCounts DrawInputRingCounters();

enum class PreparedRefusal : std::uint8_t { Size, Inaccessible, Forget, Pending, Unwatched, SnapshotChanged, Failed, Count };
inline constexpr std::size_t PreparedRefusalCount = static_cast<std::size_t>(PreparedRefusal::Count);

enum class DrawInputSource : std::uint8_t { OwnedReused, SnapshotReused, Adopted, Copied, Count };
inline constexpr std::size_t DrawInputSourceCount = static_cast<std::size_t>(DrawInputSource::Count);
bool DrawInputSourcesTimed();
void CountDrawInputSource(DrawInputSource source, std::size_t bytes, std::uint64_t nanoseconds);

struct PreparedDrawInputCounts {
    std::uint64_t requests = 0;
    std::uint64_t ranges = 0;
    std::uint64_t prepared = 0;
    std::uint64_t refused = 0;
    std::array<std::uint64_t, PreparedRefusalCount> refusedBy{};
    std::uint64_t adopted = 0;
    std::uint64_t reprepared = 0;
    std::uint64_t notReady = 0;
    std::uint64_t waited = 0;
    std::uint64_t unused = 0;
    std::uint64_t checks = 0;
    std::uint64_t checkNanoseconds = 0;
    std::uint64_t verifyDiffered = 0;
    std::uint64_t requestNanoseconds = 0;
    std::uint64_t epochFollows = 0;
    std::uint64_t collectNanoseconds = 0;
    std::array<std::uint64_t, 2> allocNanoseconds{};
    std::array<std::uint64_t, 2> poolWaitNanoseconds{};
    std::array<std::uint64_t, 2> stashHits{};
    std::uint64_t stashReturned = 0;
    std::uint64_t spareReused = 0;
    std::array<std::uint64_t, DrawInputSourceCount> sources{};
    std::array<std::uint64_t, DrawInputSourceCount> sourceBytes{};
    std::array<std::uint64_t, DrawInputSourceCount> sourceNanoseconds{};
};

PreparedDrawInputCounts PreparedDrawInputCounters();

struct PreparedUploadCounts {
    std::uint64_t requests = 0;
    std::uint64_t notReady = 0;
    std::uint64_t waited = 0;
    std::uint64_t stageAPrepared = 0;
    std::uint64_t stageAAdopted = 0;
    std::uint64_t stageAStale = 0;
    std::uint64_t stageAStaleStaging = 0;
    std::array<std::uint64_t, PreparedStalenessCount> stageAStaleBy{};
    std::uint64_t stageAStagingPredicted = 0;
    std::uint64_t stageAStagingFallback = 0;
    std::uint64_t stageAStagingMispredicted = 0;
    std::uint64_t stageAUnused = 0;
    std::uint64_t stageAPredictedHit = 0;
    std::uint64_t stageARefused = 0;
    std::uint64_t stageAFailed = 0;
    std::uint64_t stageAUnpinned = 0;
    std::uint64_t stageAVerifyDiffered = 0;
    std::uint64_t regionsPrepared = 0;
    std::uint64_t regionsAdopted = 0;
    std::uint64_t regionsReprepared = 0;
    std::uint64_t regionsUnused = 0;
    std::uint64_t regionsRefused = 0;
    std::array<std::uint64_t, PreparedRefusalCount> regionsRefusedBy{};
    std::uint64_t regionSpareReused = 0;
    std::uint64_t regionVerifyDiffered = 0;
};

PreparedUploadCounts PreparedUploadCounters();
void SetDrawInputPreparers(unsigned count);
void SetPreparedDrawInputsHook(void (*hook)(PreparedDrawInputs& prepared));
void HoldDrawInputPreparers(bool held);
void ShutdownDrawInputPreparers();
void ClearDrawInputStash(VkDevice device);

}

#endif
