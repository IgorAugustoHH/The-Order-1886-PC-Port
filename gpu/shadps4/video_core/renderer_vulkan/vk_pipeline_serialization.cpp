// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include "bb_face_depth_fix.h"
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <atomic>
#include <cstring>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <xxhash.h>
#include "common/hash.h"
#include "common/path_util.h"
#include "common/thread.h"
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

// TOport measurement for the shared pipeline map (BB_PIPELINE_MAP=<file from the exporter>):
// every pipeline this session builds new is looked up in the map by its stored name. The hit
// rate says whether names match across machines and installs before anything is prebuilt.
std::once_flag g_map_once;
std::unordered_set<u64> g_map_names;
std::atomic<u32> g_map_checked{0}, g_map_hits{0};

void LoadMapNames() {
    const char* path = std::getenv("BB_PIPELINE_MAP");
    if (!path || !*path) {
        return;
    }
    std::ifstream file{path, std::ios::binary};
    std::vector<u8> d{std::istreambuf_iterator<char>(file), {}};
    if (d.size() < 12 || std::memcmp(d.data(), "TOPM", 4) != 0) {
        std::printf("Pipeline map: %s not readable\n", path);
        return;
    }
    u32 count{};
    std::memcpy(&count, d.data() + 8, 4);
    size_t o = 12;
    for (u32 i = 0; i < count && o + 4 <= d.size(); ++i) {
        u32 key_size{}, is_compute{}, stages{};
        std::memcpy(&key_size, d.data() + o, 4);
        const u8* blob = d.data() + o + 4;
        std::memcpy(&is_compute, blob + 4, 4);
        if (is_compute) {
            u64 value{};
            std::memcpy(&value, blob + 8, 8);
            g_map_names.insert(value);
        } else {
            g_map_names.insert(XXH3_64bits(blob + 8, sizeof(GraphicsPipelineKey)));
        }
        o += 4 + key_size;
        std::memcpy(&stages, d.data() + o, 4);
        o += 4;
        for (u32 st = 0; st < stages; ++st) {
            u32 spec_size{};
            std::memcpy(&spec_size, d.data() + o + 16, 4);
            o += 20 + spec_size;
        }
    }
    std::printf("Pipeline map: %zu pipeline names loaded from %s\n", g_map_names.size(), path);
}

void NoteMapMatch(u64 name) {
    std::call_once(g_map_once, LoadMapNames);
    if (g_map_names.empty() || !name) {
        return;
    }
    const u32 checked = ++g_map_checked;
    const u32 hits = g_map_names.contains(name) ? ++g_map_hits : g_map_hits.load();
    if (checked % 100 == 0) {
        std::printf("Pipeline map: %u of %u new pipelines were in the map (%.1f%%)\n", hits,
                    checked, 100.0 * hits / checked);
    }
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
    NoteMapMatch(stored.value);
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
    NoteMapMatch(name);
    return name;
}



void NotePermutationContent(u64 perm_hash, u64 content_key) {
    if (!content_key) {
        return;
    }
    std::scoped_lock lock{g_content_mutex};
    g_perm_to_content[perm_hash] = content_key;
}

