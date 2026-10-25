// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#include "bb_compile_budget.h"
#include <chrono>
#include <mutex>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace BbCompile {
Pressure ReadPressure() {
    static std::mutex mutex;
    std::scoped_lock lock{mutex};
    static Pressure cached;
    static auto last = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(500)) return cached;
    last = now;
#ifdef _WIN32
    FILETIME idle{}, kernel{}, user{};
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (!GetSystemTimes(&idle, &kernel, &user) || !GlobalMemoryStatusEx(&memory)) {
        cached.valid = false;
        return cached;
    }
    const auto ticks = [](FILETIME v) {
        return (std::uint64_t(v.dwHighDateTime) << 32) | v.dwLowDateTime;
    };
    const auto total = ticks(kernel) + ticks(user);
    const auto idle_total = ticks(idle);
    static std::uint64_t previous_total = 0, previous_idle = 0;
    cached.available_mib = memory.ullAvailPhys / (1024 * 1024);
    cached.valid = previous_total != 0 && total > previous_total && idle_total >= previous_idle;
    if (cached.valid) {
        const double elapsed = double(total - previous_total);
        cached.cpu_percent = std::clamp(100.0 * (elapsed - double(idle_total - previous_idle)) / elapsed, 0.0, 100.0);
    }
    previous_total = total;
    previous_idle = idle_total;
#endif
    return cached;
}
} // namespace BbCompile
