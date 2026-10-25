// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "shader_recompiler/specialization.h"

int main() {
    Shader::Info info;
    Shader::StageSpecialization a, b;
    a.info = b.info = &info;
    a.buffers.emplace_back();
    b.buffers.emplace_back();
    a.bitset.set(0);
    a.buffers[0].stride = 16;
    // Runtime lookup deliberately accepts an unbound resource. Cache identity must not.
    assert(a == b);
    assert(!a.SameCacheKey(b));
    assert(!b.SameCacheKey(a));
    b = a;
    assert(a.SameCacheKey(b));
    b.buffers[0].stride = 32;
    assert(!a.SameCacheKey(b));
    b = a;
    b.buffers.clear();
    assert(!(a == b) && !(b == a)); // safe mismatch, no out-of-bounds reads
    std::puts("shader cache: exact identity versus directional compatibility passed");
}
