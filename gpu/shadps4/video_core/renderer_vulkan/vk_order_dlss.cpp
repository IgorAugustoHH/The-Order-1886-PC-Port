// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_order_dlss.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fmt/format.h>

#include "video_core/host_shaders/order_dlss_prep_comp.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

struct PushConstants {
    u32 width, height;
    float velocity_scale;
    float log2_exposure;
    u32 samples;
    u32 depth_invert;
    u32 exposure_from_texture;
};

bool EnvOn(const char* name) {
    const char* env = std::getenv(name);
    return env && env[0] == '1';
}

/// BB_ORDER_DLSS_MODE: SceneResolution::ForPreset index (0 DLAA .. 4 Ultra Performance).
int ModePreset() {
    const char* env = std::getenv("BB_ORDER_DLSS_MODE");
    if (!env || !env[0]) {
        return 0;
    }
    if (!std::strcmp(env, "quality")) return 1;
    if (!std::strcmp(env, "balanced")) return 2;
    if (!std::strcmp(env, "performance")) return 3;
    if (!std::strcmp(env, "ultra")) return 4;
    return 0;
}

} // namespace

bool OrderDlss::IsResolve(u64 cs_hash) noexcept {
    // customresolve_cs_msaa2x_temporal, customresolve_cs_msaa4x_temporal (analise/shaders_nomes)
    return cs_hash == 0xe57f0150 || cs_hash == 0x92311aca;
}

OrderDlss::OrderDlss(const Instance& instance_, Scheduler& scheduler_,
                     VideoCore::TextureCache& texture_cache_, Runtime& runtime_,
                     SceneTargets& scene_targets_)
    : instance{instance_}, scheduler{scheduler_}, texture_cache{texture_cache_},
      runtime{runtime_}, scene_targets{scene_targets_} {
    enabled = EnvOn("BB_ORDER_DLSS");
    depth_invert = EnvOn("BB_ORDER_DLSS_DEPTH_INVERT");
    if (!enabled) {
        return;
    }
    if (const int preset = ModePreset()) {
        scaled = true;
        scene_size = SceneResolution::ForPreset(preset, SceneResolution::GameSize());
        scene_targets.allow_msaa = true;
    }
    const auto device = instance.GetDevice();
    std::array<vk::DescriptorSetLayoutBinding, 7> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i] = {.binding = i,
                       .descriptorType = (i >= 3 && i <= 5) ? vk::DescriptorType::eStorageImage
                                                            : vk::DescriptorType::eSampledImage,
                       .descriptorCount = 1,
                       .stageFlags = vk::ShaderStageFlagBits::eCompute};
    }
    desc_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                      .offset = 0,
                                      .size = sizeof(PushConstants)};
    pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*desc_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    }));
    const auto module = CompileSPV(ORDER_DLSS_PREP_COMP, device);
    pipeline = Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *pipeline_layout,
            }));
    device.destroyShaderModule(module);
    std::printf("Order DLSS: on (replaces the temporal MSAA resolve; depth invert %d; scene %s)\n",
                depth_invert ? 1 : 0,
                scaled ? fmt::format("{}x{} -> {}x{}", scene_size.width, scene_size.height, SceneResolution::GameSize().width, SceneResolution::GameSize().height)
                             .c_str()
                       : "native (DLAA)");
}

OrderDlss::~OrderDlss() = default;

void OrderDlss::OpenScene() {
    if (!Scaling() || scene_open) {
        return;
    }
    if (!size_set) {
        scheduler.EndRendering();
        scene_targets.SetSize(scene_size);
        size_set = true;
    }
    scene_open = true;
}

void OrderDlss::OnTargets(VideoCore::ImageId color, VideoCore::ImageId depth) {
    if (!color || !depth) {
        return;
    }
    const auto& c = texture_cache.GetImage(color);
    if (c.info.num_samples > 1 && c.info.pixel_format == vk::Format::eR16G16B16A16Sfloat) {
        scene_color = color;
        scene_depth = depth;
    }
}

