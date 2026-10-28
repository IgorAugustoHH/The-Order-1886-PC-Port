#include "bb_scene_diag.h"
// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdio>
#include <array>
#include <map>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <vector>
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <magic_enum/magic_enum.hpp>
#include "bbport_copy.h"
#include "bbport_toggles.h"
#include "common/alignment.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/completed_writeback.h"
#include "video_core/buffer_cache/deferred_readback_merge.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    while (arena_page_bits > MIN_ARENA_PAGE_BITS &&
           (u64{2} << arena_page_bits) > instance.GetMaxBufferSize()) {
        --arena_page_bits;
    }
    arena_page_size = u64{1} << arena_page_bits;
    num_arena_pages = u64{1} << (ADDRESS_SPACE_BITS - arena_page_bits);
    if (arena_page_bits != MAX_ARENA_PAGE_BITS) {
        LOG_INFO(Render_Vulkan, "Buffer arenas of {} MiB (driver buffer limit {} MiB)",
                 arena_page_size >> 20, instance.GetMaxBufferSize() >> 20);
    }
    const vk::BufferCreateInfo probe_ci = {
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = arena_page_size,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = arena_page_size / block_size;
    blocks_per_arena_page_shift = arena_page_bits - block_shift;
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();

    backing_pool = std::make_unique<SparseBackingPool>(instance.GetAllocator());
    // bbport: off by default: in run75 the game wrote those pages in the same submission the
    // shaders wrote them, before any early copy could arrive (no fewer Finish waits).
    if (const char* env = std::getenv("BB_EARLY_WRITEBACK"); env && env[0] == '1') {
        early_writeback = true;
    }
    if (const char* env = std::getenv("BB_AIM_FOCUS_SYNC"); env && env[0] == '1') {
        focus_sync_enabled = true;
    }
    // bbport: in run76 none of 18k write faults on GPU-modified pages touched GPU-written
    // bytes; each still waited for the whole GPU (Finish). Only with Relaxed readbacks: the
    // snapshot reads guest pages that Precise mode keeps read-protected.
    if (const char* env = std::getenv("BB_DEFERRED_READBACK"); env && env[0] == '0') {
        deferred_readbacks_enabled = false;
    }
    deferred_readbacks_enabled = deferred_readbacks_enabled &&
        EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Relaxed;
    std::printf("Buffer cache: pooled sparse backing (VMA), block %u KiB\n", block_size >> 10);
    std::fprintf(stderr, "Readback merge: %s; deferred=%d\n",
                 DeferredReadbackMergeFixed() ? "fixed-1cee927" : "legacy-before-1cee927",
                 int(deferred_readbacks_enabled));

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * num_arena_pages) * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * num_arena_pages);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
}

BufferCache::~BufferCache() = default;

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        ReadMemory(device_addr, size, true, assume_locks);
    });
}

// bbport: the game fills its per-frame buffers (constants, skinning output) sequentially and
// every 4 KiB page cost a protection fault (~110k/s in Hunter's Nightmare, a fifth of each
// render worker's time in the kernel). A fault unprotects the aligned window around it instead;
// pages marked CPU-modified without being written only cost an upload when bound.
// Item: context = BufferCache, source = guest address, destination = host pointer, size,
// extra = the Buffer whose mapping holds the destination (flushed after the copy).
void BufferCache::RunGuestCopy(const BbCopy::Item& item) {
    auto* cache = static_cast<BufferCache*>(item.context);
    auto* dst = reinterpret_cast<u8*>(item.destination);
    cache->memory->CopySparseMemory(item.source, dst, item.size);
    auto* buffer = reinterpret_cast<Buffer*>(item.extra);
    buffer->Flush(dst - buffer->mapped_data.data(), item.size);
}

// Small guest copies run on the recording thread (it spins for work: no wakeup, and it is
// idle most of the time); PoolSmallCopies (toggle 524288) batches them for the copy threads.
void BufferCache::SmallGuestCopy(const BbCopy::Item& item) {
    if (scheduler.IsRecordingDeferred() && !BbToggle::Disabled(BbToggle::PoolSmallCopies)) {
        scheduler.RecordHostCopy([item] { item.run(item); });
        return;
    }
    BbCopy::QueueCopy(item);
}

void BufferCache::ExtendWriteFault(VAddr device_addr) {
    static const u64 window = [] {
        const char* env = std::getenv("BB_FAULT_WINDOW");
        const u64 kib = env ? std::strtoull(env, nullptr, 10) : 256;
        return std::bit_ceil(std::clamp<u64>(kib, 4, 1024)) * 1024;
    }();
    if (window <= TRACKER_BYTES_PER_PAGE || BbToggle::Disabled(BbToggle::FaultWindow)) {
        return;
    }
    memory_tracker->ExtendWriteFault(Common::AlignDown(device_addr, window), window);
}

namespace {
// bbport: readbacks (GPU-modified memory the CPU touches) are a fifth of the GPU command
// thread's time in The Order: 1886 (7735 Finish() waits, 38.6 s in run69). Every 5 s: how many
// came from CPU writes and from CPU reads, the bytes copied back, and the busiest 1 MiB
// regions, to tell guest reads of GPU results from rewrites of buffers the GPU wrote.
struct ReadbackStats {
    std::mutex mutex;
    u64 writes = 0, reads = 0, bytes = 0;
    u64 on_gpu_bytes = 0, beside = 0; ///< CPU writes landing on GPU-written bytes or next to them
    u64 deferred = 0, deferred_bytes = 0, changed_bytes = 0; ///< see BufferCache::DeferReadback
    std::unordered_map<u64, u32> regions;
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();

