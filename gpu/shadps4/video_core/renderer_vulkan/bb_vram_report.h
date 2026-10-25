// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// bbport: where the GPU memory goes (The Order: 1886 fills a 6 GB card; half of the heap was
// neither registered images nor sparse buffers). Images are tagged by kind when allocated;
// the report adds the allocator's statistics per heap, pipeline counts and big allocations.

#include <atomic>
#include <cstdint>
#include "video_core/renderer_vulkan/vk_common.h"
#include <vk_mem_alloc.h>

namespace Vulkan {
class Instance;
}

namespace BbVram {

enum Kind : int { Texture, Depth, Msaa, NumKinds };

void SetInstance(const Vulkan::Instance* instance);

/// Image allocated (sign +1) or about to be freed (-1); `ci` only for +1.
void NoteImage(VmaAllocator allocator, VmaAllocation allocation, int sign,
               const VkImageCreateInfo* ci = nullptr);

inline std::atomic<int64_t> graphics_pipelines{0};
inline std::atomic<int64_t> compute_pipelines{0};

/// Staging pool totals per VideoCore::MemoryType (DeviceLocal, HostUncached, HostCached).
inline std::atomic<uint64_t> staging_bytes[3]{};
inline std::atomic<uint64_t> staging_large[3]{};

/// One "VRAM:" line (called beside the frame statistics).
void Print();

} // namespace BbVram
