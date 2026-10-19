// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include "bb_compile_budget.h"
#include "bb_compile_queue.h"
#include "bb_scene_diag.h"
#include "bb_face_depth_fix.h"
#include "bb_particle_srt.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <mutex>
#include <ranges>
#include <string>
#include <unordered_set>

#include "common/hash.h"
#include "common/thread.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/motion_history.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"


#ifdef _WIN32
// bbport (Claude, 13/10): free physical RAM without pulling windows.h into this file.
extern "C" __declspec(dllimport) int __stdcall GlobalMemoryStatusEx(void* status);
#endif
namespace {
u64 AvailableRamMiB() {
#ifdef _WIN32
    struct {
        u32 length, load;
        u64 total_phys, avail_phys, total_page, avail_page, total_virtual, avail_virtual,
            avail_extended;
    } status{};
    status.length = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        return status.avail_phys >> 20;
    }
#endif
    return 0;
}
} // namespace

namespace Vulkan {

using Shader::HwStage;
using Shader::Output;
using Shader::SwStage;

constexpr static auto SpirvVersion1_6 = 0x00010600U;

constexpr static std::array DescriptorHeapSizes = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 512},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1024},
};

static u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    u32 num_outputs = 0;

    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[num_outputs++];
        misc_vec[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc_vec[1] = ctl.use_vtx_edge_flag
                          ? Output::EdgeFlag
                          : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc_vec[2] =
            ctl.use_vtx_kill_flag
                ? Output::KillFlag
                : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }

    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[num_outputs++];
        ccdist0[0] = ctl.IsClipDistEnabled(0)
                         ? Output::ClipDist0
                         : (ctl.IsCullDistEnabled(0) ? Output::CullDist0 : Output::None);
        ccdist0[1] = ctl.IsClipDistEnabled(1)
                         ? Output::ClipDist1
                         : (ctl.IsCullDistEnabled(1) ? Output::CullDist1 : Output::None);
        ccdist0[2] = ctl.IsClipDistEnabled(2)
                         ? Output::ClipDist2
                         : (ctl.IsCullDistEnabled(2) ? Output::CullDist2 : Output::None);
        ccdist0[3] = ctl.IsClipDistEnabled(3)
                         ? Output::ClipDist3
                         : (ctl.IsCullDistEnabled(3) ? Output::CullDist3 : Output::None);
    }

    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[num_outputs++];
        ccdist1[0] = ctl.IsClipDistEnabled(4)
                         ? Output::ClipDist4
                         : (ctl.IsCullDistEnabled(4) ? Output::CullDist4 : Output::None);
        ccdist1[1] = ctl.IsClipDistEnabled(5)
                         ? Output::ClipDist5
                         : (ctl.IsCullDistEnabled(5) ? Output::CullDist5 : Output::None);
        ccdist1[2] = ctl.IsClipDistEnabled(6)
                         ? Output::ClipDist6
                         : (ctl.IsCullDistEnabled(6) ? Output::CullDist6 : Output::None);
        ccdist1[3] = ctl.IsClipDistEnabled(7)
                         ? Output::ClipDist7
                         : (ctl.IsCullDistEnabled(7) ? Output::CullDist7 : Output::None);
    }

    return num_outputs;
}

const Shader::RuntimeInfo& PipelineCache::BuildRuntimeInfo(PipelineSelection& sel, HwStage stage,
                                                             SwStage l_stage) {
    auto& info = sel.runtime_infos[u32(l_stage)];
    const auto& regs = (*sel.regs);
    const auto BuildCommon = [&](const auto& program) {
        info.props.num_user_data = program.settings.num_user_regs;
        info.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
        info.props.num_allocated_vgprs = program.NumVgprs();
        info.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = program.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
    };
    info.Initialize(stage, l_stage);
    switch (stage) {
    case HwStage::Local: {
        BuildCommon(regs.ls_program);
        Shader::TessellationDataConstantBuffer tess_constants{};
        const auto* hull_info = sel.infos[u32(SwStage::TessellationControl)];
        hull_info->ReadTessConstantBuffer(tess_constants);
        info.hw.ls.ls_stride = tess_constants.ls_stride;
        break;
    }
    case HwStage::Hull:
        BuildCommon(regs.hs_program);
        break;
    case HwStage::Export:
        BuildCommon(regs.es_program);
        info.hw.es.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        break;
    case HwStage::Geometry: {
        BuildCommon(regs.gs_program);
        info.hw.gs.num_outputs = MapOutputs(info.hw.gs.outputs, regs.vs_output_control);
        info.hw.gs.output_vertices = regs.vgt_gs_max_vert_out;
        info.hw.gs.num_invocations =
            regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
        if (regs.stage_enable.raw == AmdGpu::ShaderStageEnable::LsHsEsGs) {
            info.hw.gs.in_primitive = [&]() {
                switch (regs.tess_config.topology) {
                case AmdGpu::TessellationTopology::Point:
                    return AmdGpu::PrimitiveType::PointList;
                case AmdGpu::TessellationTopology::Line:
                    return AmdGpu::PrimitiveType::LineList;
                case AmdGpu::TessellationTopology::TriangleCw:
                case AmdGpu::TessellationTopology::TriangleCcw:
                    return AmdGpu::PrimitiveType::TriangleList;
                default:
                    UNREACHABLE();
                }
            }();
        } else {
            info.hw.gs.in_primitive = regs.primitive_type;
        }
        for (u32 stream_id = 0; stream_id < Shader::GsMaxOutputStreams; ++stream_id) {
            info.hw.gs.out_primitive[stream_id] =
                regs.vgt_gs_out_prim_type.GetPrimitiveType(stream_id);
        }
        info.hw.gs.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
        info.hw.gs.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
        info.hw.gs.mode = regs.vgt_gs_mode.mode;
        const auto params_vc = AmdGpu::GetParams(regs.vs_program);
        info.hw.gs.vs_copy = params_vc.code;
        info.hw.gs.vs_copy_hash = params_vc.hash;
        DumpShader(info.hw.gs.vs_copy, info.hw.gs.vs_copy_hash, Shader::HwStage::Vertex, 0,
                   "copy.bin");
        break;
    }
    case HwStage::Vertex: {
        BuildCommon(regs.vs_program);
        info.hw.vs.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
        info.hw.vs.num_outputs = MapOutputs(info.hw.vs.outputs, regs.vs_output_control);
        info.hw.vs.emulate_depth_negative_one_to_one =
            !instance.IsDepthClipControlSupported() &&
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        info.hw.vs.clip_disable = regs.IsClipDisabled();
        info.hw.vs.motion_vectors = sel.motion;
        break;
    }
    case HwStage::Fragment: {
        BuildCommon(regs.ps_program);
        info.hw.fs.en_flags = regs.ps_input_ena;
        info.hw.fs.addr_flags = regs.ps_input_addr;
        info.hw.fs.num_inputs = regs.num_interp;
        info.hw.fs.front_face_all_bits = regs.barycentric_control.front_face_all_bits;
        info.hw.fs.num_samples =
            regs.ps_input_addr.sample_coverage_ena && regs.ps_input_ena.sample_coverage_ena
                ? regs.aa_config.NumSamples()
                : 1;
        info.hw.fs.z_export_format = regs.z_export_format;
        info.hw.fs.motion_vectors = sel.motion;
        u8 stencil_ref_export_enable = regs.depth_shader_control.stencil_op_val_export_enable |
                                       regs.depth_shader_control.stencil_test_val_export_enable;
        info.hw.fs.mrtz_mask = regs.depth_shader_control.z_export_enable |
                               (stencil_ref_export_enable << 1) |
                               (regs.depth_shader_control.mask_export_enable << 2) |
                               (regs.depth_shader_control.coverage_to_mask_enable << 3);
        const auto& cb0_blend = regs.blend_control[0];
        if (cb0_blend.enable) {
            info.hw.fs.dual_source_blending =
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_dst_factor) ||
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_src_factor);
            if (cb0_blend.separate_alpha_blend) {
                info.hw.fs.dual_source_blending |=
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_dst_factor) ||
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_src_factor);
            }
        } else {
            info.hw.fs.dual_source_blending = false;
        }
        const auto& ps_inputs = regs.ps_inputs;
        for (u32 i = 0; i < regs.num_interp; i++) {
            info.hw.fs.inputs[i] = {
                .param_index = u8(ps_inputs[i].input_offset),
                .is_default = bool(ps_inputs[i].use_default),
                .is_flat = bool(ps_inputs[i].flat_shade),
                .default_value = u8(ps_inputs[i].default_value),
            };
        }
        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {
            info.hw.fs.color_buffers[i] = sel.graphics_key.color_buffers[i];
        }
        // Lowered user clip planes ride the same emulation path as guest-exported distances, so
        // the fragment side arms whenever the hardware vertex stage lowers them, keeping its input
        // locations in sync with the shifted vertex outputs.
        const bool lowers_user_clip_planes =
            regs.clipper_control.user_clip_plane_enable &&
            !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Geometry));
        info.hw.fs.clip_distance_emulation =
            ((regs.vs_output_control.clip_distance_enable &&
              !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Local))) ||
             lowers_user_clip_planes) &&
            profile.needs_clip_distance_emulation;
        break;
    }
    case HwStage::Compute: {
        const auto& cs_pgm = liverpool->GetCsRegs();
        info.props.num_user_data = cs_pgm.settings.num_user_regs;
        info.props.num_allocated_vgprs = cs_pgm.settings.num_vgprs * 4;
        info.props.fp_denorm_mode32 = cs_pgm.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = cs_pgm.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = cs_pgm.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = cs_pgm.settings.fp_round_mode64;
        info.hw.cs.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                     cs_pgm.num_thread_z.full};
        info.hw.cs.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                                  cs_pgm.IsTgidEnabled(2)};
        info.hw.cs.shared_memory_size = cs_pgm.SharedMemSize();
        break;
    }
    default:
        break;
    }
    switch (l_stage) {
    case SwStage::Vertex:
        info.sw.vs.step_rate_0 = regs.vgt_instance_step_rate_0;
        info.sw.vs.step_rate_1 = regs.vgt_instance_step_rate_1;
        info.sw.vs.vertex_sgpr_offset = sel.draw_indirect_params.vertex_sgpr_offset;
        info.sw.vs.instance_sgpr_offset = sel.draw_indirect_params.instance_sgpr_offset;
        info.sw.vs.tess_emulated_primitive =
            regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
            regs.primitive_type == AmdGpu::PrimitiveType::QuadList;
        break;
    case SwStage::TessellationControl: {
        info.sw.tcs.num_input_control_points = regs.ls_hs_config.hs_input_control_points;
        info.sw.tcs.num_threads = regs.ls_hs_config.hs_output_control_points;
        info.sw.tcs.tess_type = regs.tess_config.type;
        info.sw.tcs.offchip_lds_enable = regs.hs_program.settings.oc_lds_en;
        break;
    }
    case SwStage::TessellationEval: {
        info.sw.tes.tess_type = regs.tess_config.type;
        info.sw.tes.tess_topology = regs.tess_config.topology;
        info.sw.tes.tess_partitioning = regs.tess_config.partitioning;
        break;
    }
    default:
        break;
    }
    return info;
}