    void Note(VAddr address, bool is_write) {
        std::scoped_lock lock{mutex};
        ++(is_write ? writes : reads);
        ++regions[address >> 20];
        const auto now = std::chrono::steady_clock::now();
        if (now - window < std::chrono::seconds(5)) {
            return;
        }
        std::vector<std::pair<u32, u64>> top;
        for (const auto& [region, count] : regions) {
            top.emplace_back(count, region);
        }
        std::ranges::sort(top, std::greater{});
        std::string text;
        for (size_t i = 0; i < top.size() && i < 6; ++i) {
            char item[48];
            std::snprintf(item, sizeof(item), " %#llx:%u", (unsigned long long)(top[i].second << 20),
                          top[i].first);
            text += item;
        }
        std::printf("Readbacks: %llu from CPU writes, %llu from CPU reads, %.1f MiB in 5 s; "
                    "busiest MiB:%s\n",
                    (unsigned long long)writes, (unsigned long long)reads,
                    double(bytes) / (1024.0 * 1024.0), text.c_str());
        std::printf("Readbacks: CPU writes on GPU-written bytes %llu, only on the same page %llu\n",
                    (unsigned long long)on_gpu_bytes, (unsigned long long)beside);
        std::printf("Readbacks: deferred (no GPU wait) %llu, %.1f MiB; bytes the CPU changed "
                    "before the GPU copy arrived %llu\n",
                    (unsigned long long)deferred, double(deferred_bytes) / (1024.0 * 1024.0),
                    (unsigned long long)changed_bytes);
        writes = reads = bytes = on_gpu_bytes = beside = 0;
        deferred = deferred_bytes = changed_bytes = 0;
        regions.clear();
        window = now;
    }
    void NoteOverlap(bool on_gpu) {
        std::scoped_lock lock{mutex};
        ++(on_gpu ? on_gpu_bytes : beside);
    }
    void NoteBytes(u64 n) {
        std::scoped_lock lock{mutex};
        bytes += n;
    }
    void NoteDeferred(u64 n) {
        std::scoped_lock lock{mutex};
        ++deferred;
        deferred_bytes += n;
    }
    void NoteChanged(u64 n) {
        std::scoped_lock lock{mutex};
        changed_bytes += n;
    }
};
ReadbackStats& Readbacks() {
    static ReadbackStats stats;
    return stats;
}
} // namespace

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    Readbacks().Note(device_addr, is_write);
    const auto flush_request = [this, device_addr, size, is_write] {
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);

        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        constexpr u64 WindowSize = 512_KB;
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + WindowSize, device_addr + size), arena_end);
        if (is_write) {
            const bool on_gpu = gpu_modified_ranges.Intersects(device_addr, size);
            Readbacks().NoteOverlap(on_gpu);
            // Registered pages: the newest completed copy, else the synchronized path below.
            const bool sun_page = SynchronizedPage(device_addr, size);
            const bool sun_ready = sun_page && DeliverSunCopies();
            if (!on_gpu && deferred_readbacks_enabled &&
                !BbToggle::Disabled(BbToggle::DeferredReadbacks) &&
                (!sun_page || sun_ready) &&
                DeferReadback(arena, window_start, window_end - window_start)) {
                memory_tracker->MarkRegionAsCpuModified(device_addr, size);
                return;
            }
        }
        // bbport: GPU data of earlier deferred readbacks in this window must reach guest
        // memory before this readback completes.
        WaitDeferredReadbacks(window_start, window_end - window_start);
        // bbport: early write-backs may already hold this data (see RecordWritebacks).
        ApplyWritebacks(window_start, window_end - window_start);
        DownloadMemory(arena, window_start, window_end - window_start);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    };
    if (assume_locks) {
        flush_request();
    } else {
        liverpool->SendCommand<true, true>(std::move(flush_request));
    }
}

void BufferCache::KeepPageSynchronized(VAddr device_addr, u64 size, u64 pgm_hash) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_SUN_READBACK_SYNC");
        return !(env && env[0] == '0');
    }();
    if (!enabled || device_addr == 0 || size == 0) {
        return;
    }
    std::scoped_lock lock{synchronized_mutex};
    if (synchronized_ranges.Intersects(device_addr, size)) {
        return;
    }
    synchronized_ranges.Add(device_addr, size);
    static u32 logged = 0;
    if (logged++ < 32) {
        std::printf("Readback sync: %#llx size %llu (shader %016llx)\n",
                    static_cast<unsigned long long>(device_addr), static_cast<unsigned long long>(size),
                    static_cast<unsigned long long>(pgm_hash));
    }
}

void BufferCache::RecordSunCopies() {
    std::scoped_lock lock{synchronized_mutex};
    std::vector<std::pair<VAddr, VAddr>> ranges;
    synchronized_ranges.ForEach([&](VAddr start, VAddr end) { ranges.emplace_back(start, end); });
    if (ranges.empty()) {
        return;
    }
    u64 total = 0;
    for (const auto& [start, end] : ranges) {
        total += Common::AlignUp(end - start, 16);
    }
    SunBatch batch{};
    batch.staging = staging_pool.Request(total, MemoryType::HostCached, 0, true);
    u64 offset = 0;
    for (const auto& [start, end] : ranges) {
        const u64 size = end - start;
        const auto* arena = GetArena(start >> block_shift, (end - 1) >> block_shift);
        runtime.CopyBuffer(arena, batch.staging.buffer,
                           std::array{vk::BufferCopy{start - arena->cpu_addr,
                                                     batch.staging.offset + offset, size}});
        batch.items.push_back({start, size, offset});
        offset += Common::AlignUp(size, 16);
    }
    batch.tick = scheduler.CurrentTick();
    sun_batches.push_back(std::move(batch));
    // Completed batches older than the newest completed one are no longer needed.
    while (sun_batches.size() > 1 && scheduler.IsFree(sun_batches[1].tick)) {
        staging_pool.FreeDeferred(sun_batches.front().staging);
        sun_batches.pop_front();
    }
    // A stalled GPU must not grow the queue without limit.
    while (sun_batches.size() > 16) {
        staging_pool.FreeDeferred(sun_batches.front().staging);
        sun_batches.pop_front();
    }
}

bool BufferCache::DeliverSunCopies() {
    std::scoped_lock lock{synchronized_mutex};
    const SunBatch* newest = nullptr;
    for (const auto& batch : sun_batches) {
        if (scheduler.IsFree(batch.tick)) {
            newest = &batch;
        }
    }
    if (!newest) {
        return false;
    }
    newest->staging.buffer->Invalidate(newest->staging.offset, newest->staging.size);
    for (const auto& item : newest->items) {
        memory->TryWriteBacking(reinterpret_cast<void*>(item.addr),
                                newest->staging.mapped + item.staging_offset, item.size);
    }
    return true;
}

