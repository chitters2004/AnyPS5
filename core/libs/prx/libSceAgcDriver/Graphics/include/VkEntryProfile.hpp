#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_VKENTRYPROFILE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_VKENTRYPROFILE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include "prx/libSceAgcDriver/Graphics/include/CommitShape.hpp"

namespace AgcDriver {

enum VkEntryKind : std::size_t { VkEntryCommands, VkEntryDescriptors, VkEntryOther, VkEntryKindCount };

inline constexpr std::array<const char*, VkEntryKindCount> VkEntryKindNames{"vkCmd", "descriptors", "other vk"};

struct VkEntryTally {
    std::array<std::uint64_t, VkEntryKindCount> nanoseconds{};
    std::array<std::uint64_t, VkEntryKindCount> calls{};
    std::uint64_t lookups = 0;
};

bool VkEntryProfiled();
VkEntryTally ThreadVkEntryTally();
std::uint64_t VkEntryUnwrappedSlots();

class VkEntryShape {
public:
    static constexpr std::size_t Phases = CommitShape::Phases;

    struct Report {
        std::uint64_t draws = 0;
        std::array<double, Phases> phaseUs{};
        std::array<VkEntryTally, Phases> phases{};
    };

    void Note(std::span<const double, Phases> phaseUs, std::span<const VkEntryTally, Phases> phases);
    Report Take();

    static std::string Line(const Report& report, std::span<const char* const, Phases> phaseNames, std::uint64_t unwrappedSlots);

private:
    Mutex mutex;
    Report counts;
};

namespace Graphics {

struct DeviceFunctions;

void WrapDeviceFunctions(DeviceFunctions& functions);
void UnwrapDeviceFunctions(DeviceFunctions& functions);

}

}

#endif