extern std::atomic<u64> g_bb_compiles_total;

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             AmdGpu::Liverpool* liverpool_, u32 sparse_page_shift)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      desc_heap{instance, scheduler.GetWorkSemaphore(), DescriptorHeapSizes} {
    sel.regs = &liverpool->regs;
    const auto& vk12_props = instance.GetVk12Properties();
    profile = Shader::Profile{
        .max_viewport_width = instance.GetMaxViewportWidth(),
        .max_viewport_height = instance.GetMaxViewportHeight(),
        .max_shared_memory_size = instance.MaxComputeSharedMemorySize(),
        .supported_spirv = SpirvVersion1_6,
        .subgroup_size = instance.SubgroupSize(),
        .sparse_page_shift = sparse_page_shift,
        .support_int8 = instance.IsShaderInt8Supported(),
        .support_int16 = instance.IsShaderInt16Supported(),
        .support_int64 = instance.IsShaderInt64Supported(),
        .support_float16 = instance.IsShaderFloat16Supported(),
        .support_float64 = instance.IsShaderFloat64Supported(),
        .supports_denorm_behavior_independence =
            vk12_props.denormBehaviorIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .supports_rounding_mode_independence =
            vk12_props.roundingModeIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .support_fp16_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat16),
        .support_fp16_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat16),
        .support_fp16_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat16),
        .support_fp32_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat32),
        .support_fp32_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat32),
        .support_fp32_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat32),
        .support_fp64_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat64),
        .support_fp64_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat64),
        .support_fp64_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat64),
        .support_fp16_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat16),
        .support_fp32_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat32),
        .support_fp64_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat64),
        .supports_image_load_store_lod = instance_.IsImageLoadStoreLodSupported(),
        .supports_native_cube_calc = instance_.IsAmdGcnShaderSupported(),
        .supports_trinary_minmax = instance_.IsAmdShaderTrinaryMinMaxSupported(),
        .supports_buffer_fp32_atomic_min_max =
            instance_.IsShaderAtomicFloatBuffer32MinMaxSupported(),
        .supports_image_fp32_atomic_min_max = instance_.IsShaderAtomicFloatImage32MinMaxSupported(),
        .supports_buffer_int64_atomics = instance_.IsBufferInt64AtomicsSupported(),
        .supports_shared_int64_atomics = instance_.IsSharedInt64AtomicsSupported(),
        .supports_workgroup_explicit_memory_layout =
            instance_.IsWorkgroupMemoryExplicitLayoutSupported(),
        .supports_amd_shader_explicit_vertex_parameter =
            instance_.IsAmdShaderExplicitVertexParameterSupported(),
        .supports_fragment_shader_barycentric = instance_.IsFragmentShaderBarycentricSupported(),
        .supports_shader_subgroup_clock = instance_.IsShaderSubgroupClockSupported(),
        .needs_manual_interpolation = instance.IsFragmentShaderBarycentricSupported() &&
                                      instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        // bbport: older NVIDIA (Pascal) has no barycentrics; BB_INTERP_INT_FIX=0/1 overrides.
        .needs_integer_interpolation_fix = [&] {
            if (const char* env = std::getenv("BB_INTERP_INT_FIX")) {
                return env[0] == '1';
            }
            return !instance.IsFragmentShaderBarycentricSupported() &&
                   instance.GetDriverID() == vk::DriverId::eNvidiaProprietary;
        }(),
        .needs_lds_barriers = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                              instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_buffer_offsets = instance.StorageMinAlignment() > 4,
        .needs_unorm_fixup = instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_clip_distance_emulation = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .supports_shader_stencil_export = instance_.IsShaderStencilExportSupported(),
    };
    // bbport: the driver's pipeline cache persists between sessions. Every session compiled
    // 2000-3000 pipelines at 36-65 ms each (The Order: 1886, 77-200 s of stalls) with no gain
    // over the previous one. The driver validates the file (vendor, device, driver version)
    // and starts empty when it does not match. BB_DRIVER_PIPELINE_CACHE=0 disables it.
    const char* driver_cache_env = std::getenv("BB_DRIVER_PIPELINE_CACHE");
    // bbport (Claude, 13/10): opt-in now (BB_DRIVER_PIPELINE_CACHE=1). Runs 97/98: no measured
    // compile-time gain, ~500 MiB of RAM held, and 30 s saves fed the RAM guard.
    const bool persist = driver_cache_env && driver_cache_env[0] == '1';
    const auto cache_path =
        Common::FS::GetUserPath(Common::FS::PathType::ShaderDir) / "vk_pipeline_cache.bin";
    const auto startup_begin = std::chrono::steady_clock::now();
    const auto startup_mark = [&](const char* stage) {
        std::fprintf(stderr, "Startup cache: %s; elapsed_ms=%.1f\n", stage,
                     std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - startup_begin).count());
        std::fflush(stderr);
    };
    startup_mark("driver file read begin");
    std::vector<u8> initial;
    if (persist) {
        if (std::FILE* f = std::fopen(cache_path.string().c_str(), "rb")) {
            std::fseek(f, 0, SEEK_END);
            const long size = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (size > 0) {
                initial.resize(size_t(size));
                if (std::fread(initial.data(), 1, initial.size(), f) != initial.size()) {
                    initial.clear();
                }
            }
            std::fclose(f);
        }
    }
    startup_mark("driver file read end");
    const vk::PipelineCacheCreateInfo cache_ci = {
        .initialDataSize = initial.size(),
        .pInitialData = initial.empty() ? nullptr : initial.data(),
    };
    startup_mark("Vulkan createPipelineCache begin");
    auto [cache_result, cache] = instance.GetDevice().createPipelineCacheUnique(cache_ci);
    startup_mark("Vulkan createPipelineCache returned");
    if (cache_result != vk::Result::eSuccess && !initial.empty()) {
        std::printf("Driver pipeline cache: file rejected (%s), starting empty\n",
                    vk::to_string(cache_result).c_str());
        initial.clear();
        auto retry = instance.GetDevice().createPipelineCacheUnique({});
        cache_result = retry.result;
        cache = std::move(retry.value);
    }
    ASSERT_MSG(cache_result == vk::Result::eSuccess, "Failed to create pipeline cache: {}",
               vk::to_string(cache_result));
    pipeline_cache = std::move(cache);
    // bbport: after the driver cache exists, so preloaded pipelines use it too.
    startup_mark("WarmUp begin");
    WarmUp();
    startup_mark("WarmUp end");
    StartAsyncCompilers();
    startup_mark("async compiler start end");
    if (persist) {
        std::printf("Driver pipeline cache: loaded %.1f MiB from %s\n",
                    double(initial.size()) / (1024.0 * 1024.0), cache_path.string().c_str());
    }
    // The recent pipelines list (preload_shaders) is saved here too, so the thread runs without
    // the driver cache: with BB_DRIVER_PIPELINE_CACHE=0 (the default) the list was never written
    // and every session started with "no recent sessions yet".
    driver_cache_saver = std::jthread([this, cache_path, persist](std::stop_token stop) {
        // bbport (Claude, 13/10): getPipelineCacheData copies the whole driver cache (~650 MiB)
        // into RAM before writing it; every 30 s that spike pushed runs 95-97/108 under the
        // 700 MiB RAM guard. Ask the size first and only copy when free RAM covers it plus a
        // margin (BB_DRIVER_CACHE_MARGIN_MB, default 1536); otherwise retry later.
        static const u64 margin_mib = [] {
            const char* env = std::getenv("BB_DRIVER_CACHE_MARGIN_MB");
            return env ? std::strtoull(env, nullptr, 10) : 1536ull;
        }();
        const auto save = [&](bool final_save) -> bool {
            size_t size = 0;
            if (instance.GetDevice().getPipelineCacheData(*pipeline_cache, &size, nullptr) !=
                    vk::Result::eSuccess ||
                size == 0) {
                return false;
            }
            const u64 free_mib = AvailableRamMiB();
            const u64 size_mib = u64(size) >> 20;
            if (!final_save && free_mib != 0 && free_mib < size_mib + margin_mib) {
                static u32 skips = 0;
                if (skips++ % 10 == 0) {
                    std::printf("Driver pipeline cache: save of %llu MiB postponed (%llu MiB free)\n",
                                (unsigned long long)size_mib, (unsigned long long)free_mib);
                }
                return false;
            }
            auto [result, data] = instance.GetDevice().getPipelineCacheData(*pipeline_cache);
            if (result != vk::Result::eSuccess || data.empty()) {
                return false;
            }
            auto temp = cache_path;
            temp += ".tmp";
            if (std::FILE* f = std::fopen(temp.string().c_str(), "wb")) {
                const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
                std::fclose(f);
                std::error_code ec;
                if (ok) {
                    std::filesystem::rename(temp, cache_path, ec);
                }
            }
            return true;
        };
        u64 saved_compiles = g_bb_compiles_total.load();
        for (u32 ticks = 0; !stop.stop_requested(); ++ticks) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (ticks % 60 != 59) {
                continue; // every 30 s
            }
            SaveRecentPipelines();
            const u64 compiles = g_bb_compiles_total.load();
            if (persist && compiles != saved_compiles && save(false)) {
                saved_compiles = compiles;
            }
        }
        if (persist && g_bb_compiles_total.load() != saved_compiles) {
            save(true);
        }
        SaveRecentPipelines();
    });
}

