// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/object_pool.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/program.h"

namespace Shader {

struct Profile;
struct RuntimeInfo;

struct Pools {
    static constexpr u32 InstPoolSize = 8192;
    static constexpr u32 BlockPoolSize = 32;

    Common::ObjectPool<IR::Inst> inst_pool;
    Common::ObjectPool<IR::Block> block_pool;

    explicit Pools() : inst_pool{InstPoolSize}, block_pool{BlockPoolSize} {}

    void ReleaseContents() {
        block_pool.ReleaseContents();
        inst_pool.ReleaseContents();
    }
};

[[nodiscard]] IR::Program TranslateProgram(const std::span<const u32>& code, Pools& pools,
                                           Info& info, RuntimeInfo& runtime_info,
                                           const Profile& profile);

/// bbport (Claude, 28/10): BB_TRANSLATE_PROFILE=1 times every translation phase (no effect off).
bool TranslateProfileOn();
/// Adds a phase of the shader being translated on this thread.
void TranslateProfileAdd(const char* phase, u64 ns);
/// Closes the shader: per-shader line (slowest phases) and the periodic phase totals.
void TranslateProfileDone(u64 pgm_hash, u32 stage, size_t instructions);

} // namespace Shader
