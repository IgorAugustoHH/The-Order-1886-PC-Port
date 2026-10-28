// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>

#include "bbport_copy.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <vector>
#include <boost/container/small_vector.hpp>

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/buffer_cache/sparse_backing_pool.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
struct SubmitInfo;
class Runtime;
class StagingBufferPool;
} // namespace Vulkan

namespace VideoCore {

class TextureCache;
class MemoryTracker;
class PageManager;

class BufferCache {
    static constexpr u64 ADDRESS_SPACE_BITS = 40;
    // bbport: arena pages are 4 GiB unless the driver's buffer size limit is lower (AMD's
    // Windows driver: 2 GiB); an arena spans up to two pages, so they shrink to half the limit.
    static constexpr u64 MAX_ARENA_PAGE_BITS = 32;
    static constexpr u64 MIN_ARENA_PAGE_BITS = 30;
    static constexpr u64 MAX_ARENA_PAGES = u64{1} << (ADDRESS_SPACE_BITS - MIN_ARENA_PAGE_BITS);
    static constexpr u64 MIN_BLOCK_SIZE = 16_KB;

public:
    /// Read-only bindings up to this size are copied into a stream buffer (bbport: public for
    /// the draw pipeline's constant ring).
    static constexpr u64 STREAM_THRESHOLD = 16_KB;

    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool,
                         TextureCache& texture_cache, PageManager& tracker);
    ~BufferCache();

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    /// Retrieves the device local DBA page table buffer.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept {
        return bda_pagetable_buffer.get();
    }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager->GetFaultBuffer();
    }

    /// Retrieves the stream buffer.
    StreamBuffer& GetStreamBuffer() noexcept {
        return stream_buffer;
    }

    /// Returns minimum granularity of a sparse memory bind.
    u32 GetSparsePageShift() const noexcept {
        return block_shift;
    }

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks = false);

    /// bbport: a guest write fault at `device_addr` was handled; unprotect its neighbourhood.
    void ExtendWriteFault(VAddr device_addr);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false, bool assume_locks = false);

    /// Validates the rendered scalar source of the focus-copy pass, never arbitrary images.
    bool IsFocusCopySource(VAddr device_addr, u32 size);

    /// bbport: CPU writes to the pages of these ranges are not deferred (BB_SUN_READBACK_SYNC=0: off).
    void KeepPageSynchronized(VAddr device_addr, u64 size, u64 pgm_hash);
    /// Copies the registered ranges to host memory; a CPU write to their pages gets the newest
    /// completed copy.
    void RecordSunCopies();

    /// Finds a buffer for the specified region.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(VAddr device_addr, u32 size,
                                                             bool is_written,
                                                             bool is_texel_buffer = false,
                                                             bool focus_copy = false);

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size);

    /// Return true when a region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// Processes the fault buffer.
    void ProcessFaultBuffer();

    /// Synchronizes all buffers needed for DMA.
    void SynchronizeDmaBuffers();

    /// Commits pending sparse buffer memory binds. Must be called before every scheduler submit.
    void SubmitPendingArenaBinds(Vulkan::SubmitInfo& info);

    /// bbport: early write-back. At the end of a guest submission (GPU thread, before its
    /// flush) the ranges shaders wrote are copied to host memory; once that submission
    /// completes they are written to guest memory and stop being GPU-modified. The Order: 1886
    /// writes, every frame, pages its shaders wrote: each write fault waited for the whole GPU
    /// (Finish) to copy them back (a fifth of the GPU command thread). Without the copies the
    /// game froze: it reads values the GPU writes there.
    void RecordWritebacks();
    /// Applies the write-backs whose submission completed. With a range: first waits for the
    /// (already submitted) write-backs covering it, so the caller needs no Finish.
    void ApplyWritebacks(VAddr wait_addr = 0, u64 wait_size = 0);
    /// Only the exclusive focus-feedback queue: poll completed work before its next copy,
    /// then record the new snapshot just after dispatch. No flush or GPU wait added.
    void PollFocusWritebacks();
    void RecordFocusWritebacks();

    /// bbport: called after each submission (Rasterizer::OnSubmit): writes the deferred
    /// readbacks whose submission completed to guest memory.
    void OnSubmitted() {
        if (has_pending_readbacks.load(std::memory_order_relaxed)) {
            ApplyDeferredReadbacks();
        }
    }

