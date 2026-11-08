// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written for TASK-027 M0b (slice 175) from the Vulkan specification and native/native_vulkan.cpp.
// Clean room: no PCSX2 sources were read. See hw_gs_vulkan_loader.hpp for why the names are renamed.

#include "hw_gs_vulkan_loader.hpp"

// Vulkan create-info and query structs are initialised with sType only (pNext defaults to null by the spec).
#pragma clang diagnostic ignored "-Wmissing-field-initializers"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gt4::hw::vulkan_loader {
namespace {

PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;

// Resolves one entry point through vkGetInstanceProcAddr; a missing name is reported, not called.
template <typename Pointer>
void resolve(Pointer &slot, VkInstance instance, const char *name, std::vector<std::string> &missing) {
  slot = reinterpret_cast<Pointer>(get_instance_proc(instance, name));
  if (!slot) missing.push_back(name);
}

} // namespace

PFN_vkVoidFunction instance_proc(VkInstance instance, const char *name) { return get_instance_proc(instance, name); }

bool load_loader(std::string &error) {
  if (get_instance_proc) return true;
  HMODULE library = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!library) {
    error = "vulkan-1.dll could not be loaded";
    return false;
  }
  get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      GetProcAddress(library, "vkGetInstanceProcAddr"));
  if (!get_instance_proc) {
    error = "vulkan-1.dll lacks vkGetInstanceProcAddr";
    return false;
  }
  gt4_hw_vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
      get_instance_proc(nullptr, "vkCreateInstance"));
  if (!gt4_hw_vkCreateInstance) {
    error = "vulkan-1.dll lacks vkCreateInstance";
    return false;
  }
  return true;
}

