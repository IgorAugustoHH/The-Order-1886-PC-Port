// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include <span>

namespace BbFaceDepth {
bool Enabled();
bool SecondaryEnabled();
void Register(vk::ShaderModule module, std::span<const u32> original, u64 vertex_hash);
vk::UniqueShaderModule Create(vk::ShaderModule original, vk::Device device, u64 pixel_hash);
void LogDraw(u64 vs, u64 ps, bool indexed, u32 indices, u32 instances, u32 samples,
             u32 depth_samples, bool depth_test, bool depth_write, u32 compare,
             bool adjusted, u64 geometry_address = 0, bool indirect = false);
}