PipelineCache::~PipelineCache() {
    for (auto& compiler : async_compilers) {
        compiler.request_stop();
    }
    {
        std::scoped_lock lock{async_mutex}; // a compiler between its check and its wait
    }
    async_cv.notify_all();
    async_compilers.clear();
}

// bbport: shader/pipeline compile time on the GPU thread, reported by BB_FRAME_STATS.
std::atomic<u64> g_bb_compile_ns;
std::atomic<u32> g_bb_compiles;
std::atomic<u64> g_bb_compiles_total; ///< never reset (the driver cache saver compares it)
namespace {
struct CompileTimer {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~CompileTimer() {
        g_bb_compile_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - start)
                                   .count());
        ++g_bb_compiles;
        ++g_bb_compiles_total;
    }
};
} // namespace

bool PipelineCache::PrepareGraphicsPipeline(PipelineSelection& worker_sel) {
    // Tessellation stages read constant buffers from memory at selection time: not prepared.
    if (worker_sel.regs->stage_enable.hs_en) {
        return false;
    }
    return RefreshGraphicsKey(worker_sel) && !worker_sel.worker->failed;
}

const GraphicsPipeline* PipelineCache::TryPreparedPipeline(const PreparedDraw& prepared) {
    if (prepared.state.load(std::memory_order_acquire) != PreparedDraw::Ready ||
        prepared.reg_checksum != liverpool->gfx_reg_checksum) {
        return nullptr;
    }
    // Same registers; the stage programs and their flattened user data (which also covers
    // everything read from guest memory for the specialization) must match as well. This is
    // the per-stage work GetProgram does on the regular path.
    const auto& regs = liverpool->regs;
    for (u32 i = 0; i < prepared.num_stages; ++i) {
        const auto& stage = prepared.stages[i];
        const auto* pgm = regs.ProgramForStage(static_cast<u32>(stage.hw_stage));
        if (!pgm || !pgm->Address<u32*>()) {
            return nullptr;
        }
        const auto params = AmdGpu::GetParams(*pgm);
        if (params.hash != stage.hash) {
            return nullptr;
        }
        auto& info = const_cast<Program*>(stage.program)->info;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        info.RefreshFlatBuf();
        if (BbParticleSrt::Enabled()) BbParticleSrt::Normalize(info);
        if (info.pgm_base != stage.pgm_base || info.flattened_ud_buf.size() != stage.flat_size ||
            std::memcmp(info.flattened_ud_buf.data(), stage.flat,
                        stage.flat_size * sizeof(u32)) != 0) {
            return nullptr;
        }
    }
    const auto it = graphics_pipelines.find(prepared.key);
    return it != graphics_pipelines.end() ? it->second.get() : nullptr;
}

