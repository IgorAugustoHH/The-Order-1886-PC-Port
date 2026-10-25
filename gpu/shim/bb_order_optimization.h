// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace BbOrderOpt {
inline unsigned Env(const char* name, unsigned maximum = 256) {
    const char* value = std::getenv(name);
    if (!value || !*value) return 0;
    char* end{};
    const unsigned long n = std::strtoul(value, &end, 10);
    return end && !*end && n <= maximum ? unsigned(n) : 0;
}
inline std::uint64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline std::atomic<std::uint64_t> submitted{0}, batch_peak{0}, producer_submit_ns{0},
    driver_submit_ns{0}, empty_downloads{0}, gc_visits{0}, gc_kept{0}, gc_evicted{0};
inline void Peak(std::atomic<std::uint64_t>& peak, std::uint64_t n) {
    auto old = peak.load(std::memory_order_relaxed);
    while (old < n && !peak.compare_exchange_weak(old, n, std::memory_order_relaxed)) {}
}
// Cumulative totals, at most once per five seconds; no per-draw file IO or GPU queries.
inline void Report(std::uint64_t now) {
    static const bool enabled = Env("BB_ORDER_OPT_STATS") != 0;
    static std::uint64_t last = 0;
    if (!enabled || now - last < 5'000'000'000ULL) return;
    last = now;
    std::printf("Order optimization totals: submits=%llu batch_peak=%llu producer_ms=%.3f "
                "driver_ms=%.3f empty_downloads=%llu gc_visits=%llu gc_kept=%llu gc_evicted=%llu\n",
        (unsigned long long)submitted.load(), (unsigned long long)batch_peak.load(),
        producer_submit_ns.load() / 1e6, driver_submit_ns.load() / 1e6,
        (unsigned long long)empty_downloads.load(), (unsigned long long)gc_visits.load(),
        (unsigned long long)gc_kept.load(), (unsigned long long)gc_evicted.load());
}
// Producers and consumer update the flag while holding the actual queue's mutex.
// A false snapshot skips only this poll; the next poll sees a concurrent publication.
class PendingDownloads {
public:
    bool Any() const { return pending.load(std::memory_order_acquire); }
    void Publish(bool any) { pending.store(any, std::memory_order_release); }
private:
    std::atomic<bool> pending{false};
};
inline bool RetainIdleImage(std::uint64_t now, std::uint64_t touched,
                            unsigned seconds, bool pressured) {
    return !pressured && seconds &&
        (now < touched || now - touched < std::uint64_t(seconds) * 1'000'000'000ULL);
}
} // namespace BbOrderOpt
