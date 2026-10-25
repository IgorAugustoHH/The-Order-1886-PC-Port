// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/types.h"

namespace VideoCore {
constexpr bool ExclusiveFocusFeedback(bool focus_sync, bool general_early_writeback) {
    return focus_sync && !general_early_writeback;
}

// Called only after the staging submission's fence has completed and only for
// ranges still owned by GPU writes. Resource snapshots defer to a newer write.
// A CPU feedback ring must publish completed samples even while the GPU records
// its next sample, retaining ownership for that newer pending write.
template <typename Write, typename Retire>
bool DeliverCompletedWritebackRange(bool feedback_only, bool written_again, VAddr address,
                                    u64 size, const u8* data, Write&& write, Retire&& retire) {
    if (written_again && !feedback_only) return false;
    write(address, data, size);
    if (!written_again) retire(address, size);
    return true;
}
} // namespace VideoCore