const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(const DrawIndirectParams params,
                                                           const PreparedDraw* prepared) {
    used_prepared = nullptr;
    if (prepared) {
        if (const auto* pipeline = TryPreparedPipeline(*prepared)) {
            used_prepared = prepared;
            NoteUsed(pipeline);
            return pipeline;
        }
    }
    sel.draw_indirect_params = params;
    if (async_pipelines && async_done_count.load(std::memory_order_acquire) != 0) {
        DrainAsyncPipelines(); // finished permutations first: this draw may need one
    }
    shader_pending = false;
    BbSceneDiag::Poll();
    if (!RefreshGraphicsKey(sel) || shader_pending) {
        BbSceneDiag::Count(shader_pending ? BbSceneDiag::draw_shader : BbSceneDiag::invalid_key);
        shader_pending = false;
        sel.fetch_shader.reset();
        return nullptr;
    }
    if (async_pipelines && !synchronous_graphics) {
        if (const auto found = graphics_pipelines.find(sel.graphics_key);
            found != graphics_pipelines.end()) {
            NoteUsed(found->second.get());
            return found->second.get();
        }
        // Not built yet: queue it once and skip this draw.
        if (async_pending.insert(sel.graphics_key).second) {
            const auto diag_key = std::hash<GraphicsPipelineKey>{}(sel.graphics_key);
            BbSceneDiag::Event("graphics_requested", diag_key);
            if (BbSceneDiag::Enabled()) {
                for (u32 stage = 0; stage < MaxShaderStages; ++stage) {
                    if (sel.infos[stage]) BbSceneDiag::Event("graphics_stage", diag_key, sel.infos[stage]->pgm_hash, stage);
                }
            }
            AsyncGraphicsJob job{};
            job.key = sel.graphics_key;
            for (u32 stage = 0; stage < MaxShaderStages; ++stage) {
                if (sel.infos[stage]) {
                    job.snapshots[stage] = std::make_unique<Shader::Info>(*sel.infos[stage]);
                    job.live_infos[stage] = sel.infos[stage];
                }
            }
            job.runtime_infos = sel.runtime_infos;
            job.fetch_shader = sel.fetch_shader;
            job.modules = sel.modules;
            {
                std::scoped_lock lock{async_mutex};
                async_jobs.push_back(std::move(job));
            }
            async_cv.notify_one();
        }
        BbSceneDiag::Count(BbSceneDiag::draw_pipeline);
        sel.fetch_shader.reset();
        return nullptr;
    }
    const auto [it, is_new] = graphics_pipelines.try_emplace(sel.graphics_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(sel.graphics_key);
        LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x}", pipeline_hash);
        BbSceneDiag::Event("graphics_requested", pipeline_hash);
        if (BbSceneDiag::Enabled()) {
            for (u32 stage = 0; stage < MaxShaderStages; ++stage)
                if (sel.infos[stage]) BbSceneDiag::Event("graphics_stage", pipeline_hash, sel.infos[stage]->pgm_hash, stage);
        }
        CompileTimer timer;

        GraphicsPipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, sel.graphics_key, *pipeline_cache, sel.infos,
            sel.runtime_infos, sel.fetch_shader, sel.modules, sdata, false);

        it.value()->storage_name = RegisterPipelineData(sel.graphics_key, pipeline_hash, sdata);
        BbSceneDiag::Event("graphics_ready", pipeline_hash);
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            for (auto stage = 0; stage < MaxShaderStages; ++stage) {
                if (sel.infos[stage]) {
                    auto& m = sel.modules[stage];
                    module_related_pipelines[m].emplace_back(sel.graphics_key);
                }
            }
        }
        sel.fetch_shader.reset();
    }
    NoteUsed(it->second.get());
    return it->second.get();
}

void PipelineCache::StartAsyncCompilers() {
    const auto flag = [](const char* name) {
        const char* value = std::getenv(name);
        return value && value[0] == '1';
    };
    fair_compile_queue = flag("BB_COMPILE_FAIR_QUEUE");
    aggressive_compile = flag("BB_COMPILE_AGGRESSIVE");
    const char* graphics_env = std::getenv("BB_GRAPHICS_SYNC");
    synchronous_graphics = graphics_env && graphics_env[0] == '1';
    const char* env = std::getenv("BB_ASYNC_PIPELINES");
    async_pipelines = !(env && env[0] == '0');
    if (!async_pipelines) {
        return;
    }
    const char* shaders_env = std::getenv("BB_ASYNC_SHADERS");
    async_shaders = !(shaders_env && shaders_env[0] == '0');
    const char* compute_env = std::getenv("BB_ASYNC_COMPUTE");
    async_compute = async_shaders && !(compute_env && compute_env[0] == '0');
    std::fprintf(stderr, "Compile mode: graphics=%s compute=%s shader_async=%d\n",
                 synchronous_graphics ? "wait" : "async-skip",
                 async_compute ? "async-skip" : "wait", int(async_shaders));
    const char* count_env = std::getenv("BB_COMPILE_THREADS");
    const u32 NumCompilers = BbCompile::WorkerCount(count_env ? count_env : "", std::thread::hardware_concurrency());
    compile_report_time = std::chrono::steady_clock::now();
    for (u32 i = 0; i < NumCompilers; ++i) {
        async_compilers.emplace_back([this, NumCompilers](std::stop_token stop) {
            Common::SetCurrentThreadName("bb:PipelineCompile");
            if (!aggressive_compile) Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
            Shader::Pools worker_pools; // the translator's pools are per thread
            for (;;) {
                AsyncGraphicsJob job;
                std::optional<AsyncShaderJob> shader_job;
                std::optional<AsyncComputeJob> compute_job;
                {
                    std::unique_lock lock{async_mutex};
                    for (;;) {
                        if (stop.stop_requested()) return;
                        const auto pressure = BbCompile::ReadPressure();
                        const auto budget = BbCompile::Budget(NumCompilers, pressure, aggressive_compile);
                        const size_t queued = async_jobs.size() + async_shader_jobs.size() + async_compute_jobs.size();
                        compile_queue_peak = std::max(compile_queue_peak, queued);
                        const auto now = std::chrono::steady_clock::now();
                        if (now - compile_report_time >= std::chrono::seconds(5)) {
                            std::fprintf(stderr, "Compile pool: workers=%u budget=%u active=%u queued=%zu peak_observed=%zu completed=%llu worker_ms_sum=%.1f cpu_system=%.1f free_ram_mib=%llu pressure_valid=%d\n",
                                NumCompilers, budget, compile_active, queued, compile_queue_peak,
                                static_cast<unsigned long long>(compile_completed), compile_worker_ms,
                                pressure.cpu_percent, static_cast<unsigned long long>(pressure.available_mib), pressure.valid);
                            compile_report_time = now;
                            std::fprintf(stderr, "Compile queue: fair=%d aggressive=%d shaders_started=%llu graphics_started=%llu compute_started=%llu queue_ms_sum=%.1f queue_max_ms=%.1f\n",
                                int(fair_compile_queue), int(aggressive_compile),
                                (unsigned long long)compile_shader_started, (unsigned long long)compile_graphics_started,
                                (unsigned long long)compile_compute_started, compile_queue_ms, compile_queue_max_ms);
                        }
                        if (queued && compile_active < budget) {
                            ++compile_active;
                            break;
                        }
                        async_cv.wait_for(lock, std::chrono::milliseconds(100));
                    }
                    if (stop.stop_requested()) {
                        return;
                    }
                    const auto kind = fair_compile_queue
                        ? BbCompile::SelectFairJob(async_shader_jobs.size(), async_jobs.size(),
                                                   async_compute_jobs.size(), compile_queue_cursor)
                        : !async_shader_jobs.empty() ? BbCompile::JobKind::Shader
                        : !async_compute_jobs.empty() ? BbCompile::JobKind::Compute
                        : BbCompile::JobKind::Graphics;
                    if (kind == BbCompile::JobKind::Shader) {
                        shader_job = std::move(async_shader_jobs.front());
                        async_shader_jobs.pop_front();
                        ++compile_shader_started;
                    } else if (kind == BbCompile::JobKind::Compute) {
                        compute_job = std::move(async_compute_jobs.front());
                        async_compute_jobs.pop_front();
                        ++compile_compute_started;
                    } else {
                        job = std::move(async_jobs.front());
                        async_jobs.pop_front();
                        ++compile_graphics_started;
                    }
                    const auto enqueued = shader_job ? shader_job->queued_at
                        : compute_job ? compute_job->queued_at : job.queued_at;
                    const double waited = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - enqueued).count();
                    compile_queue_ms += waited;
                    compile_queue_max_ms = std::max(compile_queue_max_ms, waited);
                }
                const auto job_start = std::chrono::steady_clock::now();
                const auto finish_job = [this, job_start](void*) {
                    {
                        std::scoped_lock lock{async_mutex};
                        --compile_active;
                        ++compile_completed;
                        compile_worker_ms += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - job_start).count();
                    }
                    async_cv.notify_all();
                };
                std::unique_ptr<void, decltype(finish_job)> job_scope(this, finish_job);
                if (compute_job) {
                    auto& cj = *compute_job;
                    AsyncComputeResult result{};
                    result.key = cj.key;
                    {
                        CompileTimer timer;
                        result.pipeline = std::make_unique<ComputePipeline>(
                            instance, scheduler, desc_heap, profile, *pipeline_cache, cj.key,
                            *cj.snapshot, cj.module, result.sdata, false);
                    }
                    std::array<const Shader::Info*, Shader::MaxStageTypes> live{};
                    live[u32(Shader::SwStage::Compute)] = cj.live_info;
                    result.pipeline->RebindStageInfos(live);
                    {
                        std::scoped_lock lock{async_mutex};
                        async_compute_done.push_back(std::move(result));
                    }
                    async_done_count.fetch_add(1, std::memory_order_release);
                    continue;
                }
                if (shader_job) {
                    auto& sj = *shader_job;
                    const Shader::ShaderParams params{
                        .user_data = sj.user_data, .code = sj.code, .hash = sj.hash};
                    Shader::Info info(sj.hw_stage, sj.sw_stage, params);
                    info.pgm_base = sj.pgm_base;
                    info.flatbuf_snapshot = &sj.flatbuf;
                    AsyncShaderResult result{};
                    {
                        CompileTimer timer;
                        const auto ir_program = Shader::TranslateProgram(
                            sj.code, worker_pools, info, sj.runtime_info, profile);
                        result.spv = Shader::Backend::SPIRV::EmitSPIRV(profile, sj.runtime_info,
                                                                       ir_program, sj.binding);
                        result.module = CompileSPV(result.spv, instance.GetDevice());
                        if (sj.sw_stage == SwStage::Vertex)
                            BbFaceDepth::Register(result.module, result.spv, sj.hash);
                    }
                    worker_pools.ReleaseContents();
                    result.diag_ticket = sj.diag_ticket;
                    BbSceneDiag::Event("shader_built", sj.diag_ticket, sj.hash);
                    result.program = sj.program;
                    result.spec = std::move(sj.spec);
                    {
                        std::scoped_lock lock{async_mutex};
                        async_shader_done.push_back(std::move(result));
                    }
                    async_done_count.fetch_add(1, std::memory_order_release);
                    continue;
                }
                std::array<const Shader::Info*, MaxShaderStages> infos{};
                for (u32 stage = 0; stage < MaxShaderStages; ++stage) {
                    infos[stage] = job.snapshots[stage].get();
                }
                AsyncGraphicsResult result{};
                result.key = job.key;
                {
                    CompileTimer timer;
                    result.pipeline = std::make_unique<GraphicsPipeline>(
                        instance, scheduler, desc_heap, profile, job.key, *pipeline_cache, infos,
                        job.runtime_infos, job.fetch_shader, job.modules, result.sdata, false);
                }
                result.pipeline->RebindStageInfos(job.live_infos);
                {
                    std::scoped_lock lock{async_mutex};
                    async_done.push_back(std::move(result));
                }
                async_done_count.fetch_add(1, std::memory_order_release);
            }
        });
    }
    std::printf("Compile admission: %s; fair=%d; CPU temperature unavailable; running jobs are not interrupted\n",
        aggressive_compile ? "normal priority; RAM<768MiB:1; CPU>=95%:half workers" :
                             "low priority; CPU>=85% or RAM<2048MiB:1", int(fair_compile_queue));
    std::printf("Pipelines: built on %u compile threads (BB_ASYNC_PIPELINES=0: on the GPU "
                "thread)\n",
                NumCompilers);
}

