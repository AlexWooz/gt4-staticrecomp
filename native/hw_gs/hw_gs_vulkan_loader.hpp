// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written for TASK-027 M0b (slice 175) from the Vulkan specification and this repository's own
// loader code (native/native_vulkan.cpp). Clean room: no PCSX2 sources were read.
//
// Vulkan entry points of the HWR backend, owned by this translation unit.
//
// gt4vulkan.exe also links granite-volk.lib (paraLLEl-GS). That library defines vkCreateInstance and
// the other entry-point names as data. A direct call from the HWR backend bound to that data and jumped
// into zero-filled memory (DEP violation at the first GT4_RENDERER=hw start, slice 174). So the names
// are renamed to gt4_hw_* here, and every pointer is filled from vulkan-1.dll at run time. Nothing in
// the HWR backend refers to the global Vulkan names any more.
//
// Include this header only from translation units that implement the backend (native/hw_gs/*.cpp).
//
// TASK-180: the presentation surface is the platform's own: VK_KHR_win32_surface on Windows (an HWND),
// VK_KHR_android_surface on Android (an ANativeWindow, from the SDL3 window). The loader library is vulkan-1.dll on
// Windows and libvulkan.so on Android. Everything else is the same on every platform.
#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#elif defined(__ANDROID__)
#define VK_USE_PLATFORM_ANDROID_KHR
#else
#error "hw_gs_vulkan_loader.hpp: no Vulkan surface platform for this target yet (Linux: TASK-074 L2)"
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <string>
#include <vector>

#include "hw_device_profile.hpp"

// Renames: the backend source keeps the Vulkan names; the preprocessor points them at the pointers below.
#define vkAcquireNextImageKHR gt4_hw_vkAcquireNextImageKHR
#define vkAllocateCommandBuffers gt4_hw_vkAllocateCommandBuffers
#define vkBeginCommandBuffer gt4_hw_vkBeginCommandBuffer
#define vkCmdClearColorImage gt4_hw_vkCmdClearColorImage
#define vkCmdPipelineBarrier gt4_hw_vkCmdPipelineBarrier
#define vkCreateCommandPool gt4_hw_vkCreateCommandPool
#define vkCreateDevice gt4_hw_vkCreateDevice
#define vkCreateFence gt4_hw_vkCreateFence
#define vkCreateInstance gt4_hw_vkCreateInstance
#define vkCreateSemaphore gt4_hw_vkCreateSemaphore
#define vkCreateSwapchainKHR gt4_hw_vkCreateSwapchainKHR
#ifdef _WIN32
#define vkCreateWin32SurfaceKHR gt4_hw_vkCreateWin32SurfaceKHR
#else
#define vkCreateAndroidSurfaceKHR gt4_hw_vkCreateAndroidSurfaceKHR
#endif
#define vkDestroyCommandPool gt4_hw_vkDestroyCommandPool
#define vkDestroyDevice gt4_hw_vkDestroyDevice
#define vkDestroyFence gt4_hw_vkDestroyFence
#define vkDestroyInstance gt4_hw_vkDestroyInstance
#define vkDestroySemaphore gt4_hw_vkDestroySemaphore
#define vkDestroySurfaceKHR gt4_hw_vkDestroySurfaceKHR
#define vkDestroySwapchainKHR gt4_hw_vkDestroySwapchainKHR
#define vkDeviceWaitIdle gt4_hw_vkDeviceWaitIdle
#define vkEndCommandBuffer gt4_hw_vkEndCommandBuffer
#define vkEnumerateDeviceExtensionProperties gt4_hw_vkEnumerateDeviceExtensionProperties
#define vkEnumeratePhysicalDevices gt4_hw_vkEnumeratePhysicalDevices
#define vkGetDeviceQueue gt4_hw_vkGetDeviceQueue
#define vkGetPhysicalDeviceFeatures2 gt4_hw_vkGetPhysicalDeviceFeatures2
#define vkGetPhysicalDeviceFormatProperties gt4_hw_vkGetPhysicalDeviceFormatProperties
#define vkGetPhysicalDeviceProperties gt4_hw_vkGetPhysicalDeviceProperties
#define vkGetPhysicalDeviceQueueFamilyProperties gt4_hw_vkGetPhysicalDeviceQueueFamilyProperties
#define vkGetPhysicalDeviceSurfaceCapabilitiesKHR gt4_hw_vkGetPhysicalDeviceSurfaceCapabilitiesKHR
#define vkGetPhysicalDeviceSurfaceFormatsKHR gt4_hw_vkGetPhysicalDeviceSurfaceFormatsKHR
#define vkGetPhysicalDeviceSurfacePresentModesKHR gt4_hw_vkGetPhysicalDeviceSurfacePresentModesKHR
#define vkGetPhysicalDeviceSurfaceSupportKHR gt4_hw_vkGetPhysicalDeviceSurfaceSupportKHR
#define vkGetSwapchainImagesKHR gt4_hw_vkGetSwapchainImagesKHR
#define vkQueuePresentKHR gt4_hw_vkQueuePresentKHR
#define vkQueueSubmit gt4_hw_vkQueueSubmit
#define vkResetCommandBuffer gt4_hw_vkResetCommandBuffer
#define vkResetFences gt4_hw_vkResetFences
#define vkWaitForFences gt4_hw_vkWaitForFences