// The 4 KiB pages of a CPU write fault, against the registered ranges.
bool BufferCache::SynchronizedPage(VAddr addr, u64 size) {
    const VAddr start = Common::AlignDown(addr, 4_KB);
    const VAddr end = Common::AlignUp(addr + size, 4_KB);
    std::scoped_lock lock{synchronized_mutex};
    return synchronized_ranges.Intersects(start, end - start);
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });
    if (total_size_bytes == 0) {
        // bbport: nothing left to copy (early write-backs delivered it): just stop tracking
        // the pages as GPU-modified, without waiting for the GPU.
        memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
        return;
    }
    Readbacks().NoteBytes(total_size_bytes);
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    runtime.CopyBuffer(arena, download.buffer, copies);
    scheduler.Finish();

    download.buffer->Invalidate(download.offset, download.size);
    for (const auto& copy : copies) {
        auto* dst_addr = std::bit_cast<u8*>(arena_base + copy.srcOffset);
        memory->TryWriteBacking(dst_addr, download.mapped + (copy.dstOffset - download.offset),
                                copy.size);
    }
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
}

void BufferCache::RecordWritebacks() {
    const bool focus_only = ExclusiveFocusFeedback(focus_sync_enabled, early_writeback);
    if (focus_only) {
        std::scoped_lock lock{writeback_mutex};
        // Feedback is small, but stalled GPUs must not grow an unlimited staging queue.
        // Leave pending ranges to be snapshotted once completed batches make room.
        if (writebacks.size() >= 64) return;
    }
    std::vector<std::pair<VAddr, u64>> ranges;
    {
        std::scoped_lock lock{frame_written_mutex};
        ranges.swap(frame_written);
    }
    if (ranges.empty()) {
        return;
    }
    std::ranges::sort(ranges);
    std::vector<std::pair<VAddr, u64>> merged;
    for (const auto& [addr, size] : ranges) {
        if (!merged.empty() && addr <= merged.back().first + merged.back().second) {
            const VAddr end = std::max(merged.back().first + merged.back().second, addr + size);
            merged.back().second = end - merged.back().first;
        } else {
            merged.emplace_back(addr, size);
        }
    }
    // Only what is still GPU-modified (a readback may have taken it), within a budget.
    constexpr u64 MaxWritebackBytes = 32_MB;
    std::vector<std::pair<VAddr, u64>> parts;
    u64 total = 0;
    for (const auto& [addr, size] : merged) {
        gpu_modified_ranges.ForEachInRange(addr, size, [&](VAddr start, VAddr end) {
            const u64 bytes = Common::AlignUp(end - start, 16);
            if (total + bytes <= MaxWritebackBytes) {
                parts.emplace_back(start, end - start);
                total += bytes;
            }
        });
    }
    if (parts.empty()) {
        return;
    }
    WritebackBatch batch{};
    batch.staging = staging_pool.Request(total, MemoryType::HostCached, 0, true);
    boost::container::small_vector<std::pair<const Buffer*, std::vector<vk::BufferCopy>>, 4> copies;
    u64 offset = 0;
    for (const auto& [addr, size] : parts) {
        const auto* arena = GetArena(addr >> block_shift, (addr + size - 1) >> block_shift);
        auto it = std::ranges::find(copies, arena, &decltype(copies)::value_type::first);
        if (it == copies.end()) {
            copies.emplace_back(arena, std::vector<vk::BufferCopy>{});
            it = copies.end() - 1;
        }
        it->second.push_back(vk::BufferCopy{
            .srcOffset = addr - arena->cpu_addr,
            .dstOffset = batch.staging.offset + offset,
            .size = size,
        });
        batch.items.push_back({addr, size, offset});
        offset += Common::AlignUp(size, 16);
    }
    for (const auto& [arena, list] : copies) {
        runtime.CopyBuffer(arena, batch.staging.buffer, list);
    }
    batch.tick = scheduler.CurrentTick(); // signalled by the flush that follows
    if (focus_only) {
        batch.recorded_at = std::chrono::steady_clock::now();
        batch.recorded_frame = BbStats::gpu_frames.load(std::memory_order_relaxed);
    }
    std::scoped_lock lock{writeback_mutex};
    writebacks.push_back(std::move(batch));
    if (focus_only) focus_latency.max_batches = std::max<u64>(focus_latency.max_batches, writebacks.size());
}

void BufferCache::PollFocusWritebacks() {
    if (ExclusiveFocusFeedback(focus_sync_enabled, early_writeback)) ApplyWritebacks();
}

void BufferCache::RecordFocusWritebacks() {
    if (ExclusiveFocusFeedback(focus_sync_enabled, early_writeback)) RecordWritebacks();
}

bool BufferCache::DeferReadback(const Buffer* arena, VAddr device_addr, u64 size) {
    std::vector<std::pair<VAddr, u64>> pages;
    memory_tracker->ForEachDownloadRange<false>(
        device_addr, size, [&](u64 address, u64 range) { pages.emplace_back(address, range); });
    std::vector<std::pair<VAddr, u64>> parts;
    u64 total = 0, snapshot_size = 0;
    for (const auto& [address, range] : pages) {
        gpu_modified_ranges.ForEachInRange(address, range, [&](VAddr start, VAddr end) {
            parts.emplace_back(start, end - start);
            total += Common::AlignUp(end - start, 64);
            snapshot_size += end - start;
        });
    }
    // Big readbacks (rare, tens of MiB) keep the waiting path: no snapshot that size.
    constexpr u64 MaxDeferredBytes = 4_MB;
    if (total > MaxDeferredBytes) {
        return false;
    }
    for (const auto& [address, range] : pages) {
        gpu_modified_ranges.Subtract(address, range);
    }
    if (total == 0) {
        memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
        return true;
    }
    DeferredBatch batch{};
    batch.staging = staging_pool.Request(total, MemoryType::HostCached, 0, true);
    batch.snapshot.resize(snapshot_size);
    std::vector<vk::BufferCopy> copies;
    u64 offset = 0, snapshot_offset = 0;
    for (const auto& [addr, part] : parts) {
        copies.push_back(vk::BufferCopy{
            .srcOffset = addr - arena->cpu_addr,
            .dstOffset = batch.staging.offset + offset,
            .size = part,
        });
        memory->CopySparseMemory(addr, batch.snapshot.data() + snapshot_offset, part);
        batch.items.push_back({addr, part, offset, snapshot_offset});
        offset += Common::AlignUp(part, 64);
        snapshot_offset += part;
    }
    runtime.CopyBuffer(arena, batch.staging.buffer, copies);
    batch.tick = scheduler.CurrentTick(); // signalled by the flush that ends this submission
    Readbacks().NoteDeferred(snapshot_size);
    {
        // Pending before the pages stop being GPU-modified: no upload in between takes them.
        std::scoped_lock lock{deferred_mutex};
        for (const auto& item : batch.items) {
            pending_readbacks.Add(item.addr, item.size);
        }
        deferred_readbacks.push_back(std::move(batch));
        has_pending_readbacks.store(true, std::memory_order_relaxed);
    }
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
    return true;
}