void PipelineCache::DrainAsyncPipelines() {
    std::vector<AsyncGraphicsResult> done;
    std::vector<AsyncShaderResult> shaders_done;
    std::vector<AsyncComputeResult> compute_done;
    {
        std::scoped_lock lock{async_mutex};
        done.swap(async_done);
        shaders_done.swap(async_shader_done);
        compute_done.swap(async_compute_done);
        async_done_count.store(0, std::memory_order_relaxed);
    }
    for (auto& result : shaders_done) {
        Program& program = *result.program;
        BbSceneDiag::Event("shader_ready", result.diag_ticket, program.info.pgm_hash);
        auto& pending = program.pending_specs;
        if (const auto it = std::ranges::find_if(
                pending, [&](const auto& spec) { return spec.SameCacheKey(result.spec); });
            it != pending.end()) {
            pending.erase(it);
        }
        const size_t perm_idx = program.modules.size();
        const u64 perm_hash = HashCombine(program.info.pgm_hash, perm_idx);
        RegisterShaderBinary(std::move(result.spv),
                             RegisterShaderMeta(program.info, result.spec.fetch_shader_data,
                                                result.spec, perm_hash, perm_idx));
        std::unique_lock lk{programs_mutex};
        program.AddPermut(result.module, std::move(result.spec));
    }
    for (auto& result : done) {
        BbSceneDiag::Event("graphics_ready", std::hash<GraphicsPipelineKey>{}(result.key));
        async_pending.erase(result.key);
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(result.key);
        result.pipeline->storage_name =
            RegisterPipelineData(result.key, pipeline_hash, result.sdata);
        ++num_new_pipelines;
        graphics_pipelines.try_emplace(result.key, std::move(result.pipeline));
    }
    for (auto& result : compute_done) {
        BbSceneDiag::Event("compute_ready", result.key.value);
        async_compute_pending.erase(result.key);
        result.pipeline->storage_name = RegisterPipelineData(result.key, result.sdata);
        ++num_new_pipelines;
        compute_pipelines.try_emplace(result.key, std::move(result.pipeline));
    }
}

bool PipelineCache::AsyncComputeOn() const {
    return async_compute && !BbToggle::Disabled(BbToggle::AsyncCompute);
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    {
        // bbport (Claude, 13/10): run98 lost the device on an NVIDIA "Graphics Exception:
        // SKEDCHECK11_TOTAL_THREADS" (a compute workgroup with an invalid thread count). Skip
        // and log such dispatches instead of handing them to the GPU.
        const auto& cs = liverpool->GetCsRegs();
        const u64 x = cs.num_thread_x.full, y = cs.num_thread_y.full, z = cs.num_thread_z.full;
        if (x == 0 || y == 0 || z == 0 || x > 1024 || y > 1024 || z > 64 || x * y * z > 1024) {
            static std::atomic<u32> rejected{0};
            if (rejected.fetch_add(1) < 32) {
                std::printf("Compute guard: dispatch skipped, workgroup %llux%llux%llu at %#llx\n",
                            (unsigned long long)x, (unsigned long long)y, (unsigned long long)z,
                            (unsigned long long)cs.Address<u64>());
            }
            return nullptr;
        }
    }
    if (async_pipelines && async_done_count.load(std::memory_order_acquire) != 0) {
        DrainAsyncPipelines();
    }
    shader_pending = false;
    BbSceneDiag::Poll();
    if (!RefreshComputeKey() || shader_pending) {
        BbSceneDiag::Count(shader_pending ? BbSceneDiag::dispatch_shader : BbSceneDiag::invalid_key);
        shader_pending = false; // permutation translating on a compile thread: skipped
        return nullptr;
    }
    if (AsyncComputeOn()) {
        if (const auto found = compute_pipelines.find(compute_key);
            found != compute_pipelines.end()) {
            NoteUsed(found->second.get());
            return found->second.get();
        }
        // Not built yet: queue it once and skip this dispatch.
        if (async_compute_pending.insert(compute_key).second) {
            BbSceneDiag::Event("compute_requested", compute_key.value, sel.infos[0]->pgm_hash);
            AsyncComputeJob job{};
            job.key = compute_key;
            job.snapshot = std::make_unique<Shader::Info>(*sel.infos[0]);
            job.live_info = sel.infos[0];
            job.module = sel.modules[0];
            {
                std::scoped_lock lock{async_mutex};
                async_compute_jobs.push_back(std::move(job));
            }
            async_cv.notify_one();
        }
        BbSceneDiag::Count(BbSceneDiag::dispatch_pipeline);
        return nullptr;
    }
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<ComputePipelineKey>{}(compute_key);
        LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x}", pipeline_hash);
        BbSceneDiag::Event("compute_requested", compute_key.value, sel.infos[0]->pgm_hash);
        CompileTimer timer;

        ComputePipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                       *pipeline_cache, compute_key, *sel.infos[0],
                                                       sel.modules[0], sdata, false);
        it.value()->storage_name = RegisterPipelineData(compute_key, sdata);
        BbSceneDiag::Event("compute_ready", compute_key.value);
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            auto& m = sel.modules[0];
            module_related_pipelines[m].emplace_back(compute_key);
        }
    }
    NoteUsed(it->second.get());
    return it->second.get();
}

