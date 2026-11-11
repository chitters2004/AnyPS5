#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
using namespace AgcDriver::Graphics;

struct VulkanObjects {
    std::uintptr_t next = 1;
    std::uint32_t layouts = 0;
    std::uint32_t pools = 0;
    std::uint32_t createdPools = 0;
    std::uint32_t allocatedSets = 0;
} objects;

void RequireTest(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "%s\n", message);
    std::abort();
}

VKAPI_ATTR VkResult VKAPI_CALL CreateLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*, VkDescriptorSetLayout* layout) {
    *layout = reinterpret_cast<VkDescriptorSetLayout>(objects.next++);
    ++objects.layouts;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyLayout(VkDevice, VkDescriptorSetLayout, const VkAllocationCallbacks*) {
    RequireTest(objects.layouts != 0, "layout destroyed twice");
    --objects.layouts;
}

VKAPI_ATTR VkResult VKAPI_CALL CreatePool(VkDevice, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*, VkDescriptorPool* pool) {
    *pool = reinterpret_cast<VkDescriptorPool>(objects.next++);
    ++objects.pools;
    ++objects.createdPools;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyPool(VkDevice, VkDescriptorPool, const VkAllocationCallbacks*) {
    RequireTest(objects.pools != 0, "pool destroyed twice");
    --objects.pools;
}

VKAPI_ATTR VkResult VKAPI_CALL AllocateSets(VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet* set) {
    *set = reinterpret_cast<VkDescriptorSet>(objects.next++);
    ++objects.allocatedSets;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL ResetPool(VkDevice, VkDescriptorPool, VkDescriptorPoolResetFlags) {
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Resolve(VkDevice, const char* name) {
    if (std::strcmp(name, "vkCreateDescriptorSetLayout") == 0) return reinterpret_cast<PFN_vkVoidFunction>(CreateLayout);
    if (std::strcmp(name, "vkDestroyDescriptorSetLayout") == 0) return reinterpret_cast<PFN_vkVoidFunction>(DestroyLayout);
    if (std::strcmp(name, "vkCreateDescriptorPool") == 0) return reinterpret_cast<PFN_vkVoidFunction>(CreatePool);
    if (std::strcmp(name, "vkDestroyDescriptorPool") == 0) return reinterpret_cast<PFN_vkVoidFunction>(DestroyPool);
    if (std::strcmp(name, "vkAllocateDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(AllocateSets);
    if (std::strcmp(name, "vkResetDescriptorPool") == 0) return reinterpret_cast<PFN_vkVoidFunction>(ResetPool);
    return nullptr;
}

Context TestContext() {
    Context context{};
    context.deviceProc = Resolve;
    context.limits.maxPerStageResources = 1u << 22;
    context.limits.maxDescriptorSetStorageBuffers = 4096;
    context.limits.maxDescriptorSetSampledImages = 1u << 22;
    context.limits.maxPerStageDescriptorSampledImages = 1u << 22;
    return context;
}

constexpr std::uint64_t Budget = DescriptorCache::RecycledDescriptorBudget;

struct Key {
    VkDescriptorSetLayoutBinding binding;
    std::array<std::uint32_t, 4> words;
};

Key TableKey(std::uint32_t count) {
    const VkDescriptorSetLayoutBinding binding{1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, count, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    return {binding, {binding.binding, binding.descriptorType, binding.descriptorCount, binding.stageFlags}};
}

std::vector<DescriptorCache::SetAllocation> AllocateTables(DescriptorCache& cache, VkDescriptorSetLayout layout, std::uint32_t elements, std::size_t count) {
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, elements};
    std::vector<DescriptorCache::SetAllocation> sets;
    for (std::size_t index = 0; index < count; ++index) sets.push_back(cache.AllocateRewritten(layout, std::span(&size, 1), false, elements));
    return sets;
}

void BudgetBoundsLargeSets() {
    objects = {};
    {
        DescriptorCache cache(TestContext());
        constexpr std::uint32_t elements = 1u << 20;
        const auto key = TableKey(elements);
        const auto layout = cache.Layout(key.words, std::span(&key.binding, 1));
        auto sets = AllocateTables(cache, layout, elements, 6);
        RequireTest(sets.front().descriptors == elements, "a set's weight is not its descriptor count");
        for (const auto& set : sets) cache.Recycle(layout, set);
        auto stats = cache.Counters();
        RequireTest(stats.recycledDescriptors <= Budget, "recycled sets exceed the descriptor budget");
        RequireTest(stats.recycledSets == Budget / elements && stats.recycleRefused == 6 - Budget / elements, "the budget kept the wrong number of large sets");
        const auto allocated = objects.allocatedSets;
        sets = AllocateTables(cache, layout, elements, static_cast<std::size_t>(Budget / elements));
        RequireTest(objects.allocatedSets == allocated, "a recycled set was not reused");
        stats = cache.Counters();
        RequireTest(stats.recycledSets == 0 && stats.recycledDescriptors == 0, "reusing recycled sets did not return their weight");
        for (const auto& set : sets) cache.Recycle(layout, set);
        RequireTest(cache.Counters().recycledDescriptors == Budget, "the budget did not admit sets after reuse freed it");
    }
    RequireTest(objects.layouts == 0 && objects.pools == 0, "large-set budget leaked Vulkan objects");
    std::printf("large sets: budget %llu descriptors held\n", static_cast<unsigned long long>(Budget));
}

void BudgetIsSharedAcrossKeys() {
    objects = {};
    {
        DescriptorCache cache(TestContext());
        constexpr std::uint32_t large = 1u << 20;
        constexpr std::uint32_t medium = 1u << 19;
        const auto largeKey = TableKey(large);
        const auto mediumKey = TableKey(medium);
        const auto largeLayout = cache.Layout(largeKey.words, std::span(&largeKey.binding, 1));
        const auto mediumLayout = cache.Layout(mediumKey.words, std::span(&mediumKey.binding, 1));
        const auto larges = AllocateTables(cache, largeLayout, large, static_cast<std::size_t>(Budget / large));
        const auto mediums = AllocateTables(cache, mediumLayout, medium, 2);
        for (const auto& set : larges) cache.Recycle(largeLayout, set);
        cache.Recycle(mediumLayout, mediums[0]);
        RequireTest(cache.Counters().recycleRefused == 1, "a second key was recycled past a full budget");
        static_cast<void>(AllocateTables(cache, largeLayout, large, 1));
        cache.Recycle(mediumLayout, mediums[1]);
        const auto stats = cache.Counters();
        RequireTest(stats.recycleRefused == 1 && stats.recycledDescriptors == Budget - large + medium, "freed budget was not shared with another key");
    }
    std::printf("shared budget: two keys within %llu descriptors\n", static_cast<unsigned long long>(Budget));
}

void SmallSetsKeepTheSetCap() {
    objects = {};
    {
        DescriptorCache cache(TestContext());
        const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        const std::array<std::uint32_t, 4> key{binding.binding, binding.descriptorType, binding.descriptorCount, binding.stageFlags};
        const auto layout = cache.Layout(key, std::span(&binding, 1));
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        std::vector<DescriptorCache::SetAllocation> sets;
        for (int index = 0; index < 5000; ++index) sets.push_back(cache.AllocateRewritten(layout, std::span(&size, 1)));
        for (const auto& set : sets) cache.Recycle(layout, set);
        const auto stats = cache.Counters();
        RequireTest(stats.recycledSets == 4096 && stats.recycledDescriptors == 4096 && stats.recycleRefused == 5000 - 4096, "small sets lost the per-key set cap");
    }
    std::printf("small sets: 4096 kept per key\n");
}

void TableDrawsByClass() {
    objects = {};
    DescriptorCache cache(TestContext());
    for (const std::uint32_t elements : {1u, 500u, 1024u, 1025u, 4096u, 262144u, 400000u}) cache.NoteTableDraw(elements);
    const auto draws = cache.Counters().tableDraws;
    RequireTest(draws[0] == 3 && draws[1] == 1 && draws[2] == 1 && draws[8] == 2, "table draws landed in the wrong capacity classes");
    std::printf("table classes: counted\n");
}
}

int main() {
    BudgetBoundsLargeSets();
    BudgetIsSharedAcrossKeys();
    SmallSetsKeepTheSetCap();
    TableDrawsByClass();
    std::printf("descriptor recycle budget tests passed\n");
    return 0;
}
