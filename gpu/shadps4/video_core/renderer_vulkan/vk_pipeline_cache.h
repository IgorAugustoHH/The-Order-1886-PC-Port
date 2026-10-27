// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <unordered_map>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>
#include <thread>
#include <variant>
#include <boost/container/static_vector.hpp>
#include <tsl/robin_map.h>
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"

template <>
struct std::hash<vk::ShaderModule> {
    std::size_t operator()(const vk::ShaderModule& module) const noexcept {
        return std::hash<size_t>{}(reinterpret_cast<size_t>((VkShaderModule)module));
    }
};

namespace AmdGpu {
class Liverpool;
}

namespace Serialization {
struct Archive;
}

namespace Shader {
struct Info;
}

namespace Vulkan {

class Instance;
class Scheduler;
class ShaderCache;

/// TOport: set by the presenter while the rasterizer is built, so the startup preload draws a
/// loading screen (built pipelines, expected total or 0 when unknown, seconds).
inline std::function<void(u32, u32, double)> g_preload_progress;

struct Program {
    struct Module {
        vk::ShaderModule module;
        Shader::StageSpecialization spec;
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;

    Shader::Info info;
    ModuleList modules{};
    size_t last_used = 0; ///< bbport: permutation of the previous lookup, compared first
    /// bbport: permutations being translated on a compile thread (GPU thread only).
    std::vector<Shader::StageSpecialization> pending_specs;
    /// bbport: `info` as translated, for draw-preparation workers (they must not read `info`,
    /// whose user data the GPU thread rewrites every draw). Guarded by programs_mutex.
    std::unique_ptr<Shader::Info> info_template;

    Program() = default;
    Program(Shader::HwStage stage, Shader::SwStage l_stage, Shader::ShaderParams params)
        : info{stage, l_stage, params} {}

    void AddPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec) {
        modules.emplace_back(module, std::move(spec));
    }

    void InsertPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec,
                      size_t perm_idx) {
        modules.resize(std::max(modules.size(), perm_idx + 1)); // <-- beware of realloc
        modules[perm_idx] = {module, std::move(spec)};
    }
};

struct DrawIndirectParams {
    u16 vertex_sgpr_offset;
    u32 instance_sgpr_offset;
};

} // namespace Vulkan

namespace AmdGpu {
union Regs;
}

namespace Vulkan {

/// bbport: state of one graphics/compute pipeline selection. The GPU thread owns one
/// (PipelineCache::sel); draw-preparation workers use their own with their register copies.
struct PipelineSelection {
    const AmdGpu::Regs* regs{};
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
    GraphicsPipelineKey graphics_key{};
    bool motion = false;
    DrawIndirectParams draw_indirect_params{};
    struct PrepWorker* worker{}; ///< set: read-only selection for a draw-preparation worker
};

/// bbport: a draw-preparation worker's own program state (see vk_draw_prep.h).
struct PrepWorker {
    struct Stage {
        const Program* program;
        u64 hash;
        Shader::HwStage hw_stage;
        VAddr pgm_base;
        const std::vector<u32>* flat;
    };
    // Node-based: stages keep pointers to these Infos while later stages are inserted.
    std::unordered_map<const Program*, Shader::Info> infos;
    boost::container::static_vector<Stage, MaxShaderStages> stages;
    bool failed = false;
};

struct PreparedDraw;

class PipelineCache {
public:
    explicit PipelineCache(const Instance& instance, Scheduler& scheduler,
                           AmdGpu::Liverpool* liverpool, u32 sparse_page_shift);
    ~PipelineCache();

    void WarmUp();
    void Sync();

    bool LoadComputePipeline(Serialization::Archive& ar);
    bool LoadGraphicsPipeline(Serialization::Archive& ar);
    bool LoadPipelineStage(Serialization::Archive& ar, size_t stage, u64 content_key,
                           u64& local_perm_hash);

    const GraphicsPipeline* GetGraphicsPipeline(const DrawIndirectParams params = {},
                                                const PreparedDraw* prepared = nullptr);

    /// bbport: worker side of draw preparation: selects the pipeline key for `sel.regs` without
    /// creating anything. False when a program or permutation does not exist yet.
    bool PrepareGraphicsPipeline(PipelineSelection& sel);

    /// bbport: GPU-thread side: the pipeline for a prepared draw after checking that registers
    /// and flattened user data match; null to take the regular path.
    const GraphicsPipeline* TryPreparedPipeline(const PreparedDraw& prepared);