bool PipelineCache::RefreshGraphicsKey(PipelineSelection& sel) {
    std::memset(&sel.graphics_key, 0, sizeof(GraphicsPipelineKey));
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;

    const bool db_enabled = regs.depth_buffer.DepthValid() || regs.depth_buffer.StencilValid();

    key.z_format = regs.depth_buffer.DepthValid() ? regs.depth_buffer.z_info.format
                                                  : AmdGpu::DepthBuffer::ZFormat::Invalid;
    key.stencil_format = regs.depth_buffer.StencilValid()
                             ? regs.depth_buffer.stencil_info.format
                             : AmdGpu::DepthBuffer::StencilFormat::Invalid;
    key.depth_clamp_enable = !regs.depth_render_override.disable_viewport_clamp;
    key.depth_clip_enable = regs.clipper_control.ZclipEnable();
    key.clip_space = regs.clipper_control.clip_space;
    key.provoking_vtx_last = regs.polygon_control.provoking_vtx_last;
    key.prim_type = regs.primitive_type;
    key.polygon_mode = regs.polygon_control.PolyMode();
    key.patch_control_points =
        regs.stage_enable.hs_en ? regs.ls_hs_config.hs_input_control_points : 0;
    key.logic_op = regs.color_control.rop3;
    key.depth_samples = db_enabled ? regs.depth_buffer.NumSamples() : 1;
    key.num_samples = key.depth_samples;
    key.cb_shader_mask = regs.color_shader_mask;

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

    // Only potentially animated G-buffer draws may use the extra motion attachment. The
    // ordinary stage variant is needed first to inspect the vertex shader's resources.
    bool motion_possible = false;
    {
        u32 bound = 0;
        for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
            bound += (regs.color_buffers[cb] && regs.color_target_mask.GetMask(cb)) ? 1 : 0;
        }
        motion_possible = Shader::MotionVectors::positions_address != 0 && bound >= 5 &&
                          !regs.color_buffers[Shader::MotionVectors::Output] &&
                          regs.depth_buffer.DepthValid() &&
                          regs.depth_buffer.NumSamples() == 1 && !regs.IsClipDisabled() &&
                          regs.stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::Vs;
    }
    sel.motion = false;

    // First pass to fill render target information needed by shader recompiler
    for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
            // No attachment bound or writing to it is disabled.
            continue;
        }

        // Fill color target information
        auto& color_buffer = key.color_buffers[cb];
        color_buffer.data_format = col_buf.GetDataFmt();
        color_buffer.num_format = col_buf.GetNumberFmt();
        color_buffer.num_conversion = col_buf.GetNumberConversion();
        color_buffer.export_format = regs.color_export_format.GetFormat(cb);
        color_buffer.swizzle = col_buf.Swizzle();

        const auto& bc = regs.blend_control[cb];
        color_buffer.blend_self_scale =
            bc.enable && !col_buf.info.blend_bypass &&
            (bc.color_func == AmdGpu::BlendControl::BlendFunc::Min ||
             bc.color_func == AmdGpu::BlendControl::BlendFunc::Max) &&
            bc.color_src_factor == AmdGpu::BlendControl::BlendFactor::SrcColor &&
            bc.color_dst_factor == AmdGpu::BlendControl::BlendFactor::DstColor;
    }

    // Compile and bind shader stages
    if (!RefreshGraphicsStages(sel)) {
        return false;
    }
    if (motion_possible) {
        const auto* vs = sel.infos[static_cast<u32>(Shader::SwStage::Vertex)];
        if (vs) {
            // Shaders with a bone palette (motion_history.h). Small skeletons (weapons,
            // props) also include static world pieces: the rasterizer gives those
            // history only while their constants change.
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                if (buffer.Valid() && buffer.GetStride() == 16 &&
                    Motion::ClassifyBuffer(buffer.GetSize()) != Motion::BufferRole::Other) {
                    sel.motion = true;
                    break;
                }
            }
        }
        // BB_MOTION_SELECT_LOG=1: each G-buffer vertex shader once, with its buffer sizes
        // and the selection, to find animated models that the size rule leaves out.
        static const bool select_log = [] {
            const char* value = std::getenv("BB_MOTION_SELECT_LOG");
            return value && value[0] == '1';
        }();
        if (select_log && vs) {
            std::string sizes;
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                sizes += fmt::format(" {}/{}", buffer.GetSize(), buffer.GetStride());
            }
            static std::mutex log_mutex;
            static std::unordered_set<u64> logged;
            std::scoped_lock lock{log_mutex};
            if (logged.insert(vs->pgm_hash ^ std::hash<std::string>{}(sizes)).second) {
                std::printf("Motion select: vs %016llx %s, buffers (size/stride):%s\n",
                            static_cast<unsigned long long>(vs->pgm_hash),
                            sel.motion ? "ON " : "off", sizes.c_str());
            }
        }
        // Keep the old broad path available for visual A/B tests.
        static const bool all_motion = [] {
            const char* value = std::getenv("BB_OBJECT_MOTION_ALL");
            return value && value[0] == '1';
        }();
        if (all_motion) {
            sel.motion = true;
        }
        if (sel.motion && !RefreshGraphicsStages(sel)) {
            return false;
        }
    }

    // Second pass to mask out render targets not written by shader and fill remaining info
    u8 color_samples = 0;
    bool all_color_samples_same = true;
    for (s32 cb = 0; cb < key.num_color_attachments && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (!col_buf || !target_mask) {
            continue;
        }
        if ((key.mrt_mask & (1u << cb)) == 0) {
            std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
            continue;
        }

        // Fill color blending information
        if (regs.blend_control[cb].enable && !col_buf.info.blend_bypass) {
            key.blend_controls[cb] = regs.blend_control[cb];
        }

        // Apply swizzle to target mask
        key.write_masks[cb] =
            vk::ColorComponentFlags{key.color_buffers[cb].swizzle.ApplyMask(target_mask)};

        // Fill color samples
        const u8 prev_color_samples = std::exchange(color_samples, col_buf.NumSamples());
        all_color_samples_same &= color_samples == prev_color_samples || prev_color_samples == 0;
        key.color_samples[cb] = color_samples;
        key.num_samples = std::max(key.num_samples, color_samples);
    }

    if (sel.motion) {
        constexpr u32 mv = Shader::MotionVectors::Output;
        key.motion_vectors = 1;
        key.mrt_mask |= 1u << mv;
        key.num_color_attachments = mv + 1;
        auto& color_buffer = key.color_buffers[mv];
        color_buffer.data_format = AmdGpu::DataFormat::Format32_32_32_32;
        color_buffer.num_format = AmdGpu::NumberFormat::Float;
        color_buffer.swizzle = AmdGpu::IdentityMapping;
        key.write_masks[mv] = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        key.color_samples[mv] = 1;
    }

    // Force all color samples to match depth samples to avoid unsupported MSAA configuration
    if (color_samples != 0) {
        const bool depth_mismatch = db_enabled && color_samples != key.depth_samples;
        if (!all_color_samples_same && !instance.IsMixedAnySamplesSupported() ||
            all_color_samples_same && depth_mismatch && !instance.IsMixedDepthSamplesSupported()) {
            key.color_samples.fill(key.depth_samples);
            key.num_samples = key.depth_samples;
        }
    }

    return true;
}