private:
    struct ArenaBinds {
        const Buffer* arena;
        boost::container::small_vector<vk::SparseMemoryBind, 32> binds;
    };

    ArenaBinds* BindsForArena(const Buffer* arena) {
        auto it = std::ranges::find(pending_binds, arena, &ArenaBinds::arena);
        if (it != pending_binds.end()) {
            return std::addressof(*it);
        }
        return &pending_binds.emplace_back(arena);
    }

    const Buffer* GetArena(u64 first_block, u64 last_block);

    void EnsureResident(const Buffer* arena, u64 first_block, u64 last_block);

    void DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size);

    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer);

    /// bbport: batched small copies on the copy threads (BbCopy::QueueCopy).
    static void RunGuestCopy(const BbCopy::Item& item);
    void SmallGuestCopy(const BbCopy::Item& item);

    const Buffer* UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                               size_t total_size_bytes);

    bool SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    // bbport: texel buffer ranges known not to alias an image, per image registry generation.
    struct ImageMiss {
        VAddr address = 0;
        u32 size = 0;
        u64 generation = ~0ULL;
    };
    std::array<ImageMiss, 256> image_miss_cache{};
    Vulkan::Runtime& runtime;
    Vulkan::StagingBufferPool& staging_pool;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    std::unique_ptr<MemoryTracker> memory_tracker;

    StreamBuffer stream_buffer;
    Buffer gds_buffer;
    RangeSet gpu_modified_ranges;

    /// bbport: early write-back state (see RecordWritebacks).
    struct WritebackItem {
        VAddr addr;
        u64 size;
        u64 staging_offset;
    };
    struct WritebackBatch {
        u64 tick{};
        std::chrono::steady_clock::time_point recorded_at{};
        u64 recorded_frame{};
        Vulkan::StagingBufferRef staging{};
        std::vector<WritebackItem> items;
    };
    bool WrittenAgain(VAddr addr, u64 size);

    /// bbport: deferred readbacks. A guest write beside (not on) GPU-written bytes of a page
    /// goes on at once; those bytes are copied to host memory in the current submission and
    /// written to guest memory once it completes. Until then uploads skip them (the arena holds
    /// the newest data) and the guest's bytes at the fault are kept to spot later CPU writes.
    struct DeferredItem {
        VAddr addr;
        u64 size;
        u64 staging_offset;
        u64 snapshot_offset;
    };
    struct DeferredBatch {
        u64 tick{};
        Vulkan::StagingBufferRef staging{};
        std::vector<DeferredItem> items;
        std::vector<u8> snapshot;
    };
    bool DeferReadback(const Buffer* arena, VAddr device_addr, u64 size);
    void ApplyDeferredReadbacks();
    /// Waits for the deferred readbacks overlapping the range and writes them to guest memory.
    void WaitDeferredReadbacks(VAddr addr, u64 size);
    bool PendingReadback(VAddr addr, u64 size);
    std::mutex deferred_mutex;
    std::deque<DeferredBatch> deferred_readbacks; ///< oldest first
    RangeSet pending_readbacks; ///< GPU bytes of deferred_readbacks (guarded by deferred_mutex)
    std::atomic<bool> has_pending_readbacks{false};
    bool deferred_readbacks_enabled = true; ///< BB_DEFERRED_READBACK=0 turns it off
    bool SynchronizedPage(VAddr addr, u64 size);
    std::mutex synchronized_mutex;
    RangeSet synchronized_ranges; ///< see KeepPageSynchronized (guarded by synchronized_mutex)
    bool DeliverSunCopies();
    struct SunCopy {
        VAddr addr;
        u64 size;
        u64 staging_offset;
    };
    struct SunBatch {
        u64 tick{};
        Vulkan::StagingBufferRef staging{};
        std::vector<SunCopy> items;
    };
    std::deque<SunBatch> sun_batches; ///< oldest first (guarded by synchronized_mutex)
    bool early_writeback = false; ///< BB_EARLY_WRITEBACK=1 (no gain measured in run75)
    bool focus_sync_enabled = false; ///< opt-in narrow scalar-copy path
    struct FocusLatency {
        u64 samples{}, frames_sum{}, max_frames{}, max_batches{};
        double ms_sum{}, max_ms{};
        std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    } focus_latency;
    std::mutex frame_written_mutex;
    std::vector<std::pair<VAddr, u64>> frame_written; ///< written ranges since the last record
    std::recursive_mutex writeback_mutex;
    std::deque<WritebackBatch> writebacks; ///< oldest first

    std::unique_ptr<FaultManager> fault_manager;
    std::unique_ptr<Buffer> bda_pagetable_buffer;

    std::array<const Buffer*, MAX_ARENA_PAGES> address_space{};
    u64 arena_page_bits{MAX_ARENA_PAGE_BITS};
    u64 arena_page_size{u64{1} << MAX_ARENA_PAGE_BITS};
    u64 num_arena_pages{u64{1} << (ADDRESS_SPACE_BITS - MAX_ARENA_PAGE_BITS)};
    // Declared before arenas: buffers are destroyed before their backing allocations.
    std::unique_ptr<SparseBackingPool> backing_pool;
    std::deque<Buffer> arenas;
    std::vector<ArenaBinds> pending_binds;
    Vulkan::Semaphore memory_semaphore;

    struct Backing : public Interval {
        vk::DeviceMemory memory;
        u64 offset;
        u32 block_shift;
        constexpr bool CanMergeWith(const Backing& other) const noexcept {
            return memory == other.memory && block_shift == other.block_shift &&
                   offset + ((end - start) << block_shift) == other.offset;
        }
        constexpr Backing SubRange(u64 a, u64 b) const noexcept {
            return {{a, b}, memory, offset + ((a - start) << block_shift), block_shift};
        }
    };
    IntervalList<Backing> resident_ranges;
    u64 dma_synced_generation = ~0ull; ///< cpu_modified_generation at the last full DMA walk

    u32 arena_memory_type_index{};
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};
};

} // namespace VideoCore