std::vector<u32> LoadStoredSpv(u64 pgm_hash, const Shader::StageSpecialization& spec,
                               u32 reader_revision) {
    std::vector<u32> spv;
    if (Storage::DataBase::Instance().IsOpened()) {
        Storage::DataBase::Instance().Load(
            Storage::BlobType::ShaderBinary,
            fmt::format("{:#018x}_c", PermutationContentKey(pgm_hash, spec, reader_revision)), spv);
    }
    return spv;
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
    if (auto* cache = g_area_cache.load()) {
        cache->NoteStoredProgram(info.pgm_hash, content_key);
    }

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

static bool LoadShaderMetaUnchecked(Serialization::Archive& ar, Shader::Info& info,
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

// TOport: an old or damaged blob can make the reader throw (bitset or vector sizes); such a
// stage is treated as missing (translated on first use) instead of ending the game.
bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                    Shader::StageSpecialization& spec, size_t& perm_idx) {
    // Read into copies: a blob that throws halfway must not leave the caller's program info
    // half overwritten (that program is then translated again from its own state).
    Shader::Info read_info = info;
    std::optional<Shader::Gcn::FetchShaderData> read_fetch = fetch_shader_data;
    Shader::StageSpecialization read_spec = spec;
    size_t read_idx = perm_idx;
    try {
        if (!LoadShaderMetaUnchecked(ar, read_info, read_fetch, read_spec, read_idx)) {
            return false;
        }
    } catch (...) {
        return false;
    }
    info = std::move(read_info);
    fetch_shader_data = std::move(read_fetch);
    spec = std::move(read_spec);
    perm_idx = read_idx;
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


// TOport: BB_EXPORT_PIPELINE_MAP=<file> writes the pipeline map shared with other players: every
// stored pipeline key (render state, vertex layout, stage content keys) and, per stage, the
// program hash and its specialization. No SPIR-V, no shader code and no user data from the game:
// each player translates the programs from their own dump.
// File: "TOPM" u32 version, u32 entries; per entry: u32 key_size, key blob (as stored), u32
// stages; per stage: u64 content_key, u64 pgm_hash, u32 spec_size, spec blob.
static void ExportPipelineMap(const char* out_path) {
    std::FILE* out = std::fopen(out_path, "wb");
    if (!out) {
        std::fprintf(stderr, "Pipeline map: cannot write %s\n", out_path);
        return;
    }
    const auto put = [&](const void* data, size_t size) { std::fwrite(data, 1, size, out); };
    const u32 version = 1;
    u32 entries = 0, complete = 0, no_meta = 0, old_version = 0, damaged = 0, motion_stages = 0;
    put("TOPM", 4);
    put(&version, 4);
    put(&entries, 4); // rewritten at the end
    Storage::DataBase::Instance().ForEachBlobNamed(
        Storage::BlobType::PipelineKey, [&](const std::string&, std::vector<u8>&& data) {
            Serialization::Archive ar{std::vector<u8>(data)};
            Serialization::Reader reader{ar};
            u32 key_version{}, is_compute{};
            reader.Read(key_version);
            if (key_version != Serialization::PipelineKeyVersion) {
                return;
            }
            reader.Read(is_compute);
            std::vector<u64> stages;
            if (is_compute) {
                ComputePipelineKey key{};
                key.Deserialize(ar);
                stages.push_back(key.value);
            } else {
                GraphicsPipelineKey key{};
                key.Deserialize(ar);
                for (const u64 hash : key.stage_hashes) {
                    if (hash) {
                        stages.push_back(hash);
                    }
                }
            }
            std::vector<std::pair<u64, std::vector<u8>>> specs; // pgm_hash, spec blob
            for (const u64 content : stages) {
                std::vector<u8> meta_blob;
                Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                                   fmt::format("{:#018x}", content), meta_blob);
                Shader::Info info{};
                std::optional<Shader::Gcn::FetchShaderData> fetch;
                Shader::StageSpecialization spec{};
                size_t perm_idx{};
                if (meta_blob.empty()) {
                    ++no_meta;
                    return;
                }
                // Read like LoadShaderMeta, but motion-vector vertex shaders are kept: their
                // specialization does not depend on the session's buffer addresses.
                Serialization::Archive meta_ar{std::move(meta_blob)};
                try {
                    Serialization::Reader meta{meta_ar};
                    u32 meta_version{}, binary_version{};
                    u64 stored_key{};
                    meta.Read(meta_version);
                    meta.Read(binary_version);
                    if ((meta_version != Serialization::ShaderMetaVersion && meta_version != 12u) ||
                        binary_version != Serialization::ShaderBinaryVersion) {
                        ++old_version;
                        return;
                    }
                    meta.Read(stored_key);
                    meta.Read(perm_idx);
                    spec.Deserialize(meta_ar);
                    // Only the persistent part (program hash, stage): Info::Deserialize would also
                    // register the SRT walker code in the never-freed 128 MB code buffer, which
                    // thousands of reads exhaust ("code is too big") for the running game too.
                    meta.Read(&info, sizeof(Shader::InfoPersistent));
                } catch (const std::exception& e) {
                    if (damaged++ < 3) {
                        std::printf("Pipeline map: stage %#018llx unreadable: %s\n",
                                    (unsigned long long)content, e.what());
                    }
                    return;
                } catch (...) {
                    ++damaged;
                    return;
                }
                if (info.hw_stage == Shader::HwStage::Vertex && spec.runtime_info.hw.vs.motion_vectors) {
                    ++motion_stages;
                }
                Serialization::Archive spec_ar{};
                spec.Serialize(spec_ar);
                specs.emplace_back(info.pgm_hash, spec_ar.TakeOff());
            }
            const u32 key_size = u32(data.size()), count = u32(stages.size());
            put(&key_size, 4);
            put(data.data(), data.size());
            put(&count, 4);
            for (size_t i = 0; i < stages.size(); ++i) {
                const u32 spec_size = u32(specs[i].second.size());
                put(&stages[i], 8);
                put(&specs[i].first, 8);
                put(&spec_size, 4);
                put(specs[i].second.data(), spec_size);
            }
            ++entries;
            ++complete;
        });
    std::fseek(out, 8, SEEK_SET);
    put(&entries, 4);
    std::fclose(out);
    std::printf("Pipeline map: %u pipelines written to %s (%u motion-vector stages included); "
                "skipped: %u stage without metadata, %u old cache version, %u damaged\n",
                complete, out_path, motion_stages, no_meta, old_version, damaged);
    std::fflush(stdout);
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

void PipelineCache::StartDiskProgramIndex() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_DISK_PROGRAMS");
        return !(env && env[0] == '0');
    }();
    if (!enabled || !Storage::DataBase::Instance().IsOpened()) {
        return;
    }
    // The index of the previous session: one file, ready before the first frame (reading every
    // .meta took 95 s on a 15k-entry cache, scanned file by file).
    {
        std::vector<u8> saved;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "program_index", saved);
        if (saved.size() >= 8 && (saved.size() - 8) % 16 == 0) {
            u32 version{}, count{};
            std::memcpy(&version, saved.data(), 4);
            std::memcpy(&count, saved.data() + 4, 4);
            if (version == 1 && saved.size() == 8 + size_t(count) * 16) {
                std::unordered_map<u64, std::vector<u64>> index;
                for (u32 i = 0; i < count; ++i) {
                    u64 pair[2];
                    std::memcpy(pair, saved.data() + 8 + size_t(i) * 16, 16);
                    index[pair[0]].push_back(pair[1]);
                }
                {
                    std::scoped_lock lock{disk_index_mutex};
                    disk_programs = std::move(index);
                }
                disk_index_ready.store(true, std::memory_order_release);
                std::printf("Disk programs: %u stored permutations of %zu programs from the saved "
                            "index\n", count, disk_programs.size());
            }
        }
    }
    // The full scan (a minute of small reads beside the game's own streaming) only builds the
    // first index; later sessions add what they store (NoteStoredProgram).
    const char* rescan = std::getenv("BB_DISK_INDEX_RESCAN");
    if (disk_index_ready.load() && !(rescan && rescan[0] == '1')) {
        return;
    }
    disk_index_thread = std::jthread([this](std::stop_token stop) {
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
        const auto begin = std::chrono::steady_clock::now();
        // Where pgm_hash sits in the raw InfoPersistent bytes that Info::Serialize writes.
        const Shader::InfoPersistent probe(Shader::HwStage::Vertex, Shader::SwStage::Vertex, 0);
        const size_t hash_offset = reinterpret_cast<const u8*>(&probe.pgm_hash) -
                                   reinterpret_cast<const u8*>(&probe);
        std::unordered_map<u64, std::vector<u64>> index;
        u32 metas = 0;
        Storage::DataBase::Instance().ForEachBlobNamed(
            Storage::BlobType::ShaderMeta, [&](const std::string& name, std::vector<u8>&& data) {
                if (stop.stop_requested()) {
                    return;
                }
                const u64 content_key = std::strtoull(name.c_str(), nullptr, 16);
                if (!content_key) {
                    return;
                }
                Serialization::Archive ar{std::move(data)};
                const u8* const base = ar.CurrPtr();
                Serialization::Reader meta{ar};
                u32 meta_version{}, binary_version{};
                u64 perm_hash{};
                size_t perm_idx{};
                meta.Read(meta_version);
                meta.Read(binary_version);
                if ((meta_version != Serialization::ShaderMetaVersion && meta_version != 12u) ||
                    binary_version != Serialization::ShaderBinaryVersion) {
                    return;
                }
                meta.Read(perm_hash);
                meta.Read(perm_idx);
                Shader::StageSpecialization spec{};
                spec.Deserialize(ar);
                if (ar.SizeBytes() - size_t(ar.CurrPtr() - base) < hash_offset + sizeof(u64)) {
                    return;
                }
                u64 pgm_hash{};
                std::memcpy(&pgm_hash, ar.CurrPtr() + hash_offset, sizeof(pgm_hash));
                index[pgm_hash].push_back(content_key);
                ++metas;
            });
        if (stop.stop_requested()) {
            return;
        }
        std::vector<u8> saved(8);
        const u32 version = 1;
        std::memcpy(saved.data(), &version, 4);
        u32 count = 0;
        for (const auto& [pgm_hash, keys] : index) {
            for (const u64 key : keys) {
                const u64 pair[2] = {pgm_hash, key};
                const auto* bytes = reinterpret_cast<const u8*>(pair);
                saved.insert(saved.end(), bytes, bytes + 16);
                ++count;
            }
        }
        std::memcpy(saved.data() + 4, &count, 4);
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "program_index",
                                           std::move(saved));
        {
            std::scoped_lock lock{disk_index_mutex};
            disk_programs = std::move(index);
        }
        disk_index_ready.store(true, std::memory_order_release);
        std::printf("Disk programs: %u stored permutations of %zu programs indexed in %.0f ms (saved)\n",
                    metas, disk_programs.size(),
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              begin).count());
    });
}

