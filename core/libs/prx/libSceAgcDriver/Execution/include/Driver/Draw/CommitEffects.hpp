#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_COMMITEFFECTS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_COMMITEFFECTS_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"

#include <cstdint>

namespace AgcDriver::DriverDetail {

enum class CommitEffect : std::uint8_t { CompleteWrites, BoundedWrites, Pending, Lending, CacheReplacement, ShadowPublication, Unknown, Count };

constexpr std::uint32_t CommitEffectBit(CommitEffect effect) {
    return std::uint32_t{1} << static_cast<unsigned>(effect);
}

constexpr std::uint32_t DeliveredCommitEffects = CommitEffectBit(CommitEffect::CompleteWrites) | CommitEffectBit(CommitEffect::Pending) | CommitEffectBit(CommitEffect::Lending) | CommitEffectBit(CommitEffect::ShadowPublication);

constexpr std::uint32_t CommitEffects(DrawPipeline::CommitKind kind) {
    using Kind = DrawPipeline::CommitKind;
    constexpr auto unknown = CommitEffectBit(CommitEffect::Unknown);
    switch (kind) {
    case Kind::Other: return unknown;
    case Kind::Draw: return CommitEffectBit(CommitEffect::BoundedWrites) | unknown;
    case Kind::IndirectDraw: return CommitEffectBit(CommitEffect::BoundedWrites) | unknown;
    case Kind::ColorMetadata: return unknown;
    case Kind::Dispatch: return unknown;
    case Kind::SampleDump: return CommitEffectBit(CommitEffect::CompleteWrites) | unknown;
    case Kind::DmaFill: return CommitEffectBit(CommitEffect::CompleteWrites) | unknown;
    case Kind::DmaWrite: return CommitEffectBit(CommitEffect::CompleteWrites) | CommitEffectBit(CommitEffect::CacheReplacement) | unknown;
    case Kind::DmaCopy: return CommitEffectBit(CommitEffect::BoundedWrites) | unknown;
    case Kind::Store: return CommitEffectBit(CommitEffect::CompleteWrites) | unknown;
    case Kind::EndOfPipe: return CommitEffectBit(CommitEffect::CompleteWrites) | unknown;
    case Kind::Seal: return unknown;
    case Kind::SubmitEnd: return unknown;
    case Kind::Completion: return unknown;
    case Kind::BufferFill: return CommitEffectBit(CommitEffect::CompleteWrites) | unknown;
    case Kind::Count: break;
    }
    return 0;
}

constexpr bool CommitEffectsCovered(std::uint32_t effects) {
    return effects != 0 && (effects & ~DeliveredCommitEffects) == 0;
}

constexpr bool CommitCovered(DrawPipeline::CommitKind kind) {
    return CommitEffectsCovered(CommitEffects(kind));
}

}

#endif
