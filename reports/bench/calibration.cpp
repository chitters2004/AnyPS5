#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include "shader/recompiler/ThreadOwned.hpp"
#include <vulkan/vulkan.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

volatile std::uint64_t sink = 0;

template<typename TBody>
double nanosecondsPer(std::uint64_t count, TBody&& body) {
    for (std::uint64_t i = 0; i < count / 10; ++i) body(i);
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < count; ++i) body(i);
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / static_cast<double>(count);
}

void row(const char* group, const std::string& name, double nanoseconds) {
    std::printf("| %s | %s | %.1f |\n", group, name.c_str(), nanoseconds);
    std::fflush(stdout);
}

thread_local std::uint64_t threadValue = 0;
thread_local std::uint64_t* ownedSlot = nullptr;
struct HostTag {};

[[gnu::noinline]] std::uint64_t readThreadLocal() { return ++threadValue; }
[[gnu::noinline]] std::uint64_t readThreadOwned() { return ++ShaderRecompiler::ThreadOwned(ownedSlot); }
[[gnu::noinline]] std::uint64_t readHostThreadLocal() { return ++HostThreadLocal<std::uint64_t, HostTag>(); }

void cpu() {
    constexpr std::uint64_t n = 20000000;
    row("cpu", "thread_local read and increment", nanosecondsPer(n, [](std::uint64_t) { sink += readThreadLocal(); }));
    row("cpu", "ThreadOwned slot (thread_local pointer)", nanosecondsPer(n, [](std::uint64_t) { sink += readThreadOwned(); }));
    row("cpu", "HostThreadLocal (FlsGetValue on Windows)", nanosecondsPer(n, [](std::uint64_t) { sink += readHostThreadLocal(); }));
    AgcDriver::Mutex agcMutex;
    row("cpu", "AgcDriver::Mutex lock and unlock", nanosecondsPer(n, [&](std::uint64_t) { agcMutex.lock(); agcMutex.unlock(); }));
    std::mutex mutex;
    row("cpu", "std::mutex lock and unlock", nanosecondsPer(n, [&](std::uint64_t) { mutex.lock(); mutex.unlock(); }));
    std::shared_mutex shared;
    row("cpu", "std::shared_mutex lock_shared and unlock_shared", nanosecondsPer(n, [&](std::uint64_t) { shared.lock_shared(); shared.unlock_shared(); }));
    row("cpu", "std::shared_mutex lock and unlock", nanosecondsPer(n, [&](std::uint64_t) { shared.lock(); shared.unlock(); }));
    auto pointer = std::make_shared<std::uint64_t>(7);
    row("cpu", "shared_ptr copy and release", nanosecondsPer(n, [&](std::uint64_t) { auto copy = pointer; sink += *copy; }));
    row("cpu", "steady_clock::now", nanosecondsPer(n / 4, [](std::uint64_t) { sink += static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()); }));
    row("cpu", "new and delete 64 bytes", nanosecondsPer(n / 4, [](std::uint64_t) { auto* p = new std::array<std::uint64_t, 8>(); sink += (*p)[0]; delete p; }));
    for (const std::size_t bytes : {256u, 1024u, 4096u, 16384u}) {
        std::vector<std::byte> from(bytes, std::byte{1}), to(bytes);
        row("cpu", "memcpy " + std::to_string(bytes) + " bytes", nanosecondsPer(n / 20, [&](std::uint64_t i) { from[0] = static_cast<std::byte>(i); std::memcpy(to.data(), from.data(), bytes); sink += static_cast<std::uint64_t>(to[0]); }));
    }
}

#define CHECK(call) do { const VkResult checkResult = (call); if (checkResult != VK_SUCCESS) throw std::runtime_error(std::string(#call) + " failed: " + std::to_string(checkResult)); } while (0)

PFN_vkGetInstanceProcAddr loaderEntry() {
#ifdef _WIN32
    HMODULE library = LoadLibraryA("vulkan-1.dll");
    return library != nullptr ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(library, "vkGetInstanceProcAddr"))) : nullptr;
