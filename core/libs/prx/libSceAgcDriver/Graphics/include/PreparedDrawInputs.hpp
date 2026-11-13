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

std::shared_ptr<PreparedDrawInputs> PrepareDrawInputsAhead(const std::shared_ptr<const Context>& context, const State& state, const Pm4::DrawParameters& draw, std::span<const CompiledShader> shaders);
void AwaitPreparedDrawInputs(PreparedDrawInputs& prepared);
const Context& PreparedDrawInputsContext(const PreparedDrawInputs& prepared);

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

struct AdoptedDrawInput {
    std::shared_ptr<Buffer> buffer;
    std::uint32_t derived = 0;
};

std::size_t ZeroPaddedReadableBytes(std::uint64_t address, std::size_t validBytes, std::size_t stride);
std::optional<AdoptedDrawInput> AdoptPreparedDrawInput(const Context& context, const Recorder* recorder, std::uint64_t address, std::size_t bytes, Recorder::SnapshotUse use, std::uint64_t registryGeneration, std::size_t readable);

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
    std::uint64_t abandoned = 0;
    std::uint64_t checks = 0;
    std::uint64_t checkNanoseconds = 0;
    std::uint64_t verifyDiffered = 0;
    std::uint64_t requestNanoseconds = 0;
    std::uint64_t prepareNanoseconds = 0;
    std::uint64_t waitNanoseconds = 0;
    std::array<std::uint64_t, DrawInputSourceCount> sources{};
    std::array<std::uint64_t, DrawInputSourceCount> sourceBytes{};
    std::array<std::uint64_t, DrawInputSourceCount> sourceNanoseconds{};
};

PreparedDrawInputCounts PreparedDrawInputCounters();
void SetDrawInputPreparers(unsigned count);
void SetPreparedDrawInputsHook(void (*hook)(PreparedDrawInputs& prepared));
void SetDrawInputPreparerHook(void (*hook)());
void HoldDrawInputPreparers(bool held);
void QuiesceDrawInputPreparers(VkDevice device);
void ShutdownDrawInputPreparers();

}

#endif
