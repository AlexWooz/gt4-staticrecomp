// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
//
// TASK-135: puts the HUD on screen. Backend-internal: this header includes Vulkan. The panel is drawn on the CPU (hw_hud.hpp) into one
// staging buffer per frame slot, copied to an image, and blended over the finished picture in the top right corner of the window. The
// image only ever receives the rows that changed: the graph on every presentation, the text twice a second.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../hw_gs_vulkan_loader.hpp"
#include "hw_hud.hpp"
#include "hw_hud_system.hpp"

namespace gt4::hw::hud {

class Layer {
public:
  // `slots`: the frame slots the backend records in turn; `window`: the HWND of the game's window.
  Layer(VkPhysicalDevice physical, VkDevice device, std::uint32_t slots, void *window, std::string gpu_name, std::string scale);
  ~Layer();
  Layer(const Layer &) = delete;
  Layer &operator=(const Layer &) = delete;

  // From whichever thread presents.
  void note_present();

  // Draws the HUD over `target`, which is in TRANSFER_DST_OPTIMAL before and after. Recorded outside a render pass. A failure turns
  // the HUD off for the session with a line in the log: the game goes on.
  void record(VkCommandBuffer cmd, std::uint32_t slot, VkImage target, VkFormat format, VkExtent2D extent);

  // The swapchain and the pictures are going; the device is idle.
  void targets_gone();

private:
  struct Target {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
  };
  struct Staging {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::uint32_t *mapped = nullptr;
  };

  bool ensure_resources(VkFormat format, VkExtent2D extent);
  bool create_pipeline(VkFormat format);
  bool create_panel_resources();
  void destroy_pipeline();
  void destroy_panel_resources();
  void destroy_targets();
  Target *target_for(VkImage image, VkExtent2D extent);
  bool memory_type(std::uint32_t bits, VkMemoryPropertyFlags wanted, std::uint32_t &chosen) const;
  double now_ms() const;

  VkPhysicalDevice physical_;
  VkDevice device_;
  std::uint32_t slots_;
  void *window_;
  SessionFacts facts_;

  std::mutex mutex_;  // times_ and reading_: the presenting thread and the sampler thread
  FrameTimes times_;
  SystemReading reading_;
  const std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
  std::unique_ptr<SystemSampler> sampler_;

  bool failed_ = false;
  std::unique_ptr<Canvas> canvas_;
  int font_pixels_ = 0;
  double next_text_ms_ = 0.0;
  std::vector<float> intervals_;
  bool image_ready_ = false;
  // What the panel costs the thread that presents, for the log every 1200 presentations (the game may end without destructors).
  double record_us_ = 0.0;
  std::uint64_t records_ = 0;

  VkFormat format_ = VK_FORMAT_UNDEFINED;
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkSampler sampler_object_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
  VkImage image_ = VK_NULL_HANDLE;
  VkDeviceMemory image_memory_ = VK_NULL_HANDLE;
  VkImageView image_view_ = VK_NULL_HANDLE;
  std::vector<Staging> staging_;
  std::vector<Target> targets_;
};

}  // namespace gt4::hw::hud
