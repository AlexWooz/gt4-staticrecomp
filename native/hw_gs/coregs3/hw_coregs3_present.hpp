// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from docs/COREGS3-HOOKS.md §10.3 and native/coregs3 (TASK-090 M4), clean room (decision 0070).
//
// The CoreGS 3.0 live `present` layer: a debug-only graphics pass that draws the newest complete captured car pass over
// the composed display image of CoreGS 2.0. It lives inside the HWR backend because that is the only place the device,
// the rendering queue and the gt4_hw_* Vulkan entry points live. It never creates an instance, device, swapchain or
// window, and it never touches CoreGS 2.0's targets or resources beyond writing the already composed display image it
// is handed. `GT4_COREGS3=present` is the only thing that activates it; the faithful renderer stays the reference.
//
// Colour has no attachment: the composed display image is created by core 2 without VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
// on a discrete GPU, so the fragment stage writes the R32_UINT word with an imageStore (the same packed RGBA8 word
// core2_compose.comp writes). A private D32 depth image and a GEQUAL test (depth = 1/cw, the GS's equal-depth rule) give the visibility order.
// See docs/COREGS3-HOOKS.md §10.3 for the prepared design and the deviations recorded by this implementation.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "hw_gs_vulkan_loader.hpp"  // VkInstance/VkDevice/... and the gt4_hw_* names (backend-internal header)

namespace gt4::hw::coregs3 {
// Everything the layer borrows from Backend::Impl. No ownership passes.
struct PresentConfig {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  std::uint32_t queue_family = 0;
  // VkPhysicalDeviceFeatures::depthClamp was requested and enabled on the device. The pass needs it: its clip z is
  // constant 1 and the depth is 1/cw, so a vertex with 0 < cw < 1 must be depth-clamped instead of clipped away.
  bool depth_clamp_enabled = false;
};

// Submits the recorded command buffer on Impl::queue (Impl::submit_to, shared-queue lock included) with `fence`.
// Returns false with `error` set when vkQueueSubmit refused the work.
using SubmitOverlay = std::function<bool(VkCommandBuffer command, VkFence fence, std::string &error)>;

class PresentLayer {
 public:
  PresentLayer();
  ~PresentLayer();
  PresentLayer(const PresentLayer &) = delete;
  PresentLayer &operator=(const PresentLayer &) = delete;

  // Creates the shaders, pipeline, depth image, command pool, fence and buffers on the backend's device. Returns false
  // with `error` when the device cannot run the pass (no D32 depth attachment, depth clamp not enabled); the caller then
  // keeps the game running without the layer, which is the clean refusal the mode asks for.
  bool init(const PresentConfig &config, std::string &error);
  void shutdown();

  // Takes the newest complete captured guest frame from the ring and records it in the presentation timeline
  // (gt4::coregs3::present::note_guest_frame: timestamp, camera, presentation clock). Called once per guest VBlank,
  // whether or not the frame is drawn, so the timeline samples every 60 Hz guest state. True when there was no new
  // frame (the previous one stays the reference); false with `error` on a hard failure.
  bool note_guest_frame(std::string &error);

  // Draws the presentation ticks that became due with the frame last noted over `target` (a VkImage in
  // VK_IMAGE_LAYOUT_GENERAL, VK_FORMAT_R32_UINT, width x height), then waits for the submission. At a rate above the
  // guest's 60 Hz several ticks are due per guest frame; each tick starts from the composed image saved before the
  // first one, so a tick's interpolated cars never leave ghosts of the previous tick. False with `error` on a Vulkan
  // failure; the caller disables the layer. True also when there was no new frame, no due tick or nothing drawable
  // (counted, never an error).
  bool render(std::uint64_t target, std::uint32_t width, std::uint32_t height, const SubmitOverlay &submit,
              std::string &error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace gt4::hw::coregs3