bool PipelineCache::RefreshGraphicsStages(PipelineSelection& sel) {
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;
    sel.fetch_shader = std::nullopt;

    Shader::Backend::Bindings binding{};
    const auto bind_stage = [&](HwStage stage_in, SwStage stage_out) -> bool {
        const auto stage_in_idx = static_cast<u32>(stage_in);
        const auto stage_out_idx = static_cast<u32>(stage_out);
        if (!regs.stage_enable.IsStageEnabled(stage_in_idx)) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto* pgm = regs.ProgramForStage(stage_in_idx);
        if (!pgm || !pgm->Address<u32*>()) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto params = AmdGpu::GetParams(*pgm);
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader_;
        std::tie(sel.infos[stage_out_idx], sel.modules[stage_out_idx], fetch_shader_,
                 key.stage_hashes[stage_out_idx]) =
            GetProgram(sel, stage_in, stage_out, params, binding);
        if (!sel.worker && shader_pending) {
            // bbport: still translating: later stages would start from different bindings.
            return false;
        }
        if (fetch_shader_) {
            sel.fetch_shader = fetch_shader_;
        }
        return true;
    };

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);

    bind_stage(HwStage::Fragment, SwStage::Fragment);
    if (!sel.worker && shader_pending) {
        return false;
    }

    const auto* fs_info = sel.infos[static_cast<u32>(SwStage::Fragment)];
    key.mrt_mask = fs_info ? fs_info->mrt_mask : 0u;
    key.num_color_attachments = std::bit_width(key.mrt_mask);

    switch (regs.stage_enable.raw) {
    case AmdGpu::ShaderStageEnable::VgtStages::EsGs:
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Vertex, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHsEsGs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::Vs:
        bind_stage(HwStage::Vertex, SwStage::Vertex);
        break;
    default:
        LOG_WARNING(Render_Vulkan, "unimplemented shader stage {}", (u32)regs.stage_enable.raw);
        return false;
    }

    const auto* vs_info = sel.infos[static_cast<u32>(SwStage::Vertex)];
    if (vs_info && sel.fetch_shader && !instance.IsVertexInputDynamicState()) {
        // Without vertex input dynamic state, the pipeline needs to specialize on format.
        // Stride will still be handled outside the pipeline using dynamic state.
        u32 vertex_binding = 0;
        for (const auto& attrib : sel.fetch_shader->attributes) {
            const auto& buffer = attrib.GetSharp(*vs_info);
            ASSERT_MSG(vertex_binding < MaxVertexBufferCount,
                       "Vertex attribute binding count exceeded limit: {} >= {}", vertex_binding,
                       MaxVertexBufferCount);
            key.vertex_buffer_formats[vertex_binding++] =
                Vulkan::LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        }
    }

    return true;
}

bool PipelineCache::RefreshComputeKey() {
    Shader::Backend::Bindings binding{};
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto cs_params = AmdGpu::GetParams(cs_pgm);
    std::tie(sel.infos[0], sel.modules[0], sel.fetch_shader, compute_key.value) =
        GetProgram(sel, HwStage::Compute, SwStage::Compute, cs_params, binding);
    return true;
}

vk::ShaderModule PipelineCache::CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                              const std::span<const u32>& code, size_t perm_idx,
                                              Shader::Backend::Bindings& binding) {
    const auto diag_ticket = BbSceneDiag::Ticket();
    BbSceneDiag::Event("shader_sync_begin", diag_ticket, info.pgm_hash, u32(info.sw_stage));
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.hw_stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation)" : "");
    DumpShader(code, info.pgm_hash, info.hw_stage, perm_idx, "bin");
    CompileTimer timer;

    const auto ir_program = Shader::TranslateProgram(code, pools, info, runtime_info, profile);
    auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
    DumpShader(spv, info.pgm_hash, info.hw_stage, perm_idx, "spv");

    vk::ShaderModule module;

    auto patch = GetShaderPatch(info.pgm_hash, info.hw_stage, perm_idx, "spv");
    const bool is_patched = patch && EmulatorSettings.IsPatchShaders();
    if (is_patched) {
        LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.hw_stage, info.pgm_hash);
        module = CompileSPV(*patch, instance.GetDevice());
    } else {
        module = CompileSPV(spv, instance.GetDevice());
    }

    const auto name = GetShaderName(info.hw_stage, info.pgm_hash, perm_idx);
    if (info.sw_stage == SwStage::Vertex)
        BbFaceDepth::Register(module, is_patched ? std::span<const u32>(*patch) :
                                               std::span<const u32>(spv), info.pgm_hash);
    Vulkan::SetObjectName(instance.GetDevice(), module, name);
    if (EmulatorSettings.IsShaderCollect()) {
        DebugState.CollectShader(name, info.sw_stage, module, spv, code,
                                 patch ? *patch : std::span<const u32>{}, is_patched);
    }
    // bbport: stored by GetProgram once the specialization (content key) is known.
    last_spv = std::move(spv);
    BbSceneDiag::Event("shader_sync_end", diag_ticket, info.pgm_hash, u32(info.sw_stage));
    return module;
}

