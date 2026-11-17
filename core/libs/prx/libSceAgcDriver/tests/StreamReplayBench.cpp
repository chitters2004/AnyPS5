#include "prx/libSceAgcDriver/Graphics/include/CommandList.hpp"
#include "prx/libSceAgcDriver/Graphics/include/CommandStream.hpp"
#include <SDL_loadso.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using AgcDriver::Graphics::CommandFunctions;
using AgcDriver::Graphics::CommandStream;

constexpr std::uint32_t VertexWords[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x0000001c, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000d, 0x00000011, 0x00030003,
    0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00060005, 0x0000000b,
    0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006, 0x0000000b, 0x00000000, 0x505f6c67,
    0x7469736f, 0x006e6f69, 0x00070006, 0x0000000b, 0x00000001, 0x505f6c67, 0x746e696f, 0x657a6953,
    0x00000000, 0x00070006, 0x0000000b, 0x00000002, 0x435f6c67, 0x4470696c, 0x61747369, 0x0065636e,
    0x00070006, 0x0000000b, 0x00000003, 0x435f6c67, 0x446c6c75, 0x61747369, 0x0065636e, 0x00030005,
    0x0000000d, 0x00000000, 0x00050005, 0x00000011, 0x69736f70, 0x6e6f6974, 0x00000000, 0x00040005,
    0x00000013, 0x68737550, 0x00000000, 0x00050006, 0x00000013, 0x00000000, 0x7366666f, 0x00007465,
    0x00040005, 0x00000015, 0x68737570, 0x00000000, 0x00030047, 0x0000000b, 0x00000002, 0x00050048,
    0x0000000b, 0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x0000000b, 0x00000001, 0x0000000b,
    0x00000001, 0x00050048, 0x0000000b, 0x00000002, 0x0000000b, 0x00000003, 0x00050048, 0x0000000b,
    0x00000003, 0x0000000b, 0x00000004, 0x00040047, 0x00000011, 0x0000001e, 0x00000000, 0x00030047,
    0x00000013, 0x00000002, 0x00050048, 0x00000013, 0x00000000, 0x00000023, 0x00000000, 0x00020013,
    0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017,
    0x00000007, 0x00000006, 0x00000004, 0x00040015, 0x00000008, 0x00000020, 0x00000000, 0x0004002b,
    0x00000008, 0x00000009, 0x00000001, 0x0004001c, 0x0000000a, 0x00000006, 0x00000009, 0x0006001e,
    0x0000000b, 0x00000007, 0x00000006, 0x0000000a, 0x0000000a, 0x00040020, 0x0000000c, 0x00000003,
    0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000003, 0x00040015, 0x0000000e, 0x00000020,
    0x00000001, 0x0004002b, 0x0000000e, 0x0000000f, 0x00000000, 0x00040020, 0x00000010, 0x00000001,
    0x00000007, 0x0004003b, 0x00000010, 0x00000011, 0x00000001, 0x0003001e, 0x00000013, 0x00000007,
    0x00040020, 0x00000014, 0x00000009, 0x00000013, 0x0004003b, 0x00000014, 0x00000015, 0x00000009,
    0x00040020, 0x00000016, 0x00000009, 0x00000007, 0x00040020, 0x0000001a, 0x00000003, 0x00000007,
    0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d,
    0x00000007, 0x00000012, 0x00000011, 0x00050041, 0x00000016, 0x00000017, 0x00000015, 0x0000000f,
    0x0004003d, 0x00000007, 0x00000018, 0x00000017, 0x00050081, 0x00000007, 0x00000019, 0x00000012,
    0x00000018, 0x00050041, 0x0000001a, 0x0000001b, 0x0000000d, 0x0000000f, 0x0003003e, 0x0000001b,
    0x00000019, 0x000100fd, 0x00010038,
};
constexpr std::uint32_t FragmentWords[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x0000000c, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00030010, 0x00000004,
    0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000,
    0x00040005, 0x00000009, 0x6f6c6f63, 0x00000072, 0x00040047, 0x00000009, 0x0000001e, 0x00000000,
    0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020,
    0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007,
    0x0004003b, 0x00000008, 0x00000009, 0x00000003, 0x0004002b, 0x00000006, 0x0000000a, 0x3f800000,
    0x0007002c, 0x00000007, 0x0000000b, 0x0000000a, 0x0000000a, 0x0000000a, 0x0000000a, 0x00050036,
    0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0003003e, 0x00000009,
    0x0000000b, 0x000100fd, 0x00010038,
};

