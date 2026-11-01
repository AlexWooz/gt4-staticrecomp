// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written for TASK-027 M0b (slice 175) from the Vulkan specification and native/native_vulkan.cpp.
// Clean room: no PCSX2 sources were read. See hw_gs_vulkan_loader.hpp for why the names are renamed.

#include "hw_gs_vulkan_loader.hpp"

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

} // namespace gt4::hw::vulkan_loader