    /// bbport: the prepared draw the last GetGraphicsPipeline used, or null (regular path).
    [[nodiscard]] const PreparedDraw* UsedPrepared() const noexcept {
        return used_prepared;
    }

    const ComputePipeline* GetComputePipeline();

    /// bbport: records the pipelines this session uses (by pipeline cache name). The warm-up
    /// then builds only those of the last sessions: building all 10k cached pipelines took
    /// ~3.5 GB of system RAM in the driver and the RAM guard stopped the game (run83).
    void NoteUsed(const Pipeline* pipeline) {
        if (pipeline && pipeline->storage_name &&
            !pipeline->used.load(std::memory_order_relaxed)) {
            NoteUsedSlow(pipeline);
        }
    }

    using Result = std::tuple<const Shader::Info*, vk::ShaderModule,
                              std::optional<Shader::Gcn::FetchShaderData>, u64>;
    Result GetProgram(PipelineSelection& sel, Shader::HwStage stage, Shader::SwStage l_stage,
                      const Shader::ShaderParams& params, Shader::Backend::Bindings& binding);

    std::optional<vk::ShaderModule> ReplaceShader(vk::ShaderModule module,
                                                  std::span<const u32> spv_code);

    static std::string GetShaderName(Shader::HwStage stage, u64 hash,
                                     std::optional<size_t> perm = {});

    auto& GetProfile() const {
        return profile;
    }

private:
    bool RefreshGraphicsKey(PipelineSelection& sel);
    bool RefreshGraphicsStages(PipelineSelection& sel);
    bool RefreshComputeKey();

    void DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage, size_t perm_idx,
                    std::string_view ext);
    std::optional<std::vector<u32>> GetShaderPatch(u64 hash, Shader::HwStage stage, size_t perm_idx,
                                                   std::string_view ext);
    vk::ShaderModule CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                   const std::span<const u32>& code, size_t perm_idx,
                                   Shader::Backend::Bindings& binding);
    /// bbport: SPIR-V of the last CompileModule, stored once its specialization (content key)
    /// is known. GPU thread only.
    std::vector<u32> last_spv;

    /// bbport: recently used pipelines (see NoteUsed), in work/user/shader/pipelines_recentes.txt.
    void NoteUsedSlow(const Pipeline* pipeline);
    void LoadRecentPipelines();
    void SaveRecentPipelines();
    bool PreloadPipeline(u64 name, bool compute);
    std::mutex recent_mutex;
    std::vector<std::string> recent_used;                     ///< this session, guarded
    std::vector<std::pair<std::string, u32>> recent_earlier;  ///< kept from earlier sessions
    std::unordered_set<std::string> recent_preload;
    bool recent_filter = false; ///< false: warm-up builds every cached pipeline
    u32 recent_session = 1;
    size_t recent_saved = 0;
    u32 skipped_preloads = 0;
    const Shader::RuntimeInfo& BuildRuntimeInfo(PipelineSelection& sel, Shader::HwStage stage,
                                                Shader::SwStage l_stage);

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

