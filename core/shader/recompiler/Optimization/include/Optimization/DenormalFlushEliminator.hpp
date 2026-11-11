#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_DENORMALFLUSHELIMINATOR_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_DENORMALFLUSHELIMINATOR_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ShaderRecompiler {

struct DenormalFlushEliminationStats {
    std::uint32_t removedFlushes = 0;
    std::uint32_t denormalFreeMinMax = 0;
};

class DenormalFlushEliminator {
public:
    [[nodiscard]] DenormalFlushEliminationStats Eliminate(IrProgram& program, bool hostFlushesArithmetic) const;
    std::uint32_t MarkSampledFlushImages(IrProgram& program, bool hostFlushesArithmetic) const;
};

class SampledFlushImages {
public:
    SampledFlushImages(const IrProgram& program, bool hostFlushesArithmetic);
    ~SampledFlushImages();
    SampledFlushImages(const SampledFlushImages&) = delete;
    SampledFlushImages& operator=(const SampledFlushImages&) = delete;

    [[nodiscard]] std::optional<std::vector<std::uint32_t>> Of(const IrValue& flush);
    [[nodiscard]] static const IrValue* FlushSource(const IrValue& flush);

private:
    class State;
    std::unique_ptr<State> state;
};

}

#endif
