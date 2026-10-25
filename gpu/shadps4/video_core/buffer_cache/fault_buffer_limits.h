// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace VideoCore {
// Slot zero holds the counter, leaving 1023 address slots.
inline constexpr std::size_t FaultDownloadSlots = 1024;
constexpr std::uint32_t FaultDownloadCount(std::uint64_t reported) {
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(reported, FaultDownloadSlots - 1));
}
} // namespace VideoCore
