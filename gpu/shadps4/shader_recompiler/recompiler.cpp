// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fmt/format.h>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <string>
#include <mutex>
#include <chrono>
#include "common/logging/classes.h"
#include "shader_recompiler/frontend/control_flow_graph.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/structured_control_flow.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/post_order.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"

namespace Shader {

IR::BlockList GenerateBlocks(const IR::AbstractSyntaxList& syntax_list) {
    size_t num_syntax_blocks{};
    for (const auto& [_, type] : syntax_list) {
        if (type == IR::AbstractSyntaxNode::Type::Block) {
            ++num_syntax_blocks;
        }
    }
    IR::BlockList blocks{};
    blocks.reserve(num_syntax_blocks);
    for (const auto& [data, type] : syntax_list) {
        if (type == IR::AbstractSyntaxNode::Type::Block) {
            blocks.push_back(data.block);
        }
    }
    return blocks;
}

void EmitControlFlowGraph(IR::Program& program, Pools& pools, Gcn::CFG& cfg,
                          RuntimeInfo& runtime_info, const Profile& profile) {
    Gcn::Translator translator{program.info, runtime_info, profile};
    for (auto& block : cfg) {
        const u32 start = block.begin_index;
        const u32 size = block.end_index - start + 1;
        auto* ir_block = pools.block_pool.Create(pools.inst_pool);
        ir_block->cfg_block = &block;
        block.ir_block = ir_block;
        translator.Translate(ir_block, block.begin, block.cond,
                             std::span{program.ins_list}.subspan(start, size));
        program.blocks.push_back(ir_block);
    }
    translator.EmitPrologue(program.blocks.front());
    ASSERT_MSG(!program.info.translation_failed, "Shader translation has failed");
    for (auto& block : cfg) {
        auto* ir_block = block.ir_block;
        if (block.branch_true) {
            auto* true_block = block.branch_true->ir_block;
            ir_block->AddBranch(true_block);
        }
        if (block.branch_false) {
            auto* false_block = block.branch_false->ir_block;
            ir_block->AddBranch(false_block);
        }
    }
    program.post_order_blocks = Shader::IR::PostOrder(program.blocks.front());
}

namespace {
struct PhaseTotal {
    u64 ns = 0;
    u64 count = 0;
};
std::mutex g_profile_mutex;
std::vector<std::pair<const char*, PhaseTotal>> g_profile_totals; // by first use
u64 g_profile_shaders = 0, g_profile_shader_ns = 0;
std::chrono::steady_clock::time_point g_profile_window = std::chrono::steady_clock::now();
thread_local std::vector<std::pair<const char*, u64>> tl_phases;
} // namespace

bool TranslateProfileOn() {
    static const bool on = [] {
        const char* env = std::getenv("BB_TRANSLATE_PROFILE");
        return env && env[0] == '1';
    }();
    return on;
}

void TranslateProfileAdd(const char* phase, u64 ns) {
    if (!TranslateProfileOn()) {
        return;
    }
    for (auto& [name, total] : tl_phases) {
        if (name == phase) {
            total += ns;
            return;
        }
    }
    tl_phases.emplace_back(phase, ns);
}

void TranslateProfileDone(u64 pgm_hash, u32 stage, size_t instructions) {
    if (!TranslateProfileOn()) {
        return;
    }
    auto phases = std::move(tl_phases);
    tl_phases.clear();
    u64 total = 0;
    for (const auto& [name, ns] : phases) {
        total += ns;
    }
    std::ranges::sort(phases, [](const auto& a, const auto& b) { return a.second > b.second; });
    std::scoped_lock lock{g_profile_mutex};
    for (const auto& [name, ns] : phases) {
        auto it = std::ranges::find_if(g_profile_totals, [&](const auto& e) { return e.first == name; });
        if (it == g_profile_totals.end()) {
            g_profile_totals.emplace_back(name, PhaseTotal{});
            it = g_profile_totals.end() - 1;
        }
        it->second.ns += ns;
        ++it->second.count;
    }
    ++g_profile_shaders;
    g_profile_shader_ns += total;
    static u32 logged = 0;
    if (logged++ < 400) {
        std::string top;
        for (size_t i = 0; i < phases.size() && i < 3; ++i) {
            top += fmt::format("{}{} {:.1f}", i ? ", " : "", phases[i].first, phases[i].second / 1e6);
        }
        std::printf("Translate profile: shader %016llx stage %u, %zu instructions, %.1f ms: %s\n",
                    (unsigned long long)pgm_hash, stage, instructions, total / 1e6, top.c_str());
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - g_profile_window >= std::chrono::seconds(5)) {
        g_profile_window = now;
        auto sorted = g_profile_totals;
        std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
        std::string line;
        for (size_t i = 0; i < sorted.size() && i < 12; ++i) {
            line += fmt::format("{}{} {:.0f} ms", i ? ", " : "", sorted[i].first,
                                sorted[i].second.ns / 1e6);
        }
        std::printf("Translate profile totals: %llu shaders, %.0f ms in all; %s\n",
                    (unsigned long long)g_profile_shaders, g_profile_shader_ns / 1e6, line.c_str());
    }
}

namespace {
struct PhaseTimer {
    const char* name;
    std::chrono::steady_clock::time_point start = TranslateProfileOn()
                                                      ? std::chrono::steady_clock::now()
                                                      : std::chrono::steady_clock::time_point{};
    ~PhaseTimer() {
        if (TranslateProfileOn()) {
            TranslateProfileAdd(name, u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - start).count()));
        }
    }
};
} // namespace

