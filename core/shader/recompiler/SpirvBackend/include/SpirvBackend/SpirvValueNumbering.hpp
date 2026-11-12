#ifndef CORE_SHADER_RECOMPILER_SPIRVBACKEND_SPIRVVALUENUMBERING_HPP
#define CORE_SHADER_RECOMPILER_SPIRVBACKEND_SPIRVVALUENUMBERING_HPP

#include <cstdint>
#include <span>
#include <vector>

namespace ShaderRecompiler {

[[nodiscard]] std::vector<std::uint32_t> NumberSpirvValues(std::span<const std::uint32_t> words);

}

#endif
