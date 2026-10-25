// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string_view>

namespace BbCompile {
inline unsigned WorkerCount(std::string_view text, unsigned hardware) {
    unsigned count = 2;
    if (!text.empty()) {
        unsigned parsed{};
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec == std::errc{} && result.ptr == text.data() + text.size() && parsed > 0)
            count = parsed;
    }
    return std::clamp(count, 1u, std::max(1u, hardware));
}
struct Pressure {
    double cpu_percent = -1;
    std::uint64_t available_mib = 0;
    bool valid = false;
};
inline unsigned Budget(unsigned workers, Pressure p, bool aggressive = false) {
    // Admission control only. Already running driver calls cannot be preempted.
    if (aggressive) {
        if (!p.valid) return std::min(workers, 2u);
        if (p.available_mib < 768) return 1u;
        if (p.cpu_percent >= 95) return std::max(1u, workers / 2);
        return workers;
    }
    return !p.valid || p.cpu_percent >= 85 || p.available_mib < 2048 ? 1u : workers;
}
Pressure ReadPressure();
} // namespace BbCompile
