// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/srt_resource_use.h"
int main() {
    using namespace Shader;
    using namespace Optimization;
    assert(IsSrtDescriptorOperand(IR::Opcode::ImageSampleRaw, 0));
    assert(IsSrtDescriptorOperand(IR::Opcode::ImageSampleRaw, 1));
    for (auto op : {IR::Opcode::ImageRead, IR::Opcode::ImageWrite,
         IR::Opcode::ImageQueryDimensions, IR::Opcode::ImageQueryLod,
         IR::Opcode::ImageSampleImplicitLod, IR::Opcode::ImageGather,
         IR::Opcode::ImageGradient, IR::Opcode::ImageAtomicIAdd32}) {
        assert(IsSrtDescriptorOperand(op, 0)); assert(!IsSrtDescriptorOperand(op, 1));
    }
    {
        Common::ObjectPool<IR::Inst> pool; IR::Block block(pool); IR::IREmitter ir(block);
        auto data = ir.GetUserData(IR::ScalarReg(0));
        auto fixed4 = ir.CompositeConstruct(ir.Imm32(0u),ir.Imm32(0u),ir.Imm32(0u),ir.Imm32(0u));
        auto handle = ir.ImageHandle(fixed4, fixed4);
        auto coords = ir.CompositeConstruct(data,ir.Imm32(0u),ir.Imm32(0u),ir.Imm32(0u));
        auto sample = ir.ImageRead(handle, coords, ir.Imm32(0u), ir.Imm32(0u), {});
        // Reading at this coordinate must not classify the coordinate as descriptor bits,
        // even if the resulting data later contributes to another image handle.
        auto word = ir.BitCast<IR::U32>(IR::F32(ir.CompositeExtract(sample, 0)));
        auto next = ir.CompositeConstruct(word,ir.Imm32(0u),ir.Imm32(0u),ir.Imm32(0u));
        (void)ir.ImageHandle(next, fixed4);
        assert(!SrtFeedsDescriptor(data.Inst()));
        assert(SrtFeedsDescriptor(word.Inst()));
    }
    {
        Common::ObjectPool<IR::Inst> pool; IR::Block block(pool); IR::IREmitter ir(block);
        auto word = ir.GetUserData(IR::ScalarReg(0));
        auto words = ir.CompositeConstruct(word,ir.Imm32(0u),ir.Imm32(0u),ir.Imm32(0u));
        (void)ir.ImageHandle(words, words);
        assert(SrtFeedsDescriptor(word.Inst()));
    }
    std::puts("SRT resource use: descriptors preserved; coordinates/LOD/read addresses remain data PASS");
}