// bbport: programs never taken from the disk cache, always translated in the session.
// The face shaders: a stored module came back unfit after camera cuts (face lost its texture;
// fine with the cache off). Not the cause (draw workers were); BB_FACE_FROM_CACHE=0 keeps them out.
// BB_RETRANSLATE=hash,hash (hex) adds more; those programs are translated (and with BB_DUMP_ONLY
// dumped) again.
static bool NeverFromCache(u64 pgm_hash) {
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
    static const bool face_from_cache = [] {
        const char* env = std::getenv("BB_FACE_FROM_CACHE");
        return !(env && env[0] == '0');
    }();
    static constexpr u64 FaceShaders[] = {0xf5c17eec, 0x193274a4, 0x0e4fee1c, 0x39e98e7c, 0xdc16510f};
    if (!face_from_cache && std::ranges::find(FaceShaders, pgm_hash) != std::end(FaceShaders)) {
        return true;
    }
    return std::ranges::find(retranslate, pgm_hash) != retranslate.end();
}

std::unique_ptr<Program> PipelineCache::PrepareProgramFromDisk(u64 pgm_hash) {
    if (NeverFromCache(pgm_hash)) {
        return {};
    }
    if (!disk_index_ready.load(std::memory_order_acquire)) {
        return {};
    }
    std::vector<u64> keys;
    {
        std::scoped_lock lock{disk_index_mutex};
        const auto it = disk_programs.find(pgm_hash);
        if (it == disk_programs.end()) {
            return {};
        }
        keys = it->second;
    }
    std::unique_ptr<Program> program;
    for (const u64 content_key : keys) {
        if (program && program->modules.size() >= Program::MaxPermutations) {
            break;
        }
        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", content_key), meta_blob);
        std::vector<u32> spv;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
                                           fmt::format("{:#018x}_c", content_key), spv);
        if (meta_blob.empty() || spv.empty()) {
            continue;
        }
        auto loaded = std::make_unique<Program>();
        Shader::StageSpecialization spec{};
        spec.info = &loaded->info;
        std::optional<Shader::Gcn::FetchShaderData> fetch;
        size_t stored_idx{};
        Serialization::Archive ar{std::move(meta_blob)};
        if (!LoadShaderMeta(ar, loaded->info, fetch, spec, stored_idx) ||
            loaded->info.pgm_hash != pgm_hash) {
            continue;
        }
        // As LoadPipelineStage: binaries of the old float atomic min/max fallback are not reused.
        bool umax = false, smin = false, umin = false, smax = false;
        for (size_t i = 5; i < spv.size();) {
            const u32 count = spv[i] >> 16, opcode = spv[i] & 0xffff;
            umax |= opcode == 239;
            smin |= opcode == 236;
            umin |= opcode == 237;
            smax |= opcode == 238;
            i += count ? count : 1;
        }
        if ((umax && smin) || (umin && smax)) {
            continue;
        }
        if (!program) {
            program = std::move(loaded);
        }
        spec.info = &program->info;
        const vk::ShaderModule module = CompileSPV(spv, instance.GetDevice());
        if (program->info.sw_stage == Shader::SwStage::Vertex) {
            BbFaceDepth::Register(module, spv, pgm_hash);
        }
        NotePermutationContent(HashCombine(pgm_hash, program->modules.size()), content_key);
        program->AddPermut(module, std::move(spec));
    }
    if (!program || program->modules.empty()) {
        return {};
    }
    if (!program || program->modules.empty()) {
        return {};
    }
    program->info_template = std::make_unique<Shader::Info>(program->info);
    return program;
}