IR::Program TranslateProgram(const std::span<const u32>& code, Pools& pools, Info& info,
                             RuntimeInfo& runtime_info, const Profile& profile) {
    // Ensure first instruction is expected.
    constexpr u32 token_mov_vcchi = 0xBEEB03FF;
    if (code[0] != token_mov_vcchi) {
        LOG_WARNING(Render_Recompiler, "First instruction is not s_mov_b32 vcc_hi, #imm");
    }

    Gcn::GcnCodeSlice slice(code.data(), code.data() + code.size());
    Gcn::GcnDecodeContext decoder;

    // Decode and save instructions
    IR::Program program{info};
    program.ins_list.reserve(code.size());
    { PhaseTimer phase_timer{"decode"};
    while (!slice.atEnd()) {
        program.ins_list.emplace_back(decoder.decodeInstruction(slice));
    }
    }

    // Clear any previous pooled data.
    pools.ReleaseContents();

    // Create control flow graph
    Common::ObjectPool<Gcn::Block> gcn_block_pool{64};
    const auto cfg_start = std::chrono::steady_clock::now();
    Gcn::CFG cfg{gcn_block_pool, program.ins_list};
    TranslateProfileAdd("cfg", u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - cfg_start).count()));
    { PhaseTimer phase_timer{"translate (CFG+GCN->IR)"};
    EmitControlFlowGraph(program, pools, cfg, runtime_info, profile);
    }

    // On NVIDIA GPUs HW interpolation of clip distance values seems broken, and we need to emulate
    // it with expensive discard in PS.
    { PhaseTimer phase_timer{"InjectClipDistance"}; Shader::InjectClipDistanceAttributes(program, runtime_info); }

    // Run optimization passes on unstructured graph
    if (!profile.support_float64) {
        { PhaseTimer phase_timer{"LowerFp64ToFp32"}; Shader::Optimization::LowerFp64ToFp32(program); }
    }
    { PhaseTimer phase_timer{"SsaRewritePass"}; Shader::Optimization::SsaRewritePass(program); }
    { PhaseTimer phase_timer{"ConstantPropagationPass"}; Shader::Optimization::ConstantPropagationPass(program.post_order_blocks); }
    if (info.sw_stage == SwStage::TessellationControl) {
        { PhaseTimer phase_timer{"TessellationPreprocess"}; Shader::Optimization::TessellationPreprocess(program, runtime_info); }
        { PhaseTimer phase_timer{"HullShaderTransform"}; Shader::Optimization::HullShaderTransform(program, runtime_info); }
    } else if (info.sw_stage == SwStage::TessellationEval) {
        { PhaseTimer phase_timer{"TessellationPreprocess"}; Shader::Optimization::TessellationPreprocess(program, runtime_info); }
        { PhaseTimer phase_timer{"DomainShaderTransform"}; Shader::Optimization::DomainShaderTransform(program, runtime_info); }
    }
    { PhaseTimer phase_timer{"RingAccessElimination"}; Shader::Optimization::RingAccessElimination(program, runtime_info); }
    { PhaseTimer phase_timer{"ReadLaneEliminationPass"}; Shader::Optimization::ReadLaneEliminationPass(program); }
    Shader::IR::DumpProgram(program, info, "pre-res-discover.");
    const auto discover_start = std::chrono::steady_clock::now();
    auto resources = Shader::Optimization::ResourceDiscoverPass(program, profile);
    TranslateProfileAdd("ResourceDiscoverPass", u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - discover_start).count()));
    { PhaseTimer phase_timer{"FlattenExtendedUserdataPass"}; Shader::Optimization::FlattenExtendedUserdataPass(program); }
    Shader::IR::DumpProgram(program, info, "pre-res-patch.");
    { PhaseTimer phase_timer{"ResourcePatchingPass"}; Shader::Optimization::ResourcePatchingPass(program.info, resources, profile); }
    { PhaseTimer phase_timer{"LowerBufferFormatToRaw"}; Shader::Optimization::LowerBufferFormatToRaw(program); }
    { PhaseTimer phase_timer{"SharedMemorySimplifyPass"}; Shader::Optimization::SharedMemorySimplifyPass(program, profile); }
    { PhaseTimer phase_timer{"SharedMemoryToStoragePass"}; Shader::Optimization::SharedMemoryToStoragePass(program, runtime_info, profile); }
    { PhaseTimer phase_timer{"LowerUserClipPlanes"}; Shader::Optimization::LowerUserClipPlanes(program, runtime_info); }
    { PhaseTimer phase_timer{"PhiSimplificationPass"}; Shader::Optimization::PhiSimplificationPass(program); }
    { PhaseTimer phase_timer{"InverseBallotEliminationPass"}; Shader::Optimization::InverseBallotEliminationPass(program); }
    { PhaseTimer phase_timer{"LowerHardwareIntrinsics"}; Shader::Optimization::LowerHardwareIntrinsics(program); }
    Shader::IR::DumpProgram(program, info, "pre-lower-phi.");

    // Prepare for structurization by clearing flow graph and lowering phis
    for (auto* ir_block : program.blocks) {
        ir_block->imm_predecessors.clear();
        ir_block->imm_successors.clear();
        ir_block->ssa_state.Reset();
    }
    { PhaseTimer phase_timer{"LowerPhisToRegsPass"}; Shader::Optimization::LowerPhisToRegsPass(program); }

    // Structurize control flow graph and create program.
    { PhaseTimer phase_timer{"BuildASL"}; program.syntax_list = Shader::Gcn::BuildASL(pools, cfg, info); }
    { PhaseTimer phase_timer{"GenerateBlocks"}; program.blocks = GenerateBlocks(program.syntax_list); }
    program.post_order_blocks = Shader::IR::PostOrder(program.syntax_list.front().data.block);

    // Run optimization passes on structured graph
    { PhaseTimer phase_timer{"SsaRepairPass"}; Shader::Optimization::SsaRepairPass(program); }
    { PhaseTimer phase_timer{"SsaRewritePass"}; Shader::Optimization::SsaRewritePass(program); }
    { PhaseTimer phase_timer{"SharedMemoryBarrierPass"}; Shader::Optimization::SharedMemoryBarrierPass(program, runtime_info, profile); }
    { PhaseTimer phase_timer{"DeadCodeEliminationPass"}; Shader::Optimization::DeadCodeEliminationPass(program); }
    { PhaseTimer phase_timer{"LowerWave64BallotPass"}; Shader::Optimization::LowerWave64BallotPass(program, runtime_info, profile); }
    { PhaseTimer phase_timer{"ConstantPropagationPass"}; Shader::Optimization::ConstantPropagationPass(program.post_order_blocks); }
    { PhaseTimer phase_timer{"DeadCodeEliminationPass"}; Shader::Optimization::DeadCodeEliminationPass(program); }
    if (info.sw_stage == SwStage::Fragment && profile.needs_integer_interpolation_fix) {
        { PhaseTimer phase_timer{"InterpolatedIntegerPass"}; Shader::Optimization::InterpolatedIntegerPass(program); }
    }
    { PhaseTimer phase_timer{"CollectShaderInfoPass"}; Shader::Optimization::CollectShaderInfoPass(program, profile); }
    Shader::IR::DumpProgram(program, info);

    return program;
}

} // namespace Shader
