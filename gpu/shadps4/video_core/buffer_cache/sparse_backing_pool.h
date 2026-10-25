// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <vector>
#include <vk_mem_alloc.h>

namespace VideoCore {

// Sparse bindings need memory + offset, not a dedicated VkDeviceMemory per gap.
// VMA suballocates aligned regions and accounts for them in the same budget as images.
class SparseBackingPool {
public:
    explicit SparseBackingPool(VmaAllocator allocator_) : allocator{allocator_} {}
    ~SparseBackingPool() {
        for (auto allocation : allocations) {
            vmaFreeMemory(allocator, allocation);
        }
    }
    SparseBackingPool(const SparseBackingPool&) = delete;
    SparseBackingPool& operator=(const SparseBackingPool&) = delete;

    VkResult Allocate(uint64_t size, uint64_t alignment, uint32_t memory_type,
                      VmaAllocationInfo& info) {
        const VkMemoryRequirements requirements{size, alignment, uint32_t{1} << memory_type};
        const VmaAllocationCreateInfo create_info{
            .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
            .usage = VMA_MEMORY_USAGE_UNKNOWN,
            .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        };
        VmaAllocation allocation{};
        const auto result = vmaAllocateMemory(allocator, &requirements, &create_info,
                                              &allocation, &info);
        if (result == VK_SUCCESS) {
            allocations.push_back(allocation);
            bytes += size;
        }
        return result;
    }

    uint64_t Bytes() const { return bytes; }
    size_t AllocationCount() const { return allocations.size(); }

private:
    VmaAllocator allocator;
    std::vector<VmaAllocation> allocations;
    uint64_t bytes{};
};
} // namespace VideoCore
