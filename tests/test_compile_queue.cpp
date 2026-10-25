// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "bb_compile_queue.h"
#include "bb_compile_budget.h"
int main() {
    using namespace BbCompile;
    unsigned cursor = 0, shader = 0, graphics = 0, compute = 0;
    for (unsigned i = 0; i < 10000; ++i) {
        // New shaders arrive continuously: both other queues must still make progress.
        const auto result = SelectFairJob(100000, 100000, 100000, cursor);
        shader += result == JobKind::Shader;
        graphics += result == JobKind::Graphics;
        compute += result == JobKind::Compute;
        if (i % 4 == 3) { assert(graphics == (i + 1) / 4); assert(compute == graphics); }
    }
    assert(shader == 5000 && graphics == 2500 && compute == 2500);
    for (unsigned i = 0; i < 100; ++i) assert(SelectFairJob(0, 1, 0, cursor) == JobKind::Graphics);
    assert(SelectFairJob(0, 0, 0, cursor) == JobKind::None);
    assert(Budget(4, {40, 1800, true}) == 1); // historical mode remains unchanged
    assert(Budget(4, {40, 1800, true}, true) == 4);
    assert(Budget(4, {40, 700, true}, true) == 1);
    assert(Budget(4, {96, 4000, true}, true) == 2);
    std::puts("Compile scheduling: continuous translations do not starve pipelines; admission modes passed");
}
