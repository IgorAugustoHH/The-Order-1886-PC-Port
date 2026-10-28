// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

u64 RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata);
u64 RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata);
/// bbport: stored permutations are keyed by their content (program hash and serialized
/// specialization), not by the in-session permutation hash, which depends on the order the
/// permutations appeared in: the same stored hash meant different permutations in different
/// sessions, and preloaded pipelines were built with the wrong shaders (black screen, menu
/// missing parts). Returns the content key, 0 when the cache is closed.
u64 RegisterShaderMeta(const Shader::Info& info,
                       const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                       const Shader::StageSpecialization& spec, size_t perm_hash, size_t perm_idx);
void RegisterShaderBinary(std::vector<u32>&& spv, u64 content_key);
/// Records which content key an in-session permutation hash stands for (pipelines are stored
/// with content keys in place of their stage hashes).
void NotePermutationContent(u64 perm_hash, u64 content_key);
/// bbport (BB_CACHE_VERIFY): the SPIR-V stored for this specialization, empty when none.
std::vector<u32> LoadStoredSpv(u64 pgm_hash, const Shader::StageSpecialization& spec,
                               u32 reader_revision);

} // namespace Vulkan
