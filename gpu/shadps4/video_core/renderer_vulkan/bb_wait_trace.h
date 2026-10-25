// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <string_view>

namespace BbWaitTrace {
using Clock = std::chrono::steady_clock;
inline bool Enabled() {
    static const bool enabled = std::getenv("BB_WAIT_TRACE") != nullptr;
    return enabled;
}
inline void Record(const void* scheduler, unsigned long long tick,
                   std::source_location site, Clock::time_point start,
                   Clock::time_point submitted, Clock::time_point end) {
    const double total = std::chrono::duration<double, std::milli>(end - start).count();
    if (total < 0.5) return; // Deliberately exclude fast calls and limit diagnostic I/O.
    static std::FILE* file = [] {
        const char* path = std::getenv("BB_WAIT_TRACE_FILE");
        return path ? std::fopen(path, "w") : nullptr;
    }();
    if (!file) return;
    std::string_view name = site.file_name();
    name.remove_prefix(name.find_last_of("/\\") + 1);
    const double flush = std::chrono::duration<double, std::milli>(submitted - start).count();
    const double wait = std::chrono::duration<double, std::milli>(end - submitted).count();
    const double stamp = std::chrono::duration<double, std::milli>(end.time_since_epoch()).count();
    std::fprintf(file, "Scheduler wait: site=%.*s:%u scheduler=%p tick=%llu monotonic_ms=%.3f flush_ms=%.3f wait_ms=%.3f total_ms=%.3f\n",
                 int(name.size()), name.data(), site.line(), const_cast<void*>(scheduler), tick,
                 stamp, flush, wait, total);
    std::fflush(file);
}
}
