// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include "video_core/buffer_cache/completed_writeback.h"

struct Sample { u64 ready; u32 slot; float value; };

static void Ring(bool feedback_only) {
    std::array<float, 3> cpu{};
    std::array<bool, 3> gpu_owned{};
    std::deque<Sample> pending;
    u32 publications = 0, pending_publications = 0;
    auto advance = [&](u64 completed, bool next_submission) {
        while (!pending.empty() && pending.front().ready <= completed) {
            auto sample = pending.front(); pending.pop_front();
            bool newer = next_submission;
            for (const auto& next : pending) newer |= next.slot == sample.slot;
            if (!gpu_owned[sample.slot]) continue;
            const auto before = cpu;
            const bool delivered = VideoCore::DeliverCompletedWritebackRange(
                feedback_only, newer, sample.slot * 4, 4,
                reinterpret_cast<const u8*>(&sample.value),
                [&](VAddr address, const u8* bytes, u64 size) {
                    assert(address == sample.slot * 4 && size == 4);
                    std::memcpy(&cpu[sample.slot], bytes, size);
                    ++publications;
                    if (newer) ++pending_publications;
                },
                [&](VAddr address, u64 size) {
                    assert(!newer && size == 4);
                    gpu_owned[address / 4] = false;
                });
            if (newer) assert(gpu_owned[sample.slot]); // an old sample cannot retire the new write
            if (!delivered) assert(cpu == before);
        }
    };
    // A three-slot feedback ring, with three frames of GPU latency and continuous
    // reuse. The resource policy starves; the feedback policy advances without a wait.
    for (u64 frame=0; frame<100; ++frame) {
        const u32 slot=frame%3;
        gpu_owned[slot]=true;
        pending.push_back({frame+3,slot,float(frame)+13.0f});
        advance(frame,true);
        if (frame>=3 && feedback_only) assert(publications==frame-2);
        if (!feedback_only) assert(publications==0);
    }
    advance(102,false);
    for (u32 slot=0;slot<3;++slot) {
        assert(cpu[slot]==float(99-(99-slot)%3)+13.0f);
        assert(!gpu_owned[slot]);
    }
    assert(feedback_only ? pending_publications==97 : pending_publications==0);
}

int main() {
    static_assert(VideoCore::ExclusiveFocusFeedback(true, false));
    static_assert(!VideoCore::ExclusiveFocusFeedback(false, false));
    static_assert(!VideoCore::ExclusiveFocusFeedback(true, true));
    static_assert(!VideoCore::ExclusiveFocusFeedback(false, true));
    Ring(false); Ring(true);
    // Unavailable GPU results never call the completed-range helper; CPU writes
    // outside a surviving GPU-owned range remain untouched.
    std::array<u8,16> cpu{};cpu.fill(0x7a);
    const std::array<u8,4> gpu{1,2,3,4};
    auto write=[&](VAddr address,const u8* data,u64 size){std::memcpy(cpu.data()+address,data,size);};
    u32 retired=0;
    auto retire=[&](VAddr,u64){++retired;};
    assert(!VideoCore::DeliverCompletedWritebackRange(false,true,4,4,gpu.data(),write,retire));
    assert(cpu[4]==0x7a && !retired);
    assert(VideoCore::DeliverCompletedWritebackRange(true,true,4,4,gpu.data(),write,retire));
    for (u32 i=0;i<16;++i) assert(cpu[i]==(i>=4 && i<8 ? gpu[i-4] : 0x7a));
    assert(!retired);
    assert(VideoCore::DeliverCompletedWritebackRange(true,false,4,4,gpu.data(),write,retire));
    assert(retired==1);
    std::puts("PASS focus writeback: completed three-slot feedback advances, pending writes retain ownership, resource policy and unrelated bytes preserved");
}
