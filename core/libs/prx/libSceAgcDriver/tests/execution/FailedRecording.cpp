#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "VulkanTestDevice.hpp"
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

using AgcDriver::Graphics::Context;
using AgcDriver::Graphics::Recorder;

constexpr VkDeviceSize BufferBytes = 256;
constexpr std::uint32_t Pattern = 0x5a5a5a5au;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("failed recording: " + message);
}

void RecordFillBuffer(const Context& context, VkCommandBuffer commands, VkBuffer buffer) {
    context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(commands, buffer, 0, BufferBytes, Pattern);
}

class HostBuffer {
public:
    explicit HostBuffer(const Context& context) : context(context) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = BufferBytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Require(context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &buffer) == VK_SUCCESS, "vkCreateBuffer");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, buffer, &requirements);
        constexpr VkMemoryPropertyFlags hostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        std::uint32_t type = context.memory.memoryTypeCount;
        for (std::uint32_t candidate = 0; candidate < context.memory.memoryTypeCount; ++candidate) {
            if ((requirements.memoryTypeBits & (1u << candidate)) != 0u && (context.memory.memoryTypes[candidate].propertyFlags & hostVisible) == hostVisible) {
                type = candidate;
                break;
            }
        }
        Require(type != context.memory.memoryTypeCount, "no host-visible coherent memory type");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        Require(context.Function<PFN_vkAllocateMemory>("vkAllocateMemory")(context.device, &allocation, nullptr, &memory) == VK_SUCCESS, "vkAllocateMemory");
        Require(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, buffer, memory, 0) == VK_SUCCESS, "vkBindBufferMemory");
        Require(context.Function<PFN_vkMapMemory>("vkMapMemory")(context.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS, "vkMapMemory");
        *static_cast<std::uint32_t*>(mapped) = 0;
    }

    ~HostBuffer() {
        context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
        context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
    }

    HostBuffer(const HostBuffer&) = delete;
    HostBuffer& operator=(const HostBuffer&) = delete;

    VkBuffer Handle() const { return buffer; }
    void RecordFill(VkCommandBuffer commands) const { RecordFillBuffer(context, commands, buffer); }

    std::uint32_t First() const { return *static_cast<const std::uint32_t*>(mapped); }

private:
    Context context;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
};

void SubmitOlderBatch(const Context& context, Recorder& recorder) {
    auto older = std::make_shared<HostBuffer>(context);
    older->RecordFill(recorder.Commands());
    recorder.Keep(older);
    recorder.Submit();
    Require(recorder.InFlightBatches() != 0, "the older batch is not in flight");
}

void RequireLaterWork(const Context& context, Recorder& recorder, const char* after) {
    const auto submitted = recorder.Submissions();
    auto later = std::make_shared<HostBuffer>(context);
    later->RecordFill(recorder.Commands());
    recorder.Keep(later);
    recorder.Sync();
    Require(recorder.Submissions() == submitted + 1 && later->First() == Pattern, std::string("work recorded after ") + after + " did not run");
}

void FailedPassCallback(const Context& context, Recorder& recorder) {
    SubmitOlderBatch(context, recorder);
    const auto submitted = recorder.Submissions();
    auto owned = std::make_unique<HostBuffer>(context);
    const auto start = recorder.StartDrawPass(1, false, false, {});
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {16, 16};
    rendering.layerCount = 1;
    context.Function<PFN_vkCmdBeginRenderingKHR>("vkCmdBeginRenderingKHR")(start.commands, &rendering);
    recorder.LeaveRenderPassOpen(1, Recorder::NoTiming, false, {}, [context, named = owned->Handle()](AgcDriver::Graphics::CommandState& pass) {
        RecordFillBuffer(context, pass.Commands(), named);
        throw std::bad_alloc();
    });
    bool thrown = false;
    try {
        recorder.Sync();
    } catch (const std::bad_alloc&) {
        thrown = true;
    }
    Require(thrown, "the pass callback's failure did not reach the syncing caller");
    Require(recorder.InFlightBatches() == 0, "the sync that failed in a pass callback left the older batch unfinished");
    owned.reset();
    recorder.Sync();
    Require(recorder.Submissions() == submitted, "the batch whose pass callback failed was submitted after the object it named was destroyed");
    RequireLaterWork(context, recorder, "a failed pass callback");
    std::puts("a pass callback that fails before keeping what it recorded discards its batch, after the older batch finished");
}

void FailedDriver(const Context& context, Recorder& recorder) {
    SubmitOlderBatch(context, recorder);
    const auto submitted = recorder.Submissions();
    auto unfinished = std::make_shared<HostBuffer>(context);
    unfinished->RecordFill(recorder.Commands());
    recorder.Keep(unfinished);
    AgcDriverReportFailure_nid_postfix(std::make_exception_ptr(std::runtime_error("injected driver failure")));
    std::string reason = "nothing";
    try {
        recorder.Sync();
    } catch (const std::exception& error) {
        reason = error.what();
    }
    Require(reason == "injected driver failure", "a sync after the driver failed threw '" + reason + "' instead of the failure");
    Require(recorder.Submissions() == submitted, "the open batch was submitted after the driver failed");
    Require(recorder.InFlightBatches() == 0, "the sync after the driver failed left the older batch unfinished");
    Require(unfinished->First() == 0, "the discarded batch ran");
    bool refused = false;
    try {
        static_cast<void>(recorder.Commands());
    } catch (const std::runtime_error&) {
        refused = true;
    }
    Require(refused, "a batch was opened after the driver failed");
    std::puts("after a driver failure the open batch is discarded, the older batch finishes and no batch opens");
}

}

int main() {
    bool failures = false;
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(*device);
        {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            auto* recorder = Recorder::Active();
            Require(recorder != nullptr, "the device has no active recorder");
            if (context.graphicsPipelineLibrary) {
                try {
                    FailedPassCallback(context, *recorder);
                } catch (const std::exception& error) {
                    std::cerr << error.what() << '\n';
                    failures = true;
                }
            } else {
                std::puts("a failing pass callback is not checked: the device records render passes, not dynamic rendering");
            }
            FailedDriver(context, *recorder);
        }
        try {
            AgcDriverShutdown_nid_postfix();
        } catch (...) {
        }
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