void BufferCache::ApplyDeferredReadbacks() {
    // Ranges whose guest bytes changed before the GPU data arrived: uploaded again from guest
    // memory (now merged), outside deferred_mutex (the tracker lock is taken under it in
    // SynchronizeMemory).
    std::vector<std::pair<VAddr, u64>> reupload;
    {
        std::scoped_lock lock{deferred_mutex};
        bool applied = false;
        while (!deferred_readbacks.empty() && scheduler.IsFree(deferred_readbacks.front().tick)) {
            DeferredBatch batch = std::move(deferred_readbacks.front());
            deferred_readbacks.pop_front();
            applied = true;
            batch.staging.buffer->Invalidate(batch.staging.offset, batch.staging.size);
            // GPU data goes where the guest left its bytes as they were at the fault.
            ApplyDeferredBatch(
                batch, deferred_readbacks,
                [&](const DeferredItem& item) { return batch.staging.mapped + item.staging_offset; },
                [&](VAddr addr, u8* dst, u64 size) { memory->CopySparseMemory(addr, dst, size); },
                [&](VAddr addr, const u8* src, u64 size) {
                    memory->TryWriteBacking(reinterpret_cast<void*>(addr), src, size);
                },
                [&](const DeferredItem& item, u64 changed) {
                    if (!changed) {
                        return;
                    }
                    Readbacks().NoteChanged(changed);
                    if (BbSceneDiag::Enabled()) BbSceneDiag::rewritten_readback_bytes += changed;
                    if (!gpu_modified_ranges.Intersects(item.addr, item.size)) {
                        reupload.emplace_back(item.addr, item.size);
                    }
                });
            staging_pool.FreeDeferred(batch.staging);
        }
        if (applied) {
            pending_readbacks.Clear();
            for (const auto& batch : deferred_readbacks) {
                for (const auto& item : batch.items) {
                    pending_readbacks.Add(item.addr, item.size);
                }
            }
            has_pending_readbacks.store(!deferred_readbacks.empty(), std::memory_order_relaxed);
        }
    }
    for (const auto& [addr, size] : reupload) {
        memory_tracker->MarkRegionAsCpuModified(addr, size);
    }
}

void BufferCache::WaitDeferredReadbacks(VAddr addr, u64 size) {
    if (!has_pending_readbacks.load(std::memory_order_relaxed)) {
        return;
    }
    u64 needed = 0;
    {
        std::scoped_lock lock{deferred_mutex};
        for (const auto& batch : deferred_readbacks) {
            for (const auto& item : batch.items) {
                if (addr < item.addr + item.size && item.addr < addr + size) {
                    needed = std::max(needed, batch.tick);
                }
            }
        }
    }
    if (!needed) {
        return;
    }
    if (!scheduler.IsFree(needed)) {
        if (needed >= scheduler.CurrentTick()) {
            scheduler.Finish(); // still being recorded
        } else {
            scheduler.Wait(needed);
        }
    }
    ApplyDeferredReadbacks();
}

bool BufferCache::PendingReadback(VAddr addr, u64 size) {
    if (!has_pending_readbacks.load(std::memory_order_relaxed)) {
        return false;
    }
    std::scoped_lock lock{deferred_mutex};
    return pending_readbacks.Intersects(addr, size);
}

bool BufferCache::WrittenAgain(VAddr addr, u64 size) {
    // Batches still queued are newer than the one being applied (oldest first).
    for (const auto& batch : writebacks) {
        for (const auto& item : batch.items) {
            if (addr < item.addr + item.size && item.addr < addr + size) {
                return true;
            }
        }
    }
    std::scoped_lock lock{frame_written_mutex};
    for (const auto& [written, written_size] : frame_written) {
        if (addr < written + written_size && written < addr + size) {
            return true;
        }
    }
    return false;
}

