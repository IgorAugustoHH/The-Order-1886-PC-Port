// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <xbyak/xbyak.h>
#include "common/try_emit.h"

int main() {
    Xbyak::CodeGenerator code;
    code.mov(code.eax, 42);
    const auto before = code.getSize();
    assert(!Common::TryEmit(code, [&] {
        code.push(code.rdi); // failed PushPtr used to leave this on the stack
        code.mov(code.eax, 99);
        return Common::TryEmit(code, [&] {
            code.push(code.r10);
            return false; // unsupported nested expression
        });
    }));
    assert(code.getSize() == before);
    assert(Common::TryEmit(code, [&] { code.add(code.eax, 1); return true; }));
    code.ret();
    code.ready();
    assert(code.getCode<int (*)()>()() == 43); // return address and registers stay intact
    std::puts("SRT emission: nested rollback and executable stack balance passed");
}