#else
    void* library = dlopen("libvulkan.so.1", RTLD_NOW);
    return library != nullptr ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr")) : nullptr;
#endif
}

struct Vulkan {
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    template<typename T> T get(const char* name) const { return reinterpret_cast<T>(gipa(instance, name)); }
    std::uint32_t memoryType(std::uint32_t bits, VkMemoryPropertyFlags flags) const {
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) != 0 && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
        }
        throw std::runtime_error("no matching memory type");
    }
};

void vulkan() {
    Vulkan vk;
    vk.gipa = loaderEntry();
    if (vk.gipa == nullptr) {
        std::printf("| vulkan | loader not found | - |\n");
        return;
    }
    const auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(vk.gipa(nullptr, "vkCreateInstance"));
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "calibration", 0, "calibration", 0, VK_API_VERSION_1_2};
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &application};
    CHECK(createInstance(&instanceInfo, nullptr, &vk.instance));
    std::uint32_t count = 0;
    CHECK(vk.get<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(vk.instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    CHECK(vk.get<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices")(vk.instance, &count, devices.data()));
    if (devices.empty()) throw std::runtime_error("no Vulkan device");
    const char* chosen = std::getenv("CALIBRATION_GPU");
    std::size_t index = 0;
    for (std::size_t i = 0; i < devices.size(); ++i) {
        VkPhysicalDeviceProperties properties{};
        vk.get<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(devices[i], &properties);
        std::printf("| device | %zu: %s (driver 0x%x, api %u.%u.%u) | - |\n", i, properties.deviceName, properties.driverVersion, VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion), VK_VERSION_PATCH(properties.apiVersion));
        if (chosen != nullptr ? i == std::strtoul(chosen, nullptr, 10) : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && index == 0) index = i;
    }
    vk.physical = devices[index];
    vk.get<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(vk.physical, &vk.properties);
    vk.get<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(vk.physical, &vk.memory);
    std::printf("| device | measured: %s, maxDescriptorSetStorageBuffersDynamic %u, minStorageBufferOffsetAlignment %llu | - |\n", vk.properties.deviceName, vk.properties.limits.maxDescriptorSetStorageBuffersDynamic, static_cast<unsigned long long>(vk.properties.limits.minStorageBufferOffsetAlignment));
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, 0, 1, &priority};
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &queueInfo};
    CHECK(vk.get<PFN_vkCreateDevice>("vkCreateDevice")(vk.physical, &deviceInfo, nullptr, &vk.device));
    const auto deviceProc = vk.get<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
    const auto device = [&](const char* name) { return deviceProc(vk.device, name); };
