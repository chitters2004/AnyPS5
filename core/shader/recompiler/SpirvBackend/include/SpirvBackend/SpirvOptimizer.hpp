#ifndef CORE_SHADER_RECOMPILER_SPIRVBACKEND_SPIRVOPTIMIZER_HPP
#define CORE_SHADER_RECOMPILER_SPIRVBACKEND_SPIRVOPTIMIZER_HPP

#include <cstdint>
#include <span>
#include <vector>

namespace ShaderRecompiler {

enum class SpirvOutputValidation {
    Validate,
    Caller
};

[[nodiscard]] std::vector<std::uint32_t> ValidateAndOptimizeSpirv(std::span<const std::uint32_t> spirv, std::uint32_t vulkanVersion, std::uint32_t spirvVersion, bool allowOffsetTextureOperand = false, bool optimize = true, bool specialize = false, SpirvOutputValidation output = SpirvOutputValidation::Validate);
void ValidateSpirv(std::span<const std::uint32_t> spirv, std::uint32_t vulkanVersion, std::uint32_t spirvVersion, bool allowOffsetTextureOperand);
[[nodiscard]] std::uint64_t OptimizerHostThreads();

}

#endif