void BufferCache::ApplyWritebacks(VAddr wait_addr, u64 wait_size) {
    const bool focus_sync = focus_sync_enabled;
    if (!early_writeback && !focus_sync) {
        return;
    }
    // Without general early writebacks this queue contains only the strictly
    // selected 1x1 focus-copy outputs. Do not relax resource freshness in mixed queues.
    const bool feedback_only = ExclusiveFocusFeedback(focus_sync, early_writeback);
    std::scoped_lock lock{writeback_mutex};
    if (wait_size) {
        u64 needed = 0;
        for (const auto& batch : writebacks) {
            for (const auto& item : batch.items) {
                if (wait_addr < item.addr + item.size && item.addr < wait_addr + wait_size) {
                    needed = std::max(needed, batch.tick);
                }
            }
        }
        // Submitted already: waiting for it is much shorter than Finish (which also submits
        // and waits for everything recorded since).
        if (needed && needed < scheduler.CurrentTick() && !scheduler.IsFree(needed)) {
            scheduler.Wait(needed);
        }
    }
    while (!writebacks.empty() && scheduler.IsFree(writebacks.front().tick)) {
        WritebackBatch batch = std::move(writebacks.front());
        writebacks.pop_front();
        batch.staging.buffer->Invalidate(batch.staging.offset, batch.staging.size);
        bool delivered_batch = false;
        const auto delivered_at = feedback_only ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
        const double age_ms = feedback_only ? std::chrono::duration<double, std::milli>(
            delivered_at - batch.recorded_at).count() : 0.0;
        const u64 frame = BbStats::gpu_frames.load(std::memory_order_relaxed);
        const u64 age_frames = frame >= batch.recorded_frame ? frame - batch.recorded_frame : 0;
        for (const auto& item : batch.items) {
            std::vector<std::pair<VAddr, VAddr>> still;
            gpu_modified_ranges.ForEachInRange(
                item.addr, item.size, [&](VAddr start, VAddr end) { still.emplace_back(start, end); });
            for (const auto& [start, end] : still) {
                const bool written_again = WrittenAgain(start, end - start);
                if (!DeliverCompletedWritebackRange(feedback_only, written_again, start, end - start,
                    batch.staging.mapped + item.staging_offset + (start - item.addr),
                    [this](VAddr address, const u8* bytes, u64 size) {
                        memory->TryWriteBacking(reinterpret_cast<void*>(address), bytes, size);
                    },
                    [this](VAddr address, u64 size) { gpu_modified_ranges.Subtract(address, size); })) {
                    continue;
                }
                delivered_batch = true;
                if (focus_sync && !early_writeback && start == item.addr && end - start >= 4) {
                    static u32 logged = 0;
                    static u32 last_bits = 0;
                    static auto last_log = std::chrono::steady_clock::time_point{};
                    u32 bits;
                    std::memcpy(&bits, batch.staging.mapped + item.staging_offset, sizeof(bits));
                    const auto now = std::chrono::steady_clock::now();
                    if (logged < 64 && (logged < 4 || now - last_log >= std::chrono::seconds(5) ||
                        (bits != last_bits && now - last_log >= std::chrono::seconds(1)))) {
                        ++logged;
                        last_log = now;
                        float value;
                        std::memcpy(&value, batch.staging.mapped + item.staging_offset, sizeof(value));
                        std::printf("Aim focus return: %#llx bytes %llu first_float %.9g tick %llu newer_pending %u age_ms %.3f age_frames %llu\n",
                            static_cast<unsigned long long>(start),
                            static_cast<unsigned long long>(end - start), value,
                            static_cast<unsigned long long>(batch.tick), unsigned(written_again), age_ms,
                            static_cast<unsigned long long>(age_frames));
                    }
                    last_bits = bits;
                }
            }
        }
        if (feedback_only && delivered_batch) {
            ++focus_latency.samples;
            focus_latency.ms_sum += age_ms;
            focus_latency.max_ms = std::max(focus_latency.max_ms, age_ms);
            focus_latency.frames_sum += age_frames;
            focus_latency.max_frames = std::max(focus_latency.max_frames, age_frames);
        }
        staging_pool.FreeDeferred(batch.staging);
    }
    if (feedback_only) {
        const auto now = std::chrono::steady_clock::now();
        if (now - focus_latency.window >= std::chrono::seconds(5)) {
            const double count = std::max<u64>(1, focus_latency.samples);
            std::printf("Aim focus latency: samples %llu record_to_publish_ms mean %.3f max %.3f frames_mean %.3f frames_max %llu pending_batches %llu max_batches %llu\n",
                static_cast<unsigned long long>(focus_latency.samples), focus_latency.ms_sum / count,
                focus_latency.max_ms, focus_latency.frames_sum / count,
                static_cast<unsigned long long>(focus_latency.max_frames),
                static_cast<unsigned long long>(writebacks.size()),
                static_cast<unsigned long long>(focus_latency.max_batches));
            focus_latency = {};
            focus_latency.window = now;
        }
    }
}

