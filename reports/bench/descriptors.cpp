#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <vector>
#define CHECK(x) do { if ((x) != VK_SUCCESS) throw std::runtime_error(#x); } while (0)
PFN_vkGetInstanceProcAddr gipa;
#define F(name) auto name = reinterpret_cast<PFN_##name>(gipa(instance, #name))
int main() {
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW);
    gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    VkInstance instance = VK_NULL_HANDLE;
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "bench", 0, "bench", 0, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app};
    CHECK(createInstance(&ici, nullptr, &instance));
    F(vkEnumeratePhysicalDevices); F(vkCreateDevice); F(vkGetPhysicalDeviceProperties);
    std::uint32_t count = 1; VkPhysicalDevice physical; vkEnumeratePhysicalDevices(instance, &count, &physical);
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(physical, &props);
    float priority = 1; VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, 0, 1, &priority};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &q};
    VkDevice device; CHECK(vkCreateDevice(physical, &dci, nullptr, &device));
    F(vkCreateBuffer); F(vkGetBufferMemoryRequirements); F(vkAllocateMemory); F(vkBindBufferMemory); F(vkCreateSampler);
    F(vkCreateDescriptorSetLayout); F(vkCreateDescriptorPool); F(vkAllocateDescriptorSets); F(vkUpdateDescriptorSets);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, 1 << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer buffer; CHECK(vkCreateBuffer(device, &bci, nullptr, &buffer));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(device, buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, 0};
    VkDeviceMemory memory; CHECK(vkAllocateMemory(device, &mai, nullptr, &memory)); CHECK(vkBindBufferMemory(device, buffer, memory, 0));
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSampler sampler; CHECK(vkCreateSampler(device, &sci, nullptr, &sampler));
    std::printf("device: %s\n", props.deviceName);
    for (const std::uint32_t others : {0u, 16u, 64u}) {
        const std::uint32_t buffers = 3;
        std::vector<VkDescriptorSetLayoutBinding> bindings{{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffers, VK_SHADER_STAGE_ALL, nullptr}};
        if (others) bindings.push_back({1, VK_DESCRIPTOR_TYPE_SAMPLER, others, VK_SHADER_STAGE_ALL, nullptr});
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, static_cast<std::uint32_t>(bindings.size()), bindings.data()};
        VkDescriptorSetLayout layout; CHECK(vkCreateDescriptorSetLayout(device, &lci, nullptr, &layout));
        std::vector<VkDescriptorPoolSize> sizes{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffers}};
        if (others) sizes.push_back({VK_DESCRIPTOR_TYPE_SAMPLER, others});
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 1, static_cast<std::uint32_t>(sizes.size()), sizes.data()};
        VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(device, &pci, nullptr, &pool));
        VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, pool, 1, &layout};
        VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(device, &sai, &set));
        std::vector<VkDescriptorBufferInfo> infos(buffers, {buffer, 0, 256});
        std::vector<VkDescriptorImageInfo> images(others, {sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
        std::vector<VkWriteDescriptorSet> full{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, buffers, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, infos.data()}};
        if (others) full.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, others, VK_DESCRIPTOR_TYPE_SAMPLER, images.data()});
        const auto time = [&](std::uint32_t writes) {
            const int n = 200000;
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < n; ++i) { infos[0].offset = (i & 63) * 256; vkUpdateDescriptorSets(device, writes, full.data(), 0, nullptr); }
            return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / n;
        };
        const double all = time(static_cast<std::uint32_t>(full.size())), only = time(1);
        std::printf("buffers=%u non-buffer=%2u  full rewrite %7.1f ns  buffers only %7.1f ns  saved %7.1f ns\n", buffers, others, all, only, all - only);
    }
}
