#ifndef CORE_SHADER_RECOMPILER_COMPILEDVARIANT_HPP
#define CORE_SHADER_RECOMPILER_COMPILEDVARIANT_HPP

#include "Recompiler.hpp"
#include "IntermediateRepresentation/IrMetadata/CompiledShaderInfo.hpp"
#include "Optimization/include/Optimization/BindingAllocator.hpp"
#include "Optimization/include/Optimization/DescriptorBindingBuilder.hpp"
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace ShaderRecompiler {

class BufferSubstituteTable {
public:
    explicit BufferSubstituteTable(std::vector<BufferSubstitute> slots) : slots(std::make_shared<const std::vector<BufferSubstitute>>(std::move(slots))) {}

    [[nodiscard]] std::shared_ptr<const std::vector<BufferSubstitute>> Slots() const {
        std::lock_guard lock(mutex);
        return slots;
    }

    void Learn(std::size_t slot, const BufferSubstitute& value) {
        std::lock_guard lock(mutex);
        if (slot >= slots->size() || (*slots)[slot].known) return;
        auto learned = std::make_shared<std::vector<BufferSubstitute>>(*slots);
        (*learned)[slot] = value;
        (*learned)[slot].known = true;
        slots = std::move(learned);
    }

private:
    mutable std::mutex mutex;
    std::shared_ptr<const std::vector<BufferSubstitute>> slots;
};

struct CompiledVariant {
    BindingLayout layout;
    CompiledShaderInfo info;
    CompiledBindingLayout bindings;
    CompiledShaderArtifact artifact;
    std::shared_ptr<BufferSubstituteTable> substitutes;
};

}

#endif