namespace {

// Entry points a windowless device needs: no surface, no swapchain (M4 fixtures and headless runs).
void resolve_headless(VkInstance instance, std::vector<std::string> &missing) {
  resolve(gt4_hw_vkAllocateCommandBuffers, instance, "vkAllocateCommandBuffers", missing);
  resolve(gt4_hw_vkBeginCommandBuffer, instance, "vkBeginCommandBuffer", missing);
  resolve(gt4_hw_vkCmdClearColorImage, instance, "vkCmdClearColorImage", missing);
  resolve(gt4_hw_vkCmdPipelineBarrier, instance, "vkCmdPipelineBarrier", missing);
  resolve(gt4_hw_vkCreateCommandPool, instance, "vkCreateCommandPool", missing);
  resolve(gt4_hw_vkCreateDevice, instance, "vkCreateDevice", missing);
  resolve(gt4_hw_vkCreateFence, instance, "vkCreateFence", missing);
  resolve(gt4_hw_vkCreateSemaphore, instance, "vkCreateSemaphore", missing);
  resolve(gt4_hw_vkDestroyCommandPool, instance, "vkDestroyCommandPool", missing);
  resolve(gt4_hw_vkDestroyDevice, instance, "vkDestroyDevice", missing);
  resolve(gt4_hw_vkDestroyFence, instance, "vkDestroyFence", missing);
  resolve(gt4_hw_vkDestroyInstance, instance, "vkDestroyInstance", missing);
  resolve(gt4_hw_vkDestroySemaphore, instance, "vkDestroySemaphore", missing);
  resolve(gt4_hw_vkDeviceWaitIdle, instance, "vkDeviceWaitIdle", missing);
  resolve(gt4_hw_vkEndCommandBuffer, instance, "vkEndCommandBuffer", missing);
  resolve(gt4_hw_vkEnumerateDeviceExtensionProperties, instance, "vkEnumerateDeviceExtensionProperties", missing);
  resolve(gt4_hw_vkEnumeratePhysicalDevices, instance, "vkEnumeratePhysicalDevices", missing);
  resolve(gt4_hw_vkGetDeviceQueue, instance, "vkGetDeviceQueue", missing);
  resolve(gt4_hw_vkGetPhysicalDeviceFeatures2, instance, "vkGetPhysicalDeviceFeatures2", missing);
  resolve(gt4_hw_vkGetPhysicalDeviceFormatProperties, instance, "vkGetPhysicalDeviceFormatProperties", missing);
  resolve(gt4_hw_vkGetPhysicalDeviceProperties, instance, "vkGetPhysicalDeviceProperties", missing);
  resolve(gt4_hw_vkGetPhysicalDeviceQueueFamilyProperties, instance, "vkGetPhysicalDeviceQueueFamilyProperties", missing);
  resolve(gt4_hw_vkQueueSubmit, instance, "vkQueueSubmit", missing);
  resolve(gt4_hw_vkResetCommandBuffer, instance, "vkResetCommandBuffer", missing);
  resolve(gt4_hw_vkResetFences, instance, "vkResetFences", missing);
  resolve(gt4_hw_vkWaitForFences, instance, "vkWaitForFences", missing);
}

// Surface and presentation entry points: only the windowed path needs them.
void resolve_surface(VkInstance instance, std::vector<std::string> &missing) {
  resolve(gt4_hw_vkAcquireNextImageKHR, instance, "vkAcquireNextImageKHR", missing);
  resolve(gt4_hw_vkCreateSwapchainKHR, instance, "vkCreateSwapchainKHR", missing);
  resolve(gt4_hw_vkCreateWin32SurfaceKHR, instance, "vkCreateWin32SurfaceKHR", missing);
  resolve(gt4_hw_vkDestroySurfaceKHR, instance, "vkDestroySurfaceKHR", missing);
  resolve(gt4_hw_vkDestroySwapchainKHR, instance, "vkDestroySwapchainKHR", missing);
  resolve(gt4_hw_vkGetPhysicalDeviceSurfaceCapabilitiesKHR, instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", missing);
  resolve(gt4_hw_vkGetPhysicalDeviceSurfaceFormatsKHR, instance, "vkGetPhysicalDeviceSurfaceFormatsKHR", missing);
  resolve(gt4_hw_vkGetPhysicalDeviceSurfacePresentModesKHR, instance, "vkGetPhysicalDeviceSurfacePresentModesKHR", missing);
  resolve(gt4_hw_vkGetPhysicalDeviceSurfaceSupportKHR, instance, "vkGetPhysicalDeviceSurfaceSupportKHR", missing);
  resolve(gt4_hw_vkGetSwapchainImagesKHR, instance, "vkGetSwapchainImagesKHR", missing);
  resolve(gt4_hw_vkQueuePresentKHR, instance, "vkQueuePresentKHR", missing);
}

} // namespace

bool load_instance_entry_points(VkInstance instance, std::vector<std::string> &missing) {
  resolve_headless(instance, missing);
  resolve_surface(instance, missing);
  return missing.empty();
}

bool pixel_interlock_supported(VkPhysicalDevice physical) {
  if (!gt4_hw_vkEnumerateDeviceExtensionProperties || !gt4_hw_vkGetPhysicalDeviceFeatures2) return false;
  std::uint32_t count = 0;
  gt4_hw_vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> extensions(count);
  gt4_hw_vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data());
  bool present = false;
  for (const VkExtensionProperties &extension : extensions) {
    if (std::strcmp(extension.extensionName, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME) == 0) present = true;
  }
  if (!present) return false;
  VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
  VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  features.pNext = &interlock;
  gt4_hw_vkGetPhysicalDeviceFeatures2(physical, &features);
  return interlock.fragmentShaderPixelInterlock == VK_TRUE;
}

bool load_headless_entry_points(VkInstance instance, std::vector<std::string> &missing) {
  resolve_headless(instance, missing);
  return missing.empty();
}

