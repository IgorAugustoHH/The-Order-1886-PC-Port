// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#include "bb_order_optimization.h"
#include "common/cache_gc.h"
#include "common/lru_cache.h"
#include <cassert>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

int main() {
    using BbOrderOpt::RetainIdleImage;
    constexpr std::uint64_t second = 1'000'000'000;
    // Lifetime is independent of FPS/submission count and handles long pauses.
    assert(RetainIdleImage(19*second, 0, 20, false));
    assert(!RetainIdleImage(20*second, 0, 20, false));
    assert(!RetainIdleImage(3600*second, 0, 20, false));
    assert(!RetainIdleImage(second, 0, 20, true)); // pressure overrides residency
    assert(!RetainIdleImage(second, 0, 0, false));
    assert(RetainIdleImage(0, second, 20, false));

    // Queue publication races a lock-free empty poll. Every request must be consumed.
    BbOrderOpt::PendingDownloads gate;
    std::mutex mutex;
    std::vector<unsigned> queue;
    std::atomic<unsigned> done{0}, consumed{0};
    std::vector<std::jthread> producers;
    for (unsigned p=0; p<4; ++p) producers.emplace_back([&,p] {
        for (unsigned i=0; i<10000; ++i) {
            std::lock_guard lock(mutex);
            queue.push_back(p*10000+i);
            gate.Publish(true);
        }
        done.fetch_add(1, std::memory_order_release);
    });
    while (done.load(std::memory_order_acquire) < 4 || gate.Any()) {
        if (!gate.Any()) { std::this_thread::yield(); continue; }
        std::lock_guard lock(mutex);
        consumed += queue.size();
        queue.clear();
        gate.Publish(false);
    }
    assert(consumed == 40000 && !gate.Any());

    // A large pinned prefix cannot monopolize repeated, bounded maintenance sweeps.
    Common::LeastRecentlyUsedCache<unsigned, u64> lru;
    std::vector<size_t> ids;
    for (unsigned i=0; i<600; ++i) ids.push_back(lru.Insert(i, 0));
    size_t visited=0, removed=0;
    for (u64 sweep=1; sweep<=3; ++sweep) {
        size_t visits=0;
        removed += Common::CollectLru(lru, sweep-1, 10, [&](unsigned id) {
            ++visits;
            if (id<590) { lru.Touch(ids[id], sweep); return false; }
            lru.Free(ids[id]); return true;
        }, 256);
        assert(visits <= 256);
        visited += visits;
    }
    assert(removed == 10 && visited >= 600);
    std::puts("Order cache: 40,000 concurrent requests, idle seconds/pressure and bounded GC passed");
}