constexpr std::uint32_t DrawsPerPass = 40;
constexpr std::uint32_t Extent = 64;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("stream replay bench: " + message);
}

void Check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string("stream replay bench: ") + what + " returned " + std::to_string(static_cast<int>(result)));
}

std::uint32_t EnvCount(const char* name, std::uint32_t fallback) {
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') return fallback;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (*end != '\0' || value == 0 || value > 1'000'000) throw std::runtime_error(std::string("stream replay bench: ") + name + "=" + text + " is not a count from 1 to 1000000");
    return static_cast<std::uint32_t>(value);
}

template<typename Pfn>
Pfn Thunked(const char* name, Pfn real) {
    Require(real != nullptr, std::string(name) + " is missing");
    return reinterpret_cast<Pfn>(AgcDriver::Graphics::CommandThunk(name, reinterpret_cast<PFN_vkVoidFunction>(real)));
}

struct Thunks {
    explicit Thunks(const CommandFunctions& real)
        : bindPipeline(Thunked("vkCmdBindPipeline", real.CmdBindPipeline)),
          bindDescriptorSets(Thunked("vkCmdBindDescriptorSets", real.CmdBindDescriptorSets)),
          pushConstants(Thunked("vkCmdPushConstants", real.CmdPushConstants)),
          bindVertexBuffers(Thunked("vkCmdBindVertexBuffers", real.CmdBindVertexBuffers)),
          bindIndexBuffer(Thunked("vkCmdBindIndexBuffer", real.CmdBindIndexBuffer)),
          setViewport(Thunked("vkCmdSetViewport", real.CmdSetViewport)),
          setScissor(Thunked("vkCmdSetScissor", real.CmdSetScissor)),
          setDepthBias(Thunked("vkCmdSetDepthBias", real.CmdSetDepthBias)),
          setStencilReference(Thunked("vkCmdSetStencilReference", real.CmdSetStencilReference)),
          setStencilCompareMask(Thunked("vkCmdSetStencilCompareMask", real.CmdSetStencilCompareMask)),
          drawIndexed(Thunked("vkCmdDrawIndexed", real.CmdDrawIndexed)),
          beginRenderPass(Thunked("vkCmdBeginRenderPass", real.CmdBeginRenderPass)),
          endRenderPass(Thunked("vkCmdEndRenderPass", real.CmdEndRenderPass)) {}
    PFN_vkCmdBindPipeline bindPipeline;
    PFN_vkCmdBindDescriptorSets bindDescriptorSets;
    PFN_vkCmdPushConstants pushConstants;
    PFN_vkCmdBindVertexBuffers bindVertexBuffers;
    PFN_vkCmdBindIndexBuffer bindIndexBuffer;
    PFN_vkCmdSetViewport setViewport;
    PFN_vkCmdSetScissor setScissor;
    PFN_vkCmdSetDepthBias setDepthBias;
    PFN_vkCmdSetStencilReference setStencilReference;
    PFN_vkCmdSetStencilCompareMask setStencilCompareMask;
    PFN_vkCmdDrawIndexed drawIndexed;
    PFN_vkCmdBeginRenderPass beginRenderPass;
    PFN_vkCmdEndRenderPass endRenderPass;
};

