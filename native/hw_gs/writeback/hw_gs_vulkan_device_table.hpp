// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from the Vulkan specification, clean room (decision 0069).
//
// Device-level and physical-device-level entry points used by the HWR write-back path (TASK-027 M4).
// Same discipline as hw_gs_vulkan_loader.hpp: granite-volk.lib (linked for paraLLEl-GS) defines these
// names as data, so every call goes through a pointer resolved at run time. Names are renamed to
// gt4_hwd_* by the macros below. Include hw_gs_vulkan_loader.hpp first (it includes vulkan.h).
#pragma once

#include <string>
#include <vector>

#include "hw_gs_vulkan_loader.hpp"

#define vkAllocateDescriptorSets gt4_hwd_vkAllocateDescriptorSets
#define vkAllocateMemory gt4_hwd_vkAllocateMemory
#define vkBindBufferMemory gt4_hwd_vkBindBufferMemory
#define vkBindImageMemory gt4_hwd_vkBindImageMemory
#define vkCmdBeginRenderPass gt4_hwd_vkCmdBeginRenderPass
#define vkCmdBindDescriptorSets gt4_hwd_vkCmdBindDescriptorSets
#define vkCmdBindPipeline gt4_hwd_vkCmdBindPipeline
#define vkCmdBindVertexBuffers gt4_hwd_vkCmdBindVertexBuffers
#define vkCmdBlitImage gt4_hwd_vkCmdBlitImage
#define vkCmdClearColorImage gt4_hwd_vkCmdClearColorImage
#define vkCmdCopyBufferToImage gt4_hwd_vkCmdCopyBufferToImage
#define vkCmdCopyImage gt4_hwd_vkCmdCopyImage
#define vkCreateComputePipelines gt4_hwd_vkCreateComputePipelines
#define vkCreatePipelineCache gt4_hwd_vkCreatePipelineCache
#define vkDestroyPipelineCache gt4_hwd_vkDestroyPipelineCache
#define vkGetPipelineCacheData gt4_hwd_vkGetPipelineCacheData
#define vkCmdDispatch gt4_hwd_vkCmdDispatch
#define vkCmdCopyBuffer gt4_hwd_vkCmdCopyBuffer
#define vkCmdCopyImageToBuffer gt4_hwd_vkCmdCopyImageToBuffer
#define vkCmdDraw gt4_hwd_vkCmdDraw
#define vkCmdEndRenderPass gt4_hwd_vkCmdEndRenderPass
#define vkCmdPushConstants gt4_hwd_vkCmdPushConstants
#define vkCmdSetScissor gt4_hwd_vkCmdSetScissor
#define vkCmdSetViewport gt4_hwd_vkCmdSetViewport
#define vkCreateBuffer gt4_hwd_vkCreateBuffer
#define vkCreateDescriptorPool gt4_hwd_vkCreateDescriptorPool
#define vkCreateDescriptorSetLayout gt4_hwd_vkCreateDescriptorSetLayout
#define vkCreateFramebuffer gt4_hwd_vkCreateFramebuffer
#define vkCreateGraphicsPipelines gt4_hwd_vkCreateGraphicsPipelines
#define vkCreateImage gt4_hwd_vkCreateImage
#define vkCreateImageView gt4_hwd_vkCreateImageView
#define vkCreatePipelineLayout gt4_hwd_vkCreatePipelineLayout
#define vkCreateRenderPass gt4_hwd_vkCreateRenderPass
#define vkCreateSampler gt4_hwd_vkCreateSampler
#define vkDestroySampler gt4_hwd_vkDestroySampler
#define vkCreateShaderModule gt4_hwd_vkCreateShaderModule
#define vkDestroyBuffer gt4_hwd_vkDestroyBuffer
#define vkDestroyDescriptorPool gt4_hwd_vkDestroyDescriptorPool
#define vkDestroyDescriptorSetLayout gt4_hwd_vkDestroyDescriptorSetLayout
#define vkDestroyFramebuffer gt4_hwd_vkDestroyFramebuffer
#define vkDestroyImage gt4_hwd_vkDestroyImage
#define vkDestroyImageView gt4_hwd_vkDestroyImageView
#define vkDestroyPipeline gt4_hwd_vkDestroyPipeline
#define vkDestroyPipelineLayout gt4_hwd_vkDestroyPipelineLayout
#define vkDestroyRenderPass gt4_hwd_vkDestroyRenderPass
#define vkDestroyShaderModule gt4_hwd_vkDestroyShaderModule
#define vkFreeMemory gt4_hwd_vkFreeMemory
#define vkGetBufferMemoryRequirements gt4_hwd_vkGetBufferMemoryRequirements
#define vkGetImageMemoryRequirements gt4_hwd_vkGetImageMemoryRequirements
#define vkGetPhysicalDeviceMemoryProperties gt4_hwd_vkGetPhysicalDeviceMemoryProperties
#define vkMapMemory gt4_hwd_vkMapMemory
#define vkUnmapMemory gt4_hwd_vkUnmapMemory
#define vkUpdateDescriptorSets gt4_hwd_vkUpdateDescriptorSets
#define vkResetDescriptorPool gt4_hwd_vkResetDescriptorPool
#define vkCreateQueryPool gt4_hwd_vkCreateQueryPool
#define vkDestroyQueryPool gt4_hwd_vkDestroyQueryPool
#define vkCmdResetQueryPool gt4_hwd_vkCmdResetQueryPool
#define vkCmdWriteTimestamp gt4_hwd_vkCmdWriteTimestamp
#define vkGetQueryPoolResults gt4_hwd_vkGetQueryPoolResults
#define vkCmdBeginQuery gt4_hwd_vkCmdBeginQuery
#define vkCmdEndQuery gt4_hwd_vkCmdEndQuery

