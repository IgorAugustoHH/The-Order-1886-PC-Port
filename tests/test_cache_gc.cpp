// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <vector>
#include "common/cache_gc.h"
#include "common/lru_cache.h"

int main() {
    Common::LeastRecentlyUsedCache<int, u64> cache;
    std::vector<size_t> ids;
    for (int i = 0; i < 100; ++i) ids.push_back(cache.Insert(i, 1));
    ids.push_back(cache.Insert(100, 20)); // recently used, never eligible here
    std::vector<int> removed;
    auto evict = [&](int i) {
        if (i < 80) return false; // pinned tiled images formerly consumed the whole quota
        cache.Free(ids[i]);
        removed.push_back(i);
        return true;
    };
    assert(Common::CollectLru(cache, u64{10}, 8, evict) == 8);
    assert(removed.front() == 80 && removed.back() == 87);
    assert(Common::CollectLru(cache, u64{10}, 64, evict) == 12);
    assert(removed.back() == 99);
    assert(Common::CollectLru(cache, u64{10}, 64, evict) == 0);
    assert(Common::CollectLru(cache, u64{20}, 0, evict) == 0);
    assert(Common::CollectLru(cache, u64{20}, 1, evict) == 1);
    assert(removed.back() == 100);
    std::puts("cache GC: pinned-prefix starvation, quota, cutoff and deletion iteration passed");
}