private:
    const Instance& instance;
    Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    DescriptorHeap desc_heap;
    vk::UniquePipelineCache pipeline_cache;
    /// bbport: saves the driver's pipeline cache to disk now and then (declared after it, so it
    /// stops first).
    std::jthread driver_cache_saver;
    vk::UniquePipelineLayout pipeline_layout;
    Shader::Profile profile{};
    Shader::Pools pools;
    tsl::robin_map<size_t, std::unique_ptr<Program>> program_cache;
    /// bbport: exclusive for program/permutation insertions, shared for worker lookups.
    std::shared_mutex programs_mutex;
    u64 prepared_hits = 0, prepared_misses = 0;
    const PreparedDraw* used_prepared = nullptr;
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_pipelines;
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>> graphics_pipelines;
    PipelineSelection sel{}; ///< GPU thread selection state
    ComputePipelineKey compute_key{};
    u32 num_new_pipelines{}; // new pipelines added to the cache since the game start

    // Only if Config::collectShadersForDebug()
    tsl::robin_map<vk::ShaderModule,
                   std::vector<std::variant<GraphicsPipelineKey, ComputePipelineKey>>>
        module_related_pipelines;

    /// bbport: graphics pipelines built on compile threads (BB_ASYNC_PIPELINES, on by default).
    /// A draw whose pipeline is not built yet is skipped instead of stalling the GPU thread for
    /// the driver's compilation (40-60 ms each; The Order: 1886 cutscenes spent most of their
    /// time there). The job carries snapshots of the stages' Info: the GPU thread rewrites the
    /// live ones every draw.
    struct AsyncGraphicsJob {
        std::chrono::steady_clock::time_point queued_at = std::chrono::steady_clock::now();
        GraphicsPipelineKey key{};
        std::array<std::unique_ptr<Shader::Info>, MaxShaderStages> snapshots{};
        std::array<const Shader::Info*, MaxShaderStages> live_infos{};
        std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
        std::array<vk::ShaderModule, MaxShaderStages> modules{};
    };
    struct AsyncGraphicsResult {
        GraphicsPipelineKey key{};
        std::unique_ptr<GraphicsPipeline> pipeline;
        GraphicsPipeline::SerializationSupport sdata{};
    };
    /// A new permutation of a known graphics program, translated on a compile thread. The
    /// user data and code are owned copies; the original guest address is retained for inline
    /// constants. A delayed job must not translate recycled guest program memory.
    struct AsyncShaderJob {
        std::chrono::steady_clock::time_point queued_at = std::chrono::steady_clock::now();
        u64 diag_ticket{};
        Program* program{};
        Shader::StageSpecialization spec{};
        Shader::HwStage hw_stage{};
        Shader::SwStage sw_stage{};
        std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
        std::vector<u32> flatbuf; ///< the draw's flattened user data (see Info::flatbuf_snapshot)
        std::vector<u32> code{}; ///< owned: guest memory can be recycled before compilation
        VAddr pgm_base{}; ///< preserve guest address for inline constant-buffer fixups
        u64 hash{};
        Shader::RuntimeInfo runtime_info{};
        Shader::Backend::Bindings binding{};
    };
    struct AsyncShaderResult {
        u64 diag_ticket{};
        Program* program{};
        Shader::StageSpecialization spec{};
        vk::ShaderModule module{};
        std::vector<u32> spv;
    };
    /// bbport: compute pipelines built on a compile thread (BB_ASYNC_COMPUTE=0: on the GPU
    /// thread). In The Order: 1886 a lighting compute shader (0x804aeead, 33 images) gets a new
    /// permutation whenever its image types change: ~150 ms each, nearly all the stutters left
    /// in run77. The dispatch is skipped until it is built.
    struct AsyncComputeJob {
        std::chrono::steady_clock::time_point queued_at = std::chrono::steady_clock::now();
        ComputePipelineKey key{};
        std::unique_ptr<Shader::Info> snapshot;
        const Shader::Info* live_info{};
        vk::ShaderModule module{};
    };
    struct AsyncComputeResult {
        ComputePipelineKey key{};
        std::unique_ptr<ComputePipeline> pipeline;
        ComputePipeline::SerializationSupport sdata{};
    };
    std::deque<AsyncComputeJob> async_compute_jobs;     ///< guarded by async_mutex
    std::vector<AsyncComputeResult> async_compute_done; ///< guarded by async_mutex
    std::unordered_set<ComputePipelineKey> async_compute_pending; ///< GPU thread only
    bool async_compute = false;
    bool AsyncComputeOn() const;
    std::deque<AsyncShaderJob> async_shader_jobs;       ///< guarded by async_mutex
    std::vector<AsyncShaderResult> async_shader_done;   ///< guarded by async_mutex
    bool async_shaders = false;
    bool shader_pending = false; ///< GPU thread: a stage's permutation is still translating
    void StartAsyncCompilers();
    void DrainAsyncPipelines();
    bool async_pipelines = false;
    bool synchronous_graphics = false; ///< diagnostic: keep compute workers enabled
    std::mutex async_mutex;
    std::condition_variable async_cv;
    std::deque<AsyncGraphicsJob> async_jobs;        ///< guarded by async_mutex
    std::vector<AsyncGraphicsResult> async_done;    ///< guarded by async_mutex
    std::unordered_set<GraphicsPipelineKey> async_pending; ///< GPU thread only
    std::atomic<u32> async_done_count{0};
    u32 compile_active = 0; // guarded by async_mutex
    u64 compile_completed = 0;
    double compile_worker_ms = 0;
    size_t compile_queue_peak = 0;
    bool fair_compile_queue = false, aggressive_compile = false;
    unsigned compile_queue_cursor = 0;
    double compile_queue_ms = 0, compile_queue_max_ms = 0;
    u64 compile_shader_started = 0, compile_graphics_started = 0, compile_compute_started = 0;
    std::chrono::steady_clock::time_point compile_report_time{};
    /// Last: stopped and joined before anything they use is destroyed.
    std::vector<std::jthread> async_compilers;
};

} // namespace Vulkan
