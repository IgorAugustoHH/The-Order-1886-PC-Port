// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <string>
#include "video_core/renderer_vulkan/bb_vram_report.h"
#include "video_core/renderer_vulkan/vk_instance.h"

namespace BbVram {

namespace {
const Vulkan::Instance* g_instance = nullptr;
// [kind][0 = device-local memory, 1 = system memory]
std::atomic<int64_t> g_bytes[NumKinds][2]{};
std::atomic<int64_t> g_count[NumKinds][2]{};
std::atomic<int> g_big_logged{0};

int KindOf(const VkImageCreateInfo& ci) {
    if (ci.samples != VK_SAMPLE_COUNT_1_BIT) {
        return Msaa;
    }
    switch (ci.format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_S8_UINT:
        return Depth;
    default:
        return Texture;
    }
}

bool IsDeviceLocal(VmaAllocator allocator, uint32_t memory_type) {
    VkMemoryPropertyFlags flags{};
    vmaGetMemoryTypeProperties(allocator, memory_type, &flags);
    return (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
}

double MiB(int64_t bytes) {
    return double(bytes) / (1024.0 * 1024.0);
}
} // namespace

void SetInstance(const Vulkan::Instance* instance) {
    g_instance = instance;
}

void NoteImage(VmaAllocator allocator, VmaAllocation allocation, int sign,
               const VkImageCreateInfo* ci) {
    if (!allocator || !allocation) {
        return;
    }
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(allocator, allocation, &info);
    int kind;
    if (sign > 0 && ci) {
        kind = KindOf(*ci);
        vmaSetAllocationUserData(allocator, allocation,
                                 reinterpret_cast<void*>(static_cast<uintptr_t>(kind + 1)));
    } else {
        const auto tag = reinterpret_cast<uintptr_t>(info.pUserData);
        if (tag == 0 || tag > NumKinds) {
            return; // not counted when allocated
        }
        kind = int(tag - 1);
    }
    const int where = IsDeviceLocal(allocator, info.memoryType) ? 0 : 1;
    g_bytes[kind][where] += sign * int64_t(info.size);
    g_count[kind][where] += sign;

    if (sign > 0 && ci && (info.size >= (64ull << 20) || where == 1) &&
        g_big_logged.fetch_add(1) < 40) {
        std::printf("VRAM: %s image %ux%ux%u, %u mips, %u layers, %u samples, format %d: "
                    "%.1f MiB\n",
                    where ? "SYSTEM-MEMORY" : "big", ci->extent.width, ci->extent.height,
                    ci->extent.depth, ci->mipLevels, ci->arrayLayers, unsigned(ci->samples),
                    int(ci->format), MiB(int64_t(info.size)));
    }
}

void Print() {
    if (!g_instance) {
        return;
    }
    const VmaAllocator allocator = g_instance->GetAllocator();
    const VkPhysicalDeviceMemoryProperties* props = nullptr;
    vmaGetMemoryProperties(allocator, &props);
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(allocator, budgets);
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(allocator, &stats);

    std::string heaps;
    for (uint32_t heap = 0; heap < props->memoryHeapCount; ++heap) {
        const bool device = props->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
        const auto& s = stats.memoryHeap[heap].statistics;
        const int64_t usage = int64_t(budgets[heap].usage);
        if (usage == 0 && s.blockBytes == 0) {
            continue;
        }
        char text[256];
        std::snprintf(text, sizeof(text),
                      " [heap %u %s: usage %.0f / budget %.0f MiB; allocator blocks %.0f MiB "
                      "(%u), used %.0f MiB (%u allocations); outside the allocator %.0f MiB]",
                      heap, device ? "VRAM" : "RAM", MiB(usage), MiB(int64_t(budgets[heap].budget)),
                      MiB(int64_t(s.blockBytes)), s.blockCount, MiB(int64_t(s.allocationBytes)),
                      s.allocationCount, MiB(usage - int64_t(s.blockBytes)));
        heaps += text;
    }
    static const char* const names[NumKinds] = {"textures", "depth", "MSAA"};
    std::string images;
    for (int kind = 0; kind < NumKinds; ++kind) {
        char text[160];
        std::snprintf(text, sizeof(text), " %s %.0f MiB (%lld)", names[kind],
                      MiB(g_bytes[kind][0].load()), (long long)g_count[kind][0].load());
        images += text;
        if (g_count[kind][1].load() != 0) {
            std::snprintf(text, sizeof(text), " + %.0f MiB (%lld) IN SYSTEM MEMORY",
                          MiB(g_bytes[kind][1].load()), (long long)g_count[kind][1].load());
            images += text;
        }
    }
    std::printf("VRAM: host images:%s; staging VRAM %.0f MiB (%llu large), upload %.0f MiB "
                "(%llu large), readback %.0f MiB (%llu large); pipelines %lld graphics, "
                "%lld compute;%s\n",
                images.c_str(), MiB(int64_t(staging_bytes[0].load())),
                (unsigned long long)staging_large[0].load(), MiB(int64_t(staging_bytes[1].load())),
                (unsigned long long)staging_large[1].load(), MiB(int64_t(staging_bytes[2].load())),
                (unsigned long long)staging_large[2].load(), (long long)graphics_pipelines.load(),
                (long long)compute_pipelines.load(), heaps.c_str());
}

} // namespace BbVram
