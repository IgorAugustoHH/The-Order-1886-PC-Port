// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
// Offscreen Vulkan test. No window, game, cache, or guest memory is opened.
#include <vulkan/vulkan.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/color_to_ms_depth_frag.h"
#include "video_core/host_shaders/ms_depth_to_color_frag.h"

static void Check(VkResult result) { assert(result == VK_SUCCESS); }
struct TestGpu {
    VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{};
    VkQueue queue{}; uint32_t family{}; VkCommandPool pool{};
    VkDescriptorSetLayout descriptors{}; VkPipelineLayout layout{};
    VkPhysicalDeviceMemoryProperties memory{};
    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
        for (uint32_t i=0;i<memory.memoryTypeCount;++i)
            if ((bits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&flags)==flags) return i;
        assert(false); return 0;
    }
    TestGpu() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_3;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo=&app;
        Check(vkCreateInstance(&ci,nullptr,&instance));
        uint32_t count=0; Check(vkEnumeratePhysicalDevices(instance,&count,nullptr)); assert(count);
        std::vector<VkPhysicalDevice> devices(count);
        Check(vkEnumeratePhysicalDevices(instance,&count,devices.data())); physical=devices.front();
        VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(physical,&props);
        std::printf("Offscreen GPU: %s\n",props.deviceName);
        vkGetPhysicalDeviceMemoryProperties(physical,&memory);
        vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data());
        while (!(families.at(family).queueFlags&VK_QUEUE_GRAPHICS_BIT)) ++family;
        const float priority=1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        f13.dynamicRendering=VK_TRUE;
        VkPhysicalDeviceFeatures features{}; features.sampleRateShading=VK_TRUE;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.pNext=&f13;
        di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi; di.pEnabledFeatures=&features;
        Check(vkCreateDevice(physical,&di,nullptr,&device)); vkGetDeviceQueue(device,family,0,&queue);
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.queueFamilyIndex=family;
        Check(vkCreateCommandPool(device,&pi,nullptr,&pool));
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo dci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dci.bindingCount=1; dci.pBindings=&binding;
        Check(vkCreateDescriptorSetLayout(device,&dci,nullptr,&descriptors));
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount=1; li.pSetLayouts=&descriptors;
        Check(vkCreatePipelineLayout(device,&li,nullptr,&layout));
    }
    ~TestGpu() {
        vkDestroyPipelineLayout(device,layout,nullptr); vkDestroyDescriptorSetLayout(device,descriptors,nullptr);
        vkDestroyCommandPool(device,pool,nullptr); vkDestroyDevice(device,nullptr); vkDestroyInstance(instance,nullptr);
    }
    VkShaderModule Shader(const uint32_t* words,size_t size) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; ci.codeSize=size; ci.pCode=words;
        VkShaderModule module{}; Check(vkCreateShaderModule(device,&ci,nullptr,&module)); return module;
    }
    VkPipeline Pipeline(VkFormat format,uint32_t samples,bool depth) {
        auto vs=Shader(FS_TRI_VERT,sizeof(FS_TRI_VERT));
        auto fs=depth ? Shader(COLOR_TO_MS_DEPTH_FRAG,sizeof(COLOR_TO_MS_DEPTH_FRAG))
                      : Shader(MS_DEPTH_TO_COLOR_FRAG,sizeof(MS_DEPTH_TO_COLOR_FRAG));
        std::array<VkPipelineShaderStageCreateInfo,2> stages{};
        for (auto& s:stages) { s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; s.pName="main"; }
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; stages[0].module=vs;
        stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module=fs;
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount=vp.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.lineWidth=1; rs.polygonMode=VK_POLYGON_MODE_FILL;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples=static_cast<VkSampleCountFlagBits>(samples);
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable=ds.depthWriteEnable=depth; ds.depthCompareOp=VK_COMPARE_OP_ALWAYS;
        VkPipelineColorBlendAttachmentState ba{}; ba.colorWriteMask=15;
        VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        bs.attachmentCount=depth ? 0 : 1; bs.pAttachments=&ba;
        std::array dynamic{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount=2; dy.pDynamicStates=dynamic.data();
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.depthAttachmentFormat=depth ? format : VK_FORMAT_UNDEFINED;
        ri.colorAttachmentCount=depth ? 0 : 1; ri.pColorAttachmentFormats=&format;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.pNext=&ri; ci.stageCount=2; ci.pStages=stages.data(); ci.pVertexInputState=&vi;
        ci.pInputAssemblyState=&ia; ci.pViewportState=&vp; ci.pRasterizationState=&rs;
        ci.pMultisampleState=&ms; ci.pDepthStencilState=&ds; ci.pColorBlendState=&bs;
        ci.pDynamicState=&dy; ci.layout=layout;
        VkPipeline result{}; Check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&ci,nullptr,&result));
        vkDestroyShaderModule(device,vs,nullptr); vkDestroyShaderModule(device,fs,nullptr); return result;
    }
};
struct Image {
    TestGpu& gpu; VkImage image{}; VkDeviceMemory memory{}; VkImageView view{}; VkImageAspectFlags aspect;
    Image(TestGpu& g,VkFormat format,uint32_t samples,bool depth):gpu(g),aspect(depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.imageType=VK_IMAGE_TYPE_2D;
        ci.format=format; ci.extent={8,8,1}; ci.mipLevels=ci.arrayLayers=1;
        ci.samples=static_cast<VkSampleCountFlagBits>(samples); ci.tiling=VK_IMAGE_TILING_OPTIMAL;
        ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|
            (depth?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        Check(vkCreateImage(gpu.device,&ci,nullptr,&image));
        VkMemoryRequirements req{}; vkGetImageMemoryRequirements(gpu.device,image,&req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size;
        ai.memoryTypeIndex=gpu.MemoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(vkAllocateMemory(gpu.device,&ai,nullptr,&memory)); Check(vkBindImageMemory(gpu.device,image,memory,0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=image;
        vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=format; vi.subresourceRange={aspect,0,1,0,1};
        Check(vkCreateImageView(gpu.device,&vi,nullptr,&view));
    }
    ~Image(){vkDestroyImageView(gpu.device,view,nullptr);vkDestroyImage(gpu.device,image,nullptr);vkFreeMemory(gpu.device,memory,nullptr);}
    void Barrier(VkCommandBuffer cmd,VkImageLayout old,VkImageLayout next,VkAccessFlags src,VkAccessFlags dst) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.srcAccessMask=src;b.dstAccessMask=dst;
        b.oldLayout=old;b.newLayout=next;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.image=image;b.subresourceRange={aspect,0,1,0,1};
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
    }
};
static void Roundtrip(TestGpu& gpu,uint32_t samples) {
    const VkFormat format=samples==2 ? VK_FORMAT_R32G32_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT;
    Image input(gpu,VK_FORMAT_R32G32B32A32_SFLOAT,1,false),depth(gpu,VK_FORMAT_D32_SFLOAT,samples,true),output(gpu,format,1,false);
    auto forward=gpu.Pipeline(VK_FORMAT_D32_SFLOAT,samples,true),reverse=gpu.Pipeline(format,1,false);
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,2};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pci.maxSets=2;pci.poolSizeCount=1;pci.pPoolSizes=&ps;
    VkDescriptorPool pool{};Check(vkCreateDescriptorPool(gpu.device,&pci,nullptr,&pool));
    std::array layouts{gpu.descriptors,gpu.descriptors}; std::array<VkDescriptorSet,2> sets{};
    VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};sai.descriptorPool=pool;sai.descriptorSetCount=2;sai.pSetLayouts=layouts.data();
    Check(vkAllocateDescriptorSets(gpu.device,&sai,sets.data()));
    for (uint32_t i=0;i<2;++i) {
        VkDescriptorImageInfo ii{VK_NULL_HANDLE,i==0?input.view:depth.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=sets[i];w.descriptorCount=1;
        w.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;w.pImageInfo=&ii;vkUpdateDescriptorSets(gpu.device,1,&w,0,nullptr);
    }
    VkBuffer buffer{};VkDeviceMemory memory{};
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=8*8*samples*sizeof(float);bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    Check(vkCreateBuffer(gpu.device,&bi,nullptr,&buffer));VkMemoryRequirements req{};vkGetBufferMemoryRequirements(gpu.device,buffer,&req);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=req.size;
    ma.memoryTypeIndex=gpu.MemoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Check(vkAllocateMemory(gpu.device,&ma,nullptr,&memory));Check(vkBindBufferMemory(gpu.device,buffer,memory,0));
    VkCommandBuffer cmd{};VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool=gpu.pool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;Check(vkAllocateCommandBuffers(gpu.device,&cai,&cmd));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};Check(vkBeginCommandBuffer(cmd,&begin));
    input.Barrier(cmd,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
    const std::array<float,4> values{0.125f,0.25f,0.5f,0.75f};VkClearColorValue clear{};std::memcpy(clear.float32,values.data(),sizeof(values));
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(cmd,input.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);
    input.Barrier(cmd,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
    depth.Barrier(cmd,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,0,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    output.Barrier(cmd,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    for (uint32_t step=0;step<2;++step) {
        VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};a.imageView=step==0?depth.view:output.view;
        a.imageLayout=step==0?VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;a.clearValue.depthStencil.depth=1;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};ri.renderArea={{0,0},{8,8}};ri.layerCount=1;
        if (step==0) ri.pDepthAttachment=&a; else {ri.colorAttachmentCount=1;ri.pColorAttachments=&a;}
        vkCmdBeginRendering(cmd,&ri);vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,step==0?forward:reverse);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,gpu.layout,0,1,&sets[step],0,nullptr);
        VkViewport vp{0,0,8,8,0,1};VkRect2D sc{{0,0},{8,8}};vkCmdSetViewport(cmd,0,1,&vp);vkCmdSetScissor(cmd,0,1,&sc);
        vkCmdDraw(cmd,3,1,0,0);vkCmdEndRendering(cmd);
        if (step==0) depth.Barrier(cmd,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
    }
    output.Barrier(cmd,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={8,8,1};
    vkCmdCopyImageToBuffer(cmd,output.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    Check(vkEndCommandBuffer(cmd));VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cmd;
    Check(vkQueueSubmit(gpu.queue,1,&si,VK_NULL_HANDLE));Check(vkQueueWaitIdle(gpu.queue));
    void* mapped{};Check(vkMapMemory(gpu.device,memory,0,VK_WHOLE_SIZE,0,&mapped));
    for (uint32_t pixel=0;pixel<64;++pixel) for (uint32_t sample=0;sample<samples;++sample)
        assert(std::memcmp(static_cast<float*>(mapped)+pixel*samples+sample,&values[sample],sizeof(float))==0);
    vkUnmapMemory(gpu.device,memory);std::printf("Depth alias GPU: %ux samples -> distinct channels, all 64 pixels exact PASS\n",samples);
    vkFreeCommandBuffers(gpu.device,gpu.pool,1,&cmd);vkDestroyDescriptorPool(gpu.device,pool,nullptr);
    vkDestroyPipeline(gpu.device,forward,nullptr);vkDestroyPipeline(gpu.device,reverse,nullptr);
    vkDestroyBuffer(gpu.device,buffer,nullptr);vkFreeMemory(gpu.device,memory,nullptr);
}
int main(){TestGpu gpu;Roundtrip(gpu,2);Roundtrip(gpu,4);}