namespace {
// bbport: BB_BUFFER_STATS=1 — how buffer bindings reach the GPU, by guest region (256 MiB),
// printed every 5 s: small read-only copies into the stream buffer, arena bindings, and the
// bytes those re-upload after CPU writes. Input for engine-level short paths.
struct BufferStats {
    struct Region {
        u64 stream_count{}, stream_bytes{}, arena_count{}, arena_bytes{}, upload_bytes{};
    };
    std::map<u64, Region> regions;
    /// Hot window: frame of the last binding per 64 KiB block, and the distribution of the
    /// frames between uses (1, 2, 3, 4, 5-8, 9-16, 17+): the ring's reuse distance.
    std::unordered_map<u64, u64> last_use;
    std::array<u64, 7> reuse{};
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    u64 calls = 0;
};
/// 256 MiB regions; 1 MiB inside the hot 0x104xxxxxxx window (the engine's frame data).
BufferStats& Stats();
void NoteHotUse(VAddr address, u32 size) {
    if ((address >> 28) != 0x104) {
        return;
    }
    auto& st = Stats();
    const u64 frame = BbStats::gpu_frames.load(std::memory_order_relaxed);
    for (u64 block = address >> 16; block <= (address + size - 1) >> 16; ++block) {
        auto [it, inserted] = st.last_use.try_emplace(block, frame);
        if (!inserted && it->second != frame) {
            const u64 d = frame - it->second;
            ++st.reuse[d <= 4 ? d - 1 : d <= 8 ? 4 : d <= 16 ? 5 : 6];
            it->second = frame;
        }
    }
}
u64 RegionKey(VAddr address) {
    return (address >> 28) == 0x104 ? (address >> 20) | (1ull << 40) : address >> 28;
}
bool BufferStatsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_BUFFER_STATS");
        return value && value[0] == '1';
    }();
    return enabled;
}
BufferStats& Stats() {
    static BufferStats stats;
    return stats;
}
void PrintBufferStats() {
    auto& st = Stats();
    if ((++st.calls & 4095) != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - st.window).count();
    if (seconds < 5.0) {
        return;
    }
    std::printf("Buffer stats (%.1f s, per second): region, stream copies/bytes, arena bindings, "
                "re-uploaded bytes\n", seconds);
    std::printf("  hot window reuse distance in frames (1,2,3,4,5-8,9-16,17+): %llu %llu %llu %llu "
                "%llu %llu %llu\n",
                (unsigned long long)st.reuse[0], (unsigned long long)st.reuse[1],
                (unsigned long long)st.reuse[2], (unsigned long long)st.reuse[3],
                (unsigned long long)st.reuse[4], (unsigned long long)st.reuse[5],
                (unsigned long long)st.reuse[6]);
    st.reuse = {};
    for (const auto& [region, r] : st.regions) {
        const u64 base = (region >> 40) ? (region & ((1ull << 40) - 1)) << 20 : region << 28;
        if ((region >> 40) && r.stream_bytes + r.upload_bytes < 1e6 * seconds) {
            continue; // quiet MiB of the hot window
        }
        std::printf("  %#012llx: %8.0f copies %8.2f MB, %8.0f arena %8.2f MB bound, %8.2f MB uploads\n",
                    static_cast<unsigned long long>(base), r.stream_count / seconds,
                    r.stream_bytes / seconds / 1e6, r.arena_count / seconds,
                    r.arena_bytes / seconds / 1e6,
                    r.upload_bytes / seconds / 1e6);
    }
    st.regions.clear();
    st.window = now;
}
} // namespace

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer,
                                                        bool focus_copy) {
    // bbport: garbage V#s (shader resource tables the recompiler could not resolve, The Order:
    // 1886) ask for 2-4 GiB ranges; synchronizing them exhausts VRAM and system RAM. No real
    // PS4 buffer binding is that large: synchronize at most 256 MiB.
    constexpr u32 MaxBinding = 256_MB;
    if (size > MaxBinding) {
        static std::atomic<int> warned{0};
        if (warned.fetch_add(1) < 8) {
            LOG_WARNING(Render, "Buffer binding of {} MiB at {:#x} limited to {} MiB", size >> 20,
                        device_addr, MaxBinding >> 20);
        }
        size = MaxBinding;
    }
    const bool stats = BufferStatsEnabled();
    if (stats) {
        PrintBufferStats();
        NoteHotUse(device_addr, size);
    }
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    // The Order's focus copy reads a rendered scalar image as an ordinary buffer.
    // Only its caller opts in. Validate the image before using the image->buffer path.
    const bool focus_image = focus_copy && !is_written && IsFocusCopySource(device_addr, size);
    is_texel_buffer |= focus_image;
    if (focus_image && Vulkan::FrameCapture::Active()) {
        const auto id = texture_cache.FindImageFromRange(device_addr, size);
        if (id) {
            if (const auto path = Vulkan::FrameCapture::FocusScalarPath(texture_cache.GetImage(id).info, id.index);
                !path.empty()) texture_cache.DumpFocusScalar(id, path);
        }
    }
    if (!is_written && !focus_image && size <= STREAM_THRESHOLD && !IsRegionGpuModified(device_addr, size)) {
        if (stats) {
            auto& region = Stats().regions[RegionKey(device_addr)];
            ++region.stream_count;
            region.stream_bytes += size;
        }
        // bbport: the guest data is copied on a copy thread, started now; submission and
        // guest-visible fences wait for it (Scheduler::WaitHostCopies).
        if (!stream_buffer.mapped_data.empty() &&
            !BbToggle::Disabled(BbToggle::DeferredStreamCopies)) {
            if (const auto offset = stream_buffer.Reserve(size, instance.UniformMinAlignment())) {
                SmallGuestCopy({
                    .run = &RunGuestCopy,
                    .context = this,
                    .source = device_addr,
                    .destination = reinterpret_cast<u64>(stream_buffer.mapped_data.data() + *offset),
                    .size = size,
                    .extra = reinterpret_cast<u64>(static_cast<Buffer*>(&stream_buffer)),
                });
                return {&stream_buffer, *offset};
            }
        }
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        return {&stream_buffer, offset};
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block);
    const u64 uploaded_before = BbStats::buffer_upload_bytes.load(std::memory_order_relaxed);
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    if (focus_image) {
        static u32 logged = 0;
        if (logged++ < 12)
            std::printf("Aim focus copy: scalar source %#llx bytes %u image-to-buffer enabled\n",
                        static_cast<unsigned long long>(device_addr), size);
    }
    if (stats) {
        auto& region = Stats().regions[RegionKey(device_addr)];
        ++region.arena_count;
        region.arena_bytes += size;
        region.upload_bytes +=
            BbStats::buffer_upload_bytes.load(std::memory_order_relaxed) - uploaded_before;
    }
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
        // bbport: copied back at the end of the submission (RecordWritebacks); bigger
        // bindings are left to on-demand readbacks.
        constexpr u64 MaxWritebackRange = 4_MB;
        if ((early_writeback && size <= MaxWritebackRange) || (focus_copy && size <= 64_KB)) {
            std::scoped_lock lock{frame_written_mutex};
            frame_written.emplace_back(device_addr, size);
        }
    }
    return {arena, arena->Offset(device_addr)};
}

bool BufferCache::IsFocusCopySource(VAddr device_addr, u32 size) {
    if (!device_addr || !size || size > 64_KB) return false;
    const ImageId id = texture_cache.FindImageFromRange(device_addr, size);
    if (!id) return false;
    const auto& info = texture_cache.GetImage(id).info;
    return info.guest_address == device_addr &&
           info.pixel_format == vk::Format::eR32Sfloat &&
           info.size.width == 1 && info.size.height == 1 && info.size.depth == 1 &&
           info.num_samples == 1 && info.resources.levels == 1 &&
           info.resources.layers == 1 && info.guest_size <= 64_KB;
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    if (!BbToggle::Disabled(BbToggle::DeferredUploads)) {
        // bbport: texture data is copied on the copy threads (streaming: 100+ MB per frame);
        // the upload reads the staging only after submission, which waits for the copies.
        constexpr u64 Chunk = 1_MB;
        for (u64 offset = 0; offset < staging.size; offset += Chunk) {
            const BbCopy::Item item{
                .run = &RunGuestCopy,
                .context = this,
                .source = device_addr + offset,
                .destination = reinterpret_cast<u64>(staging.mapped + offset),
                .size = std::min(Chunk, staging.size - offset),
                .extra = reinterpret_cast<u64>(staging.buffer),
            };
            if (staging.size < Chunk) {
                SmallGuestCopy(item);
            } else {
                BbCopy::Async([item] { item.run(item); });
            }
        }
        return {staging.buffer, staging.offset};
    }
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    // bbport: bytes of a deferred readback are only up to date in the arena.
    return memory_tracker->IsRegionGpuModified(addr, size) || PendingReadback(addr, size);
}

void BufferCache::ProcessFaultBuffer() {
    fault_manager->ProcessFaultBuffer();
}

