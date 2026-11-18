#ifndef CORE_SHADER_RECOMPILER_COMPILEDVARIANT_HPP
#define CORE_SHADER_RECOMPILER_COMPILEDVARIANT_HPP

#include "Recompiler.hpp"
#include "IntermediateRepresentation/IrMetadata/CompiledShaderInfo.hpp"
#include "Optimization/include/Optimization/BindingAllocator.hpp"
#include "Optimization/include/Optimization/DescriptorBindingBuilder.hpp"
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace ShaderRecompiler {

class BufferSubstituteTable {
public:
    using Snapshot = std::shared_ptr<const std::vector<BufferSubstitute>>;

    explicit BufferSubstituteTable(std::vector<BufferSubstitute> slots) : current(std::make_shared<const std::vector<BufferSubstitute>>(std::move(slots))) {}

    [[nodiscard]] Snapshot Slots() const {
        std::lock_guard lock(mutex);
        return current;
    }

    [[nodiscard]] static std::span<const BufferSubstitute> View(const Snapshot& snapshot) noexcept {
        return snapshot != nullptr ? std::span<const BufferSubstitute>(*snapshot) : std::span<const BufferSubstitute>{};
    }

    void Learn(std::size_t slot, const BufferSubstitute& value) {
        std::lock_guard lock(mutex);
        if (slot >= current->size() || (*current)[slot].known) return;
        auto learned = std::make_shared<std::vector<BufferSubstitute>>(*current);
        (*learned)[slot] = value;
        (*learned)[slot].known = true;
        current = std::move(learned);
    }

private:
    mutable std::mutex mutex;
    Snapshot current;
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
