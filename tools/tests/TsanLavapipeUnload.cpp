#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <cstdint>
#include <cstdio>

namespace {

PFN_vkGetInstanceProcAddr OpenLoader() {
    void* library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) return nullptr;
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
}

bool Probe(PFN_vkGetInstanceProcAddr get) {
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(get(VK_NULL_HANDLE, "vkCreateInstance"));
    if (create == nullptr) return false;
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &application;
    VkInstance instance = VK_NULL_HANDLE;
    if (create(&info, nullptr, &instance) != VK_SUCCESS) return false;
    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get(instance, "vkEnumeratePhysicalDevices"));
    const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get(instance, "vkDestroyInstance"));
    std::uint32_t devices = 0;
    if (enumerate != nullptr) enumerate(instance, &devices, nullptr);
    if (destroy != nullptr) destroy(instance, nullptr);
    return devices != 0;
}

}

int main() {
    const auto get = OpenLoader();
    if (get == nullptr || !Probe(get)) {
        std::puts("skipped, no Vulkan loader or device");
        return 77;
    }
    if (!Probe(get)) {
        std::puts("the second Vulkan probe found no device");
        return 1;
    }
    std::puts("the Vulkan driver was loaded and unloaded twice without a sanitizer abort");
    return 0;
}
