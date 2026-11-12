#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

constexpr int Skipped = 77;

PFN_vkGetInstanceProcAddr LoadLoader() {
#ifdef _WIN32
    const auto library = LoadLibraryA("vulkan-1.dll");
    return library == nullptr ? nullptr : reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(library, "vkGetInstanceProcAddr")));
#else
    auto* library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    return library == nullptr ? nullptr : reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
#endif
}

int Unavailable(const char* what) {
    std::printf("vvl sync canary: %s\n", what);
    if (std::getenv("ANYPS5_REQUIRE_VULKAN") != nullptr) return 1;
    return Skipped;
}

}

int main() {
    const auto* sync = std::getenv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC");
    if (sync == nullptr || std::string_view(sync) == "0" || std::string_view(sync) == "false") {
        std::puts("vvl sync canary: VK_KHRONOS_VALIDATION_VALIDATE_SYNC does not enable sync validation, so there is nothing to check");
        return Skipped;
    }
    const auto getInstanceProcAddr = LoadLoader();
    if (getInstanceProcAddr == nullptr) return Unavailable("no Vulkan loader");
    const auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(getInstanceProcAddr(nullptr, "vkCreateInstance"));
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "agc_vvl_sync_canary";
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &application;
    VkInstance instance = VK_NULL_HANDLE;
    if (createInstance == nullptr || createInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) return Unavailable("no Vulkan instance");
    const auto instanceProc = [&](const char* name) { return getInstanceProcAddr(instance, name); };
    const auto enumerateDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(instanceProc("vkEnumeratePhysicalDevices"));
    const auto queueFamilies = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(instanceProc("vkGetPhysicalDeviceQueueFamilyProperties"));
    const auto memoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(instanceProc("vkGetPhysicalDeviceMemoryProperties"));
    const auto createDevice = reinterpret_cast<PFN_vkCreateDevice>(instanceProc("vkCreateDevice"));
    const auto getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(instanceProc("vkGetDeviceProcAddr"));
    const auto destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(instanceProc("vkDestroyInstance"));
    std::uint32_t count = 0;
    enumerateDevices(instance, &count, nullptr);
    if (count == 0) {
        destroyInstance(instance, nullptr);
        return Unavailable("no Vulkan device");
    }
    std::vector<VkPhysicalDevice> physicalDevices(count);
    enumerateDevices(instance, &count, physicalDevices.data());
    const auto physical = physicalDevices.front();
    std::uint32_t familyCount = 0;
    queueFamilies(physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    queueFamilies(physical, &familyCount, families.data());
    std::uint32_t family = familyCount;
    for (std::uint32_t index = 0; index < familyCount && family == familyCount; ++index) {
        if ((families[index].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) != 0) family = index;
    }
    if (family == familyCount) {
        destroyInstance(instance, nullptr);
        return Unavailable("no queue family that can fill buffers");
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    VkDevice device = VK_NULL_HANDLE;
    if (createDevice(physical, &deviceInfo, nullptr, &device) != VK_SUCCESS) {
        destroyInstance(instance, nullptr);
        return Unavailable("no Vulkan device could be created");
    }
    const auto deviceProc = [&](const char* name) { return getDeviceProcAddr(device, name); };
    const auto createBuffer = reinterpret_cast<PFN_vkCreateBuffer>(deviceProc("vkCreateBuffer"));
    const auto bufferRequirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(deviceProc("vkGetBufferMemoryRequirements"));
    const auto allocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(deviceProc("vkAllocateMemory"));
    const auto bindBufferMemory = reinterpret_cast<PFN_vkBindBufferMemory>(deviceProc("vkBindBufferMemory"));
    const auto createCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(deviceProc("vkCreateCommandPool"));
    const auto allocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(deviceProc("vkAllocateCommandBuffers"));
    const auto beginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(deviceProc("vkBeginCommandBuffer"));
    const auto cmdFillBuffer = reinterpret_cast<PFN_vkCmdFillBuffer>(deviceProc("vkCmdFillBuffer"));
    const auto endCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(deviceProc("vkEndCommandBuffer"));
    const auto getDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(deviceProc("vkGetDeviceQueue"));
    const auto queueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(deviceProc("vkQueueSubmit"));
    const auto queueWaitIdle = reinterpret_cast<PFN_vkQueueWaitIdle>(deviceProc("vkQueueWaitIdle"));
    const auto destroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(deviceProc("vkDestroyCommandPool"));
    const auto destroyBuffer = reinterpret_cast<PFN_vkDestroyBuffer>(deviceProc("vkDestroyBuffer"));
    const auto freeMemory = reinterpret_cast<PFN_vkFreeMemory>(deviceProc("vkFreeMemory"));
    const auto destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(deviceProc("vkDestroyDevice"));
    constexpr VkDeviceSize bytes = 4096;
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = bytes;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    int result = 1;
    if (createBuffer(device, &bufferInfo, nullptr, &buffer) == VK_SUCCESS) {
        VkMemoryRequirements requirements{};
        bufferRequirements(device, buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memory{};
        memoryProperties(physical, &memory);
        std::uint32_t type = memory.memoryTypeCount;
        for (std::uint32_t index = 0; index < memory.memoryTypeCount && type == memory.memoryTypeCount; ++index) {
            if ((requirements.memoryTypeBits & (1u << index)) != 0) type = index;
        }
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        VkDeviceMemory backing = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = family;
        VkCommandPool pool = VK_NULL_HANDLE;
        if (type != memory.memoryTypeCount && allocateMemory(device, &allocation, nullptr, &backing) == VK_SUCCESS && bindBufferMemory(device, buffer, backing, 0) == VK_SUCCESS && createCommandPool(device, &poolInfo, nullptr, &pool) == VK_SUCCESS) {
            VkCommandBufferAllocateInfo commandsInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            commandsInfo.commandPool = pool;
            commandsInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            commandsInfo.commandBufferCount = 1;
            VkCommandBuffer commands = VK_NULL_HANDLE;
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (allocateCommandBuffers(device, &commandsInfo, &commands) == VK_SUCCESS && beginCommandBuffer(commands, &begin) == VK_SUCCESS) {
                cmdFillBuffer(commands, buffer, 0, bytes, 0x11111111u);
                cmdFillBuffer(commands, buffer, 0, bytes, 0x22222222u);
                if (endCommandBuffer(commands) == VK_SUCCESS) {
                    VkQueue queue = VK_NULL_HANDLE;
                    getDeviceQueue(device, family, 0, &queue);
                    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                    submit.commandBufferCount = 1;
                    submit.pCommandBuffers = &commands;
                    if (queueSubmit(queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS && queueWaitIdle(queue) == VK_SUCCESS) result = 0;
                }
            }
            destroyCommandPool(device, pool, nullptr);
        }
        destroyBuffer(device, buffer, nullptr);
        if (backing != VK_NULL_HANDLE) freeMemory(device, backing, nullptr);
    }
    destroyDevice(device, nullptr);
    destroyInstance(instance, nullptr);
    std::puts(result == 0 ? "vvl sync canary: recorded two fills of one buffer with no barrier between them; sync validation must report the write after write" : "vvl sync canary: the two fills could not be recorded and submitted");
    std::fflush(stdout);
    return result;
}
