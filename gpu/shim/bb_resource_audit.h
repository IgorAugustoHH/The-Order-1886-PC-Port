// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

// Translation/resource census only. Never reads guest memory and never changes a binding.
// Disabled in the speed profile. Limits and missing records are explicit in the file.
namespace BbResourceAudit {
inline constexpr std::uint64_t MaxBytes = 16 * 1024 * 1024;
inline constexpr std::size_t MaxUnique = 16384;
inline std::mutex mutex;
inline std::FILE* output{};
inline std::uint64_t bytes{}, dropped{};
inline bool exhausted{};
inline bool details_exhausted{};
inline std::unordered_set<std::string> seen;
inline std::atomic<bool> fault_enabled{false};
inline bool Enabled() {
    static const bool enabled = [] {
        const char* path = std::getenv("BB_RESOURCE_AUDIT_FILE");
        const bool active = path && *path;
        fault_enabled.store(active, std::memory_order_relaxed);
        return active;
    }();
    return enabled;
}
inline std::string Quote(std::string_view value) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char ch : value) {
        if (ch == '"' || ch == '\\') { out += '\\'; out += char(ch); }
        else if (ch < 32) { out += "\\u00"; out += hex[ch >> 4]; out += hex[ch & 15]; }
        else out += char(ch);
    }
    out += '"';
    return out;
}
inline std::uint64_t EpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
// Avoid constructing a large JSON payload on every draw after its first sample.
inline bool AlreadySeen(std::string_view signature) {
    std::scoped_lock lock(mutex);
    return exhausted || details_exhausted || seen.contains(std::string(signature));
}
inline void Write(std::string_view event, std::string_view fields,
                  std::string_view signature = {}, bool flush = false) {
    if (!Enabled()) return;
    std::scoped_lock lock(mutex);
    if (!output && !exhausted) {
        output = std::fopen(std::getenv("BB_RESOURCE_AUDIT_FILE"), "w");
        if (!output) { exhausted = true; std::fprintf(stderr, "Resource audit: cannot open file\n"); return; }
        std::fprintf(output, "{\"event\":\"audit_begin\",\"max_bytes\":%llu,\"max_unique_details\":%zu}\n",
                     (unsigned long long)MaxBytes, MaxUnique);
    }
    if (exhausted) { ++dropped; return; }
    if (!signature.empty()) {
        if (seen.contains(std::string(signature))) return;
        if (seen.size() >= MaxUnique) {
            details_exhausted = true;
            if (!dropped++) {
                std::fprintf(output, "{\"event\":\"detail_limit\",\"max_unique_details\":%zu}\n", MaxUnique);
                std::fflush(output);
            }
            return;
        }
        seen.emplace(signature);
    }
    const auto line = "{\"event\":" + Quote(event) + ",\"epoch_ms\":" +
                      std::to_string(EpochMs()) + (fields.empty() ? "" : ",") + std::string(fields) + "}\n";
    if (bytes + line.size() > MaxBytes) {
        std::fprintf(output, "{\"event\":\"file_limit\",\"bytes\":%llu}\n", (unsigned long long)bytes);
        std::fflush(output); exhausted = true; ++dropped; return;
    }
    std::fwrite(line.data(), 1, line.size(), output);
    bytes += line.size();
    if (flush) std::fflush(output);
}

struct FaultSlot {
    std::atomic<unsigned> state{0}; // 0 free, 1 writer owns, 2 ready for ordinary code
    std::uintptr_t code{}, address{};
};
inline std::array<FaultSlot, 64> faults;
inline std::atomic<std::uint64_t> fault_cursor{0}, faults_dropped{0};
static_assert(std::atomic<unsigned>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
// Signal handler: no I/O, allocation, locks or guest-memory reads.
inline void RecordFault(const void* code, const void* address) {
    if (!fault_enabled.load(std::memory_order_relaxed)) return;
    auto& slot = faults[fault_cursor.fetch_add(1, std::memory_order_relaxed) % faults.size()];
    unsigned free = 0;
    if (!slot.state.compare_exchange_strong(free, 1, std::memory_order_acquire)) {
        faults_dropped.fetch_add(1, std::memory_order_relaxed); return;
    }
    slot.code = reinterpret_cast<std::uintptr_t>(code);
    slot.address = reinterpret_cast<std::uintptr_t>(address);
    slot.state.store(2, std::memory_order_release);
}
inline void PollFaults() {
    if (!Enabled()) return;
    for (auto& slot : faults) {
        unsigned ready = 2;
        if (!slot.state.compare_exchange_strong(ready, 1, std::memory_order_acquire)) continue;
        const auto fields = "\"walker_pc\":" + std::to_string(slot.code) +
                            ",\"fault_address\":" + std::to_string(slot.address);
        slot.state.store(0, std::memory_order_release);
        Write("srt_runtime_fault", fields);
    }
    const auto lost = faults_dropped.exchange(0, std::memory_order_relaxed);
    if (lost) Write("fault_ring_overflow", "\"lost\":" + std::to_string(lost));
    std::scoped_lock lock(mutex);
    if (output) std::fflush(output);
}
} // namespace BbResourceAudit
