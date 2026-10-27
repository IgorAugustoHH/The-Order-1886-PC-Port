// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include "bb_face_depth_fix.h"
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <xxhash.h>
#include "common/hash.h"
#include "common/path_util.h"
#include "common/serdes.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
static constexpr u32 ShaderBinaryVersion = 10u; // bbport: + upstream #5348 indirect ids, #5352 sharp pattern
static constexpr u32 ShaderMetaVersion = 13u; // SRT recovery ABI; older executables reject new readers
static constexpr u32 PipelineKeyVersion = 6u; // bbport: stage hashes are content keys
} // namespace Serialization

namespace Vulkan {

namespace {
std::mutex g_content_mutex;
std::unordered_map<u64, u64> g_perm_to_content; ///< in-session permutation hash -> content key

u64 ContentOf(u64 perm_hash) {
    std::scoped_lock lock{g_content_mutex};
    const auto it = g_perm_to_content.find(perm_hash);
    return it == g_perm_to_content.end() ? 0 : it->second;
}

/// Program hash, versions and the serialized specialization. Padding in the serialized
/// structures can only make two sessions miss each other's entries, never mix them.
u64 PermutationContentKey(u64 pgm_hash, const Shader::StageSpecialization& spec, u32 reader_revision) {
    Serialization::Archive ar;
    spec.Serialize(ar);
    const auto bytes = ar.TakeOff();
    const u64 seed = pgm_hash ^ (u64{Serialization::ShaderMetaVersion} << 48) ^
                     (u64{Serialization::ShaderBinaryVersion} << 32) ^
                     (u64{reader_revision} << 16);
    return XXH3_64bits_withSeed(bytes.data(), bytes.size(), seed) | 1; // never 0
}
} // namespace


u64 RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return 0;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    // bbport: stored with the content key of its stage (see RegisterShaderMeta).
    ComputePipelineKey stored = key;
    stored.value = ContentOf(key.value);
    if (!stored.value) {
        return 0;
    }

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    stored.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", stored.value), ar.TakeOff());
    return stored.value;
}

u64 RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return 0;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    // bbport: stored with the content keys of its stages (see RegisterShaderMeta), under a
    // name derived from them, not from the in-session hash.
    GraphicsPipelineKey stored = key;
    for (auto& stage_hash : stored.stage_hashes) {
        if (stage_hash && !(stage_hash = ContentOf(stage_hash))) {
            return 0;
        }
    }
    const u64 name = XXH3_64bits(&stored, sizeof(stored));

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    stored.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", name), ar.TakeOff());
    return name;
}



void NotePermutationContent(u64 perm_hash, u64 content_key) {
    if (!content_key) {
        return;
    }
    std::scoped_lock lock{g_content_mutex};
    g_perm_to_content[perm_hash] = content_key;
}

