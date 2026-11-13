#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "VulkanTestDevice.hpp"
#include <exception>
#include <iostream>

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(*device);
        constexpr VkDeviceSize bytes = 65536;
        AgcDriver::Graphics::DeviceBuffer target(context, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        AgcDriver::Graphics::CommandBatch batch(context);
        const auto fill = context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer");
        fill(batch.Handle(), target.Handle(), 0, bytes, 0x11111111u);
        fill(batch.Handle(), target.Handle(), 0, bytes, 0x22222222u);
        batch.SubmitAndWait();
        std::cout << "sync validation canary recorded two unordered fills of one buffer, a write after write that live sync validation reports above\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
