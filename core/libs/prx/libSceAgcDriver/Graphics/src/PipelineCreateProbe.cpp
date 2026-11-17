#include "prx/libSceAgcDriver/Graphics/include/PipelineCreateProbe.hpp"

#include "prx/libc/include/HostThreadSlot.hpp"

namespace AgcDriver::Graphics {

namespace {
struct CommittedDrawSlot {};
using CommittedDraw = HostThreadSlot<bool, CommittedDrawSlot>;
}

void EnterCommittedDraw() {
    CommittedDraw::Set(true);
}

void LeaveCommittedDraw() {
    CommittedDraw::Set(false);
}

bool InCommittedDraw() {
    return CommittedDraw::Get();
}

PipelineCacheOutcome CacheOutcomeOf(const VkPipelineCreationFeedback& feedback, bool chained) {
    if (!chained || (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) == 0) return PipelineCacheOutcome::Unknown;
    return (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0 ? PipelineCacheOutcome::Hit : PipelineCacheOutcome::Miss;
}

PipelineCreateProbe::PipelineCreateProbe(bool feedbackSupported, const void*& next) : active(PipelineCreatesProfiled()) {
    if (!active || !feedbackSupported) return;
    info.pNext = next;
    info.pPipelineCreationFeedback = &feedback;
    slot = &next;
    original = next;
    next = &info;
    chained = true;
}

PipelineCreateProbe::~PipelineCreateProbe() {
    if (slot != nullptr) *slot = original;
}

void PipelineCreateProbe::Start() {
    if (!active) return;
    feedback = {};
    started = std::chrono::steady_clock::now();
}

void PipelineCreateProbe::Finish(PipelineCreateKind kind, MonolithicReason reason) {
    if (!active) return;
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    DevicePipelineCreates().Note(kind, InCommittedDraw(), CacheOutcomeOf(feedback, chained), ms, reason);
}

}