inline PFN_vkAllocateDescriptorSets gt4_hwd_vkAllocateDescriptorSets = nullptr;
inline PFN_vkAllocateMemory gt4_hwd_vkAllocateMemory = nullptr;
inline PFN_vkBindBufferMemory gt4_hwd_vkBindBufferMemory = nullptr;
inline PFN_vkBindImageMemory gt4_hwd_vkBindImageMemory = nullptr;
inline PFN_vkCmdBeginRenderPass gt4_hwd_vkCmdBeginRenderPass = nullptr;
inline PFN_vkCreateSampler gt4_hwd_vkCreateSampler = nullptr;
inline PFN_vkDestroySampler gt4_hwd_vkDestroySampler = nullptr;
inline PFN_vkCmdBindDescriptorSets gt4_hwd_vkCmdBindDescriptorSets = nullptr;
inline PFN_vkCmdBindPipeline gt4_hwd_vkCmdBindPipeline = nullptr;
inline PFN_vkCmdBindVertexBuffers gt4_hwd_vkCmdBindVertexBuffers = nullptr;
inline PFN_vkCmdBlitImage gt4_hwd_vkCmdBlitImage = nullptr;
inline PFN_vkCmdClearColorImage gt4_hwd_vkCmdClearColorImage = nullptr;
inline PFN_vkCmdCopyBufferToImage gt4_hwd_vkCmdCopyBufferToImage = nullptr;
inline PFN_vkCmdCopyImage gt4_hwd_vkCmdCopyImage = nullptr;
inline PFN_vkCreateComputePipelines gt4_hwd_vkCreateComputePipelines = nullptr;
inline PFN_vkCreatePipelineCache gt4_hwd_vkCreatePipelineCache = nullptr;
inline PFN_vkDestroyPipelineCache gt4_hwd_vkDestroyPipelineCache = nullptr;
inline PFN_vkGetPipelineCacheData gt4_hwd_vkGetPipelineCacheData = nullptr;
inline PFN_vkCmdDispatch gt4_hwd_vkCmdDispatch = nullptr;
inline PFN_vkCmdCopyBuffer gt4_hwd_vkCmdCopyBuffer = nullptr;
inline PFN_vkCmdCopyImageToBuffer gt4_hwd_vkCmdCopyImageToBuffer = nullptr;
inline PFN_vkCmdDraw gt4_hwd_vkCmdDraw = nullptr;
inline PFN_vkCmdEndRenderPass gt4_hwd_vkCmdEndRenderPass = nullptr;
inline PFN_vkCmdPushConstants gt4_hwd_vkCmdPushConstants = nullptr;
inline PFN_vkCmdSetScissor gt4_hwd_vkCmdSetScissor = nullptr;
inline PFN_vkCmdSetViewport gt4_hwd_vkCmdSetViewport = nullptr;
inline PFN_vkCreateBuffer gt4_hwd_vkCreateBuffer = nullptr;
inline PFN_vkCreateDescriptorPool gt4_hwd_vkCreateDescriptorPool = nullptr;
inline PFN_vkCreateDescriptorSetLayout gt4_hwd_vkCreateDescriptorSetLayout = nullptr;
inline PFN_vkCreateFramebuffer gt4_hwd_vkCreateFramebuffer = nullptr;
inline PFN_vkCreateGraphicsPipelines gt4_hwd_vkCreateGraphicsPipelines = nullptr;
inline PFN_vkCreateImage gt4_hwd_vkCreateImage = nullptr;
inline PFN_vkCreateImageView gt4_hwd_vkCreateImageView = nullptr;
inline PFN_vkCreatePipelineLayout gt4_hwd_vkCreatePipelineLayout = nullptr;
inline PFN_vkCreateRenderPass gt4_hwd_vkCreateRenderPass = nullptr;
inline PFN_vkCreateShaderModule gt4_hwd_vkCreateShaderModule = nullptr;
inline PFN_vkDestroyBuffer gt4_hwd_vkDestroyBuffer = nullptr;
inline PFN_vkDestroyDescriptorPool gt4_hwd_vkDestroyDescriptorPool = nullptr;
inline PFN_vkDestroyDescriptorSetLayout gt4_hwd_vkDestroyDescriptorSetLayout = nullptr;
inline PFN_vkDestroyFramebuffer gt4_hwd_vkDestroyFramebuffer = nullptr;
inline PFN_vkDestroyImage gt4_hwd_vkDestroyImage = nullptr;
inline PFN_vkDestroyImageView gt4_hwd_vkDestroyImageView = nullptr;
inline PFN_vkDestroyPipeline gt4_hwd_vkDestroyPipeline = nullptr;
inline PFN_vkDestroyPipelineLayout gt4_hwd_vkDestroyPipelineLayout = nullptr;
inline PFN_vkDestroyRenderPass gt4_hwd_vkDestroyRenderPass = nullptr;
inline PFN_vkDestroyShaderModule gt4_hwd_vkDestroyShaderModule = nullptr;
inline PFN_vkFreeMemory gt4_hwd_vkFreeMemory = nullptr;
inline PFN_vkGetBufferMemoryRequirements gt4_hwd_vkGetBufferMemoryRequirements = nullptr;
inline PFN_vkGetImageMemoryRequirements gt4_hwd_vkGetImageMemoryRequirements = nullptr;
inline PFN_vkGetPhysicalDeviceMemoryProperties gt4_hwd_vkGetPhysicalDeviceMemoryProperties = nullptr;
inline PFN_vkMapMemory gt4_hwd_vkMapMemory = nullptr;
inline PFN_vkUnmapMemory gt4_hwd_vkUnmapMemory = nullptr;
inline PFN_vkUpdateDescriptorSets gt4_hwd_vkUpdateDescriptorSets = nullptr;
inline PFN_vkResetDescriptorPool gt4_hwd_vkResetDescriptorPool = nullptr;  // CORE2
inline PFN_vkCreateQueryPool gt4_hwd_vkCreateQueryPool = nullptr;
inline PFN_vkDestroyQueryPool gt4_hwd_vkDestroyQueryPool = nullptr;
inline PFN_vkCmdResetQueryPool gt4_hwd_vkCmdResetQueryPool = nullptr;
inline PFN_vkCmdWriteTimestamp gt4_hwd_vkCmdWriteTimestamp = nullptr;
inline PFN_vkGetQueryPoolResults gt4_hwd_vkGetQueryPoolResults = nullptr;
inline PFN_vkCmdBeginQuery gt4_hwd_vkCmdBeginQuery = nullptr;  // CORE2 B4: pipeline statistics
inline PFN_vkCmdEndQuery gt4_hwd_vkCmdEndQuery = nullptr;

namespace gt4::hw::vulkan_device {

// Resolves every pointer above through vkGetInstanceProcAddr (taken from the loaded vulkan-1.dll),
// which also resolves device-level names. Missing names are listed; returns true when none is missing.
bool load_device_entry_points(VkInstance instance, std::vector<std::string> &missing);

} // namespace gt4::hw::vulkan_device