bool PipelineCache::LoadProgramFromDisk(u64 pgm_hash) {
    std::unique_ptr<Program> program;
    double lead_ms = -1.0;
    {
        std::scoped_lock lock{area_mutex};
        const auto it = prepared_programs.find(pgm_hash);
        if (it != prepared_programs.end()) {
            program = std::move(it->second.program);
            lead_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                it->second.ready_at).count();
            prepared_programs.erase(it);
        }
    }
    const auto begin = std::chrono::steady_clock::now();
    if (!program) {
        program = PrepareProgramFromDisk(pgm_hash);
    }
    if (!program) {
        return false;
    }
    const size_t permutations = program->modules.size();
    {
        std::unique_lock lk{programs_mutex};
        program_cache.emplace(pgm_hash, std::move(program));
    }
    static u32 logged = 0;
    if (logged++ < 300) {
        if (lead_ms >= 0) {
            std::printf("Disk program ready: %016llx, %zu permutations, prepared %.0f ms before use\n",
                        (unsigned long long)pgm_hash, permutations, lead_ms);
        } else {
            std::printf("Disk program loaded: %016llx, %zu permutations, %.2f ms\n",
                        (unsigned long long)pgm_hash, permutations,
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                  begin).count());
        }
    }
    return true;
}

void PipelineCache::NoteStoredProgram(u64 pgm_hash, u64 content_key) {
    if (!disk_index_ready.load(std::memory_order_acquire)) {
        return;
    }
    std::scoped_lock lock{disk_index_mutex};
    auto& keys = disk_programs[pgm_hash];
    if (std::ranges::find(keys, content_key) == keys.end()) {
        keys.push_back(content_key);
        disk_index_dirty = true;
    }
}