inline PFN_vkAcquireNextImageKHR gt4_hw_vkAcquireNextImageKHR = nullptr;
inline PFN_vkAllocateCommandBuffers gt4_hw_vkAllocateCommandBuffers = nullptr;
inline PFN_vkBeginCommandBuffer gt4_hw_vkBeginCommandBuffer = nullptr;
inline PFN_vkCmdClearColorImage gt4_hw_vkCmdClearColorImage = nullptr;
inline PFN_vkCmdPipelineBarrier gt4_hw_vkCmdPipelineBarrier = nullptr;
inline PFN_vkCreateCommandPool gt4_hw_vkCreateCommandPool = nullptr;
inline PFN_vkCreateDevice gt4_hw_vkCreateDevice = nullptr;
inline PFN_vkCreateFence gt4_hw_vkCreateFence = nullptr;
inline PFN_vkCreateInstance gt4_hw_vkCreateInstance = nullptr;
inline PFN_vkCreateSemaphore gt4_hw_vkCreateSemaphore = nullptr;
inline PFN_vkCreateSwapchainKHR gt4_hw_vkCreateSwapchainKHR = nullptr;
#ifdef _WIN32
inline PFN_vkCreateWin32SurfaceKHR gt4_hw_vkCreateWin32SurfaceKHR = nullptr;
#else
inline PFN_vkCreateAndroidSurfaceKHR gt4_hw_vkCreateAndroidSurfaceKHR = nullptr;
#endif
inline PFN_vkDestroyCommandPool gt4_hw_vkDestroyCommandPool = nullptr;
inline PFN_vkDestroyDevice gt4_hw_vkDestroyDevice = nullptr;
inline PFN_vkDestroyFence gt4_hw_vkDestroyFence = nullptr;
inline PFN_vkDestroyInstance gt4_hw_vkDestroyInstance = nullptr;
inline PFN_vkDestroySemaphore gt4_hw_vkDestroySemaphore = nullptr;
inline PFN_vkDestroySurfaceKHR gt4_hw_vkDestroySurfaceKHR = nullptr;
inline PFN_vkDestroySwapchainKHR gt4_hw_vkDestroySwapchainKHR = nullptr;
inline PFN_vkDeviceWaitIdle gt4_hw_vkDeviceWaitIdle = nullptr;
inline PFN_vkEndCommandBuffer gt4_hw_vkEndCommandBuffer = nullptr;
inline PFN_vkEnumerateDeviceExtensionProperties gt4_hw_vkEnumerateDeviceExtensionProperties = nullptr;
inline PFN_vkEnumeratePhysicalDevices gt4_hw_vkEnumeratePhysicalDevices = nullptr;
inline PFN_vkGetDeviceQueue gt4_hw_vkGetDeviceQueue = nullptr;
inline PFN_vkGetPhysicalDeviceFeatures2 gt4_hw_vkGetPhysicalDeviceFeatures2 = nullptr;
inline PFN_vkGetPhysicalDeviceFormatProperties gt4_hw_vkGetPhysicalDeviceFormatProperties = nullptr;
inline PFN_vkGetPhysicalDeviceProperties gt4_hw_vkGetPhysicalDeviceProperties = nullptr;
inline PFN_vkGetPhysicalDeviceQueueFamilyProperties gt4_hw_vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
inline PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR gt4_hw_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
inline PFN_vkGetPhysicalDeviceSurfaceFormatsKHR gt4_hw_vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
inline PFN_vkGetPhysicalDeviceSurfacePresentModesKHR gt4_hw_vkGetPhysicalDeviceSurfacePresentModesKHR = nullptr;
inline PFN_vkGetPhysicalDeviceSurfaceSupportKHR gt4_hw_vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
inline PFN_vkGetSwapchainImagesKHR gt4_hw_vkGetSwapchainImagesKHR = nullptr;
inline PFN_vkQueuePresentKHR gt4_hw_vkQueuePresentKHR = nullptr;
inline PFN_vkQueueSubmit gt4_hw_vkQueueSubmit = nullptr;
inline PFN_vkResetCommandBuffer gt4_hw_vkResetCommandBuffer = nullptr;
inline PFN_vkResetFences gt4_hw_vkResetFences = nullptr;
inline PFN_vkWaitForFences gt4_hw_vkWaitForFences = nullptr;