gt4::hw::DeviceProfile probe_device_profile(VkInstance instance, VkPhysicalDevice physical) {
  gt4::hw::DeviceProfile profile;
  if (!gt4_hw_vkGetPhysicalDeviceProperties || !gt4_hw_vkGetPhysicalDeviceFeatures2) return profile;
  VkPhysicalDeviceProperties properties{};
  gt4_hw_vkGetPhysicalDeviceProperties(physical, &properties);
  VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  gt4_hw_vkGetPhysicalDeviceFeatures2(physical, &features);
  profile.probed = true;
  profile.name = properties.deviceName;
  profile.vendor_id = properties.vendorID;
  profile.device_id = properties.deviceID;
  profile.device_type = static_cast<std::uint32_t>(properties.deviceType);
  profile.api_version = properties.apiVersion;
  profile.driver_version = properties.driverVersion;
  profile.device_class = gt4::hw::classify_device(profile.vendor_id, profile.device_type);
  profile.integrated_defaults = gt4::hw::integrated_defaults(profile.device_type);
  profile.large_points_supported = features.features.largePoints == VK_TRUE;
  profile.point_size_max = properties.limits.pointSizeRange[1];
  profile.point_size_granularity = properties.limits.pointSizeGranularity;
  profile.wide_lines_supported = features.features.wideLines == VK_TRUE;
  profile.line_width_max = properties.limits.lineWidthRange[1];
  profile.storage_image_array_dynamic_indexing_supported = features.features.shaderStorageImageArrayDynamicIndexing == VK_TRUE;

  if (const char *pretend = std::getenv("GT4_HW_PRETEND_MISSING")) {
    if (std::strstr(pretend, "large_points")) profile.large_points_supported = false;
    if (std::strstr(pretend, "dynamic_indexing")) profile.storage_image_array_dynamic_indexing_supported = false;
    std::fprintf(stderr, "[hw] GT4_HW_PRETEND_MISSING=%s: those features are treated as missing (diagnostics)\n", pretend);
  }

  if (gt4_hw_vkEnumerateDeviceExtensionProperties) {
    std::uint32_t count = 0;
    gt4_hw_vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    gt4_hw_vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data());
    for (const VkExtensionProperties &extension : extensions) {
      if (std::strcmp(extension.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0) profile.memory_budget = true;
      // TASK-184: the ordering and attachment-read extensions, by name (the feature bits are queried below, and only
      // where the extension is present: a feature struct of an extension the device lacks has no defined answer).
      if (std::strcmp(extension.extensionName, VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME) == 0) {
        profile.ordering.raster_order_ext = true;
      }
      if (std::strcmp(extension.extensionName, VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME) == 0) {
        profile.ordering.raster_order_arm = true;
      }
      if (std::strcmp(extension.extensionName, VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME) == 0) {
        profile.ordering.dynamic_rendering_local_read = true;
      }
      if (std::strcmp(extension.extensionName, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME) == 0) {
        profile.ordering.attachment_feedback_loop_layout = true;
      }
      if (std::strcmp(extension.extensionName, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME) == 0) {
        profile.ordering.attachment_feedback_loop_dynamic_state = true;
      }
      if (std::strcmp(extension.extensionName, VK_EXT_SHADER_TILE_IMAGE_EXTENSION_NAME) == 0) {
        profile.ordering.shader_tile_image = true;
      }
      if (std::strcmp(extension.extensionName, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME) == 0) {
        profile.ordering.fragment_shader_interlock = true;
      }
    }
  }
  // TASK-184: the feature bits, each struct chained only when its extension is present. The colour/depth/stencil ordering
  // bits are aliases between the EXT and the ARM spelling, so one struct answers for both.
  {
    VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT order{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT};
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR localRead{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR};
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT feedbackLayout{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
    VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT feedbackDynamic{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceShaderTileImageFeaturesEXT tileImage{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TILE_IMAGE_FEATURES_EXT};
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 chain{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkBaseOutStructure *tail = reinterpret_cast<VkBaseOutStructure *>(&chain);
    auto link = [&tail](void *structure) {
      auto *node = reinterpret_cast<VkBaseOutStructure *>(structure);
      node->pNext = nullptr;
      tail->pNext = node;
      tail = node;
    };
    if (profile.ordering.raster_order_ext || profile.ordering.raster_order_arm) link(&order);
    if (profile.ordering.dynamic_rendering_local_read) link(&localRead);
    if (profile.ordering.attachment_feedback_loop_layout) link(&feedbackLayout);
    if (profile.ordering.attachment_feedback_loop_dynamic_state) link(&feedbackDynamic);
    if (profile.ordering.shader_tile_image) link(&tileImage);
    if (profile.ordering.fragment_shader_interlock) link(&interlock);
    if (chain.pNext != nullptr) gt4_hw_vkGetPhysicalDeviceFeatures2(physical, &chain);
    profile.ordering.raster_order_color_supported = order.rasterizationOrderColorAttachmentAccess == VK_TRUE;
    profile.ordering.raster_order_depth_supported = order.rasterizationOrderDepthAttachmentAccess == VK_TRUE;
    profile.ordering.raster_order_stencil_supported = order.rasterizationOrderStencilAttachmentAccess == VK_TRUE;
    profile.ordering.dynamic_rendering_local_read =
        profile.ordering.dynamic_rendering_local_read && localRead.dynamicRenderingLocalRead == VK_TRUE;
    profile.ordering.attachment_feedback_loop_layout =
        profile.ordering.attachment_feedback_loop_layout && feedbackLayout.attachmentFeedbackLoopLayout == VK_TRUE;
    profile.ordering.attachment_feedback_loop_dynamic_state =
        profile.ordering.attachment_feedback_loop_dynamic_state && feedbackDynamic.attachmentFeedbackLoopDynamicState == VK_TRUE;
    profile.ordering.shader_tile_image_color = tileImage.shaderTileImageColorReadAccess == VK_TRUE;
    profile.ordering.shader_tile_image_depth = tileImage.shaderTileImageDepthReadAccess == VK_TRUE;
    profile.ordering.shader_tile_image_stencil = tileImage.shaderTileImageStencilReadAccess == VK_TRUE;
    profile.ordering.interlock_sample = interlock.fragmentShaderSampleInterlock == VK_TRUE;
    profile.ordering.interlock_pixel = interlock.fragmentShaderPixelInterlock == VK_TRUE;
    profile.ordering.interlock_shading_rate = interlock.fragmentShaderShadingRateInterlock == VK_TRUE;
  }
  const auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(
      instance_proc(instance, "vkGetPhysicalDeviceMemoryProperties2"));
  const auto properties1 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
      instance_proc(instance, "vkGetPhysicalDeviceMemoryProperties"));
  VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
  VkPhysicalDeviceMemoryProperties2 memory{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
  if (profile.memory_budget && properties2) {
    memory.pNext = &budget;
    properties2(physical, &memory);
  } else if (properties1) {
    profile.memory_budget = false;
    properties1(physical, &memory.memoryProperties);
  } else {
    profile.memory_budget = false;
  }
  profile.heap_count = std::min<std::uint32_t>(memory.memoryProperties.memoryHeapCount,
                                               static_cast<std::uint32_t>(profile.heaps.size()));
  for (std::uint32_t i = 0; i < profile.heap_count; ++i) {
    gt4::hw::HeapInfo &heap = profile.heaps[i];
    heap.size = memory.memoryProperties.memoryHeaps[i].size;
    heap.device_local = (memory.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
    if (profile.memory_budget) {
      heap.budget = budget.heapBudget[i];
      heap.usage = budget.heapUsage[i];
    }
  }
  // TASK-182 step 3: the unified-memory test: a memory type the GPU sees as its own and the CPU can write without a copy.
  // An APU or a phone reports one. A discrete GPU normally does not (its VRAM is device-local, its host-visible heap is
  // system memory), but one with resizable BAR may expose part of its VRAM that way - the RX 9060 XT here does - so the device
  // class, not this bit alone, decides the K rule's default (gt4::hw::k_rule_default_on). Read from the memory types, next
  // to the heaps above.
  for (std::uint32_t i = 0; i < memory.memoryProperties.memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags flags = memory.memoryProperties.memoryTypes[i].propertyFlags;
    if ((flags & (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) ==
        (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
      profile.unified_memory = true;
      break;
    }
  }
  return profile;
}

void enable_profile_features(gt4::hw::DeviceProfile &profile, VkPhysicalDeviceFeatures &features) {
  // Points are drawn gl_PointSize = scale wide (hw_vertex.vert): above 1 they need largePoints (core 2 draws them as two
  // triangles without it). The deinterlace kernel indexes its history images with a push constant (hw_deinterlace_scaled.comp),
  // which needs shaderStorageImageArrayDynamicIndexing (without it the constant-index build of the kernel runs).
  profile.large_points_enabled = profile.large_points_supported;
  profile.storage_image_array_dynamic_indexing_enabled = profile.storage_image_array_dynamic_indexing_supported;
  if (profile.large_points_enabled) features.largePoints = VK_TRUE;
  if (profile.storage_image_array_dynamic_indexing_enabled) features.shaderStorageImageArrayDynamicIndexing = VK_TRUE;
}

const char *memory_budget_extension(const gt4::hw::DeviceProfile &profile) {
  return profile.memory_budget ? VK_EXT_MEMORY_BUDGET_EXTENSION_NAME : nullptr;
}

} // namespace gt4::hw::vulkan_loader
