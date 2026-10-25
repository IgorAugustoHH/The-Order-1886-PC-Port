// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <set>
#include <vector>
#define VMA_IMPLEMENTATION
#include "video_core/buffer_cache/sparse_backing_pool.h"

int main() {
    // A minimal C Vulkan harness avoids renderer-wide startup and Vulkan-Hpp platform ABI.
    const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                               .apiVersion = VK_API_VERSION_1_2};
    const VkInstanceCreateInfo instance_ci{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                          .pApplicationInfo = &app};
    VkInstance instance{};
    assert(vkCreateInstance(&instance_ci, nullptr, &instance) == VK_SUCCESS);
    uint32_t count = 0;
    assert(vkEnumeratePhysicalDevices(instance, &count, nullptr) == VK_SUCCESS && count);
    std::vector<VkPhysicalDevice> devices(count);
    assert(vkEnumeratePhysicalDevices(instance, &count, devices.data()) == VK_SUCCESS);
    const auto physical = devices.front();
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    assert(family < count);
    const float priority = 1;
    const VkDeviceQueueCreateInfo queue_ci{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceVulkan12Features features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                       .pNext = &features12};
    vkGetPhysicalDeviceFeatures2(physical, &features);
    assert(features12.bufferDeviceAddress);
    const VkDeviceCreateInfo device_ci{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features12, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_ci};
    VkDevice device{};
    assert(vkCreateDevice(physical, &device_ci, nullptr, &device) == VK_SUCCESS);
    const VmaAllocatorCreateInfo allocator_ci{
        .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice = physical, .device = device, .instance = instance,
        .vulkanApiVersion = VK_API_VERSION_1_2};
    VmaAllocator allocator{};
    assert(vmaCreateAllocator(&allocator_ci, &allocator) == VK_SUCCESS);
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    uint32_t type = 0;
    while (type < properties.memoryTypeCount &&
           !(properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        ++type;
    assert(type < properties.memoryTypeCount);
    VmaTotalStatistics before{}, after{};
    vmaCalculateStatistics(allocator, &before);
    {
        VideoCore::SparseBackingPool pool(allocator);
        std::vector<VmaAllocationInfo> regions;
        std::set<VkDeviceMemory> memories;
        // More suballocations than the common 4096 VkDeviceMemory limit, only 256 MiB.
        for (size_t i = 0; i < 4097; ++i) {
            VmaAllocationInfo info{};
            assert(pool.Allocate(65536, 65536, type, info) == VK_SUCCESS);
            assert(info.offset % 65536 == 0 && info.size >= 65536);
            for (const auto& old : regions) {
                if (old.deviceMemory == info.deviceMemory) {
                    assert(info.offset + 65536 <= old.offset || old.offset + 65536 <= info.offset);
                }
            }
            memories.insert(info.deviceMemory);
            regions.push_back(info);
        }
        assert(pool.AllocationCount() == 4097);
        assert(pool.Bytes() == 4097ull * 65536);
        assert(memories.size() < 64);
        bool nonzero_offset = false;
        for (const auto& region : regions) nonzero_offset |= region.offset != 0;
        assert(nonzero_offset);
        std::printf("sparse backing: 4097 aligned, non-overlapping allocations in %zu device blocks\n",
                    memories.size());
    }
    vmaCalculateStatistics(allocator, &after);
    assert(before.total.statistics.allocationCount == after.total.statistics.allocationCount);
    assert(before.total.statistics.allocationBytes == after.total.statistics.allocationBytes);
    std::puts("sparse backing: allocation ownership cleanup passed");
    vmaDestroyAllocator(allocator);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
}