namespace gt4::hw::vulkan_loader {

// Loads vulkan-1.dll (libvulkan.so on Android) and vkGetInstanceProcAddr, then vkCreateInstance. False with `error` on
// failure.
bool load_loader(std::string &error);

// The instance extension that creates this platform's presentation surface (VK_KHR_win32_surface, VK_KHR_android_surface).
const char *platform_surface_extension();

// Fills every instance-level pointer for `instance`. Names the entry points that did not resolve.
bool load_instance_entry_points(VkInstance instance, std::vector<std::string> &missing);

// Fills only the entry points a windowless device needs (no surface, no swapchain). False with the
// missing names when one did not resolve; used by the M4 fixtures and headless runs.
bool load_headless_entry_points(VkInstance instance, std::vector<std::string> &missing);

// VK_EXT_fragment_shader_interlock with fragmentShaderPixelInterlock on `physical` (IN-345). A device that
// has it is created with the extension and the feature (see interlock_features) so the HWR pixel shader can
// order the read-modify-write of overlapping fragments in primitive order inside one draw.
bool pixel_interlock_supported(VkPhysicalDevice physical);

// An entry point outside the tables above (diagnostics), null when it does not resolve.
PFN_vkVoidFunction instance_proc(VkInstance instance, const char *name);

// TASK-179: the capability probe (hw_device_profile.hpp): identity, class, the features drawing at scale relies on and
// every memory heap with its VK_EXT_memory_budget budget. Fills the `*_supported` fields; the `*_enabled` ones stay false
// until enable_profile_features. GT4_HW_PRETEND_MISSING=large_points,dynamic_indexing (diagnostics, A/B) reports the
// named features as missing, so their fallbacks run on a device that has them.
gt4::hw::DeviceProfile probe_device_profile(VkInstance instance, VkPhysicalDevice physical);

// Turns on in `features` what the profile says the device has and the renderer uses (largePoints,
// shaderStorageImageArrayDynamicIndexing) and records that in the profile's `*_enabled` fields.
void enable_profile_features(gt4::hw::DeviceProfile &profile, VkPhysicalDeviceFeatures &features);

// VK_EXT_memory_budget, to be enabled on the device when the profile found it (null otherwise).
const char *memory_budget_extension(const gt4::hw::DeviceProfile &profile);

} // namespace gt4::hw::vulkan_loader