void BufferCache::SynchronizeDmaBuffers() {
    static const bool skip_clean = [] {
        // bbport: on by default (BB_DMA_CLEAN_SKIP=0: off).
        const char* env = std::getenv("BB_DMA_CLEAN_SKIP");
        return !(env && env[0] == '0');
    }();
    static const bool trace = [] {
        const char* env = std::getenv("BB_DMA_TRACE");
        return env && env[0] == '1';
    }();
    // bbport: the walk below only uploads CPU-modified pages and leaves none behind, so
    // while no page became CPU-modified (and no range became resident) since the last full
    // walk it would find nothing. ~600 shaders use DMA: this runs for most draws, and the
    // walk visits every resident range (BB_DMA_SYNC_GENERATION=0: always walk).
    static const bool skip_unchanged = [] {
        const char* env = std::getenv("BB_DMA_SYNC_GENERATION");
        return !(env && env[0] == '0');
    }();
    const auto begin = trace ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
    // Read before the walk: a page marked during it bumps the generation again.
    const u64 generation = cpu_modified_generation.load(std::memory_order_acquire);
    const bool unchanged = skip_clean && skip_unchanged && generation == dma_synced_generation;
    u64 visited_ranges = 0, visited_bytes = 0, clean_ranges = 0;
    for (const auto& range : resident_ranges) {
        if (unchanged) {
            break;
        }
        const u64 page = range.start >> (arena_page_bits - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        // Same read-only dirty predicate already used inside ForEachUploadRange.
        // Avoid constructing the upload machinery for an entirely clean range.
        // Do not cache this result across draws: CPU writes must remain visible.
        const bool clean = skip_clean &&
            !BbToggle::Disabled(BbToggle::LockFreeUploadCheck) &&
            !memory_tracker->IsRegionCpuModified(device_addr, size);
        if (!clean) {
            SynchronizeMemory(address_space[page], device_addr, size, false, false);
        }
        if (trace) {
            clean_ranges += clean;
            ++visited_ranges;
            visited_bytes += size;
        }
    }
    dma_synced_generation = generation;
    if (trace) {
        static auto last = begin;
        static u64 calls = 0, ranges = 0, bytes = 0, skipped = 0, unchanged_calls = 0;
        static double time_ms = 0;
        const auto now = std::chrono::steady_clock::now();
        ++calls;
        unchanged_calls += unchanged;
        ranges += visited_ranges;
        bytes += visited_bytes;
        skipped += clean_ranges;
        time_ms += std::chrono::duration<double, std::milli>(now - begin).count();
        if (now - last >= std::chrono::seconds(5)) {
            std::fprintf(stderr, "DMA sync: %llu calls, %llu range visits, %.1f MiB examined (not uploaded), %.2f ms CPU\n",
                static_cast<unsigned long long>(calls), static_cast<unsigned long long>(ranges),
                double(bytes) / 1048576.0, time_ms);
            std::fprintf(stderr, "DMA clean skip: %llu of %llu range visits; %llu calls without new CPU writes\n",
                static_cast<unsigned long long>(skipped), static_cast<unsigned long long>(ranges),
                static_cast<unsigned long long>(unchanged_calls));
            calls = ranges = bytes = unchanged_calls = 0;
            time_ms = 0;
            skipped = 0;
            last = now;
        }
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const auto* first_arena = address_space[first_page];
    const auto* last_arena = address_space[last_page];
    if (first_arena == last_arena) {
        if (!first_arena) {
            const u64 base_block = Common::AlignDownPow2<u64>(first_block, blocks_per_arena_page);
            const u64 num_pages = last_page - first_page + 1;
            const auto* new_arena =
                &arenas.emplace_back(instance, base_block << block_shift,
                                     num_pages << arena_page_bits, MemoryType::Sparse);
            address_space[first_page] = new_arena;
            address_space[last_page] = new_arena;
        }
        return address_space[first_page];
    }

    LOG_WARNING(Render, "Migrating arena");

    const u64 first_addr = first_arena ? first_arena->cpu_addr : (first_page << arena_page_bits);
    const u64 first_size = first_arena ? first_arena->size_bytes : arena_page_size;
    const u64 last_size = last_arena ? last_arena->size_bytes : arena_page_size;

    const u64 base_block = first_addr >> block_shift;
    const u64 total_size = first_size + last_size;
    const u64 end_block = (first_addr + total_size) >> block_shift;
    auto* new_arena = &arenas.emplace_back(instance, first_addr, total_size, MemoryType::Sparse);
    auto* bind = BindsForArena(new_arena);
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = backing.offset + ((start - backing.start) << block_shift),
        });
    });

    u64 base_page = first_addr >> arena_page_bits;
    for (u32 page = 0; page < (first_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    base_page = last_page;
    for (u32 page = 0; page < (last_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    return new_arena;
}

std::atomic<u64> g_bb_resident_buffer_bytes{0};

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        return;
    }
    BbStats::Timer timer{BbStats::t_resident};

    const u64 bytes = u64{resident_blocks} << block_shift;
    VmaAllocationInfo allocation_info{};
    auto result = backing_pool->Allocate(bytes, block_size, arena_memory_type_index,
                                         allocation_info);
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        // Image destruction is deferred until submitted work completes. Drain it once
        // before retrying; never switch to a host-memory heap on the 16 GB machine.
        scheduler.Finish();
        scheduler.PopPendingOperations();
        result = backing_pool->Allocate(bytes, block_size, arena_memory_type_index,
                                        allocation_info);
    }
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "Sparse backing allocation failed: request=%llu KiB, "
                     "resident=%llu MiB, suballocations=%zu, heap=%llu MiB, budget=%llu MiB\n",
                     (unsigned long long)(bytes >> 10),
                     (unsigned long long)(backing_pool->Bytes() >> 20),
                     backing_pool->AllocationCount(),
                     (unsigned long long)(instance.GetDeviceMemoryUsage() >> 20),
                     (unsigned long long)(instance.GetDeviceMemoryBudgetNow() >> 20));
    }
    Vulkan::Check(vk::Result{result});
    const vk::DeviceMemory device_memory{allocation_info.deviceMemory};
    // Count only successful allocations; offsets below include the VMA suballocation.
    {
        const u64 total = g_bb_resident_buffer_bytes.fetch_add(bytes) + bytes;
        static std::atomic<int> big_logged{0};
        if (bytes >= 32_MB && big_logged.fetch_add(1) < 24) {
            std::printf("Buffer cache: %llu MiB made resident at %#llx (request %#llx-%#llx), "
                        "%llu MiB in total\n",
                        (unsigned long long)(bytes >> 20),
                        (unsigned long long)(first_block << block_shift),
                        (unsigned long long)(first_block << block_shift),
                        (unsigned long long)((last_block + 1) << block_shift),
                        (unsigned long long)(total >> 20));
        }
    }

    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(resident_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);

    u64 memory_offset = allocation_info.offset;
    ArenaBinds* binds = BindsForArena(arena);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    for (const auto& range : bind_ranges) {
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = device_memory;
        backing.offset = memory_offset;
        backing.block_shift = block_shift;
        resident_ranges.Add(backing);
        BumpCpuModifiedGeneration(); // the next DMA sync walks the new range

        LOG_INFO(Render, "Making range start={}, end={} resident", backing.start, backing.end);

        const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (range.start << block_shift) - arena->cpu_addr,
            .size = (range.end - range.start) << block_shift,
            .memory = device_memory,
            .memoryOffset = memory_offset,
        });
        memory_offset += bind.size;

        for (u32 block = 0; block < bind.size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + bind.resourceOffset + block;
        }
        const u64 copy_size = (backing.end - backing.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, backing.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    const Buffer* src_buffer{};
    const auto add_copy = [&](u64 addr, u64 size) {
        copies.emplace_back(total_size_bytes, addr, size);
        total_size_bytes += size;
    };
    memory_tracker->ForEachUploadRange(
        device_addr, size, is_written,
        [&](u64 addr, u64 size) {
            // bbport: the guest's copy of deferred readback bytes is stale until it arrives;
            // the arena keeps them.
            if (!has_pending_readbacks.load(std::memory_order_relaxed)) {
                add_copy(addr, size);
                return;
            }
            std::scoped_lock lock{deferred_mutex};
            pending_readbacks.ForEachNotInRange(addr, size, add_copy);
            // Pending bytes the guest wrote after the fault are newer than the arena's GPU
            // data: skipping them left the arena half new CPU data, half old GPU data until
            // the readback arrived (ApplyDeferredReadbacks keeps these bytes in guest memory).
            ForEachRewrittenPending(
                deferred_readbacks, addr, size,
                [&](VAddr src, u8* dst, u64 n) { memory->CopySparseMemory(src, dst, n); },
                add_copy);
        },
        [&] { src_buffer = UploadCopies(arena, copies, total_size_bytes); });

    if (src_buffer) {
        runtime.CopyBuffer(src_buffer, arena, copies);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

const Buffer* BufferCache::UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                                        size_t total_size_bytes) {
    if (copies.empty()) {
        return nullptr;
    }
    // bbport: a raw buffer over a whole mapping (The Order: 1886 binds 4 GiB ranges) would need
    // one staging allocation of that size. Upload it in batches of at most 64 MiB, each copied
    // to the arena here; nullptr tells the caller there is nothing left to copy.
    constexpr u64 MaxStaging = 64_MB;
    if (total_size_bytes > MaxStaging) {
        std::vector<vk::BufferCopy> batch;
        u64 batch_bytes = 0;
        const auto flush = [&] {
            if (const Buffer* staging = UploadCopies(arena, batch, batch_bytes)) {
                runtime.CopyBuffer(staging, arena, batch);
            }
            batch.clear();
            batch_bytes = 0;
        };
        for (const auto& copy : copies) {
            for (u64 done = 0; done < copy.size;) {
                const u64 part = std::min<u64>(copy.size - done, MaxStaging - batch_bytes);
                batch.emplace_back(batch_bytes, copy.dstOffset + done, part);
                batch_bytes += part;
                done += part;
                if (batch_bytes == MaxStaging) {
                    flush();
                }
            }
        }
        if (!batch.empty()) {
            flush();
        }
        return nullptr;
    }
    BbStats::buffer_upload_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
    if (BbSceneDiag::Enabled()) BbSceneDiag::buffer_bytes += total_size_bytes;
    const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
    // bbport: the guest memory is copied into staging on the copy threads, started now in
    // groups of about 1 MiB; submission and guest-visible fences wait for them
    // (Scheduler::WaitHostCopies).
    if (!BbToggle::Disabled(BbToggle::DeferredUploads) && total_size_bytes < 1_MB) {
        // Small uploads join the calling thread's batch.
        for (auto& copy : copies) {
            SmallGuestCopy({
                .run = &RunGuestCopy,
                .context = this,
                .source = copy.dstOffset,
                .destination = reinterpret_cast<u64>(staging.mapped + copy.srcOffset),
                .size = copy.size,
                .extra = reinterpret_cast<u64>(staging.buffer),
            });
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        return staging.buffer;
    }
    if (!BbToggle::Disabled(BbToggle::DeferredUploads)) {
        struct HostCopy {
            VAddr source;
            u8* destination;
            u64 size;
        };
        using Group = boost::container::small_vector<HostCopy, 8>;
        auto group = std::make_shared<Group>();
        u64 group_bytes = 0;
        const auto launch = [&] {
            BbCopy::Async([group, memory = memory, staging] {
                for (const auto& copy : *group) {
                    memory->CopySparseMemory(copy.source, copy.destination, copy.size);
                }
                staging.Flush();
            });
        };
        for (auto& copy : copies) {
            group->push_back({copy.dstOffset, staging.mapped + copy.srcOffset, copy.size});
            group_bytes += copy.size;
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
            if (group_bytes >= 1_MB) {
                launch();
                group = std::make_shared<Group>();
                group_bytes = 0;
            }
        }
        if (!group->empty()) {
            launch();
        }
        return staging.buffer;
    }
    const auto copy = [&](std::size_t i) {
        memory->CopySparseMemory(copies[i].dstOffset, staging.mapped + copies[i].srcOffset,
                                 copies[i].size);
    };
    if (total_size_bytes >= 2_MB && copies.size() > 1) {
        BbCopy::ParallelFor(copies.size(), copy);
    } else {
        for (std::size_t i = 0; i < copies.size(); ++i) {
            copy(i);
        }
    }
    for (auto& copy : copies) {
        copy.srcOffset += staging.offset;
        copy.dstOffset -= arena->cpu_addr;
    }
    staging.Flush();
    return staging.buffer;
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}", magic_enum::enum_name(*type));
        }
    }
    // bbport: most texel buffers alias no image; remember misses until images change.
    const u64 generation = texture_cache.RegistryGeneration();
    auto& miss = image_miss_cache[((device_addr >> 6) ^ size * 0x9E3779B1u) % image_miss_cache.size()];
    if (miss.address == device_addr && miss.size == size && miss.generation == generation &&
        !BbToggle::Disabled(BbToggle::TextureBindingMemo)) {
        return false;
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        miss = {device_addr, size, generation};
        return false;
    }
    // bbport: the lookups above only read what the texture binding helper leaves alone; the
    // copy below changes image state, so the helper finishes first.
    runtime.BeforeImageAccess();
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    return true;
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    auto submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    pending_binds.clear();
}

} // namespace VideoCore