class Device {
public:
    Device() {
#ifdef _WIN32
        library = SDL_LoadObject("vulkan-1.dll");
#elif defined(__APPLE__)
        library = SDL_LoadObject("libvulkan.1.dylib");
#else
        library = SDL_LoadObject("libvulkan.so.1");
#endif
        Require(library != nullptr, "cannot load Vulkan");
        try {
            create();
        } catch (...) {
            release();
            throw;
        }
    }
    ~Device() { release(); }
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    const CommandFunctions& Real() const { return real; }
    const char* Name() const { return properties.deviceName; }
    VkCommandBuffer Buffer() const { return commands; }
    VkPipeline Pipeline(std::uint32_t index) const { return pipelines[index]; }
    VkPipelineLayout Layout() const { return layout; }
    const VkDescriptorSet* Sets(std::uint32_t index) const { return &sets[index * 2]; }
    VkBuffer Vertices() const { return vertices; }
    VkRenderPass Pass() const { return pass; }
    VkFramebuffer Framebuffer() const { return framebuffer; }

    void Begin() const {
        VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(beginBuffer(commands, &info), "vkBeginCommandBuffer");
    }
    void End() const { Check(endBuffer(commands), "vkEndCommandBuffer"); }
    void Reset() const { Check(resetPool(device, commandPool, 0), "vkResetCommandPool"); }
    void SubmitAndWait() const {
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        Check(device_<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
        Check(device_<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(queue), "vkQueueWaitIdle");
    }

private:
    template<typename T>
    T instance_(const char* name) const {
        const auto result = reinterpret_cast<T>(instanceProc(instance, name));
        Require(result != nullptr, std::string(name) + " is missing");
        return result;
    }
    template<typename T>
    T device_(const char* name) const {
        const auto result = reinterpret_cast<T>(deviceProc(device, name));
        Require(result != nullptr, std::string(name) + " is missing");
        return result;
    }

    void create() {
        instanceProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_LoadFunction(library, "vkGetInstanceProcAddr"));
        Require(instanceProc != nullptr, "missing vkGetInstanceProcAddr");
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &application;
#ifdef __APPLE__
        const char* portability = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
        instanceInfo.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        instanceInfo.enabledExtensionCount = 1;
        instanceInfo.ppEnabledExtensionNames = &portability;
#endif
        Check(instance_<PFN_vkCreateInstance>("vkCreateInstance")(&instanceInfo, nullptr, &instance), "vkCreateInstance");
        choosePhysical();
        std::uint32_t count = 0;
        const auto queues = instance_<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
        queues(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        queues(physical, &count, families.data());
        std::uint32_t family = 0;
        while (family < count && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) ++family;
        Require(family < count, "no graphics queue");
        const float priority = 1;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        const char* subset = "VK_KHR_portability_subset";
        if (hasExtension(subset)) {
            deviceInfo.enabledExtensionCount = 1;
            deviceInfo.ppEnabledExtensionNames = &subset;
        }
        Check(instance_<PFN_vkCreateDevice>("vkCreateDevice")(physical, &deviceInfo, nullptr, &device), "vkCreateDevice");
        deviceProc = instance_<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        AgcDriver::Graphics::ResolveCommandFunctions(real, device, deviceProc);
        device_<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(device, family, 0, &queue);
        beginBuffer = device_<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer");
        endBuffer = device_<PFN_vkEndCommandBuffer>("vkEndCommandBuffer");
        resetPool = device_<PFN_vkResetCommandPool>("vkResetCommandPool");
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = family;
        Check(device_<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device, &poolInfo, nullptr, &commandPool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = commandPool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        Check(device_<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device, &allocate, &commands), "vkAllocateCommandBuffers");
        createTarget();
        createBuffers();
        createDescriptors();
        createPipelines();
    }

    bool hasExtension(const char* name) const {
        const auto enumerate = instance_<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
        std::uint32_t count = 0;
        Check(enumerate(physical, nullptr, &count, nullptr), "vkEnumerateDeviceExtensionProperties");
        std::vector<VkExtensionProperties> available(count);
        Check(enumerate(physical, nullptr, &count, available.data()), "vkEnumerateDeviceExtensionProperties");
        return std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties& item) { return std::strcmp(item.extensionName, name) == 0; });
    }

    void choosePhysical() {
        const auto enumerate = instance_<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        std::uint32_t count = 0;
        Check(enumerate(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
        Require(count != 0, "no Vulkan device");
        std::vector<VkPhysicalDevice> devices(count);
        Check(enumerate(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
        const char* requested = std::getenv("ANYPS5_GPU");
        const std::string_view request = requested != nullptr ? requested : "";
        const auto describe = instance_<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties");
        int best = -1;
        for (std::uint32_t index = 0; index < count; ++index) {
            VkPhysicalDeviceProperties candidate{};
            describe(devices[index], &candidate);
            const std::string_view name(candidate.deviceName);
            const bool named = std::search(name.begin(), name.end(), request.begin(), request.end(), [](char left, char right) {
                return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
            }) != name.end();
            if (!request.empty() && !named) continue;
            const int rank = candidate.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : candidate.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : candidate.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 1 : 0;
            if (rank <= best) continue;
            best = rank;
            physical = devices[index];
            properties = candidate;
        }
        Require(physical != VK_NULL_HANDLE, "no Vulkan device whose name contains ANYPS5_GPU=\"" + std::string(request) + "\"");
        instance_<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical, &memory);
    }

    std::uint32_t memoryType(std::uint32_t allowed, VkMemoryPropertyFlags wanted) const {
        for (std::uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
            if ((allowed & (1u << index)) != 0 && (memory.memoryTypes[index].propertyFlags & wanted) == wanted) return index;
        }
        throw std::runtime_error("stream replay bench: no memory type for the bench resources");
    }

    VkDeviceMemory bind(VkMemoryRequirements needs, VkMemoryPropertyFlags wanted) {
        VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        info.allocationSize = needs.size;
        info.memoryTypeIndex = memoryType(needs.memoryTypeBits, wanted);
        VkDeviceMemory result = VK_NULL_HANDLE;
        Check(device_<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &info, nullptr, &result), "vkAllocateMemory");
        allocations.push_back(result);
        return result;
    }

    void createTarget() {
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {Extent, Extent, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        Check(device_<PFN_vkCreateImage>("vkCreateImage")(device, &imageInfo, nullptr, &image), "vkCreateImage");
        VkMemoryRequirements needs{};
        device_<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(device, image, &needs);
        Check(device_<PFN_vkBindImageMemory>("vkBindImageMemory")(device, image, bind(needs, 0), 0), "vkBindImageMemory");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = imageInfo.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Check(device_<PFN_vkCreateImageView>("vkCreateImageView")(device, &viewInfo, nullptr, &view), "vkCreateImageView");
        VkAttachmentDescription attachment{};
        attachment.format = imageInfo.format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;
        VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        passInfo.attachmentCount = 1;
        passInfo.pAttachments = &attachment;
        passInfo.subpassCount = 1;
        passInfo.pSubpasses = &subpass;
        Check(device_<PFN_vkCreateRenderPass>("vkCreateRenderPass")(device, &passInfo, nullptr, &pass), "vkCreateRenderPass");
        VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = pass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &view;
        framebufferInfo.width = Extent;
        framebufferInfo.height = Extent;
        framebufferInfo.layers = 1;
        Check(device_<PFN_vkCreateFramebuffer>("vkCreateFramebuffer")(device, &framebufferInfo, nullptr, &framebuffer), "vkCreateFramebuffer");
    }

    void createBuffers() {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = 64 * 1024;
        info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        Check(device_<PFN_vkCreateBuffer>("vkCreateBuffer")(device, &info, nullptr, &vertices), "vkCreateBuffer");
        VkMemoryRequirements needs{};
        device_<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device, vertices, &needs);
        const auto bound = bind(needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Check(device_<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device, vertices, bound, 0), "vkBindBufferMemory");
        void* mapped = nullptr;
        Check(device_<PFN_vkMapMemory>("vkMapMemory")(device, bound, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
        std::memset(mapped, 0, static_cast<std::size_t>(info.size));
        device_<PFN_vkUnmapMemory>("vkUnmapMemory")(device, bound);
    }

    void createDescriptors() {
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.bindingCount = 1;
        setInfo.pBindings = &binding;
        Check(device_<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(device, &setInfo, nullptr, &setLayout), "vkCreateDescriptorSetLayout");
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<std::uint32_t>(sets.size())};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = static_cast<std::uint32_t>(sets.size());
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &size;
        Check(device_<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(device, &poolInfo, nullptr, &descriptorPool), "vkCreateDescriptorPool");
        std::array<VkDescriptorSetLayout, 4> layouts{setLayout, setLayout, setLayout, setLayout};
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = descriptorPool;
        allocate.descriptorSetCount = static_cast<std::uint32_t>(sets.size());
        allocate.pSetLayouts = layouts.data();
        Check(device_<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(device, &allocate, sets.data()), "vkAllocateDescriptorSets");
        std::array<VkDescriptorBufferInfo, 4> buffers{};
        std::array<VkWriteDescriptorSet, 4> writes{};
        for (std::size_t index = 0; index < sets.size(); ++index) {
            buffers[index] = {vertices, index * 256, 256};
            writes[index] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[index].dstSet = sets[index];
            writes[index].descriptorCount = 1;
            writes[index].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[index].pBufferInfo = &buffers[index];
        }
        device_<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    VkShaderModule module(const std::uint32_t* words, std::size_t bytes) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = bytes;
        info.pCode = words;
        VkShaderModule result = VK_NULL_HANDLE;
        Check(device_<PFN_vkCreateShaderModule>("vkCreateShaderModule")(device, &info, nullptr, &result), "vkCreateShaderModule");
        return result;
    }

    void createPipelines() {
        const std::array<VkDescriptorSetLayout, 2> setLayouts{setLayout, setLayout};
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = static_cast<std::uint32_t>(setLayouts.size());
        layoutInfo.pSetLayouts = setLayouts.data();
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &push;
        Check(device_<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
        const auto vertex = module(VertexWords, sizeof(VertexWords));
        const auto fragment = module(FragmentWords, sizeof(FragmentWords));
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex, "main", nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main", nullptr};
        VkVertexInputBindingDescription vertexBinding{0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attribute{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        input.vertexBindingDescriptionCount = 1;
        input.pVertexBindingDescriptions = &vertexBinding;
        input.vertexAttributeDescriptionCount = 1;
        input.pVertexAttributeDescriptions = &attribute;
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.depthBiasEnable = VK_TRUE;
        raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;
        const std::array<VkDynamicState, 5> dynamic{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK};
        VkPipelineDynamicStateCreateInfo dynamicInfo{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamicInfo.dynamicStateCount = static_cast<std::uint32_t>(dynamic.size());
        dynamicInfo.pDynamicStates = dynamic.data();
        std::array<VkGraphicsPipelineCreateInfo, 2> infos{};
        std::array<VkPipelineRasterizationStateCreateInfo, 2> rasters{raster, raster};
        rasters[1].cullMode = VK_CULL_MODE_BACK_BIT;
        for (std::size_t index = 0; index < infos.size(); ++index) {
            auto& info = infos[index];
            info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            info.stageCount = static_cast<std::uint32_t>(stages.size());
            info.pStages = stages.data();
            info.pVertexInputState = &input;
            info.pInputAssemblyState = &assembly;
            info.pViewportState = &viewport;
            info.pRasterizationState = &rasters[index];
            info.pMultisampleState = &multisample;
            info.pColorBlendState = &blend;
            info.pDynamicState = &dynamicInfo;
            info.layout = layout;
            info.renderPass = pass;
        }
        const VkResult created = device_<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines")(device, VK_NULL_HANDLE, static_cast<std::uint32_t>(infos.size()), infos.data(), nullptr, pipelines.data());
        device_<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(device, vertex, nullptr);
        device_<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(device, fragment, nullptr);
        Check(created, "vkCreateGraphicsPipelines");
    }

    void release() noexcept {
        if (device != VK_NULL_HANDLE) {
            device_<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(device);
            for (const auto pipeline : pipelines) {
                if (pipeline != VK_NULL_HANDLE) device_<PFN_vkDestroyPipeline>("vkDestroyPipeline")(device, pipeline, nullptr);
            }
            if (layout != VK_NULL_HANDLE) device_<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(device, layout, nullptr);
            if (descriptorPool != VK_NULL_HANDLE) device_<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(device, descriptorPool, nullptr);
            if (setLayout != VK_NULL_HANDLE) device_<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(device, setLayout, nullptr);
            if (vertices != VK_NULL_HANDLE) device_<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device, vertices, nullptr);
            if (framebuffer != VK_NULL_HANDLE) device_<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer")(device, framebuffer, nullptr);
            if (pass != VK_NULL_HANDLE) device_<PFN_vkDestroyRenderPass>("vkDestroyRenderPass")(device, pass, nullptr);
            if (view != VK_NULL_HANDLE) device_<PFN_vkDestroyImageView>("vkDestroyImageView")(device, view, nullptr);
            if (image != VK_NULL_HANDLE) device_<PFN_vkDestroyImage>("vkDestroyImage")(device, image, nullptr);
            for (const auto allocation : allocations) device_<PFN_vkFreeMemory>("vkFreeMemory")(device, allocation, nullptr);
            if (commandPool != VK_NULL_HANDLE) device_<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device, commandPool, nullptr);
            instance_<PFN_vkDestroyDevice>("vkDestroyDevice")(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE) instance_<PFN_vkDestroyInstance>("vkDestroyInstance")(instance, nullptr);
        if (library != nullptr) SDL_UnloadObject(library);
    }

    void* library = nullptr;
    PFN_vkGetInstanceProcAddr instanceProc = nullptr;
    PFN_vkGetDeviceProcAddr deviceProc = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    CommandFunctions real{};
    PFN_vkBeginCommandBuffer beginBuffer = nullptr;
    PFN_vkEndCommandBuffer endBuffer = nullptr;
    PFN_vkResetCommandPool resetPool = nullptr;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkBuffer vertices = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 4> sets{};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::array<VkPipeline, 2> pipelines{};
    std::vector<VkDeviceMemory> allocations;
};

std::uint64_t RecordBatch(const Device& device, const Thunks& t, VkCommandBuffer commands, std::uint32_t draws, std::uint32_t batch) {
    std::uint64_t issued = 0;
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = device.Pass();
    begin.framebuffer = device.Framebuffer();
    begin.renderArea = {{0, 0}, {Extent, Extent}};
    for (std::uint32_t draw = 0; draw < draws; ++draw) {
        if (draw % DrawsPerPass == 0) {
            if (draw != 0) {
                t.endRenderPass(commands);
                ++issued;
            }
            t.beginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
            ++issued;
        }
        const std::uint32_t variant = (draw + batch) & 1u;
        const std::uint32_t step = (draw * 7u + batch) % 61u;
        t.bindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, device.Pipeline(variant));
        t.bindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, device.Layout(), 0, 1, device.Sets(variant), 0, nullptr);
        t.bindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, device.Layout(), 1, 1, device.Sets(variant) + 1, 0, nullptr);
        const std::array<float, 4> offset{static_cast<float>(step) * 0.001f, 0, 0, 0};
        t.pushConstants(commands, device.Layout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(offset), offset.data());
        const VkBuffer vertices = device.Vertices();
        const VkDeviceSize vertexOffset = 4096 + 16u * step;
        t.bindVertexBuffers(commands, 0, 1, &vertices, &vertexOffset);
        t.bindIndexBuffer(commands, device.Vertices(), 32768 + 4u * step, VK_INDEX_TYPE_UINT32);
        const VkViewport viewport{0, 0, static_cast<float>(Extent - (step & 7u)), static_cast<float>(Extent), 0, 1};
        t.setViewport(commands, 0, 1, &viewport);
        const VkRect2D scissor{{0, 0}, {Extent - (step & 3u), Extent}};
        t.setScissor(commands, 0, 1, &scissor);
        t.setDepthBias(commands, static_cast<float>(step & 3u), 0, 0.5f);
        t.setStencilReference(commands, VK_STENCIL_FACE_FRONT_AND_BACK, step & 0xffu);
        t.setStencilCompareMask(commands, VK_STENCIL_FACE_FRONT_AND_BACK, 0xffu);
        t.drawIndexed(commands, 3, 1, 0, 0, 0);
        issued += 12;
    }
    t.endRenderPass(commands);
    return issued + 1;
}

struct Arm {
    const char* name;
    double nanoseconds = 0;
    std::uint64_t commands = 0;
    std::uint64_t draws = 0;
};

void Print(const Arm& arm) {
    std::printf("stream-replay-bench arm=%s draws=%llu commands=%llu ns_per_command=%.2f us_per_draw=%.4f commands_per_draw=%.3f\n", arm.name,
        static_cast<unsigned long long>(arm.draws), static_cast<unsigned long long>(arm.commands), arm.nanoseconds / static_cast<double>(arm.commands),
        arm.nanoseconds / 1000.0 / static_cast<double>(arm.draws), static_cast<double>(arm.commands) / static_cast<double>(arm.draws));
}

void PrintDerived(const char* name, double usPerDraw) {
    std::printf("stream-replay-bench arm=%s us_per_draw=%.4f\n", name, usPerDraw);
}

double Since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
}

int Run() {
    std::unique_ptr<Device> device;
    try {
        device = std::make_unique<Device>();
    } catch (const std::exception& error) {
        const char* requested = std::getenv("ANYPS5_GPU");
        if ((requested != nullptr && *requested != '\0') || std::getenv("ANYPS5_REQUIRE_VULKAN") != nullptr) throw;
        std::printf("skipped, no usable Vulkan device: %s\n", error.what());
        return 77;
    }
    const std::uint32_t batches = EnvCount("APS5_REPLAY_BENCH_BATCHES", 50);
    const std::uint32_t draws = EnvCount("APS5_REPLAY_BENCH_DRAWS", 3000);
    const Thunks thunks(device->Real());
    std::printf("stream-replay-bench device=\"%s\" batches=%u draws_per_batch=%u draws_per_pass=%u\n", device->Name(), batches, draws, DrawsPerPass);
    Arm direct{"D"};
    Arm append{"A"};
    Arm replay{"R"};
    device->Begin();
    const auto warm = RecordBatch(*device, thunks, device->Buffer(), draws, 0);
    device->End();
    device->SubmitAndWait();
    device->Reset();
    for (std::uint32_t batch = 0; batch <= batches; ++batch) {
        const bool counted = batch != 0;
        auto start = std::chrono::steady_clock::now();
        device->Begin();
        const auto directCommands = RecordBatch(*device, thunks, device->Buffer(), draws, batch);
        device->End();
        const double directNs = Since(start);
        device->Reset();
        start = std::chrono::steady_clock::now();
        auto stream = std::make_unique<CommandStream>(device->Real());
        const auto streamedCommands = RecordBatch(*device, thunks, stream->Handle(), draws, batch);
        stream->Seal();
        const double appendNs = Since(start);
        Require(streamedCommands == directCommands && stream->Ops() == directCommands && directCommands == warm, "batch " + std::to_string(batch) + " streamed " + std::to_string(stream->Ops()) + " ops for " + std::to_string(directCommands) + " direct commands");
        start = std::chrono::steady_clock::now();
        device->Begin();
        AgcDriver::Graphics::Replay(*stream, device->Buffer(), device->Real());
        device->End();
        const double replayNs = Since(start);
        if (batch == 1) device->SubmitAndWait();
        device->Reset();
        if (!counted) continue;
        direct.nanoseconds += directNs;
        append.nanoseconds += appendNs;
        replay.nanoseconds += replayNs;
        for (auto* arm : {&direct, &append, &replay}) {
            arm->commands += directCommands;
            arm->draws += draws;
        }
    }
    Print(direct);
    Print(append);
    Print(replay);
    const double perDraw = 1000.0 * static_cast<double>(direct.draws);
    PrintDerived("A+R-D", (append.nanoseconds + replay.nanoseconds - direct.nanoseconds) / perDraw);
    PrintDerived("D-A", (direct.nanoseconds - append.nanoseconds) / perDraw);
    return 0;
}

}

int main() {
    try {
        return Run();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
