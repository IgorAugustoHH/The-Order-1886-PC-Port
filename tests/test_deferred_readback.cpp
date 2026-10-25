// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
// Deferred readbacks (BufferCache::DeferReadback): a guest write beside GPU-written bytes lets
// the guest go on while those bytes travel back. Models the arena (GPU copy), guest memory and
// the snapshots taken at the faults, and runs the upload (SynchronizeMemory) and apply
// (ApplyDeferredReadbacks) decisions of deferred_readback_merge.h on them.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#include "video_core/buffer_cache/deferred_readback_merge.h"

namespace {
constexpr u64 Pending = 64; // GPU-written bytes [0, 64), deferred
constexpr u64 Size = 128;   // bytes [64, 128) are ordinary CPU data

struct Item {
    VAddr addr;
    u64 size;
    u64 staging_offset;
    u64 snapshot_offset;
};
struct Batch {
    std::vector<Item> items;
    std::vector<u8> snapshot;
    std::vector<u8> staging;
};

struct Model {
    std::vector<u8> arena = std::vector<u8>(Size, 0);
    std::vector<u8> guest = std::vector<u8>(Size, 0);
    std::deque<Batch> batches; // oldest first, as BufferCache::deferred_readbacks

    void Gpu(u64 offset, u64 size, u8 value) {
        std::memset(arena.data() + offset, value, size);
    }
    void Cpu(u64 offset, u64 size, u8 value) {
        std::memset(guest.data() + offset, value, size);
    }
    void Fault(u64 offset, u64 size) { // DeferReadback: snapshot + GPU copy of the bytes
        Batch b;
        b.items.push_back({offset, size, 0, 0});
        b.snapshot.assign(guest.begin() + offset, guest.begin() + offset + size);
        b.staging.assign(arena.begin() + offset, arena.begin() + offset + size);
        batches.push_back(std::move(b));
    }
    // SynchronizeMemory of a CPU-modified range while readbacks are pending. Without
    // `rewritten`, the old behaviour: pending bytes are skipped.
    void Upload(VAddr addr, u64 size, bool rewritten) {
        VideoCore::RangeSet pending;
        for (const auto& b : batches) {
            for (const auto& item : b.items) {
                pending.Add(item.addr, item.size);
            }
        }
        const auto copy = [&](VAddr a, u64 n) { std::memcpy(&arena[a], &guest[a], n); };
        pending.ForEachNotInRange(addr, size, copy);
        if (rewritten) {
            std::vector<std::pair<VAddr, u64>> runs;
            VideoCore::ForEachRewrittenPending(
                batches, addr, size,
                [&](VAddr a, u8* dst, u64 n) { std::memcpy(dst, &guest[a], n); },
                [&](VAddr a, u64 n) { runs.emplace_back(a, n); });
            for (size_t i = 1; i < runs.size(); ++i) { // each byte at most once
                assert(runs[i - 1].first + runs[i - 1].second <= runs[i].first ||
                       runs[i].first + runs[i].second <= runs[i - 1].first);
            }
            for (const auto& [a, n] : runs) {
                copy(a, n);
            }
        }
    }
    void ApplyOldest(u64* changed_total = nullptr) { // one batch, oldest first
        Batch b = std::move(batches.front());
        batches.pop_front();
        VideoCore::ApplyDeferredBatch(
            b, batches, [&](const Item& item) { return b.staging.data() + item.staging_offset; },
            [&](VAddr a, u8* dst, u64 n) { std::memcpy(dst, &guest[a], n); },
            [&](VAddr a, const u8* src, u64 n) { std::memcpy(&guest[a], src, n); },
            [&](const Item&, u64 changed) {
                if (changed_total) {
                    *changed_total += changed;
                }
            });
    }
    bool Arena(u64 offset, u64 size, u8 value) const {
        for (u64 i = offset; i < offset + size; ++i) {
            if (arena[i] != value) {
                return false;
            }
        }
        return true;
    }
};
} // namespace

