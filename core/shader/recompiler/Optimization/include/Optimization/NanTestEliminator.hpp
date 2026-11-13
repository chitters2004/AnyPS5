#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_NANTESTELIMINATOR_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_NANTESTELIMINATOR_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include <cstdint>

namespace ShaderRecompiler {

struct NanTestEliminationStats {
    std::uint32_t tests = 0;
    std::uint32_t removedTests = 0;
    std::uint64_t nanoseconds = 0;
};

class NanTestEliminator {
public:
    [[nodiscard]] NanTestEliminationStats Eliminate(IrProgram& program) const;
};

[[nodiscard]] bool NanProofDisabled();
void RecordNanTests(std::uint64_t codeAddress, const NanTestEliminationStats& stats);
[[nodiscard]] NanTestEliminationStats NanTestTotals();

}

#endif