#define DEVICE(name) const auto name = reinterpret_cast<PFN_##name>(device(#name))
    DEVICE(vkCreateBuffer); DEVICE(vkDestroyBuffer); DEVICE(vkGetBufferMemoryRequirements); DEVICE(vkAllocateMemory); DEVICE(vkBindBufferMemory);
    DEVICE(vkCreateImage); DEVICE(vkGetImageMemoryRequirements); DEVICE(vkBindImageMemory); DEVICE(vkCreateImageView); DEVICE(vkCreateSampler);
    DEVICE(vkCreateDescriptorSetLayout); DEVICE(vkCreateDescriptorPool); DEVICE(vkResetDescriptorPool); DEVICE(vkAllocateDescriptorSets); DEVICE(vkUpdateDescriptorSets);
    DEVICE(vkCreatePipelineLayout); DEVICE(vkCreateCommandPool); DEVICE(vkResetCommandPool); DEVICE(vkAllocateCommandBuffers); DEVICE(vkBeginCommandBuffer); DEVICE(vkEndCommandBuffer); DEVICE(vkCmdBindDescriptorSets);
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, 1 << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
    VkBuffer buffer;
    CHECK(vkCreateBuffer(vk.device, &bufferInfo, nullptr, &buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(vk.device, buffer, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size * 4, vk.memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
    VkDeviceMemory bufferMemory;
    CHECK(vkAllocateMemory(vk.device, &allocation, nullptr, &bufferMemory));
    CHECK(vkBindBufferMemory(vk.device, buffer, bufferMemory, 0));
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {4, 4, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImage image;
    CHECK(vkCreateImage(vk.device, &imageInfo, nullptr, &image));
    VkMemoryRequirements imageRequirements;
    vkGetImageMemoryRequirements(vk.device, image, &imageRequirements);
    VkMemoryAllocateInfo imageAllocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, imageRequirements.size, vk.memoryType(imageRequirements.memoryTypeBits, 0)};
    VkDeviceMemory imageMemory;
    CHECK(vkAllocateMemory(vk.device, &imageAllocation, nullptr, &imageMemory));
    CHECK(vkBindImageMemory(vk.device, image, imageMemory, 0));
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view;
    CHECK(vkCreateImageView(vk.device, &viewInfo, nullptr, &view));
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSampler sampler;
    CHECK(vkCreateSampler(vk.device, &samplerInfo, nullptr, &sampler));
    const std::array<VkDescriptorSetLayoutBinding, 4> bindings{{{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16, VK_SHADER_STAGE_ALL, nullptr}, {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16, VK_SHADER_STAGE_ALL, nullptr}, {2, VK_DESCRIPTOR_TYPE_SAMPLER, 16, VK_SHADER_STAGE_ALL, nullptr}, {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 2, VK_SHADER_STAGE_ALL, nullptr}}};
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 4, bindings.data()};
    VkDescriptorSetLayout layout;
    CHECK(vkCreateDescriptorSetLayout(vk.device, &layoutInfo, nullptr, &layout));
    const std::array<VkDescriptorPoolSize, 4> sizes{{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16 * 64}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16 * 64}, {VK_DESCRIPTOR_TYPE_SAMPLER, 16 * 64}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 2 * 64}}};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 64, 4, sizes.data()};
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(vk.device, &poolInfo, nullptr, &pool));
    VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, pool, 1, &layout};
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(vk.device, &setInfo, &set));
    std::vector<VkDescriptorBufferInfo> buffers(16, VkDescriptorBufferInfo{buffer, 0, 256});
    std::vector<VkDescriptorImageInfo> images(16, VkDescriptorImageInfo{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    std::vector<VkDescriptorImageInfo> samplers(16, VkDescriptorImageInfo{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
    const std::array<VkDescriptorBufferInfo, 2> dynamicBuffers{{{buffer, 0, 256}, {buffer, 0, 256}}};
    const auto write = [&](std::uint32_t binding, VkDescriptorType type, std::uint32_t descriptors) {
        VkWriteDescriptorSet entry{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, binding, 0, descriptors, type};
        if (type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) entry.pBufferInfo = buffers.data();
        else if (type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) entry.pBufferInfo = dynamicBuffers.data();
        else entry.pImageInfo = type == VK_DESCRIPTOR_TYPE_SAMPLER ? samplers.data() : images.data();
        return entry;
    };
    constexpr std::uint64_t n = 200000;
    for (const std::uint32_t descriptors : {1u, 4u, 16u}) {
        const auto entry = write(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, descriptors);
        row("vulkan", "vkUpdateDescriptorSets " + std::to_string(descriptors) + " storage buffers", nanosecondsPer(n, [&](std::uint64_t i) { buffers[0].offset = (i & 63) * 256; vkUpdateDescriptorSets(vk.device, 1, &entry, 0, nullptr); }));
    }
    {
        const auto entry = write(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16);
        row("vulkan", "vkUpdateDescriptorSets 16 sampled images", nanosecondsPer(n, [&](std::uint64_t) { vkUpdateDescriptorSets(vk.device, 1, &entry, 0, nullptr); }));
    }
    {
        const auto entry = write(2, VK_DESCRIPTOR_TYPE_SAMPLER, 16);
        row("vulkan", "vkUpdateDescriptorSets 16 samplers", nanosecondsPer(n, [&](std::uint64_t) { vkUpdateDescriptorSets(vk.device, 1, &entry, 0, nullptr); }));
    }
    {
        const std::array<VkWriteDescriptorSet, 4> all{write(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4), write(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16), write(2, VK_DESCRIPTOR_TYPE_SAMPLER, 16), write(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 2)};
        row("vulkan", "vkUpdateDescriptorSets whole set (4 buffers, 16 images, 16 samplers, 2 dynamic)", nanosecondsPer(n, [&](std::uint64_t) { vkUpdateDescriptorSets(vk.device, 4, all.data(), 0, nullptr); }));
        const std::array<VkWriteDescriptorSet, 2> buffersOnly{write(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4), write(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 2)};
        row("vulkan", "vkUpdateDescriptorSets buffers only of that set (4 + 2 dynamic)", nanosecondsPer(n, [&](std::uint64_t) { vkUpdateDescriptorSets(vk.device, 2, buffersOnly.data(), 0, nullptr); }));
    }
    CHECK(vkResetDescriptorPool(vk.device, pool, 0));
    row("vulkan", "vkAllocateDescriptorSets one set (pool reset every 64)", nanosecondsPer(n / 4, [&](std::uint64_t i) {
        if (i % 64 == 63) vkResetDescriptorPool(vk.device, pool, 0);
        VkDescriptorSet allocated;
        if (vkAllocateDescriptorSets(vk.device, &setInfo, &allocated) != VK_SUCCESS) throw std::runtime_error("vkAllocateDescriptorSets failed");
    }));
    CHECK(vkResetDescriptorPool(vk.device, pool, 0));
    CHECK(vkAllocateDescriptorSets(vk.device, &setInfo, &set));
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &layout};
    VkPipelineLayout pipelineLayout;
    CHECK(vkCreatePipelineLayout(vk.device, &pipelineLayoutInfo, nullptr, &pipelineLayout));
    VkCommandPoolCreateInfo commandPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, 0};
    VkCommandPool commandPool;
    CHECK(vkCreateCommandPool(vk.device, &commandPoolInfo, nullptr, &commandPool));
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, commandPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer commands;
    CHECK(vkAllocateCommandBuffers(vk.device, &commandInfo, &commands));
    const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    const auto bindLoop = [&](std::uint32_t dynamicCount) {
        CHECK(vkResetCommandPool(vk.device, commandPool, 0));
        CHECK(vkBeginCommandBuffer(commands, &begin));
        const std::array<std::uint32_t, 2> offsets{0, 256};
        const double nanoseconds = nanosecondsPer(n, [&](std::uint64_t i) {
            if (i % 20000 == 19999) {
                vkEndCommandBuffer(commands);
                vkResetCommandPool(vk.device, commandPool, 0);
                vkBeginCommandBuffer(commands, &begin);
            }
            vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &set, dynamicCount, offsets.data());
        });
        CHECK(vkEndCommandBuffer(commands));
        return nanoseconds;
    };
    row("vulkan", "vkCmdBindDescriptorSets with 2 dynamic offsets (includes a pool reset every 20000)", bindLoop(2));
    row("vulkan", "vkCreateBuffer + vkBindBufferMemory + vkDestroyBuffer (exact-size buffer per draw input)", nanosecondsPer(n / 4, [&](std::uint64_t i) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, 256 + (i & 255) * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
        VkBuffer created;
        if (vkCreateBuffer(vk.device, &info, nullptr, &created) != VK_SUCCESS) throw std::runtime_error("vkCreateBuffer failed");
        VkMemoryRequirements needed;
        vkGetBufferMemoryRequirements(vk.device, created, &needed);
        if (vkBindBufferMemory(vk.device, created, bufferMemory, (i & 7) * 65536) != VK_SUCCESS) throw std::runtime_error("vkBindBufferMemory failed");
        vkDestroyBuffer(vk.device, created, nullptr);
    }));
}

}

int main() {
    std::printf("| group | operation | ns per operation |\n|---|---|---|\n");
#ifdef _WIN32
    std::printf("| host | Windows, MinGW build | - |\n");
#else
    std::printf("| host | not Windows | - |\n");
#endif
    try {
        cpu();
        vulkan();
    } catch (const std::exception& error) {
        std::printf("| error | %s | - |\n", error.what());
        return 1;
    }
    return 0;
}