u64 RegisterShaderMeta(const Shader::Info& info,
                       const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                       const Shader::StageSpecialization& spec, size_t perm_hash,
                       size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return 0;
    }
    const u64 content_key = PermutationContentKey(info.pgm_hash, spec, info.srt_info.reader_revision);
    NotePermutationContent(perm_hash, content_key);

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    meta.Write(Serialization::ShaderBinaryVersion);

    meta.Write(content_key);
    meta.Write(perm_idx); // informative: the loader assigns its own index

    spec.Serialize(ar);
    info.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", content_key), ar.TakeOff());
    return content_key;
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 content_key) {
    if (!Storage::DataBase::Instance().IsOpened() || !content_key || spv.empty()) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_c", content_key), std::move(spv));
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                    Shader::StageSpecialization& spec, size_t& perm_idx) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion && meta_version != 12u) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version != Serialization::ShaderBinaryVersion) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    spec.Deserialize(ar);
    if (!info.Deserialize(ar)) return false;

    // Motion vertex shaders embed session-local buffer device addresses. They must be
    // recompiled for the current allocation, never loaded from a previous process.
    if (info.hw_stage == Shader::HwStage::Vertex && spec.runtime_info.hw.vs.motion_vectors) {
        return false;
    }

    fetch_shader_data = spec.fetch_shader_data;
    return true;
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar) {
    compute_key.Deserialize(ar);
    const u64 name = compute_key.value; // content key, as stored
    if (!PreloadPipeline(name, true)) {
        return false;
    }

    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", compute_key.value), meta_blob);
    if (meta_blob.empty()) {
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};

    u64 local_perm_hash{};
    if (!LoadPipelineStage(meta_ar, 0, compute_key.value, local_perm_hash)) {
        return false;
    }
    // bbport: stored with the content key; this session knows it by its own permutation hash.
    compute_key.value = local_perm_hash;

    // Eligibility was checked before loading metadata/modules to avoid retaining
    // shaders for pipelines that are deferred to first use.
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (is_new) {
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                       *pipeline_cache, compute_key,
                                                       *sel.infos[0], sel.modules[0], sdata, true);
        it.value()->storage_name = name;
    }

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);

    return is_new;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer sdata{ar};

    sdata.Write(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Write(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Write(&divisors, sizeof(divisors));
    sdata.Write(multisampling);
    sdata.Write(tcs);
    sdata.Write(tes);
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    return true;
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar) {
    sel.graphics_key.Deserialize(ar);
    // Name as RegisterPipelineData stored it (the key with the stages' content keys).
    const u64 name = XXH3_64bits(&sel.graphics_key, sizeof(sel.graphics_key));
    if (!PreloadPipeline(name, false)) {
        return false;
    }

    GraphicsPipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    const auto reset = [&] {
        sel.infos.fill(nullptr);
        sel.modules.fill(nullptr);
        sel.fetch_shader.reset();
    };
    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        auto& hash = sel.graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            reset();
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        u64 local_perm_hash{};
        if (!LoadPipelineStage(meta_ar, stage_idx, hash, local_perm_hash)) {
            reset();
            return false;
        }
        // bbport: stored with content keys; this session knows the stages by its own hashes.
        hash = local_perm_hash;
    }

    const auto [it, is_new] = graphics_pipelines.try_emplace(sel.graphics_key);
    if (is_new) {
        it.value() = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, sel.graphics_key, *pipeline_cache, sel.infos,
            sel.runtime_infos, sel.fetch_shader, sel.modules, sdata, true);
        it.value()->storage_name = name;
    }
    reset();
    return is_new;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage, u64 content_key,
                                      u64& local_perm_hash) {
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    size_t stored_idx{};
    if (!LoadShaderMeta(ar, program->info, sel.fetch_shader, spec, stored_idx)) {
        return false;
    }

    // bbport: BB_RETRANSLATE=hash,hash (hex): those programs are not loaded from the cache, so
    // the session translates (and with BB_DUMP_ONLY dumps) them again.
    static const std::vector<u64> retranslate = [] {
        std::vector<u64> list;
        if (const char* env = std::getenv("BB_RETRANSLATE")) {
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
    if (std::ranges::find(retranslate, program->info.pgm_hash) != retranslate.end()) {
        return false;
    }

    std::vector<u32> spv{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_c", content_key), spv);
    if (spv.empty()) {
        return false;
    }
    // bbport: binaries from the broken float atomic min/max fallback (an unsigned and a signed
    // atomic of opposite kinds on the same word, see AtomicByFloatSign) are translated again.
    {
        bool umax = false, smin = false, umin = false, smax = false;
        for (size_t i = 5; i < spv.size();) {
            const u32 count = spv[i] >> 16, opcode = spv[i] & 0xffff;
            umax |= opcode == 239; // OpAtomicUMax
            smin |= opcode == 236; // OpAtomicSMin
            umin |= opcode == 237; // OpAtomicUMin
            smax |= opcode == 238; // OpAtomicSMax
            i += count ? count : 1;
        }
        if ((umax && smin) || (umin && smax)) {
            return false;
        }
    }

    // bbport: the stored index belongs to the session that wrote it. A permutation already
    // loaded with the same identity is reused; otherwise it is appended, and this session's
    // permutation hash follows from where it lands.
    const u64 pgm_hash = program->info.pgm_hash;
    auto [it_pgm, new_program] = program_cache.try_emplace(pgm_hash);
    if (new_program) {
        it_pgm.value() = std::move(program);
    }
    Program& target = *it_pgm.value();
    size_t perm_idx = target.modules.size();
    for (size_t i = 0; i < target.modules.size(); ++i) {
        if (target.modules[i].module && target.modules[i].spec.SameCacheKey(spec)) {
            perm_idx = i;
            break;
        }
    }
    vk::ShaderModule module{};
    spec.info = &target.info;
    sel.runtime_infos[stage] = spec.runtime_info;
    if (perm_idx < target.modules.size()) {
        module = target.modules[perm_idx].module;
    } else {
        module = CompileSPV(spv, instance.GetDevice());
        if (target.info.sw_stage == Shader::SwStage::Vertex)
            BbFaceDepth::Register(module, spv, pgm_hash);
        target.AddPermut(module, std::move(spec));
    }
    local_perm_hash = HashCombine(pgm_hash, perm_idx);
    NotePermutationContent(local_perm_hash, content_key);

    sel.infos[stage] = &target.info;
    sel.modules[stage] = module;

    return true;
}

void PipelineCache::WarmUp() {
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        return;
    }

    Storage::DataBase::Instance().Open();

    // Check if cache is compatible
    std::vector<u8> profile_data{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    if (profile_data.empty()) {
        Storage::DataBase::Instance().FinishPreload();

        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
        return;
    }
    if (profile_data.size() != sizeof(Shader::Profile)) {
        LOG_WARNING(Render, "Pipeline cache profile has unexpected size ({} != {})",
                    profile_data.size(), sizeof(Shader::Profile));
    }
    Shader::Profile cached_profile{};
    if (profile_data.size() == sizeof(Shader::Profile)) {
        std::memcpy(&cached_profile, profile_data.data(), sizeof(cached_profile));
    }
    if (profile_data.size() != sizeof(Shader::Profile) || cached_profile != profile) {
        // bbport: upstream closed the cache for the session here, so it was never rewritten
        // and every later session compiled every shader again (stutters on each new area).
        // Start a fresh cache for this build and GPU instead.
        LOG_WARNING(Render, "Pipeline cache isn't compatible with current system: rebuilding it");
        Storage::DataBase::Instance().Clear();
        Storage::DataBase::Instance().FinishPreload();
        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
        return;
    }

    LoadRecentPipelines();
    // Diagnostic: keep the persistent cache open, but defer startup loading.
    // Do not remove or clear any shader/driver cache files.
    if (const char* preload = std::getenv("BB_PRELOAD_PIPELINES");
        preload && preload[0] == '0') {
        Storage::DataBase::Instance().FinishPreload();
        std::fprintf(stderr, "Pipeline cache: startup preload bypassed (BB_PRELOAD_PIPELINES=0); persistent cache preserved\n");
        std::fflush(stderr);
        return;
    }
    // TOport: no list of recent sessions yet (first session with this cache): nothing would
    // be built, and reading every stored pipeline only delayed the start (24 s for 7186).
    if (recent_filter && recent_preload.empty()) {
        Storage::DataBase::Instance().FinishPreload();
        std::printf("Pipeline cache: 0 pipelines built at start (no recent sessions yet); all on "
                    "first use\n");
        return;
    }
    u32 num_pipelines{};
    u32 num_total_pipelines{};
    const char* budget_env = std::getenv("BB_PRELOAD_BUDGET_MS");
    const long budget_ms = budget_env ? std::max(0L, std::strtol(budget_env, nullptr, 10)) : 0;
    const auto preload_begin = std::chrono::steady_clock::now();
    u32 budget_deferred{};

    Storage::DataBase::Instance().ForEachBlob(
        Storage::BlobType::PipelineKey, [&](std::vector<u8>&& data) {
            ++num_total_pipelines;
            // Admission budget only: an individual Vulkan call is not interrupted.
            if (budget_ms && std::chrono::steady_clock::now() - preload_begin >=
                                 std::chrono::milliseconds(budget_ms)) {
                ++skipped_preloads;
                ++budget_deferred;
                return;
            }

            Serialization::Archive ar{std::move(data)};
            Serialization::Reader pldata{ar};

            u32 version{};
            pldata.Read(version);
            if (version != Serialization::PipelineKeyVersion) {
                return;
            }

            u32 is_compute{};
            pldata.Read(is_compute);

            bool result{};
            if (is_compute) {
                result = LoadComputePipeline(ar);
            } else {
                result = LoadGraphicsPipeline(ar);
            }

            if (result) {
                ++num_pipelines;
            }
            // TOport: progress every 2 s, so a long preload is visible in the log (and in the
            // test console that follows it) instead of looking like a hang.
            static auto last_report = preload_begin;
            const auto now = std::chrono::steady_clock::now();
            if (now - last_report >= std::chrono::seconds(2)) {
                last_report = now;
                std::printf("Startup preload progress: %u built, %u scanned, %.0f s\n",
                            num_pipelines, num_total_pipelines,
                            std::chrono::duration<double>(now - preload_begin).count());
                std::fflush(stdout);
            }
        });

    std::fprintf(stderr, "Startup preload: built=%u scanned=%u budget_deferred=%u budget_ms=%ld elapsed_ms=%.1f\n",
                 num_pipelines, num_total_pipelines, budget_deferred, budget_ms,
                 std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - preload_begin).count());
    std::fflush(stderr);
    LOG_INFO(Render, "Preloaded {} pipelines", num_pipelines);
    std::printf("Pipeline cache: %u of %u pipelines built at start (used in the last sessions), "
                "%u on first use\n",
                num_pipelines, num_total_pipelines, skipped_preloads);
    if (num_total_pipelines > num_pipelines + skipped_preloads) {
        LOG_WARNING(Render, "{} stale pipelines were found. Consider re-generating the cache",
                    num_total_pipelines - num_pipelines - skipped_preloads);
    }

    Storage::DataBase::Instance().FinishPreload();
}

namespace {
std::filesystem::path RecentPipelinesPath() {
    return Common::FS::GetUserPath(Common::FS::PathType::ShaderDir) / "pipelines_recentes.txt";
}
std::string PipelineName(u64 name, bool compute) {
    return fmt::format("{}_{:#018x}", compute ? 'c' : 'g', name);
}
/// Sessions whose pipelines the warm-up builds: this one's two predecessors.
constexpr u32 RecentSessions = 2;
} // namespace

bool PipelineCache::PreloadPipeline(u64 name, bool compute) {
    if (!recent_filter || recent_preload.contains(PipelineName(name, compute))) {
        return true;
    }
    ++skipped_preloads;
    return false;
}

void PipelineCache::NoteUsedSlow(const Pipeline* pipeline) {
    if (pipeline->used.exchange(true)) {
        return;
    }
    std::scoped_lock lock{recent_mutex};
    recent_used.push_back(PipelineName(pipeline->storage_name, pipeline->IsCompute()));
}

// File: "sessao N" and then one "name session" line per pipeline.
void PipelineCache::LoadRecentPipelines() {
    std::ifstream file{RecentPipelinesPath()};
    std::string word;
    u32 last_session = 0;
    if (!(file >> word >> last_session) || word != "sessao") {
        // No list yet: nothing is preloaded (all of it is what exhausted the RAM); the
        // driver's pipeline cache keeps first-use builds to a few milliseconds.
        recent_filter = true;
        return;
    }
    recent_session = last_session + 1;
    std::string name;
    u32 session = 0;
    while (file >> name >> session) {
        if (session + RecentSessions >= recent_session) {
            recent_preload.insert(name);
            recent_earlier.emplace_back(name, session);
        }
    }
    recent_filter = true;
}

void PipelineCache::SaveRecentPipelines() {
    std::vector<std::string> used;
    {
        std::scoped_lock lock{recent_mutex};
        if (recent_used.size() == recent_saved) {
            return;
        }
        recent_saved = recent_used.size();
        used = recent_used;
    }
    std::unordered_set<std::string> current{used.begin(), used.end()};
    const auto path = RecentPipelinesPath();
    auto temp = path;
    temp += ".tmp";
    {
        std::ofstream file{temp, std::ios::trunc};
        file << "sessao " << recent_session << '\n';
        for (const auto& name : used) {
            file << name << ' ' << recent_session << '\n';
        }
        // Earlier sessions' entries the next session still preloads.
        for (const auto& [name, session] : recent_earlier) {
            if (session + RecentSessions > recent_session && !current.contains(name)) {
                file << name << ' ' << session << '\n';
            }
        }
        if (!file) {
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
}

void PipelineCache::Sync() {
    Storage::DataBase::Instance().Close();
}

} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);

    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

    srt.Read(this, sizeof(*this));

    if (walker_func_size) {
        // New field uses the old four padding bytes: archive layout stays compatible.
        // Rebuild only legacy readers with permanent patches/unresolved zero slots;
        // valid old readers and unrelated cache entries remain reusable.
        if (reader_revision != SrtReaderRevision &&
            !IsLegacySrtWalkerSafe(ar.CurrPtr(), walker_func_size)) return false;
        walker_func = RegisterWalkerCode(ar.CurrPtr(), walker_func_size);
        ar.Advance(walker_func_size);
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());

    if (fetch_shader_data) {
        spec.Write(sizeof(*fetch_shader_data));
        fetch_shader_data->Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader
