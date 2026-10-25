// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <windows.h>
#include <xbyak/xbyak.h>
#include "core/signals.h"
#include "common/serdes.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/passes/srt.h"
#include "shader_recompiler/ir/program.h"

static uintptr_t start_a, end_a, start_b, end_b;
static bool mapped_a = true;
extern "C" uint64_t runtime_memory_clamp(uintptr_t a, uint64_t n) {
    const uintptr_t end = mapped_a && start_a <= a && a < end_a ? end_a :
                          start_b <= a && a < end_b ? end_b : 0;
    return end ? std::min<uint64_t>(n, end - a) : 0;
}
static LONG CALLBACK HandleFault(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        Core::Signals::Instance()->DispatchAccessViolation(e,
            reinterpret_cast<void*>(e->ExceptionRecord->ExceptionInformation[1])))
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}
int main() {
    using namespace Shader;
    auto* handler = AddVectoredExceptionHandler(1, HandleFault);
    assert(handler);
    static_assert(sizeof(PersistentSrtInfo) == 24);
    static_assert(offsetof(PersistentSrtInfo, reader_revision) == 20);
    // Exercise the production flatten pass and its mapped-pointer guard.
    std::array<u32, 2> table_a{11, 12}, table_b{21, 22};
    start_a = reinterpret_cast<uintptr_t>(table_a.data()); end_a = start_a + sizeof(table_a);
    start_b = reinterpret_cast<uintptr_t>(table_b.data()); end_b = start_b + sizeof(table_b);
    std::array<u32, 16> ud{};
    std::memcpy(ud.data(), &start_a, 8); std::memcpy(ud.data() + 2, &start_b, 8);
    Info info{}; info.pgm_hash = 0xface; info.user_data = ud;
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool); IR::IREmitter ir(block);
    auto root_a = ir.CompositeConstruct(ir.GetUserData(IR::ScalarReg(0)), ir.GetUserData(IR::ScalarReg(1)));
    auto root_b = ir.CompositeConstruct(ir.GetUserData(IR::ScalarReg(2)), ir.GetUserData(IR::ScalarReg(3)));
    auto a = ir.ReadConst(root_a, ir.Imm32(0u));
    auto b = ir.ReadConst(root_b, ir.Imm32(0u));
    IR::Program program(info); program.blocks.push_back(&block); program.post_order_blocks.push_back(&block);
    Optimization::FlattenExtendedUserdataPass(program);
    const auto off_a = a.Inst()->Flags<u16>(), off_b = b.Inst()->Flags<u16>();
    assert(off_a >= 16 && off_b >= 16 && off_a != off_b);
    assert(info.flattened_ud_buf[off_a] == 11 && info.flattened_ud_buf[off_b] == 21);
    std::vector<u8> original(reinterpret_cast<u8*>(info.srt_info.walker_func),
                             reinterpret_cast<u8*>(info.srt_info.walker_func) + info.srt_info.walker_func_size);
    mapped_a = false; info.RefreshFlatBuf();
    assert(info.flattened_ud_buf[off_a] == 0 && info.flattened_ud_buf[off_b] == 21);
    assert(std::memcmp(info.flattened_ud_buf.data(), ud.data(), sizeof(ud)) == 0);
    mapped_a = true; table_a[0] = 99; info.RefreshFlatBuf();
    assert(info.flattened_ud_buf[off_a] == 99 && info.flattened_ud_buf[off_b] == 21);
    assert(std::memcmp(original.data(), reinterpret_cast<void*>(info.srt_info.walker_func), original.size()) == 0);
    Serialization::Archive written_cache; info.srt_info.Serialize(written_cache);
    Serialization::Archive cache(written_cache.TakeOff());
    PersistentSrtInfo restored; assert(restored.Deserialize(cache));
    std::array<u32, 32> flat{}; restored.walker_func(ud.data(), flat.data(), SrtMemoryMapped);
    assert(flat[off_a] == 99 && flat[off_b] == 21);

    // Real Windows access violations in an imported reader.
    Xbyak::CodeGenerator raw;
    raw.push(raw.rdi); raw.mov(raw.rdi, raw.ptr[raw.rdi]);
    raw.mov(raw.r10d, raw.dword[raw.rdi]); raw.mov(raw.dword[raw.rsi], raw.r10d);
    raw.pop(raw.rdi); raw.ret(); raw.ready();
    auto reader = RegisterWalkerCode(raw.getCode(), raw.getSize());
    u64 pointer = start_a; u32 out = 0;
    reader(reinterpret_cast<u32*>(&pointer), &out, SrtMemoryMapped); assert(out == 99);
    pointer = 0; reader(reinterpret_cast<u32*>(&pointer), &out, SrtMemoryMapped); assert(out == 0);
    reader(nullptr, &out, SrtMemoryMapped); assert(out == 0); // RDI and R10D failures
    pointer = start_a; table_a[0] = 123;
    reader(reinterpret_cast<u32*>(&pointer), &out, SrtMemoryMapped); assert(out == 123);
    assert(std::memcmp(raw.getCode(), reinterpret_cast<void*>(reader), raw.getSize()) == 0);
    assert(IsLegacySrtWalkerSafe(raw.getCode(), raw.getSize()));
    Xbyak::CodeGenerator bad;
    bad.xor_(bad.rdi, bad.rdi); bad.nop(); bad.ret(); bad.ready();
    assert(!IsLegacySrtWalkerSafe(bad.getCode(), bad.getSize()));
    PersistentSrtInfo old{}; old.reader_revision = 0;
    old.walker_func = reinterpret_cast<PFN_SrtWalker>(const_cast<u8*>(bad.getCode()));
    old.walker_func_size = bad.getSize(); Serialization::Archive bad_written; old.Serialize(bad_written);
    Serialization::Archive bad_cache(bad_written.TakeOff());
    assert(!restored.Deserialize(bad_cache));
    Xbyak::CodeGenerator zero;
    zero.mov(zero.dword[zero.rsi + 64], 0); zero.ret(); zero.ready();
    assert(!IsLegacySrtWalkerSafe(zero.getCode(), zero.getSize()));
    RemoveVectoredExceptionHandler(handler);
    std::puts("SRT recovery: valid/unmapped/valid, sibling preservation, real faults, immutable bytes, cache recovery PASS");
}
