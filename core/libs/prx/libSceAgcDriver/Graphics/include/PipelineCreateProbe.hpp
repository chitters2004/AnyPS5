#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINECREATEPROBE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINECREATEPROBE_HPP

#include <chrono>

#include <vulkan/vulkan.h>

#include "prx/libSceAgcDriver/Graphics/include/PipelineCreates.hpp"

namespace AgcDriver::Graphics {

class PipelineCreateProbe {
public:
    PipelineCreateProbe(bool feedback, const void*& next);
    PipelineCreateProbe(const PipelineCreateProbe&) = delete;
    PipelineCreateProbe& operator=(const PipelineCreateProbe&) = delete;
    ~PipelineCreateProbe();

    void Start();
    void Finish(PipelineCreateKind kind, MonolithicReason reason = MonolithicReason::Count);

private:
    const void** slot = nullptr;
    const void* original = nullptr;
    bool active = false;
    bool chained = false;
    VkPipelineCreationFeedback feedback{};
    VkPipelineCreationFeedbackCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO};
    std::chrono::steady_clock::time_point started{};
};

PipelineCacheOutcome CacheOutcomeOf(const VkPipelineCreationFeedback& feedback, bool chained);
void EnterCommittedDraw();
void LeaveCommittedDraw();
bool InCommittedDraw();

}

#endif