PipelineCache::Result PipelineCache::GetProgram(PipelineSelection& sel, HwStage hw_stage,
                                                SwStage sw_stage,
                                                const Shader::ShaderParams& params,
                                                Shader::Backend::Bindings& binding) {
    auto runtime_info = BuildRuntimeInfo(sel, hw_stage, sw_stage);
    if (sel.worker) {
        // bbport: draw-preparation worker: look up only, with the worker's own Info copy.
        auto& worker = *sel.worker;
        std::shared_lock lk{programs_mutex};
        const auto found_program = program_cache.find(params.hash);
        if (found_program == program_cache.end() || !found_program->second->info_template) {
            worker.failed = true;
            return {};
        }
        const Program* program = found_program->second.get();
        auto [it_info, new_info] = worker.infos.try_emplace(program, *program->info_template);
        auto& info = it_info->second;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        // The walk of resource tables and the fetch shader parse read guest memory through
        // pointers in the registers; ahead of the GPU thread that memory may already be
        // reused. A fault returns here (runtime_fault_recover) and the draw is left to the GPU
        // thread. A jump out of the specialization leaks its partial allocations (rare).
        sigjmp_buf recover;
        if (sigsetjmp(recover, 0)) {
            worker.failed = true;
            return {};
        }
        runtime_fault_recover = &recover;
        info.RefreshFlatBuf();
        if (BbParticleSrt::Enabled()) BbParticleSrt::Normalize(info);
        auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);
        runtime_fault_recover = nullptr;
        const auto it = std::ranges::find(program->modules, spec, &Program::Module::spec);
        if (it == program->modules.end()) {
            worker.failed = true;
            return {};
        }
        info.AddBindings(binding);
        const size_t perm_idx = std::distance(program->modules.begin(), it);
        worker.stages.push_back(
            {program, params.hash, hw_stage, info.pgm_base, &info.flattened_ud_buf});
        return std::make_tuple(&info, it->module, it->spec.fetch_shader_data,
                               HashCombine(params.hash, perm_idx));
    }

    auto it_pgm = program_cache.find(params.hash); // this thread is the only writer
    if (it_pgm == program_cache.end()) {
        auto new_program = std::make_unique<Program>(hw_stage, sw_stage, params);
        auto start = binding;
        const auto module =
            CompileModule(new_program->info, runtime_info, params.code, 0, binding);
        auto spec = Shader::StageSpecialization(new_program->info, runtime_info, profile, start);
        const auto perm_hash = HashCombine(params.hash, 0);

        RegisterShaderBinary(std::move(last_spv), RegisterShaderMeta(new_program->info,
                                                                     spec.fetch_shader_data, spec,
                                                                     perm_hash, 0));
        new_program->AddPermut(module, std::move(spec));
        new_program->info_template = std::make_unique<Shader::Info>(new_program->info);
        Program* program = new_program.get();
        {
            std::unique_lock lk{programs_mutex};
            program_cache.emplace(params.hash, std::move(new_program));
        }
        return std::make_tuple(&program->info, module, program->modules[0].spec.fetch_shader_data,
                               perm_hash);
    }

    auto& program = it_pgm.value();
    if (!program->info_template) {
        // Programs loaded by the pipeline cache warm-up get their template on first use.
        std::unique_lock lk{programs_mutex};
        program->info_template = std::make_unique<Shader::Info>(program->info);
    }
    auto& info = program->info;
    info.pgm_base = params.Base(); // Needs to be actualized for inline cbuffer address fixup
    info.user_data = params.user_data;
    info.RefreshFlatBuf();
    if (BbParticleSrt::Enabled()) {
        const auto normalized=BbParticleSrt::Normalize(info);
        BbSceneDiag::ParticleSrt(info.pgm_hash,normalized.applied,normalized.active,
                                normalized.inactive_images);
    }
    auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);

    size_t perm_idx = program->modules.size();
    u64 perm_hash = HashCombine(params.hash, perm_idx);

    vk::ShaderModule module{};

    // bbport: consecutive draws of a program almost always use the same permutation.
    auto it = program->last_used < program->modules.size() &&
                      program->modules[program->last_used].spec == spec
                  ? program->modules.begin() + program->last_used
                  : std::ranges::find(program->modules, spec, &Program::Module::spec);
    if (it != program->modules.end()) {
        program->last_used = std::distance(program->modules.begin(), it);
    }
    // Vertex, pixel and compute programs: tessellation stages read guest constant buffers
    // while translating (ReadTessConstantBuffer).
    if (it == program->modules.end() && async_shaders &&
        ((sw_stage == SwStage::Compute && AsyncComputeOn()) ||
         ((sw_stage == SwStage::Vertex || sw_stage == SwStage::Fragment) &&
          !sel.regs->stage_enable.hs_en && !synchronous_graphics))) {
        // bbport: translated on a compile thread; this draw is skipped (see AsyncShaderJob).
        shader_pending = true;
        if (std::ranges::find(program->pending_specs, spec) == program->pending_specs.end()) {
            AsyncShaderJob job{};
            job.diag_ticket = BbSceneDiag::Ticket();
            BbSceneDiag::Event("shader_requested", job.diag_ticket, params.hash, u32(sw_stage));
            job.program = program.get();
            job.spec = spec;
            job.hw_stage = hw_stage;
            job.sw_stage = sw_stage;
            std::ranges::copy(params.user_data, job.user_data.begin());
            job.flatbuf = info.flattened_ud_buf; // refreshed for this draw above
            job.code.assign(params.code.begin(), params.code.end());
            job.pgm_base = params.Base();
            job.hash = params.hash;
            job.runtime_info = runtime_info;
            job.binding = binding;
            program->pending_specs.push_back(std::move(spec));
            {
                std::scoped_lock lock{async_mutex};
                async_shader_jobs.push_back(std::move(job));
            }
            async_cv.notify_one();
        }
        return {};
    }
    if (it == program->modules.end()) {
        static const bool perm_trace = std::getenv("BB_PERM_TRACE") != nullptr;
        if (perm_trace && !program->modules.empty()) {
            LOG_WARNING(Render_Vulkan, "Permutation {} of {:#x}: {}", perm_idx, params.hash,
                        spec.DescribeDifference(program->modules[0].spec));
        }
        auto new_info = Shader::Info(hw_stage, sw_stage, params);
        module = CompileModule(new_info, runtime_info, params.code, perm_idx, binding);

        RegisterShaderBinary(std::move(last_spv), RegisterShaderMeta(info, spec.fetch_shader_data,
                                                                     spec, perm_hash, perm_idx));
        std::unique_lock lk{programs_mutex};
        program->AddPermut(module, std::move(spec));
    } else {
        info.AddBindings(binding);
        module = it->module;
        perm_idx = std::distance(program->modules.begin(), it);
        perm_hash = HashCombine(params.hash, perm_idx);
    }
    return std::make_tuple(&program->info, module,
                           program->modules[perm_idx].spec.fetch_shader_data, perm_hash);
}

std::optional<vk::ShaderModule> PipelineCache::ReplaceShader(vk::ShaderModule module,
                                                             std::span<const u32> spv_code) {
    std::optional<vk::ShaderModule> new_module{};
    for (const auto& [_, program] : program_cache) {
        for (auto& m : program->modules) {
            if (m.module == module) {
                const auto& d = instance.GetDevice();
                d.destroyShaderModule(m.module);
                m.module = CompileSPV(spv_code, d);
                if (program->info.sw_stage == SwStage::Vertex)
                    BbFaceDepth::Register(m.module, spv_code, program->info.pgm_hash);
                new_module = m.module;
            }
        }
    }
    if (module_related_pipelines.contains(module)) {
        auto& pipeline_keys = module_related_pipelines[module];
        for (auto& key : pipeline_keys) {
            if (std::holds_alternative<GraphicsPipelineKey>(key)) {
                auto& graphics_key = std::get<GraphicsPipelineKey>(key);
                graphics_pipelines.erase(graphics_key);
            } else if (std::holds_alternative<ComputePipelineKey>(key)) {
                auto& compute_key = std::get<ComputePipelineKey>(key);
                compute_pipelines.erase(compute_key);
            }
        }
    }
    return new_module;
}

std::string PipelineCache::GetShaderName(Shader::HwStage stage, u64 hash,
                                         std::optional<size_t> perm) {
    if (perm) {
        return fmt::format("{}_{:#018x}_{}", stage, hash, *perm);
    }
    return fmt::format("{}_{:#018x}", stage, hash);
}

void PipelineCache::DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage,
                               size_t perm_idx, std::string_view ext) {
    // Match the IR dump filter so an investigation can retain original GCN and
    // generated SPIR-V without dumping every shader in the game.
    static const std::vector<u64> only = [] {
        std::vector<u64> list;
        if (const char* env = std::getenv("BB_DUMP_ONLY")) {
            for (const char* p = env; *p;) {
                char* end = nullptr;
                const u64 value = std::strtoull(p, &end, 16);
                if (end == p) {
                    ++p;
                    continue;
                }
                list.push_back(value);
                p = end;
            }
        }
        return list;
    }();
    if (!only.empty() ? std::ranges::find(only, hash) == only.end()
                      : !EmulatorSettings.IsDumpShaders()) {
        return;
    }

    using namespace Common::FS;
    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create};
    file.WriteSpan(code);
}

std::optional<std::vector<u32>> PipelineCache::GetShaderPatch(u64 hash, Shader::HwStage stage,
                                                              size_t perm_idx,
                                                              std::string_view ext) {

    using namespace Common::FS;
    const auto patch_dir = GetUserPath(PathType::ShaderDir) / "patch";
    if (!std::filesystem::exists(patch_dir)) {
        std::filesystem::create_directories(patch_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto filepath = patch_dir / filename;
    if (!std::filesystem::exists(filepath)) {
        return {};
    }
    const auto file = IOFile{patch_dir / filename, FileAccessMode::Read};
    std::vector<u32> code(file.GetSize() / sizeof(u32));
    file.Read(code);
    return code;
}
} // namespace Vulkan
