// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from the Vulkan specification, clean room (decision 0069). See the header for the naming rule.
#include "hw_gs_vulkan_device_table.hpp"

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace gt4::hw::vulkan_device {
namespace {

template <typename Pointer>
void resolve(Pointer &slot, PFN_vkGetInstanceProcAddr proc, VkInstance instance, const char *name,
             std::vector<std::string> &missing) {
  slot = reinterpret_cast<Pointer>(proc(instance, name));
  if (!slot) missing.push_back(name);
}

} // namespace

bool load_device_entry_points(VkInstance instance, std::vector<std::string> &missing) {
#ifdef _WIN32
  HMODULE library = GetModuleHandleW(L"vulkan-1.dll");
  if (!library) {
    missing.push_back("vulkan-1.dll (not loaded)");
    return false;
  }
  auto proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(library, "vkGetInstanceProcAddr"));
#else
  // TASK-180: libvulkan.so is already open (hw_gs_vulkan_loader.cpp); RTLD_NOLOAD only returns that handle.
  void *library = dlopen("libvulkan.so", RTLD_NOW | RTLD_NOLOAD);
  if (!library) {
    missing.push_back("libvulkan.so (not loaded)");
    return false;
  }
  auto proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
#endif
  if (!proc) {
    missing.push_back("vkGetInstanceProcAddr");
    return false;
  }
  resolve(gt4_hwd_vkAllocateDescriptorSets, proc, instance, "vkAllocateDescriptorSets", missing);
  resolve(gt4_hwd_vkAllocateMemory, proc, instance, "vkAllocateMemory", missing);
  resolve(gt4_hwd_vkBindBufferMemory, proc, instance, "vkBindBufferMemory", missing);
  resolve(gt4_hwd_vkBindImageMemory, proc, instance, "vkBindImageMemory", missing);
  resolve(gt4_hwd_vkCmdBeginRenderPass, proc, instance, "vkCmdBeginRenderPass", missing);
  resolve(gt4_hwd_vkCmdBindDescriptorSets, proc, instance, "vkCmdBindDescriptorSets", missing);
  resolve(gt4_hwd_vkCmdBindPipeline, proc, instance, "vkCmdBindPipeline", missing);
  resolve(gt4_hwd_vkCmdBindVertexBuffers, proc, instance, "vkCmdBindVertexBuffers", missing);
  resolve(gt4_hwd_vkCmdBlitImage, proc, instance, "vkCmdBlitImage", missing);
  resolve(gt4_hwd_vkCmdClearColorImage, proc, instance, "vkCmdClearColorImage", missing);
  resolve(gt4_hwd_vkCmdCopyBufferToImage, proc, instance, "vkCmdCopyBufferToImage", missing);
  resolve(gt4_hwd_vkCmdCopyImage, proc, instance, "vkCmdCopyImage", missing);
  resolve(gt4_hwd_vkCreateComputePipelines, proc, instance, "vkCreateComputePipelines", missing);
  resolve(gt4_hwd_vkCreatePipelineCache, proc, instance, "vkCreatePipelineCache", missing);
  resolve(gt4_hwd_vkDestroyPipelineCache, proc, instance, "vkDestroyPipelineCache", missing);
  resolve(gt4_hwd_vkGetPipelineCacheData, proc, instance, "vkGetPipelineCacheData", missing);
  resolve(gt4_hwd_vkCmdDispatch, proc, instance, "vkCmdDispatch", missing);
  resolve(gt4_hwd_vkCmdCopyBuffer, proc, instance, "vkCmdCopyBuffer", missing);
  resolve(gt4_hwd_vkCreateQueryPool, proc, instance, "vkCreateQueryPool", missing);
  resolve(gt4_hwd_vkDestroyQueryPool, proc, instance, "vkDestroyQueryPool", missing);
  resolve(gt4_hwd_vkCmdResetQueryPool, proc, instance, "vkCmdResetQueryPool", missing);
  resolve(gt4_hwd_vkCmdWriteTimestamp, proc, instance, "vkCmdWriteTimestamp", missing);
  resolve(gt4_hwd_vkGetQueryPoolResults, proc, instance, "vkGetQueryPoolResults", missing);
  resolve(gt4_hwd_vkCmdBeginQuery, proc, instance, "vkCmdBeginQuery", missing);
  resolve(gt4_hwd_vkCmdEndQuery, proc, instance, "vkCmdEndQuery", missing);
  resolve(gt4_hwd_vkCmdCopyImageToBuffer, proc, instance, "vkCmdCopyImageToBuffer", missing);
  resolve(gt4_hwd_vkCmdDraw, proc, instance, "vkCmdDraw", missing);
  resolve(gt4_hwd_vkCmdEndRenderPass, proc, instance, "vkCmdEndRenderPass", missing);
  resolve(gt4_hwd_vkCmdPushConstants, proc, instance, "vkCmdPushConstants", missing);
  resolve(gt4_hwd_vkCmdSetScissor, proc, instance, "vkCmdSetScissor", missing);
  resolve(gt4_hwd_vkCmdSetViewport, proc, instance, "vkCmdSetViewport", missing);
  resolve(gt4_hwd_vkCreateBuffer, proc, instance, "vkCreateBuffer", missing);
  resolve(gt4_hwd_vkCreateDescriptorPool, proc, instance, "vkCreateDescriptorPool", missing);
  resolve(gt4_hwd_vkCreateDescriptorSetLayout, proc, instance, "vkCreateDescriptorSetLayout", missing);
  resolve(gt4_hwd_vkCreateFramebuffer, proc, instance, "vkCreateFramebuffer", missing);
  resolve(gt4_hwd_vkCreateGraphicsPipelines, proc, instance, "vkCreateGraphicsPipelines", missing);
  resolve(gt4_hwd_vkCreateImage, proc, instance, "vkCreateImage", missing);
  resolve(gt4_hwd_vkCreateImageView, proc, instance, "vkCreateImageView", missing);
  resolve(gt4_hwd_vkCreatePipelineLayout, proc, instance, "vkCreatePipelineLayout", missing);
  resolve(gt4_hwd_vkCreateRenderPass, proc, instance, "vkCreateRenderPass", missing);
  resolve(gt4_hwd_vkCreateSampler, proc, instance, "vkCreateSampler", missing);
  resolve(gt4_hwd_vkDestroySampler, proc, instance, "vkDestroySampler", missing);
  resolve(gt4_hwd_vkCreateShaderModule, proc, instance, "vkCreateShaderModule", missing);
  resolve(gt4_hwd_vkDestroyBuffer, proc, instance, "vkDestroyBuffer", missing);
  resolve(gt4_hwd_vkDestroyDescriptorPool, proc, instance, "vkDestroyDescriptorPool", missing);
  resolve(gt4_hwd_vkDestroyDescriptorSetLayout, proc, instance, "vkDestroyDescriptorSetLayout", missing);
  resolve(gt4_hwd_vkDestroyFramebuffer, proc, instance, "vkDestroyFramebuffer", missing);
  resolve(gt4_hwd_vkDestroyImage, proc, instance, "vkDestroyImage", missing);
  resolve(gt4_hwd_vkDestroyImageView, proc, instance, "vkDestroyImageView", missing);
  resolve(gt4_hwd_vkDestroyPipeline, proc, instance, "vkDestroyPipeline", missing);
  resolve(gt4_hwd_vkDestroyPipelineLayout, proc, instance, "vkDestroyPipelineLayout", missing);
  resolve(gt4_hwd_vkDestroyRenderPass, proc, instance, "vkDestroyRenderPass", missing);
  resolve(gt4_hwd_vkDestroyShaderModule, proc, instance, "vkDestroyShaderModule", missing);
  resolve(gt4_hwd_vkFreeMemory, proc, instance, "vkFreeMemory", missing);
  resolve(gt4_hwd_vkGetBufferMemoryRequirements, proc, instance, "vkGetBufferMemoryRequirements", missing);
  resolve(gt4_hwd_vkGetImageMemoryRequirements, proc, instance, "vkGetImageMemoryRequirements", missing);
  resolve(gt4_hwd_vkGetPhysicalDeviceMemoryProperties, proc, instance, "vkGetPhysicalDeviceMemoryProperties",
          missing);
  resolve(gt4_hwd_vkMapMemory, proc, instance, "vkMapMemory", missing);
  resolve(gt4_hwd_vkUnmapMemory, proc, instance, "vkUnmapMemory", missing);
  resolve(gt4_hwd_vkUpdateDescriptorSets, proc, instance, "vkUpdateDescriptorSets", missing);
  resolve(gt4_hwd_vkResetDescriptorPool, proc, instance, "vkResetDescriptorPool", missing);
  return missing.empty();
}

} // namespace gt4::hw::vulkan_device
