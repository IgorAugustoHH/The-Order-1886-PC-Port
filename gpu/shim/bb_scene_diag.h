// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <set>
#include <array>
#include "bbport_toggles.h"
#include "bb_resource_audit.h"

// Opt-in, bounded telemetry. Timestamps are UTC epoch milliseconds.
namespace BbSceneDiag {
inline bool Enabled() {
    static const bool enabled = std::getenv("BB_SCENE_DIAG_FILE") != nullptr;
    return enabled;
}
inline std::uint64_t EpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
inline std::atomic<std::uint64_t> tickets{0};
inline std::atomic<std::uint64_t> draw_shader{0}, draw_pipeline{0}, dispatch_shader{0},
    dispatch_pipeline{0}, invalid_key{0}, image_bytes{0}, image_uploads{0};
inline std::atomic<std::uint64_t> buffer_bytes{0}, rewritten_readback_bytes{0};
inline std::mutex output_mutex;
inline std::FILE* output{};
inline std::uint64_t detail_lines{}, next_summary{};
inline void OpenLocked() {
    static bool attempted = false;
    if (attempted) return;
    attempted = true;
    output = std::fopen(std::getenv("BB_SCENE_DIAG_FILE"), "w");
    if (output) {
        std::fprintf(output, "epoch_ms,frame,event,id,a,b\n");
        std::fflush(output);
    } else {
        std::fprintf(stderr, "Scene diagnostic: cannot open output file\n");
    }
}
inline void Event(const char* event, std::uint64_t id = 0,
                  std::uint64_t a = 0, std::uint64_t b = 0) {
    if (!Enabled()) return;
    std::scoped_lock lock(output_mutex);
    OpenLocked();
    if (!output) return;
    if (detail_lines < 20000) {
        std::fprintf(output, "%llu,%llu,%s,%llu,%llu,%llu\n",
            (unsigned long long)EpochMs(), (unsigned long long)BbStats::gpu_frames.load(),
            event, (unsigned long long)id, (unsigned long long)a, (unsigned long long)b);
        ++detail_lines;
    } else if (detail_lines == 20000) {
        std::fprintf(output, "%llu,0,detail_limit,20000,0,0\n", (unsigned long long)EpochMs());
        ++detail_lines;
    }
}
inline void Poll() {
    if (!Enabled()) return;
    // Called from the GPU thread; summaries flush events at most once per second.
    const auto now = EpochMs();
    if (now < next_summary) return;
    next_summary = now + 1000;
    BbResourceAudit::PollFaults();
    std::scoped_lock lock(output_mutex);
    OpenLocked();
    if (!output) return;
    const auto line = [&](const char* name, std::uint64_t value) {
        std::fprintf(output, "%llu,%llu,%s,0,%llu,0\n", (unsigned long long)now,
            (unsigned long long)BbStats::gpu_frames.load(), name, (unsigned long long)value);
    };
    line("draw_wait_shader", draw_shader.exchange(0));
    line("draw_wait_pipeline", draw_pipeline.exchange(0));
    line("dispatch_wait_shader", dispatch_shader.exchange(0));
    line("dispatch_wait_pipeline", dispatch_pipeline.exchange(0));
    line("invalid_key", invalid_key.exchange(0));
    line("image_upload_bytes", image_bytes.exchange(0));
    line("image_upload_count", image_uploads.exchange(0));
    line("buffer_upload_bytes", buffer_bytes.exchange(0));
    line("rewritten_readback_bytes", rewritten_readback_bytes.exchange(0));
    std::fflush(output);
}
inline void Count(std::atomic<std::uint64_t>& counter) {
    if (Enabled()) ++counter;
}
inline std::uint64_t Ticket() {
    return Enabled() ? ++tickets : 0;
}
inline bool FocusShader(std::uint64_t hash) {
    switch (hash) {
    case 0x804aeead: case 0x8c8a4817: case 0xa8792bbb: case 0xad441fb1:
    case 0xc75dd317: case 0xa7b66f58: case 0x96b4876f: case 0xbeb70f07:
    case 0xa6e55bd3: case 0xc74df487: return true;
    default: return false;
    }
}
inline void ParticleSrt(std::uint64_t hash,bool applied,std::uint32_t active,std::uint32_t inactive) {
    if (!Enabled()) return;
    switch (hash) {
    case 0xc75dd317:case 0xad441fb1:case 0xa8792bbb:case 0x8c8a4817:break;
    default:return;
    }
    // No per-draw CSV, copies, GPU readback or unbounded table census.
    static std::atomic<std::uint32_t> last[4]{{~0u},{~0u},{~0u},{~0u}};
    const auto index=hash==0xc75dd317?0:hash==0xad441fb1?1:hash==0xa8792bbb?2:3;
    const auto state=applied?active:~1u;
    if (last[index].exchange(state,std::memory_order_relaxed)!=state)
        Event(applied?"particle_srt_active_count":"particle_srt_schema_rejected",hash,active,inactive);
}
inline void Binding(std::uint64_t hash, std::uint64_t address, std::uint64_t slot) {
    if (!Enabled() || !FocusShader(hash)) return;
    static std::mutex mutex;
    static std::set<std::array<std::uint64_t, 3>> seen;
    {
        std::scoped_lock lock(mutex);
        if (seen.size() >= 4096 || !seen.insert({hash, address, slot}).second) return;
    }
    Event("focus_texture_descriptor", hash, address, slot);
}
} // namespace BbSceneDiag
