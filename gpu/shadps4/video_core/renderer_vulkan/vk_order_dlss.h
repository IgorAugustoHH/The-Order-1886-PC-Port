// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (The Order: 1886): NVIDIA DLSS in place of the game's temporal MSAA resolve
// (customresolve_cs_msaa2x/4x_temporal). The resolve already receives everything DLSS needs:
// the MSAA HDR scene, the prepass velocity target and the exposure; the scene depth is the depth
// target drawn with that color. DLSS writes the resolve's output (also the game's history), so
// bloom, DoF, the focus and the tonemap downstream are untouched. BB_ORDER_DLSS=1 with BB_DLSS=1
// and the inherited Bloodborne upscaler off (BB_UPSCALER=none).
// BB_ORDER_DLSS_MODE=quality|balanced|performance|ultra: the scene (from the first pass with the
// MSAA scene depth to the resolve) is drawn into reduced SceneTargets proxies and DLSS
// reconstructs the 1920x1080 resolve output; without it, DLAA. See docs/order_dlss.md.
#pragma once

#include <array>
#include <chrono>
#include <span>

#include "common/types.h"
#include "video_core/renderer_vulkan/scene_resolution.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/types.h"

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;
class SceneTargets;

class OrderDlss {
public:
    OrderDlss(const Instance& instance, Scheduler& scheduler, VideoCore::TextureCache& texture_cache,
              Runtime& runtime, SceneTargets& scene_targets);
    ~OrderDlss();

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }
    [[nodiscard]] static bool IsResolve(u64 cs_hash) noexcept;

    /// A DLSS mode below DLAA: the scene is drawn at a reduced size.
    [[nodiscard]] bool Scaling() const noexcept {
        return enabled && !failed && scaled;
    }
    /// From the first pass drawing the MSAA scene depth to the resolve.
    [[nodiscard]] bool SceneOpen() const noexcept {
        return scene_open;
    }
    /// Before a pass with an eligible MSAA depth target: opens the reduced scene.
    void OpenScene();

    /// Every draw: remembers the depth target an MSAA RGBA16F scene color is drawn with.
    void OnTargets(VideoCore::ImageId color, VideoCore::ImageId depth);

    /// After the resolve's resources are bound: records DLSS into its output. True when the
    /// game's dispatch must be skipped.
    bool Replace(std::span<const VideoCore::ImageId> images, std::span<const u32> flatbuf);

private:
    bool EnsureResources(u32 width, u32 height, u32 out_width, u32 out_height);
    vk::ImageView TempView(const VideoCore::Image& image, vk::Format format,
                           vk::ImageAspectFlags aspect);

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    SceneTargets& scene_targets;

    bool enabled = false;
    bool failed = false;
    bool depth_invert = false;
    bool scaled = false;
    bool size_set = false;
    bool scene_open = false;
    SceneResolution::Size scene_size{};
    VideoCore::ImageId scene_color{};
    VideoCore::ImageId scene_depth{};
    u64 frames = 0;
    bool reset = true;
    std::chrono::steady_clock::time_point last_time{};

    u32 width = 0, height = 0, out_width = 0, out_height = 0;
    VideoCore::UniqueImage color_image, motion_image, depth_image, output_image;
    vk::UniqueImageView color_view, motion_view, depth_view, output_view;
    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline pipeline;
};

} // namespace Vulkan
