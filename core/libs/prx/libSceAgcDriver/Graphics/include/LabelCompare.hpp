#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LABELCOMPARE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LABELCOMPARE_HPP

#include <cstdint>

namespace AgcDriver::Graphics::LabelCompare {

enum class BlockMode : std::uint8_t { Store, Keep, OnGpu, Fallback };

struct BlockState {
    bool kept = false;
    bool unknown = false;
    bool stampedNow = false;
    bool changedNow = false;
    bool writerOver = false;
    bool leaseOver = false;
};

inline BlockMode Classify(const BlockState& block) {
    if (block.unknown) return BlockMode::Store;
    if (block.kept) return BlockMode::Keep;
    if (!block.writerOver && !block.leaseOver) return block.stampedNow && block.changedNow ? BlockMode::Keep : BlockMode::Store;
    if (block.leaseOver && !block.stampedNow && (block.changedNow || block.writerOver)) return BlockMode::Fallback;
    if (block.stampedNow || block.leaseOver) return BlockMode::OnGpu;
    return BlockMode::Store;
}

inline BlockMode ClassifyCompared(bool kept, bool changedNow, bool pending) {
    if (kept) return BlockMode::Keep;
    if (!pending) return changedNow ? BlockMode::Keep : BlockMode::Store;
    return BlockMode::OnGpu;
}

inline bool Written(BlockMode mode, bool changedOnGpu) {
    return mode == BlockMode::Keep || (mode == BlockMode::OnGpu && changedOnGpu);
}

inline const char* ModeName(BlockMode mode) {
    switch (mode) {
        case BlockMode::Store: return "store";
        case BlockMode::Keep: return "keep";
        case BlockMode::OnGpu: return "gpu";
        case BlockMode::Fallback: return "fallback";
    }
    return "?";
}

}

#endif
