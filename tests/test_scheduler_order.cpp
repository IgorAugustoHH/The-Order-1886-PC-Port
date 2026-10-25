// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
// Production Scheduler + Vulkan on the actual device, no window, game or guest dump.
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "bb_order_optimization.h"
#include "video_core/texture_cache/image.h"
#include <vk_mem_alloc.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <semaphore>
#include <thread>

// Vulkan-Hpp's dispatch base changes layout with NDEBUG. Match the production library
// while retaining mandatory test checks (including calls with side effects).
#undef assert
#define assert(condition) do { if (!(condition)) { \
    std::fprintf(stderr,"Test failed: %s at line %d\n",#condition,__LINE__); \
    std::abort(); } } while (0)

static void Env(const char* key, const char* value) {
#ifdef _WIN32
    assert(_putenv_s(key,value) == 0);
#else
    assert(setenv(key,value,1) == 0);
#endif
}

int main(int argc, char** argv) {
    assert(argc == 3);
    Env("BB_ASYNC_SUBMIT", argv[1]); Env("BB_VK_RECORD_THREADS", argv[2]);
    Env("BB_VK_RECORD_THREAD", "1"); Env("BB_VK_SEGMENT_KB", "1");
    Env("BB_GPU_PROFILE", "0");
    Env("BB_VMA_BLOCK_MB", "64"); Env("BB_VMA_DEDICATED_MB", "4");
    Env("VK_LOADER_LAYERS_DISABLE", "~implicit~");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    using namespace Vulkan;
    Instance instance(0, false);
    vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic dispatch;
    dispatch.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    dispatch.init(instance.GetInstance()); dispatch.init(instance.GetDevice());
    Scheduler scheduler(instance, true);
    VkBuffer buffer{}; VmaAllocation allocation{}; VmaAllocationInfo ai{};
    const VkBufferCreateInfo bi{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=100*512*sizeof(unsigned), .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo ac{.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT |
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT, .usage=VMA_MEMORY_USAGE_AUTO,
        .requiredFlags=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    assert(vmaCreateBuffer(instance.GetAllocator(), &bi, &ac, &buffer, &allocation, &ai)==VK_SUCCESS);
    std::atomic<unsigned> copies{0}, binds{0};
    for (unsigned batch=0; batch<100; ++batch) {
        // Force multiple independently recorded segments. Barriers retain stream order.
        for (unsigned i=0; i<512; ++i) {
            const unsigned value=batch*512+i;
            scheduler.Record([&, i, value](vk::CommandBuffer cmd) {
                dispatch.vkCmdUpdateBuffer(cmd, buffer, value*sizeof(unsigned), sizeof(unsigned), &value);
            });
            scheduler.KickRecording();
        }
        scheduler.RecordHostCopy([&,batch] {
            assert(copies.load() == batch);
            copies.store(batch+1);
        });
        SubmitInfo info{};
        info.before_submit.emplace_back([&,batch] {
            assert(copies.load() >= batch+1);
            assert(binds.load() == batch);
            binds.store(batch+1);
        });
        scheduler.Flush(info);
    }
    scheduler.Record([&](vk::CommandBuffer cmd) {
        const VkMemoryBarrier barrier{.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask=VK_ACCESS_HOST_READ_BIT};
        dispatch.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 1, &barrier, 0, nullptr, 0, nullptr);
    });
    scheduler.Finish(); scheduler.SyncRecording();
    assert(copies == 100 && binds == 100);
    auto* words=static_cast<unsigned*>(ai.pMappedData);
    for (unsigned i=0; i<100*512; ++i) assert(words[i] == i);
    if (std::atoi(argv[2])>1) assert(Scheduler::recorded_segments > Scheduler::recorded_submissions);
    assert(BbOrderOpt::batch_peak <= 8);

    // The Order's descriptor tables can exceed a chunk. Native recording must still be valid.
    std::vector<u32> descriptors(RecordChunk::Capacity/sizeof(u32)+100, 7);
    const auto data = scheduler.RecordData<u32>(descriptors);
    assert(data.size()==descriptors.size() && !scheduler.IsRecordingDeferred());
    scheduler.Record([&,data](vk::CommandBuffer cmd) {
        assert(data.back()==7);
        const unsigned final=123456;
        dispatch.vkCmdUpdateBuffer(cmd, buffer, 0, sizeof(final), &final);
    });
    scheduler.Finish(); scheduler.SyncRecording();
    assert(words[0]==123456);

    // Exercise production image allocation + ownership transfer, including dedicated images.
    const vk::ImageCreateInfo ci{.imageType=vk::ImageType::e2D,
        .format=vk::Format::eR8G8B8A8Uint, .extent={1024,1024,1}, .mipLevels=1,
        .arrayLayers=1, .samples=vk::SampleCountFlagBits::e1, .tiling=vk::ImageTiling::eOptimal,
        .usage=vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc};
    VideoCore::UniqueImage original(instance.GetDevice(),instance.GetAllocator());
    original.Create(ci);
    const auto bytes=original.size_bytes;
    VideoCore::UniqueImage image(std::move(original));
    assert(image.device==instance.GetDevice() && image.size_bytes==bytes && !original);
    VmaAllocationInfo2 image_alloc{};
    vmaGetAllocationInfo2(instance.GetAllocator(),image.allocation,&image_alloc);
    assert(image_alloc.dedicatedMemory);
    const vk::ImageViewCreateInfo vi{.image=image.image, .viewType=vk::ImageViewType::e2D,
        .format=ci.format, .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
    auto [view_result,view]=instance.GetDevice().createImageView(vi,nullptr,dispatch);
    assert(view_result==vk::Result::eSuccess);
    scheduler.Record([&,handle=image.image](vk::CommandBuffer cmd) {
        const VkImageMemoryBarrier barrier{.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=handle,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        dispatch.vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    });
    RenderState state{};
    state.width=state.height=1024;state.num_layers=state.num_color_attachments=1;
    state.color_attachments[0]={view,vk::ImageLayout::eColorAttachmentOptimal,{1,2,3,4},1};
    scheduler.BeginRendering(state);
    scheduler.Record([&](vk::CommandBuffer cmd) {
        const VkClearAttachment attachment{.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,
            .colorAttachment=0,.clearValue={.color={.uint32={77,78,79,80}}}};
        const VkClearRect rect{.rect={{0,0},{1024,1024}},.baseArrayLayer=0,.layerCount=1};
        dispatch.vkCmdClearAttachments(cmd,1,&attachment,1,&rect);
    });
    for (unsigned i=0;i<200;++i) {
        scheduler.BeginRendering(state);
        scheduler.Record([&](vk::CommandBuffer cmd){dispatch.vkCmdSetLineWidth(cmd,1.0f);});
        scheduler.KickRecording();
    }
    scheduler.EndRendering();
    scheduler.Record([&,handle=image.image](vk::CommandBuffer cmd) {
        const VkImageMemoryBarrier barrier{.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=handle,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        dispatch.vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        const VkBufferImageCopy region{.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
            .imageExtent={1,1,1}};
        dispatch.vkCmdCopyImageToBuffer(cmd,handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&region);
        const VkMemoryBarrier host{.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
        dispatch.vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
            0,1,&host,0,nullptr,0,nullptr);
    });
    scheduler.Finish();scheduler.SyncRecording();
    const auto* pixel=static_cast<const unsigned char*>(ai.pMappedData);
    assert(pixel[0]==77 && pixel[1]==78 && pixel[2]==79 && pixel[3]==80);
    instance.GetDevice().destroyImageView(view,nullptr,dispatch);
    vmaDestroyBuffer(instance.GetAllocator(),buffer,allocation);
    std::printf("Scheduler: async=%s threads=%s; 51,200 GPU writes, ordered copies/binds, "
                "bounded batches, oversized descriptors, render-pass LOAD and dedicated VMA passed\n",argv[1],argv[2]);
}