bool OrderDlss::EnsureResources(u32 w, u32 h, u32 ow, u32 oh) {
    if (w == width && h == height && ow == out_width && oh == out_height && output_view) {
        return true;
    }
    scheduler.Finish(); // the previous images may still be in use
    const auto device = instance.GetDevice();
    const auto allocator = instance.GetAllocator();
    const auto make = [&](VideoCore::UniqueImage& image, vk::UniqueImageView& view,
                          vk::Format format, vk::ImageUsageFlags usage, u32 iw, u32 ih) {
        view.reset();
        image = VideoCore::UniqueImage(device, allocator);
        image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {iw, ih, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        view = Check(device.createImageViewUnique({
            .image = vk::Image(image),
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
    };
    const auto rw = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;
    make(color_image, color_view, vk::Format::eR16G16B16A16Sfloat, rw, w, h);
    make(motion_image, motion_view, vk::Format::eR16G16Sfloat, rw, w, h);
    make(depth_image, depth_view, vk::Format::eR32Sfloat, rw, w, h);
    make(output_image, output_view, vk::Format::eR16G16B16A16Sfloat,
         rw | vk::ImageUsageFlagBits::eTransferSrc, ow, oh);
    width = w;
    height = h;
    out_width = ow;
    out_height = oh;
    reset = true;
    std::printf("Order DLSS: resources %ux%u -> %ux%u\n", w, h, ow, oh);
    return true;
}

vk::ImageView OrderDlss::TempView(const VideoCore::Image& image, vk::Format format,
                                  vk::ImageAspectFlags aspect) {
    const auto device = instance.GetDevice();
    const auto view = Check(device.createImageView({
        .image = vk::Image(image.backing->image),
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {aspect, 0, 1, 0, 1},
    }));
    scheduler.DeferOperation([device, view] { device.destroyImageView(view); });
    return view;
}

bool OrderDlss::Replace(std::span<const VideoCore::ImageId> images, std::span<const u32> flatbuf) {
    // The reduced scene ends at the resolve, whether DLSS runs or not.
    const bool was_open = scene_open;
    scene_open = false;
    Dlss* dlss = Dlss::Get();
    if (!enabled || failed || !dlss || !dlss->Available() || flatbuf.size() < 66) {
        return false;
    }
    VideoCore::ImageId exposure_id{}, velocity_id{}, color_id{}, output_id{};
    for (const auto id : images) {
        const auto& image = texture_cache.GetImage(id);
        const auto format = image.info.pixel_format;
        if (format == vk::Format::eR32Sfloat && image.info.size.width == 1) {
            exposure_id = id;
        } else if (format == vk::Format::eA2B10G10R10UnormPack32) {
            velocity_id = id;
        } else if (format == vk::Format::eR16G16B16A16Sfloat && image.info.num_samples > 1) {
            color_id = id;
        } else if (format == vk::Format::eR16G16B16A16Sfloat &&
                   image.backing->state.layout == vk::ImageLayout::eGeneral) {
            output_id = id; // bound as storage; the history is bound for sampling
        }
    }
    static u32 logged = 0;
    const auto skip = [&](const char* why) {
        if (logged < 8) {
            ++logged;
            std::printf("Order DLSS: game resolve kept (%s)\n", why);
        }
        return false;
    };
    if (!exposure_id || !velocity_id || !color_id || !output_id) {
        return skip("resolve resources not recognized");
    }
    if (color_id != scene_color || !scene_depth) {
        return skip("no depth target drawn with the scene color");
    }
    auto& color = texture_cache.GetImage(color_id);
    auto& velocity = texture_cache.GetImage(velocity_id);
    auto& exposure = texture_cache.GetImage(exposure_id);
    auto& output = texture_cache.GetImage(output_id);
    auto& depth = texture_cache.GetImage(scene_depth);
    const u32 ow = output.info.size.width, oh = output.info.size.height;
    const auto same = [&](const VideoCore::Image& i) {
        return i.info.size.width == ow && i.info.size.height == oh;
    };
    if (!same(color) || !same(velocity) || !same(depth) ||
        depth.info.num_samples != color.info.num_samples || !depth.info.props.is_depth ||
        !(output.usage_flags & vk::ImageUsageFlagBits::eTransferDst)) {
        return skip("resource sizes or formats differ");
    }
    // The reduced scene: every input has a proxy at the scene size (drawn this frame).
    const bool reduced = was_open && scene_targets.Reduced() &&
                         scene_targets.Eligible(color) && scene_targets.Eligible(velocity) &&
                         scene_targets.Eligible(depth);
    if (Scaling() && !reduced) {
        static u32 native_logged = 0;
        if (native_logged < 8) {
            ++native_logged;
            std::printf("Order DLSS: scene drawn at native size this frame (open %d)\n",
                        was_open ? 1 : 0);
        }
    }
    const u32 w = reduced ? scene_targets.Size().width : ow;
    const u32 h = reduced ? scene_targets.Size().height : oh;
    if (!EnsureResources(w, h, ow, oh)) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - last_time).count();
    if (frame_ms <= 0.0f || frame_ms > 250.0f) {
        reset = true; // paused, loading or a skipped resolve: the history is stale
        frame_ms = 16.6f;
    }
    last_time = now;

    float velocity_scale, log2_exposure;
    std::memcpy(&velocity_scale, &flatbuf[63], 4);
    std::memcpy(&log2_exposure, &flatbuf[60], 4);
    float exposure_flag;
    std::memcpy(&exposure_flag, &flatbuf[61], 4);

    scheduler.EndRendering();
    vk::ImageView color_in, velocity_in, depth_in;
    vk::ImageLayout color_layout, velocity_layout, depth_layout;
    if (reduced) {
        // The proxies hold this frame's scene; Read leaves them in General for compute reads.
        VideoCore::ImageViewInfo info;
        info.format = color.info.pixel_format;
        const auto c = scene_targets.Read(color_id, info);
        info.format = depth.info.pixel_format;
        const auto d = scene_targets.Read(scene_depth, info);
        color_in = c.view;
        // The native velocity (single-sample): the resolve's binding skipped proxy resolves
        // (Rasterizer::DispatchRecord), so this transition resolves its 2x proxy now.
        runtime.Transit(&velocity, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
        velocity_in =
            TempView(velocity, velocity.info.pixel_format, vk::ImageAspectFlagBits::eColor);
        depth_in = d.view;
        color_layout = c.layout;
        velocity_layout = velocity.backing->state.layout;
        depth_layout = d.layout;
    } else {
        runtime.Transit(&depth, vk::ImageLayout::eDepthStencilReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
        color_in = TempView(color, color.info.pixel_format, vk::ImageAspectFlagBits::eColor);
        velocity_in =
            TempView(velocity, velocity.info.pixel_format, vk::ImageAspectFlagBits::eColor);
        depth_in = TempView(depth, depth.info.pixel_format, vk::ImageAspectFlagBits::eDepth);
        color_layout = color.backing->state.layout;
        velocity_layout = velocity.backing->state.layout;
        depth_layout = depth.backing->state.layout;
    }
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto own_barrier = [&](vk::Image image, vk::ImageLayout old_layout,
                                 vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                 vk::ImageLayout new_layout, vk::PipelineStageFlags2 dst_stage,
                                 vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .image = image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    };
    const auto all = vk::PipelineStageFlagBits2::eAllCommands;
    const auto rw = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
    for (const auto image : {vk::Image(color_image), vk::Image(motion_image),
                             vk::Image(depth_image), vk::Image(output_image)}) {
        own_barrier(image, vk::ImageLayout::eUndefined, all, vk::AccessFlagBits2::eNone,
                    vk::ImageLayout::eGeneral, all, rw);
    }

    const vk::DescriptorImageInfo infos[7] = {
        {.imageView = color_in, .imageLayout = color_layout},
        {.imageView = velocity_in, .imageLayout = velocity_layout},
        {.imageView = depth_in, .imageLayout = depth_layout},
        {.imageView = *color_view, .imageLayout = vk::ImageLayout::eGeneral},
        {.imageView = *motion_view, .imageLayout = vk::ImageLayout::eGeneral},
        {.imageView = *depth_view, .imageLayout = vk::ImageLayout::eGeneral},
        {.imageView = TempView(exposure, exposure.info.pixel_format, vk::ImageAspectFlagBits::eColor),
         .imageLayout = exposure.backing->state.layout},
    };
    std::array<vk::WriteDescriptorSet, 7> writes{};
    for (u32 i = 0; i < writes.size(); ++i) {
        writes[i] = {.dstBinding = i,
                     .descriptorCount = 1,
                     .descriptorType = (i >= 3 && i <= 5) ? vk::DescriptorType::eStorageImage
                                                          : vk::DescriptorType::eSampledImage,
                     .pImageInfo = &infos[i]};
    }
    // The game's velocity is in output (1920x1080) pixels; DLSS takes render pixels.
    const PushConstants push{
        .width = w,
        .height = h,
        .velocity_scale = velocity_scale * float(w) / float(ow),
        .log2_exposure = log2_exposure,
        .samples = color.info.num_samples,
        .depth_invert = depth_invert ? 1u : 0u,
        .exposure_from_texture = exposure_flag != 0.0f ? 1u : 0u,
    };
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                         &push);
    cmdbuf.dispatch((w + 7) / 8, (h + 7) / 8, 1);
    for (const auto image : {vk::Image(color_image), vk::Image(motion_image),
                             vk::Image(depth_image)}) {
        own_barrier(image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderWrite, vk::ImageLayout::eGeneral, all,
                    vk::AccessFlagBits2::eShaderRead);
    }

    const Dlss::FeatureDesc desc{w, h, ow, oh, Dlss::QualityForScale(float(ow) / float(w)), true};
    bool ok = true;
    if (!dlss->HasFeature(desc)) {
        scheduler.WaitSubmitted();
        dlss->ReleaseFeature();
        ok = dlss->CreateFeature(cmdbuf, desc);
        reset = true;
        std::printf("Order DLSS: feature %ux%u -> %ux%u quality %d %s\n", w, h, ow, oh,
                    desc.quality, ok ? "created" : "FAILED");
    }
    if (ok) {
        ok = dlss->Evaluate(cmdbuf, {
            .color = {vk::Image(color_image), *color_view, vk::Format::eR16G16B16A16Sfloat,
                      vk::ImageAspectFlagBits::eColor, w, h},
            .depth = {vk::Image(depth_image), *depth_view, vk::Format::eR32Sfloat,
                      vk::ImageAspectFlagBits::eColor, w, h},
            .motion = {vk::Image(motion_image), *motion_view, vk::Format::eR16G16Sfloat,
                       vk::ImageAspectFlagBits::eColor, w, h},
            .output = {vk::Image(output_image), *output_view, vk::Format::eR16G16B16A16Sfloat,
                       vk::ImageAspectFlagBits::eColor, ow, oh},
            .jitter_x = 0.0f,
            .jitter_y = 0.0f,
            .reset = reset,
            .frame_ms = frame_ms,
            .sharpness = 0.0f,
        });
    }
    if (!ok) {
        // Nothing was written to the game's output yet: its own resolve runs this frame.
        std::printf("Order DLSS: evaluation failed; the game's resolve takes over\n");
        failed = true;
        return false;
    }
    reset = false;

    own_barrier(vk::Image(output_image), vk::ImageLayout::eGeneral, all, rw,
                vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eTransfer,
                vk::AccessFlagBits2::eTransferRead);
    runtime.Transit(&output, vk::ImageLayout::eTransferDstOptimal,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
    runtime.FlushBarriers();
    const vk::ImageCopy region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent = {ow, oh, 1},
    };
    cmdbuf.copyImage(vk::Image(output_image), vk::ImageLayout::eTransferSrcOptimal,
                     vk::Image(output.backing->image), vk::ImageLayout::eTransferDstOptimal,
                     region);
    if (++frames == 1 || frames % 1800 == 0) {
        std::printf("Order DLSS: %llu frames reconstructed (%ux%u -> %ux%u, %u samples, velocity "
                    "scale %g)\n",
                    (unsigned long long)frames, w, h, ow, oh, color.info.num_samples,
                    velocity_scale);
    }
    return true;
}

} // namespace Vulkan