int main() {
    // Runs: alternating changed/unchanged bytes, boundaries included.
    {
        const u8 before[8] = {1, 1, 1, 1, 1, 1, 1, 1};
        const u8 current[8] = {2, 2, 1, 1, 1, 3, 1, 4};
        std::vector<std::pair<VAddr, u64>> changed, unchanged;
        VideoCore::ForEachDeferredRun<true>(
            100, before, current, 8, [&](VAddr a, u64 s) { changed.emplace_back(a, s); });
        VideoCore::ForEachDeferredRun<false>(
            100, before, current, 8, [&](VAddr a, u64 s) { unchanged.emplace_back(a, s); });
        assert((changed == std::vector<std::pair<VAddr, u64>>{{100, 2}, {105, 1}, {107, 1}}));
        assert((unchanged == std::vector<std::pair<VAddr, u64>>{{102, 3}, {106, 1}}));
    }

    // The CPU rewrites a buffer the GPU filled before, and the GPU uses it before the readback
    // arrives. Skipping the pending bytes (old behaviour) leaves the arena mixed: the first
    // half old GPU data, the second half the CPU's new data.
    {
        Model m;
        m.Gpu(0, Pending, 0xAA);
        m.Fault(0, Pending);
        m.Cpu(0, Size, 0x55);
        m.Upload(0, Size, false);
        assert(m.Arena(0, Pending, 0xAA) && m.Arena(Pending, Size - Pending, 0x55));
    }
    {
        Model m;
        m.Gpu(0, Pending, 0xAA);
        m.Fault(0, Pending);
        m.Cpu(0, Size, 0x55);
        m.Upload(0, Size, true);
        assert(m.Arena(0, Size, 0x55));
        u64 changed = 0;
        m.ApplyOldest(&changed);
        assert(changed == Pending);
        assert(m.guest == m.arena); // the GPU data does not overwrite the CPU's newer bytes
    }

    // Only part of the pending bytes rewritten: the rest keeps the GPU data on both sides.
    {
        Model m;
        m.Gpu(0, Pending, 0xAA);
        m.Fault(0, Pending);
        m.Cpu(16, 16, 0x55);
        m.Upload(0, Size, true);
        assert(m.Arena(0, 16, 0xAA) && m.Arena(16, 16, 0x55) && m.Arena(32, 32, 0xAA));
        m.ApplyOldest();
        assert(m.guest == m.arena);
    }

    // Nothing rewritten: uploads leave the GPU data alone, as before.
    {
        Model m;
        m.Gpu(0, Pending, 0xAA);
        m.Fault(0, Pending);
        m.Cpu(Pending, Size - Pending, 0x55);
        m.Upload(0, Size, true);
        assert(m.Arena(0, Pending, 0xAA));
        m.ApplyOldest();
        assert(m.guest == m.arena);
    }

    // Two batches over the same bytes: the CPU wrote them after the first fault, the GPU then
    // wrote them again (newer than the CPU) and a second fault deferred them. The older
    // snapshot must not make uploads put the CPU's older bytes over the GPU's newer ones, and
    // applying the older batch must not make the newer one take its data for a CPU write.
    {
        Model m;
        m.Gpu(0, Pending, 0xAA);
        m.Fault(0, Pending);
        m.Cpu(0, Pending, 0x55);
        m.Upload(0, Pending, true); // the draw that writes the buffer syncs it first
        m.Gpu(0, Pending, 0xBB);
        m.Fault(0, Pending);
        m.Cpu(Pending, Size - Pending, 0x66);
        m.Upload(0, Size, true);
        assert(m.Arena(0, Pending, 0xBB) && m.Arena(Pending, Size - Pending, 0x66));
        m.ApplyOldest();
        m.ApplyOldest();
        assert(m.guest == m.arena);
    }
    // Two batches, the CPU never touches the GPU bytes: applying the older batch's data first
    // made the newer batch see changed bytes and keep the older GPU data in guest memory.
    {
        Model m;
        m.Gpu(0, Pending, 0xAA);
        m.Fault(0, Pending);
        m.Gpu(0, Pending, 0xBB);
        m.Fault(0, Pending);
        u64 changed = 0;
        m.ApplyOldest(&changed);
        m.ApplyOldest(&changed);
        assert(changed == 0);
        assert(m.guest == m.arena);
    }
    std::puts("deferred readback: rewritten pending bytes uploaded, newest snapshot decides");
}
