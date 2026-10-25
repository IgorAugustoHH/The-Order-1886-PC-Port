// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>

namespace Common {
// Pinned entries must not consume the removal quota and starve reclaimable entries
// later in the LRU. try_evict returns true only after actually removing an entry.
template <typename Cache, typename Tick, typename Evict>
size_t CollectLru(Cache& cache, Tick cutoff, size_t quota, Evict&& try_evict) {
    size_t removed = 0;
    cache.ForEachItemBelow(cutoff, [&](auto id) {
        if (removed == quota) {
            return true;
        }
        if (try_evict(id)) {
            ++removed;
        }
        return removed == quota;
    });
    return removed;
}
} // namespace Common