void PipelineCache::SaveDiskIndex() {
    std::vector<u8> saved(8);
    u32 count = 0;
    {
        std::scoped_lock lock{disk_index_mutex};
        if (!disk_index_dirty) {
            return;
        }
        disk_index_dirty = false;
        for (const auto& [pgm_hash, keys] : disk_programs) {
            for (const u64 key : keys) {
                const u64 pair[2] = {pgm_hash, key};
                const auto* bytes = reinterpret_cast<const u8*>(pair);
                saved.insert(saved.end(), bytes, bytes + 16);
                ++count;
            }
        }
    }
    const u32 version = 1;
    std::memcpy(saved.data(), &version, 4);
    std::memcpy(saved.data() + 4, &count, 4);
    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "program_index",
                                       std::move(saved));
}

void PipelineCache::StartAreaPrefetch() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_AREA_PREFETCH");
        return !(env && env[0] == '0');
    }();
    if (!enabled || !Storage::DataBase::Instance().IsOpened()) {
        std::scoped_lock lock{area_mutex};
        area_index_state = 0; // nothing to build
        return;
    }
    std::vector<u8> saved;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "package_index", saved);
    {
        std::scoped_lock lock{area_mutex};
        u32 version{}, count{};
        if (saved.size() >= 8) {
            std::memcpy(&version, saved.data(), 4);
            std::memcpy(&count, saved.data() + 4, 4);
        }
        if (version == 1 && saved.size() == 8 + size_t(count) * 16) {
            for (u32 i = 0; i < count; ++i) {
                u64 pair[2];
                std::memcpy(pair, saved.data() + 8 + size_t(i) * 16, 16);
                package_shaders[pair[0]].push_back(pair[1]);
            }
            area_index_state = 0;
            std::printf("Area prefetch: %zu packages, %u shader entries from the saved index\n",
                        package_shaders.size(), count);
        } else {
            area_index_state = 1;
            std::printf("Area prefetch: no package index yet; it is built in the background\n");
        }
    }
    area_thread = std::jthread([this](std::stop_token stop) { AreaPrefetchLoop(stop); });
}

int PipelineCache::AreaIndexNeeded() {
    std::scoped_lock lock{area_mutex};
    return area_index_state;
}

void PipelineCache::AreaPackageShader(u64 package, u64 pgm_hash) {
    std::scoped_lock lock{area_mutex};
    auto& list = package_building[package];
    if (std::ranges::find(list, pgm_hash) == list.end()) {
        list.push_back(pgm_hash);
    }
}

void PipelineCache::AreaIndexDone() {
    std::vector<u8> saved(8);
    u32 count = 0;
    {
        std::scoped_lock lock{area_mutex};
        for (const auto& [package, hashes] : package_building) {
            for (const u64 hash : hashes) {
                const u64 pair[2] = {package, hash};
                const auto* bytes = reinterpret_cast<const u8*>(pair);
                saved.insert(saved.end(), bytes, bytes + 16);
                ++count;
            }
        }
        package_shaders = std::move(package_building);
        package_building.clear();
        area_index_state = 0;
    }
    const u32 version = 1;
    std::memcpy(saved.data(), &version, 4);
    std::memcpy(saved.data() + 4, &count, 4);
    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "package_index",
                                       std::move(saved));
    std::printf("Area prefetch: package index built, %u shader entries (saved)\n", count);
}

void PipelineCache::AreaPackageOpened(u64 package) {
    {
        std::scoped_lock lock{area_mutex};
        if (!package_shaders.contains(package)) {
            static u32 logged = 0;
            if (logged++ < 20) {
                std::printf("Area prefetch: package %016llx read, no shaders in the index%c",
                            (unsigned long long)package, 10);
            }
            return;
        }
        if (std::ranges::find(opened_packages, package) == opened_packages.end()) {
            opened_packages.push_back(package);
        }
    }
    area_cv.notify_one();
}

void PipelineCache::AreaPrefetchLoop(std::stop_token stop) {
    Common::SetCurrentThreadName("bb:AreaPrefetch");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
    // Prepared programs nobody used yet are capped (their modules hold driver memory).
    constexpr size_t MaxPrepared = 3000;
    for (;;) {
        u64 package{};
        std::vector<u64> hashes;
        {
            std::unique_lock lock{area_mutex};
            if (!area_cv.wait_for(lock, stop, std::chrono::seconds(60),
                                  [&] { return !opened_packages.empty(); })) {
                if (stop.stop_requested()) {
                    return;
                }
                lock.unlock();
                SaveDiskIndex();
                continue;
            }
            package = opened_packages.front();
            opened_packages.pop_front();
            hashes = package_shaders[package];
        }
        if (!disk_index_ready.load(std::memory_order_acquire)) {
            continue;
        }
        const auto begin = std::chrono::steady_clock::now();
        u32 prepared = 0, known = 0;
        for (const u64 hash : hashes) {
            if (stop.stop_requested()) {
                return;
            }
            {
                std::shared_lock lk{programs_mutex};
                if (program_cache.count(hash)) {
                    ++known;
                    continue;
                }
            }
            {
                std::scoped_lock lock{area_mutex};
                if (prepared_programs.contains(hash)) {
                    ++known;
                    continue;
                }
            }
            auto program = PrepareProgramFromDisk(hash);
            if (!program) {
                continue;
            }
            std::scoped_lock lock{area_mutex};
            prepared_programs.emplace(hash,
                                      PreparedProgram{std::move(program), std::chrono::steady_clock::now()});
            prepared_order.push_back(hash);
            ++prepared;
            while (prepared_order.size() > MaxPrepared) {
                const auto it = prepared_programs.find(prepared_order.front());
                if (it != prepared_programs.end() && it->second.program) {
                    for (auto& module : it->second.program->modules) {
                        instance.GetDevice().destroyShaderModule(module.module);
                    }
                    prepared_programs.erase(it);
                }
                prepared_order.pop_front();
            }
        }
        static u32 logged = 0;
        if (logged++ < 200) {
            std::printf("Area prefetch: package %016llx, %zu shaders (%u already loaded), %u prepared "
                        "in %.0f ms\n",
                        (unsigned long long)package, hashes.size(), known, prepared,
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                  begin).count());
        }
    }
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

    if (NeverFromCache(program->info.pgm_hash)) {
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
    // Exported 60 s after start, once the preload and the menu are done, on its own thread.
    if (const char* map_path = std::getenv("BB_EXPORT_PIPELINE_MAP"); map_path && *map_path) {
        std::thread{[path = std::string{map_path}] {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            std::printf("Pipeline map: export started\n");
            std::fflush(stdout);
            ExportPipelineMap(path.c_str());
        }}.detach();
    }
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
    const u32 expected = recent_filter ? u32(recent_preload.size()) : 0;
    auto last_frame = preload_begin;
    if (g_preload_progress) {
        g_preload_progress(0, expected, 0.0);
    }

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
            if (g_preload_progress && now - last_frame >= std::chrono::milliseconds(33)) {
                last_frame = now;
                g_preload_progress(num_pipelines, expected,
                                   std::chrono::duration<double>(now - preload_begin).count());
            }
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
