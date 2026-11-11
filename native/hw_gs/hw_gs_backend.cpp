// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from the TASK-026 specification (docs/hw-renderer-spec), clean room per decision 0069.
// Implements chapter 12.3 (stubs for the GS data path), 12.5 (device, presentation, pacing
// entry), 12.6 (feature report) and 13.6 (swapchain, RDNA4 recreation rule) at milestone M0.

#include "hw_gs_backend.hpp"

// Vulkan create-info structs are initialised with sType only (pNext defaults to null by the spec).
#pragma clang diagnostic ignored "-Wmissing-field-initializers"

#include "../pacing.hpp"
#include "hw_gs_present_rect.hpp"
#include "hw_present_latency.hpp"
#include "hw_present_transform.hpp"
#include "hw_present_spacing.hpp"
#include "hw_queue_lock.hpp"
#include "hw_shared_present.hpp"

#include "hw_gs_vulkan_loader.hpp" // Vulkan names renamed to gt4_hw_*; pointers loaded at run time
#include "coregs3/hw_coregs3_present.hpp"
#include "coregs3_present.hpp"
#include "coregs3_live_world.hpp"
#include "hud/hw_hud_layer.hpp"
#include "writeback/hw_gs_vulkan_device_table.hpp"
#include "writeback/hw_gs_present_scheduler.hpp"
#include "writeback/hw_gs_writeback.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <memory>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace gt4::hw {
namespace {

constexpr std::uint32_t privileged_base = 0x12000000u;
constexpr std::uint32_t privileged_end = 0x12001FFFu; // inclusive
constexpr std::size_t privileged_slots = (privileged_end - privileged_base + 1) / 8;
constexpr std::uint32_t amd_vendor_id = 0x1002;
// Resource ring (chapter 12.5) and the in-flight presentation limit.
constexpr std::uint32_t frame_slots = 3;
constexpr std::uint32_t presents_in_flight = 2;
constexpr std::uint32_t max_swapchain_images = 8;

const char *vk_result_text(VkResult result) {
  switch (result) {
  case VK_SUCCESS: return "VK_SUCCESS";
  case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
  case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
  case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
  case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
  default: return "VkResult";
  }
}

// True when the environment variable `name` is set to exactly `value`.
bool environment_is(const char *name, const char *value) {
  const char *text = std::getenv(name);
  return text != nullptr && std::strcmp(text, value) == 0;
}

bool has_extension(const std::vector<VkExtensionProperties> &available, const char *name) {
  return std::any_of(available.begin(), available.end(),
                     [name](const VkExtensionProperties &p) { return std::strcmp(p.extensionName, name) == 0; });
}

bool format_supports(VkPhysicalDevice device, VkFormat format, VkFormatFeatureFlags required, bool optimal) {
  VkFormatProperties props{};
  vkGetPhysicalDeviceFormatProperties(device, format, &props);
  const VkFormatFeatureFlags have = optimal ? props.optimalTilingFeatures : props.linearTilingFeatures;
  return (have & required) == required;
}

// The formats every path uses (chapter 12.6 as built). TASK-179: the list named D32_SFLOAT depth attachments, R16_UINT
// sampled, R16G16B16A16_UNORM blendable and R32_SFLOAT colour attachments, which nothing creates (core 2 draws into
// R32_UINT storage images and has no depth attachment), so a device without them was refused for nothing. Formats only
// one path uses are checked by that path: R8G8B8A8_UNORM blendable colour attachments by core 2's ROP path (IN-481),
// which stays off without them.
struct FormatRequirement {
  VkFormat format;
  VkFormatFeatureFlags flags;
  const char *name;
};

constexpr FormatRequirement required_formats[] = {
    // Every target and texel image of both cores.
    {VK_FORMAT_R32_UINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, "R32_UINT storage image"},
    // The presentation's upload image: written, halved and blitted into the swapchain with a linear filter.
    {VK_FORMAT_R8G8B8A8_UNORM,
     VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
     "R8G8B8A8_UNORM blit with a linear filter"},
};

} // namespace

struct Backend::Impl {
  Options options;
  DeviceReport report;
  Stats stats;

  StreamEventHandler stream_handler = nullptr;
  void *stream_context = nullptr;
  std::array<std::uint64_t, privileged_slots> privileged{};
  int gif_path_state[4] = {0, 0, 0, 0};

  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  // IN-516: a second queue of the family, where it has one; TASK-174: `queue` itself on the shared path.
  VkQueue present_queue = VK_NULL_HANDLE;
  std::uint32_t queue_family = 0;

  // How frames reach the display (chosen in create_device, logged on the "[hw] presentation queue" line):
  // - rendering_queue: the GS thread acquires the swapchain image, draws into it and presents, all on the rendering queue (a device
  //   with one queue in its graphics family before TASK-174; GT4_HW_PRESENT_QUEUE=0 and GT4_HW_PRESENT_SHARED_QUEUE=0 keep it).
  // - own_queue (IN-516): the GS thread draws a picture on the rendering queue, a second queue of the family copies it into the
  //   swapchain image and presents; with frames left out the presentation thread of TASK-082 does that part.
  // - shared_queue (TASK-174): the same pictures, with the copy and the present always on the presentation thread, but on the
  //   rendering queue, the family's only one. Every queue call on it holds `queue_lock`, the renderer's submissions included.
  enum class PresentPath { rendering_queue, own_queue, shared_queue };
  PresentPath present_path = PresentPath::rendering_queue;
  bool uses_pictures() const { return present_path != PresentPath::rendering_queue; }
  bool shares_queue() const { return present_path == PresentPath::shared_queue; }
  static const char *present_path_name(PresentPath path) {
    switch (path) {
    case PresentPath::rendering_queue: return "rendering_queue";
    case PresentPath::own_queue: return "own_queue";
    case PresentPath::shared_queue: return "shared_queue";
    }
    return "unknown";
  }
  gt4::hw::QueueLock queue_lock;
  using QueueCaller = gt4::hw::QueueLock::Caller;

  // TASK-174: every vkQueueSubmit and vkDeviceWaitIdle of this file goes through these two (vkQueuePresentKHR through
  // present_locked). On the shared queue they hold `queue_lock`; on the other paths each queue is used by one thread at a time and
  // they call Vulkan directly, as before.
  VkResult submit_to(VkQueue on, const VkSubmitInfo &submit, VkFence fence, QueueCaller caller) {
    if (!shares_queue()) return vkQueueSubmit(on, 1, &submit, fence);
    return queue_lock.run(caller, [&] { return vkQueueSubmit(on, 1, &submit, fence); });
  }
  // Callers make sure the presentation thread has nothing on its way first (pacer_drain or pacer_close).
  void device_wait_idle() {
    if (!device) return;
    if (!shares_queue()) {
      vkDeviceWaitIdle(device);
      return;
    }
    queue_lock.run(QueueCaller::device_idle, [&] { return vkDeviceWaitIdle(device); });
  }

  // TASK-174 counters for the presentation report, on every path. `present_requests` counts the presentations asked of the path
  // (present_frame with a swapchain); each ends as shown (vkQueuePresentKHR returned VK_SUCCESS or VK_SUBOPTIMAL_KHR), not shown
  // because the presentation side was behind (`presents_dropped`: no free slot, picture or swapchain image in time), failed (an
  // acquire or present error: out of date, surface lost), or left out by the latency control of IN-521.
  std::uint64_t present_requests = 0;  // GS thread
  std::atomic<std::uint64_t> presents_shown{0};
  std::atomic<std::uint64_t> presents_failed{0};
  std::uint64_t presents_left_out = 0;  // GS thread
  // The GS thread's time in the presentation step (present_frame, whole) and inside its calls that can wait: fence waits, acquire,
  // vkQueueSubmit and vkQueuePresentKHR, the shared queue's lock included. Microseconds, session totals, GS thread only.
  double gs_present_step_us = 0;
  double gs_present_wait_us = 0;
  template <typename Call> auto gs_waits(Call &&call) -> decltype(call()) {
    const auto start = std::chrono::steady_clock::now();
    auto result = std::forward<Call>(call)();
    gs_present_wait_us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    return result;
  }
  VkImage images[max_swapchain_images] = {};
  VkImageLayout image_layouts[max_swapchain_images] = {};
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkCommandBuffer command_buffers[frame_slots] = {};
  VkFence fences[frame_slots] = {};
  VkSemaphore acquire_semaphores[frame_slots] = {};
  std::uint64_t submissions = 0;
  // IN-516: per slot, the picture drawn on the main queue and what the second queue needs to show it.
  // TASK-146: the show commands have their own pool. With frames left out, the presentation thread records them while the GS
  // thread records `command_buffers`, and a command pool may be used by one thread at a time (Vulkan external
  // synchronization): sharing `command_pool` corrupted it on NVIDIA, and a later vkQueueSubmit on the second queue read freed
  // driver memory (owner's laptop, access violation in nvoglv64.dll, 2026-11-03). Only one thread at a time uses this pool:
  // the presentation thread while frames are left out, the GS thread otherwise, with pacer_drain() between them.
  VkCommandPool show_pool = VK_NULL_HANDLE;
  VkCommandBuffer show_commands[frame_slots] = {};
  VkFence draw_fences[frame_slots] = {};
  VkSemaphore picture_ready[frame_slots] = {};
  VkImage pictures[frame_slots] = {};
  VkDeviceMemory picture_memory[frame_slots] = {};
  VkImageLayout picture_layouts[frame_slots] = {};
  std::atomic<std::uint64_t> presents_dropped{0};  // GS thread and presentation thread
  std::uint64_t presents_dropped_reported = 0;
  // What record_frame and record_black draw into: the swapchain images, or the pictures of IN-516.
  VkImage *draw_images = images;
  VkImageLayout *draw_layouts = image_layouts;
  VkImageLayout draw_final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  gt4::pacing::PresentMode present_mode = gt4::pacing::PresentMode::fifo;
  bool fifo_latest_ready = false;  // IN-515: the device has VK_PRESENT_MODE_FIFO_LATEST_READY_KHR and it is enabled

  // GT4_HW_FRAMETIME_LOG=<csv path> (diagnostic, off by default): one line per presentation, the source of the frametime
  // graphs (scripts/coregs2_bench/frametime_plot.py). `present_ms` is when vkQueuePresentKHR was called, what an overlay
  // like RivaTuner draws; `shown_ms` is when the display took the frame (VK_KHR_present_wait2, on a thread of its own, 0
  // where the device lacks it). A frame dropped by the mode gets the time of the frame shown in its place.
  struct FrametimeEntry {
    std::uint64_t id, call;
    double presentMs, callMs;
    VkSwapchainKHR swapchain;
  };
  const char *frametime_path = std::getenv("GT4_HW_FRAMETIME_LOG");
  std::FILE *frametime_file = nullptr;
  // IN-521: the display queue control; it needs the shown times as well. GT4_HW_LATENCY_CONTROL=0: off (A/B).
  const bool latency_control_on = [] {
    const char *text = std::getenv("GT4_HW_LATENCY_CONTROL");
    return text == nullptr || std::strcmp(text, "0") != 0;
  }();
  bool want_shown_times() const { return frametime_path != nullptr || latency_control_on; }
  gt4::hw::PresentLatencyControl latency_control;
  double frametime_last_shown = 0.0;
  bool frametime_shown_possible = false;  // the instance has VK_KHR_get_surface_capabilities2
  bool present_wait_enabled = false;
  PFN_vkWaitForPresent2KHR wait_for_present = nullptr;
  std::thread frametime_thread;
  std::mutex frametime_mutex;
  std::condition_variable frametime_changed;
  std::deque<FrametimeEntry> frametime_queue;
  bool frametime_stop = false, frametime_waiting = false;
  std::uint64_t frametime_done_id = 0;  // TASK-174: the last presentation id this thread is done with (shown, or given up on)
  std::uint64_t present_calls = 0, present_ids = 0;
  const std::chrono::steady_clock::time_point frametime_start = std::chrono::steady_clock::now();

  double frametime_now_ms() const {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frametime_start).count();
  }

  // TASK-146: vkAcquireNextImageKHR, vkQueuePresentKHR and vkWaitForPresent2KHR each need the swapchain to themselves
  // (externsync in the specification). The frametime thread's wait used to block on the swapchain for up to a refresh
  // while the presenting thread acquired and presented on it (the validation layer's thread check: 10 overlaps in 1,000
  // frames of a replay, in every mode). So every call on the swapchain holds this mutex, and the frametime thread asks
  // without waiting (timeout 0) every half millisecond instead of blocking: a present is never held up for more than one
  // such call.
  std::mutex swapchain_mutex;
  static constexpr std::int64_t kShownPollUs = 500;

  VkResult acquire_image(std::uint64_t timeout_ns, VkSemaphore semaphore, std::uint32_t &index) {
    const std::lock_guard<std::mutex> lock(swapchain_mutex);
    return vkAcquireNextImageKHR(device, swapchain, timeout_ns, semaphore, VK_NULL_HANDLE, &index);
  }

  // TASK-174: on the shared queue the present also needs the queue to itself (queue_lock first, then the swapchain mutex). Only the
  // presentation thread presents there.
  VkResult present_locked(VkQueue on, const VkPresentInfoKHR &present) {
    if (shares_queue()) {
      return queue_lock.run(QueueCaller::presentation_thread, [&] {
        const std::lock_guard<std::mutex> lock(swapchain_mutex);
        return vkQueuePresentKHR(on, &present);
      });
    }
    const std::lock_guard<std::mutex> lock(swapchain_mutex);
    return vkQueuePresentKHR(on, &present);
  }

  // On the frametime thread: when the presentation `id` reached the display, in the log's milliseconds; 0 when it did not
  // within half a second.
  double wait_until_shown(VkSwapchainKHR on, std::uint64_t id, HANDLE timer) {
    VkPresentWait2InfoKHR info{VK_STRUCTURE_TYPE_PRESENT_WAIT_2_INFO_KHR};
    info.presentId = id;
    info.timeout = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    for (;;) {
      VkResult result;
      {
        const std::lock_guard<std::mutex> lock(swapchain_mutex);
        result = wait_for_present(device, on, &info);
      }
      if (result == VK_SUCCESS) return frametime_now_ms();
      if (result != VK_TIMEOUT || std::chrono::steady_clock::now() >= deadline) return 0.0;
      if (timer != nullptr) {
        LARGE_INTEGER due{};
        due.QuadPart = -kShownPollUs * 10;  // relative, in 100 ns units
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
      } else {
        std::this_thread::yield();
      }
    }
  }

  void frametime_worker() {
    // A high-resolution timer for the half-millisecond polls (a plain Sleep rounds to the system tick).
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    std::uint64_t finished_id = 0;
    for (;;) {
      FrametimeEntry entry{};
      {
        std::unique_lock<std::mutex> lock(frametime_mutex);
        frametime_waiting = false;
        if (finished_id != 0) frametime_done_id = finished_id;  // wakes wait_for_previous_shown too
        frametime_changed.notify_all();
        frametime_changed.wait(lock, [&] { return frametime_stop || !frametime_queue.empty(); });
        if (frametime_queue.empty()) {
          if (timer != nullptr) CloseHandle(timer);
          return;
        }
        entry = frametime_queue.front();
        frametime_queue.pop_front();
        frametime_waiting = true;
      }
      const double shownMs = wait_for_present != nullptr ? wait_until_shown(entry.swapchain, entry.id, timer) : 0.0;
      finished_id = entry.id;
      if (shownMs > 0.0 && latency_control_on) {
        if (frametime_last_shown > 0.0) latency_control.shown(shownMs - entry.presentMs, shownMs - frametime_last_shown);
        frametime_last_shown = shownMs;
      }
      if (frametime_file) {
        std::fprintf(frametime_file, "%llu,%llu,%.3f,%.3f,%.3f\n", static_cast<unsigned long long>(entry.id),
                     static_cast<unsigned long long>(entry.call), entry.presentMs, entry.callMs, shownMs);
        if (entry.id % 300 == 0) std::fflush(frametime_file);
      }
    }
  }

  // Before the swapchain goes: nothing may still wait on it.
  void frametime_drain() {
    if (!frametime_thread.joinable()) return;
    std::unique_lock<std::mutex> lock(frametime_mutex);
    frametime_changed.wait(lock, [&] { return frametime_queue.empty() && !frametime_waiting; });
  }

  void frametime_close() {
    if (!frametime_thread.joinable()) return;
    {
      const std::lock_guard<std::mutex> lock(frametime_mutex);
      frametime_stop = true;
    }
    frametime_changed.notify_all();
    frametime_thread.join();
    if (frametime_file) std::fclose(frametime_file);
    frametime_file = nullptr;
  }

  // TASK-135, TASK-175: the performance HUD (GT4_HW_HUD=1 the full panel, GT4_HW_HUD=fps the frame rate alone); null when off.
  const gt4::hw::hud::Mode hud_mode = gt4::hw::hud::mode_from_setting(std::getenv("GT4_HW_HUD"));
  const bool hud_wanted = hud_mode != gt4::hw::hud::Mode::off;
  std::unique_ptr<gt4::hw::hud::Layer> hud_layer;

  // vkQueuePresentKHR with the log's id and times, counted as shown or failed (TASK-174).
  VkResult queue_present(VkQueue on, VkPresentInfoKHR &present) {
    const VkResult result = queue_present_timed(on, present);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
      ++presents_shown;
    } else {
      ++presents_failed;
    }
    return result;
  }

  VkResult queue_present_timed(VkQueue on, VkPresentInfoKHR &present) {
    if (hud_layer) hud_layer->note_present();
    if (!frametime_path && !present_wait_enabled) return present_locked(on, present);
    if (frametime_path && !frametime_file) {
      frametime_file = std::fopen(frametime_path, "w");
      if (frametime_file) {
        std::fprintf(frametime_file, "id,vblank,present_ms,call_ms,shown_ms\n");
      } else {
        std::fprintf(stderr, "[hw] GT4_HW_FRAMETIME_LOG: cannot open %s\n", frametime_path);
        frametime_path = nullptr;
        if (!present_wait_enabled) return present_locked(on, present);
      }
    }
    if (!frametime_thread.joinable()) frametime_thread = std::thread([this] { frametime_worker(); });
    const std::uint64_t id = ++present_ids;
    VkPresentId2KHR presentId{VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR};
    presentId.swapchainCount = 1;
    presentId.pPresentIds = &id;
    if (present_wait_enabled) present.pNext = &presentId;
    const double start = frametime_now_ms();
    const VkResult result = present_locked(on, present);
    const double end = frametime_now_ms();
    {
      const std::lock_guard<std::mutex> lock(frametime_mutex);
      frametime_queue.push_back(FrametimeEntry{id, present_calls, start, end - start, swapchain});
    }
    frametime_changed.notify_all();
    return result;
  }

  bool independent_blend = false;
  // TASK-179: the capability probe of the chosen device, with what it was created with (logged once).
  DeviceProfile profile;
  // TASK-179: the swapchain's pre-transform and the blit axes it reverses (hw_present_transform.hpp).
  PresentTransform present_transform;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
  VkExtent2D extent{};
  std::uint32_t image_count = 0;
  VkSemaphore render_finished[max_swapchain_images] = {};
  HWND window = nullptr;
  std::atomic<bool> resize_pending{false};  // also set by the presentation thread of TASK-082
  // TASK-145: why the swapchain is rebuilt (logged with each rebuild), and a VK_SUBOPTIMAL_KHR seen since the last frame
  // end. A suboptimal swapchain still presents; it is rebuilt only when the surface size changed, since every rebuild
  // makes the compositor show what lies behind the window for a frame.
  enum class Rebuild : int { none, window_size, acquire_out_of_date, present_out_of_date, suboptimal_size };
  std::atomic<int> rebuild_reason{static_cast<int>(Rebuild::none)};
  std::atomic<bool> suboptimal_seen{false};
  std::uint64_t suboptimal_kept = 0;
  void request_rebuild(Rebuild reason) {
    rebuild_reason = static_cast<int>(reason);
    resize_pending = true;
  }
  // An acquire or present result: out of date asks for a rebuild, suboptimal is noted for the frame end.
  void note_present_result(VkResult result, Rebuild if_out_of_date) {
    if (result == VK_ERROR_OUT_OF_DATE_KHR) request_rebuild(if_out_of_date);
    if (result == VK_SUBOPTIMAL_KHR) suboptimal_seen = true;
  }
  bool initialised = false;
  // TASK-080: the race view's display-aspect fill request (pack_display_aspect), set from the EE thread.
  std::atomic<std::uint64_t> display_aspect{0};

  // M4: GS front end, shadow local memory and the GPU write-back drawer (writeback/).
  std::unique_ptr<hwgs::writeback::Renderer> renderer;

  // TASK-090 M4: the debug-only CoreGS 3.0 live `present` layer (docs/COREGS3-HOOKS.md §10.3). Null unless
  // GT4_COREGS3=present and the device can run the pass; it touches only the composed display image it is handed.
  std::unique_ptr<gt4::hw::coregs3::PresentLayer> coregs3_present;
  std::uint64_t coregs3_overlay_flush = UINT64_MAX;  // stats.flushes of the frame that already carries the layer
  bool depth_clamp_enabled = false;                  // VkPhysicalDeviceFeatures::depthClamp requested and enabled
  // TASK-090 host clock: the window presents each due presentation tick at the display's rate instead of once per guest
  // VBlank. It needs the picture presentation path (a second queue or the shared queue) so a scheduled picture can be
  // shown later without blocking the GS thread; GT4_COREGS3_HOST_CLOCK=0 turns it off (A/B).
  bool coregs3_host_clock = false;
  bool host_clock_presented = false;  // end_frame presented this VBlank's ticks; present() must not add a normal slot
  static constexpr std::size_t kHostClockQueueLimit = 6;  // pictures handed to the presenter and not shown yet

  // Draws the newest complete captured car pass over a composed display image, once per frame, on the rendering queue.
  // A Vulkan failure disables the layer and never the game: `present` is a diagnostic, not the faithful path.
  void coregs3_guest_frame() {
    if (!coregs3_present) return;
    std::string error;
    if (!coregs3_present->note_guest_frame(error)) {
      std::fprintf(stderr, "[coregs3] present timeline stopped: %s\n", error.c_str());
      coregs3_present.reset();
    }
  }

  void overlay_coregs3(VkImage image, std::uint32_t width, std::uint32_t height) {
    if (!coregs3_present || image == VK_NULL_HANDLE) return;
    if (coregs3_overlay_flush == stats.flushes) return;  // the window path already drew it for this frame
    coregs3_overlay_flush = stats.flushes;
    const gt4::hw::coregs3::SubmitOverlay submit = [this](VkCommandBuffer command, VkFence fence, std::string &error) {
      VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      info.commandBufferCount = 1;
      info.pCommandBuffers = &command;
      const VkResult result = submit_to(queue, info, fence, QueueCaller::gs_thread);
      if (result != VK_SUCCESS) {
        error = std::string("vkQueueSubmit failed for the CoreGS 3.0 present pass: ") + vk_result_text(result);
        return false;
      }
      return true;
    };
    std::string error;
    if (!coregs3_present->render(reinterpret_cast<std::uint64_t>(image), width, height, submit, error)) {
      std::fprintf(stderr, "[coregs3] present layer stopped: %s\n", error.c_str());
      coregs3_present.reset();
    }
  }

  // TASK-090 2D overlay split: after the present layer drew the world and the cars into the frame's 3D image, composite
  // the 2D layer the VBlank composed next to it (GpuFrame::overlay). A no-op for a frame that was not split.
  void composite_coregs3_overlay() {
    if (renderer) renderer->CompositeCoregs3Overlay(composedFrame);
  }

  static std::int64_t steady_now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  // TASK-090 host clock: draws each due presentation tick over `image` and hands its picture to the presenter at the
  // tick's scheduled host time (coregs3_host_clock.hpp), one display period apart. Every tick starts from the composed
  // image saved before the first one, so the interpolated cars of one tick never appear in the next. Returns true when
  // this VBlank's presentation was handled here (at least one picture handed over), false when there was nothing to
  // present and the caller should fall back to the normal once-per-VBlank present.
  bool overlay_coregs3_host_clock(VkImage image, std::uint32_t width, std::uint32_t height) {
    if (!coregs3_present || image == VK_NULL_HANDLE) return false;
    if (coregs3_overlay_flush == stats.flushes) return true;  // this frame already carries the layer
    const gt4::coregs3::HostPresentationClock::Plan &plan = gt4::coregs3::present::host_clock_plan();
    if (plan.count == 0) return false;
    coregs3_overlay_flush = stats.flushes;
    const gt4::hw::coregs3::SubmitOverlay submit = [this](VkCommandBuffer command, VkFence fence, std::string &error) {
      VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      info.commandBufferCount = 1;
      info.pCommandBuffers = &command;
      const VkResult result = submit_to(queue, info, fence, QueueCaller::gs_thread);
      if (result != VK_SUCCESS) {
        error = std::string("vkQueueSubmit failed for the CoreGS 3.0 present pass: ") + vk_result_text(result);
        return false;
      }
      return true;
    };
    bool handed = false;
    for (std::uint32_t index = 0; index < plan.count; ++index) {
      bool drew = false;
      std::string error;
      if (!coregs3_present->render_tick(reinterpret_cast<std::uint64_t>(image), width, height, plan.ticks[index].index,
                                        submit, drew, error)) {
        std::fprintf(stderr, "[coregs3] present layer stopped: %s\n", error.c_str());
        coregs3_present.reset();
        break;
      }
      if (!drew) continue;  // nothing drawable for this instant: the base image is the picture
      // TASK-090 2D overlay split: every tick starts from the 3D checkpoint image; the 2D layer is composited over the
      // tick's world before the presenter copies it (the guest-rate HUD reused by every host-clock tick).
      composite_coregs3_overlay();
      present_host_clock_picture(plan.ticks[index].show_ns);
      handed = true;
    }
    return handed;
  }

  // One host-clock presentation: the image currently in the renderer's display surface (drawn by overlay_coregs3_host_clock
  // just before this call) is drawn into a picture slot on the rendering queue and handed to the presentation thread,
  // which shows it at `show_ns`. Nothing waits for the display on the GS thread.
  void present_host_clock_picture(std::int64_t show_ns) {
    if (!swapchain || !renderer) return;
    ++present_requests;
    const std::int64_t show_us = show_ns / 1000;
    if (show_us <= steady_now_us()) stats.host_clock_late++;
    const auto step_start = std::chrono::steady_clock::now();
    present_host_clock_path(frameOnGpu && frame.valid ? &frame : nullptr, show_us);
    gs_present_step_us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - step_start).count();
  }

  // M6 interface: the frame end composes into `frame`, the scheduler decides whether it reaches the
  // display, and `shownFrame` is what the display currently holds (a hold re-presents exactly this).
  hwgs::present::Scheduler scheduler;
  bool pixel_interlock = false;  // VK_EXT_fragment_shader_interlock enabled on the device (IN-345)
  std::uint64_t presentedVblank = UINT64_MAX;  // flush count of the VBlank whose presentation image is in `frame`
  hwgs::writeback::FrameOutput frame;
  hwgs::writeback::FrameOutput shownFrame;
  bool frameOnGpu = false;  // IN-364: `frame` is the renderer's display image (no pixels on the CPU)
  hwgs::writeback::GpuFrame composedFrame;  // TASK-090: the last composed GPU frame (its 2D layer for the split)
  bool shownOnGpu = false;
  VkImage gpuImage = VK_NULL_HANDLE;
  std::uint64_t guestFrames = 0;
  double uploadUs = 0;
  double presentUs = 0;
  double composeTotalUs = 0;        // session sum behind stats().compose_us
  std::uint64_t composedFrames = 0;

  // Present path: staging buffer and RGBA8 image for the composed display frame (recreated on resize).
  struct FrameUpload {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t levels = 1;  // IN-375: mip levels for the downscale of an upscaled frame
  } upload;

  ~Impl() { shutdown(); }

  void shutdown() {
    pacer_close();  // the presentation thread is joined before anything it uses goes
    device_wait_idle();
    frametime_close();
    hud_layer.reset();
    destroy_upload();
    coregs3_present.reset();  // the live layer's images, pipeline and command pool go before the device
    renderer.reset();  // GPU drawer resources are released before the device
    destroy_swapchain();
    if (device) {
      for (std::uint32_t i = 0; i < frame_slots; ++i) {
        if (fences[i]) vkDestroyFence(device, fences[i], nullptr);
        if (acquire_semaphores[i]) vkDestroySemaphore(device, acquire_semaphores[i], nullptr);
        if (draw_fences[i]) vkDestroyFence(device, draw_fences[i], nullptr);
        if (picture_ready[i]) vkDestroySemaphore(device, picture_ready[i], nullptr);
        fences[i] = VK_NULL_HANDLE;
        acquire_semaphores[i] = VK_NULL_HANDLE;
        draw_fences[i] = VK_NULL_HANDLE;
        picture_ready[i] = VK_NULL_HANDLE;
      }
      if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
      if (show_pool) vkDestroyCommandPool(device, show_pool, nullptr);
      vkDestroyDevice(device, nullptr);
    }
    if (surface && instance) vkDestroySurfaceKHR(instance, surface, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
    command_pool = VK_NULL_HANDLE;
    show_pool = VK_NULL_HANDLE;
    device = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
    surface = VK_NULL_HANDLE;
    initialised = false;
  }

  bool memory_type(std::uint32_t bits, VkMemoryPropertyFlags wanted, std::uint32_t &chosen) const {
    VkPhysicalDeviceMemoryProperties props{};
    gt4_hwd_vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted) {
        chosen = i;
        return true;
      }
    }
    return false;
  }

  void destroy_upload() {
    if (!device) return;
    if (upload.mapped) gt4_hwd_vkUnmapMemory(device, upload.buffer_memory);
    if (upload.buffer) gt4_hwd_vkDestroyBuffer(device, upload.buffer, nullptr);
    if (upload.buffer_memory) gt4_hwd_vkFreeMemory(device, upload.buffer_memory, nullptr);
    if (upload.image) gt4_hwd_vkDestroyImage(device, upload.image, nullptr);
    if (upload.image_memory) gt4_hwd_vkFreeMemory(device, upload.image_memory, nullptr);
    upload = FrameUpload{};
  }

  // Creates the staging buffer and the RGBA8 image for a width x height frame (no-op when they fit).
  bool ensure_upload(std::uint32_t width, std::uint32_t height) {
    if (upload.image && upload.width == width && upload.height == height) return true;
    destroy_upload();
    upload.width = width;
    upload.height = height;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4;

    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size = bytes;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (gt4_hwd_vkCreateBuffer(device, &buffer, nullptr, &upload.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    gt4_hwd_vkGetBufferMemoryRequirements(device, upload.buffer, &req);
    std::uint32_t type = 0;
    if (!memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     type)) {
      return false;
    }
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (gt4_hwd_vkAllocateMemory(device, &alloc, nullptr, &upload.buffer_memory) != VK_SUCCESS) return false;
    gt4_hwd_vkBindBufferMemory(device, upload.buffer, upload.buffer_memory, 0);
    if (gt4_hwd_vkMapMemory(device, upload.buffer_memory, 0, bytes, 0, &upload.mapped) != VK_SUCCESS) return false;

    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.extent = {width, height, 1};
    // IN-375: an upscaled frame larger than the window is reduced by 2x2 box steps (linear half-size blits) before
    // the final linear blit, so the final blit never skips source pixels (a single linear blit of a 6x frame into a
    // 1440p window samples only 4 of about 7 x 7 source pixels per output pixel and aliases).
    std::uint32_t levels = 1;
    while (levels < 4 && (width >> levels) >= 320 && (height >> levels) >= 224) ++levels;
    upload.levels = levels;
    image.mipLevels = levels;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (gt4_hwd_vkCreateImage(device, &image, nullptr, &upload.image) != VK_SUCCESS) return false;
    gt4_hwd_vkGetImageMemoryRequirements(device, upload.image, &req);
    if (!memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type)) return false;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (gt4_hwd_vkAllocateMemory(device, &alloc, nullptr, &upload.image_memory) != VK_SUCCESS) return false;
    gt4_hwd_vkBindImageMemory(device, upload.image, upload.image_memory, 0);
    return true;
  }

  void destroy_swapchain() {
    if (!swapchain) return;
    frametime_drain();
    if (hud_layer) hud_layer->targets_gone();
    for (std::uint32_t i = 0; i < image_count; ++i) {
      if (render_finished[i]) vkDestroySemaphore(device, render_finished[i], nullptr);
      render_finished[i] = VK_NULL_HANDLE;
    }
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
    image_count = 0;
    destroy_pictures();
  }

  void destroy_pictures() {
    for (std::uint32_t i = 0; i < frame_slots; ++i) {
      if (pictures[i]) gt4_hwd_vkDestroyImage(device, pictures[i], nullptr);
      if (picture_memory[i]) gt4_hwd_vkFreeMemory(device, picture_memory[i], nullptr);
      pictures[i] = VK_NULL_HANDLE;
      picture_memory[i] = VK_NULL_HANDLE;
    }
  }

  // IN-516: one window-sized picture per slot, in the swapchain's format.
  bool create_pictures() {
    for (std::uint32_t i = 0; i < frame_slots; ++i) {
      VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      image.imageType = VK_IMAGE_TYPE_2D;
      image.format = swapchain_format;
      image.extent = {extent.width, extent.height, 1};
      image.mipLevels = 1;
      image.arrayLayers = 1;
      image.samples = VK_SAMPLE_COUNT_1_BIT;
      image.tiling = VK_IMAGE_TILING_OPTIMAL;
      image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    (hud_wanted ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VkImageUsageFlags{0});
      image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      if (gt4_hwd_vkCreateImage(device, &image, nullptr, &pictures[i]) != VK_SUCCESS) return false;
      VkMemoryRequirements req{};
      gt4_hwd_vkGetImageMemoryRequirements(device, pictures[i], &req);
      std::uint32_t type = 0;
      if (!memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type)) return false;
      VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      alloc.allocationSize = req.size;
      alloc.memoryTypeIndex = type;
      if (gt4_hwd_vkAllocateMemory(device, &alloc, nullptr, &picture_memory[i]) != VK_SUCCESS) return false;
      gt4_hwd_vkBindImageMemory(device, pictures[i], picture_memory[i], 0);
      picture_layouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    return true;
  }

  // Feature report and device choice (chapter 12.6). Returns false with `error` set when no device qualifies.
  bool create_instance(std::string &error) {
    if (!vulkan_loader::load_loader(error)) return false;
    const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
                                VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME};
    // The frametime log's shown times need the third one (the log goes on without them).
    const auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        vulkan_loader::instance_proc(nullptr, "vkEnumerateInstanceExtensionProperties"));
    bool surface_capabilities2 = false;
    if (want_shown_times() && enumerate != nullptr) {
      std::uint32_t count = 0;
      enumerate(nullptr, &count, nullptr);
      std::vector<VkExtensionProperties> available(count);
      enumerate(nullptr, &count, available.data());
      surface_capabilities2 = has_extension(available, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    }
    frametime_shown_possible = surface_capabilities2;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "GT4Recomp HWR";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = surface_capabilities2 ? 3u : 2u;
    info.ppEnabledExtensionNames = extensions;
    const VkResult result = vkCreateInstance(&info, nullptr, &instance);
    if (result != VK_SUCCESS) {
      error = std::string("vkCreateInstance failed: ") + vk_result_text(result);
      return false;
    }
    std::vector<std::string> missing;
    if (!vulkan_loader::load_instance_entry_points(instance, missing)) {
      error = "Vulkan entry points missing from the loader:";
      for (const std::string &name : missing) error += " " + name;
      return false;
    }
    return true;
  }

  // Requirements check for one physical device; fills `missing` with refusal lines.
  void inspect_device(VkPhysicalDevice candidate, std::vector<std::string> &missing,
                      DeviceReport &found) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(candidate, &props);
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.pNext = &v12;
    vkGetPhysicalDeviceFeatures2(candidate, &features2);
    const VkPhysicalDeviceFeatures &core = features2.features;

    found.device_name = props.deviceName;
    found.vendor_id = props.vendorID;
    found.amd = props.vendorID == amd_vendor_id;
    found.max_image_dimension_2d = props.limits.maxImageDimension2D;
    found.max_compute_workgroup_count_x = props.limits.maxComputeWorkGroupCount[0];

    if (props.apiVersion < VK_API_VERSION_1_2) missing.push_back("Vulkan 1.2 core");
    if (!v12.timelineSemaphore) missing.push_back("timelineSemaphore");
    if (!core.dualSrcBlend) missing.push_back("dualSrcBlend");
    if (!core.fragmentStoresAndAtomics) missing.push_back("fragmentStoresAndAtomics");
    for (const auto &requirement : required_formats) {
      if (!format_supports(candidate, requirement.format, requirement.flags, true))
        missing.push_back(requirement.name);
    }

    std::uint32_t extension_count = 0;
    vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> available(extension_count);
    vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extension_count, available.data());
    if (!has_extension(available, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) missing.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    bool has_graphics = false;
    std::uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
    for (const auto &family : families) has_graphics |= (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
    if (!has_graphics) missing.push_back("graphics queue");

    // Optional items (chapter 12.6): reported, not enabled in M0.
    const std::pair<const char *, bool> optional_features[] = {
        {"samplerAnisotropy", core.samplerAnisotropy != VK_FALSE},
        {"wideLines", core.wideLines != VK_FALSE},
        {"largePoints", core.largePoints != VK_FALSE},
        {"textureCompressionBC", core.textureCompressionBC != VK_FALSE},
    };
    const char *optional_extensions[] = {
        "VK_KHR_dynamic_rendering", "VK_EXT_rasterization_order_attachment_access",
        "VK_EXT_attachment_feedback_loop_layout", "VK_KHR_push_descriptor", "VK_EXT_memory_budget",
        "VK_EXT_provoking_vertex", "VK_EXT_line_rasterization", "VK_EXT_swapchain_maintenance1",
    };
    found.optional_found.clear();
    found.optional_missing.clear();
    for (const auto &[name, present] : optional_features) {
      (present ? found.optional_found : found.optional_missing).push_back(name);
    }
    for (const char *name : optional_extensions) {
      (has_extension(available, name) ? found.optional_found : found.optional_missing).push_back(name);
    }
    queue_family = 0;
    for (std::uint32_t i = 0; i < family_count; ++i) {
      if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
        queue_family = i;
        break;
      }
    }
  }

  bool choose_device(std::string &error) {
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count == 0) {
      error = "no Vulkan physical device";
      return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    VkPhysicalDevice best = VK_NULL_HANDLE;
    int best_score = -1;
    std::vector<std::string> first_refusal;
    for (VkPhysicalDevice candidate : devices) {
      std::vector<std::string> missing;
      DeviceReport found;
      inspect_device(candidate, missing, found);
      if (!missing.empty()) {
        if (first_refusal.empty()) first_refusal = missing;
        continue;
      }
      VkPhysicalDeviceProperties props{};
      vkGetPhysicalDeviceProperties(candidate, &props);
      // The launcher's Graphics card row, by name (GT4_HW_ADAPTER); no card with that name: the best one as usual.
      const char *wanted_card = std::getenv("GT4_HW_ADAPTER");
      const int score = wanted_card != nullptr && std::strcmp(wanted_card, props.deviceName) == 0 ? 3
                        : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU                ? 2
                                                                                                  : 1;
      if (score > best_score) {
        best_score = score;
        best = candidate;
      }
    }
    if (best == VK_NULL_HANDLE) {
      error = "no device has the required features; first refusal:";
      for (const auto &item : first_refusal) error += " " + item + ";";
      return false;
    }
    physical = best;
    // Re-inspect the chosen device so queue_family and the report describe it.
    std::vector<std::string> missing;
    inspect_device(physical, missing, report);
    profile = vulkan_loader::probe_device_profile(instance, physical);
    return true;
  }

  bool create_device(std::string &error) {
    const float priorities[2] = {1.0f, 1.0f};
    std::uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
    // TASK-174: the presentation path (PresentPath). GT4_HW_PRESENT_QUEUE=0: the rendering queue path on any device (A/B of IN-516).
    // GT4_HW_PRESENT_FORCE_ONE_QUEUE=1 (diagnostic): the family is taken to have one queue, so a desktop can run the path of the
    // Steam Deck. On one queue, GT4_HW_PRESENT_SHARED_QUEUE=0 keeps the rendering queue path of before TASK-174.
    const std::uint32_t family_queues = families[queue_family].queueCount;
    const bool forced_one_queue = environment_is("GT4_HW_PRESENT_FORCE_ONE_QUEUE", "1");
    const std::uint32_t usable_queues = forced_one_queue ? 1u : family_queues;
    const bool present_queue_off = environment_is("GT4_HW_PRESENT_QUEUE", "0");
    const bool shared_queue_off = environment_is("GT4_HW_PRESENT_SHARED_QUEUE", "0");
    present_path = present_queue_off   ? PresentPath::rendering_queue
                   : usable_queues >= 2 ? PresentPath::own_queue
                   : shared_queue_off   ? PresentPath::rendering_queue
                                        : PresentPath::shared_queue;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = present_path == PresentPath::own_queue ? 2u : 1u;
    queue_info.pQueuePriorities = priorities;

    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.pNext = &v12;
    features2.features.dualSrcBlend = VK_TRUE;
    features2.features.fragmentStoresAndAtomics = VK_TRUE;
    independent_blend = false;
    // CORE2 B4: pipeline statistics (fragment and primitive counts in the timing lines) where the device has them.
    VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    vkGetPhysicalDeviceFeatures2(physical, &supported);
    features2.features.pipelineStatisticsQuery = supported.features.pipelineStatisticsQuery;
    features2.features.independentBlend = supported.features.independentBlend;  // IN-481
    independent_blend = supported.features.independentBlend == VK_TRUE;
    // TASK-179: points wider than one pixel (largePoints) and the deinterlace kernel's indexed history images
    // (shaderStorageImageArrayDynamicIndexing) were used without being enabled. Enabled where the device has them; where
    // it does not, core 2 draws points as two triangles and runs the constant-index build of the kernel.
    vulkan_loader::enable_profile_features(profile, features2.features);
    // TASK-090 M4: the live present pass writes depth = 1/cw from a constant clip z, so a vertex with 0 < cw < 1 must be
    // depth-clamped instead of clipped away. Only requested in GT4_COREGS3=present; the layer refuses without it.
    depth_clamp_enabled = gt4::coregs3::present::active() && supported.features.depthClamp == VK_TRUE;
    if (depth_clamp_enabled) features2.features.depthClamp = VK_TRUE;

    // IN-345: pixel interlock lets the GPU path draw a batch of triangles in one draw (no barriers).
    // GT4_HW_INTERLOCK=0 leaves it off on a device that has it: the path of GPUs without it (most mobile GPUs), for
    // measuring what that path costs (diagnostics).
    const char *interlockSwitch = std::getenv("GT4_HW_INTERLOCK");
    pixel_interlock = gt4::hw::vulkan_loader::pixel_interlock_supported(physical) &&
                      !(interlockSwitch != nullptr && std::strcmp(interlockSwitch, "0") == 0);
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    interlock.fragmentShaderPixelInterlock = VK_TRUE;
    if (pixel_interlock) v12.pNext = &interlock;
    std::vector<const char *> extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (pixel_interlock) extensions.push_back(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
    // TASK-179: the heap budgets core 2 sizes its rings from.
    if (const char *budget = vulkan_loader::memory_budget_extension(profile)) extensions.push_back(budget);
    // IN-515: the present mode that keeps no old frames in the queue, on request (choose_present_mode).
    std::uint32_t extension_count = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> available(extension_count);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, available.data());
    const char *latest_ready_name = has_extension(available, VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME)
                                        ? VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME
                                    : has_extension(available, VK_EXT_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME)
                                        ? VK_EXT_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME
                                        : nullptr;
    const char *latest_ready_switch = std::getenv("GT4_HW_FIFO_LATEST_READY");
    if (latest_ready_switch == nullptr || std::strcmp(latest_ready_switch, "1") != 0) latest_ready_name = nullptr;
    VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR latest_ready{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR};
    if (latest_ready_name != nullptr) {
      VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      query.pNext = &latest_ready;
      vkGetPhysicalDeviceFeatures2(physical, &query);
      fifo_latest_ready = latest_ready.presentModeFifoLatestReady == VK_TRUE;
    }
    if (fifo_latest_ready) {
      extensions.push_back(latest_ready_name);
      latest_ready.pNext = features2.pNext;
      features2.pNext = &latest_ready;
    }
    VkPhysicalDevicePresentId2FeaturesKHR present_id2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR};
    VkPhysicalDevicePresentWait2FeaturesKHR present_wait2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR};
    if (want_shown_times() && frametime_shown_possible && has_extension(available, VK_KHR_PRESENT_ID_2_EXTENSION_NAME) &&
        has_extension(available, VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME)) {
      present_id2.presentId2 = VK_TRUE;
      present_wait2.presentWait2 = VK_TRUE;
      present_id2.pNext = &present_wait2;
      present_wait2.pNext = features2.pNext;
      features2.pNext = &present_id2;
      extensions.push_back(VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
      extensions.push_back(VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME);
      present_wait_enabled = true;
    }
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &features2;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue_info;
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    const VkResult result = vkCreateDevice(physical, &info, nullptr, &device);
    if (result != VK_SUCCESS) {
      error = std::string("vkCreateDevice failed: ") + vk_result_text(result);
      return false;
    }
    if (present_wait_enabled) {
      wait_for_present = reinterpret_cast<PFN_vkWaitForPresent2KHR>(
          vulkan_loader::instance_proc(instance, "vkWaitForPresent2KHR"));
    }
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    if (present_path == PresentPath::own_queue) vkGetDeviceQueue(device, queue_family, 1, &present_queue);
    if (present_path == PresentPath::shared_queue) present_queue = queue;
    const char *path_text = present_path == PresentPath::own_queue ? "its own (IN-516)"
                            : present_path == PresentPath::shared_queue
                                ? "the rendering queue, shared with the presentation thread (TASK-174)"
                                : "the rendering queue";
    const char *reason = present_queue_off  ? ", GT4_HW_PRESENT_QUEUE=0"
                         : forced_one_queue ? ", one queue forced by GT4_HW_PRESENT_FORCE_ONE_QUEUE=1"
                         : shared_queue_off && present_path == PresentPath::rendering_queue ? ", GT4_HW_PRESENT_SHARED_QUEUE=0"
                                                                                             : "";
    std::fprintf(stderr, "[hw] presentation queue: %s (graphics family %u with %u queue%s%s)\n", path_text, queue_family,
                 family_queues, family_queues == 1 ? "" : "s", reason);
    std::fprintf(stderr, "[hw] device profile: %s\n", profile_line(profile).c_str());

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family;
    vkCreateCommandPool(device, &pool_info, nullptr, &command_pool);

    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = command_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = frame_slots;
    vkAllocateCommandBuffers(device, &alloc, command_buffers);
    if (uses_pictures()) {
      vkCreateCommandPool(device, &pool_info, nullptr, &show_pool);  // TASK-146: never the GS thread's pool
      alloc.commandPool = show_pool;
      vkAllocateCommandBuffers(device, &alloc, show_commands);
    }

    for (std::uint32_t i = 0; i < frame_slots; ++i) {
      VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
      vkCreateFence(device, &fence_info, nullptr, &fences[i]);
      VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      vkCreateSemaphore(device, &semaphore_info, nullptr, &acquire_semaphores[i]);
      if (uses_pictures()) {
        vkCreateFence(device, &fence_info, nullptr, &draw_fences[i]);
        vkCreateSemaphore(device, &semaphore_info, nullptr, &picture_ready[i]);
      }
    }
    return true;
  }

  // A surface without the wanted mode takes the nearest one instead of refusing the game (the HD 620's Vulkan has no
  // mailbox, which the launcher's "Fast" vsync asks for).
  bool choose_present_mode(VkPresentModeKHR &chosen) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, modes.data());
    const auto has = [&](VkPresentModeKHR mode) { return std::find(modes.begin(), modes.end(), mode) != modes.end(); };
    const gt4::pacing::PresentMode available = gt4::pacing::available_present_mode(
        present_mode, has(VK_PRESENT_MODE_MAILBOX_KHR), has(VK_PRESENT_MODE_IMMEDIATE_KHR));
    if (available != present_mode) {
      std::fprintf(stderr, "[hw] present mode %s is not available on this surface: using %s\n",
                   gt4::pacing::present_mode_name(present_mode), gt4::pacing::present_mode_name(available));
      present_mode = available;
    }
    chosen = available == gt4::pacing::PresentMode::mailbox     ? VK_PRESENT_MODE_MAILBOX_KHR
             : available == gt4::pacing::PresentMode::immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                                                : VK_PRESENT_MODE_FIFO_KHR;
    // IN-515 (not the default): this mode shows the newest ready frame at each flip and drops the older ones, so the queue
    // empties by itself. It did not remove the waits of IN-516 on the virtual display (11.04 against 10.96 ms in the showroom),
    // and with presentations near the flip it would skip and repeat frames where FIFO keeps them all, so plain FIFO stays.
    // GT4_HW_FIFO_LATEST_READY=1 asks for it (A/B).
    if (chosen == VK_PRESENT_MODE_FIFO_KHR && fifo_latest_ready && has(VK_PRESENT_MODE_FIFO_LATEST_READY_KHR)) {
      chosen = VK_PRESENT_MODE_FIFO_LATEST_READY_KHR;
    }
    return true;
  }

  // Creates the swapchain for the current window size. oldSwapchain is never passed (chapter 13.6):
  // the caller destroys the old swapchain after waiting for its presents.
  bool create_swapchain(std::string &error) {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
    RECT client{};
    GetClientRect(window, &client);
    const auto width = static_cast<std::uint32_t>(client.right - client.left);
    const auto height = static_cast<std::uint32_t>(client.bottom - client.top);
    if (caps.currentExtent.width != 0xFFFFFFFFu) {
      extent = caps.currentExtent;
    } else {
      extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
      extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
      // Minimised window: keep no swapchain; the next present retries after a resize.
      resize_pending = true;
      return true;
    }

    std::uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, formats.data());
    // UNORM only: the PS2 output is display-referred (chapter 12.5).
    bool format_found = false;
    for (const auto &format : formats) {
      if (format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_R8G8B8A8_UNORM) {
        swapchain_format = format.format;
        format_found = true;
        break;
      }
    }
    if (!format_found) {
      error = "surface has no UNORM B8G8R8A8 or R8G8B8A8 format";
      return false;
    }

    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    choose_present_mode(mode);

    // IN-511: vsync with two images: the present's submission waits for the image on screen to be released, so all the
    // renderer's GPU work queued after it waits for the display too (a window at 6x: GS thread 8.47 ms against 6.47 with
    // three, the same as no vsync). GT4_HW_SWAPCHAIN_IMAGES=2 is the A/B.
    std::uint32_t wanted_images = mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? 2 : 3;
    if (const char *forced = std::getenv("GT4_HW_SWAPCHAIN_IMAGES")) wanted_images = static_cast<std::uint32_t>(std::atoi(forced));
    if (caps.maxImageCount != 0) wanted_images = std::min(wanted_images, caps.maxImageCount);
    wanted_images = std::max(wanted_images, caps.minImageCount);
    if (wanted_images > max_swapchain_images) wanted_images = max_swapchain_images;

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface;
    info.minImageCount = wanted_images;
    info.imageFormat = swapchain_format;
    info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // TASK-179: the pre-transform is the one the picture is drawn with (hw_present_transform.hpp). It was the surface's
    // currentTransform with the picture drawn unrotated: right only while that is the identity (always on Windows). Now
    // the identity wherever the surface takes it, and the platform's compositor turns the picture for a rotated display.
    present_transform = choose_present_transform(caps.supportedTransforms, caps.currentTransform);
    if (!present_transform.handled) {
      error = std::string("the surface takes only quarter-turn pre-transforms (current: ") +
              transform_name(caps.currentTransform) + "), which this renderer cannot draw";
      return false;
    }
    if (present_transform.pre_transform != kTransformIdentity || caps.currentTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
      std::fprintf(stderr, "[hw] swapchain pre-transform: %s (the surface's current: %s)%s\n",
                   transform_name(present_transform.pre_transform), transform_name(caps.currentTransform),
                   present_transform.upright() ? "" : "; the picture is blitted mirrored to match, the performance HUD stays off");
    }
    info.preTransform = static_cast<VkSurfaceTransformFlagBitsKHR>(present_transform.pre_transform);
    info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                              ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                              : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    info.presentMode = mode;
    if (present_wait_enabled) {
      // The device has the extensions; the surface must take them too.
      const auto capabilities2 = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
          vulkan_loader::instance_proc(instance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
      VkSurfaceCapabilitiesPresentWait2KHR wait2{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_WAIT_2_KHR};
      VkSurfaceCapabilitiesPresentId2KHR id2{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR};
      id2.pNext = &wait2;
      VkSurfaceCapabilities2KHR queried{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
      queried.pNext = &id2;
      VkPhysicalDeviceSurfaceInfo2KHR surface_info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
      surface_info.surface = surface;
      if (capabilities2 == nullptr || capabilities2(physical, &surface_info, &queried) != VK_SUCCESS || !id2.presentId2Supported ||
          !wait2.presentWait2Supported) {
        present_wait_enabled = false;
        wait_for_present = nullptr;
      }
    }
    if (present_wait_enabled) {
      info.flags |= VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR | VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR;
    }
    info.clipped = VK_TRUE;
    info.oldSwapchain = VK_NULL_HANDLE;  // RDNA4 rule (chapter 13.6): never pass the old swapchain

    const VkResult result = vkCreateSwapchainKHR(device, &info, nullptr, &swapchain);
    if (result != VK_SUCCESS) {
      error = std::string("vkCreateSwapchainKHR failed: ") + vk_result_text(result);
      return false;
    }
    vkGetSwapchainImagesKHR(device, swapchain, &image_count, nullptr);
    if (image_count > max_swapchain_images) image_count = max_swapchain_images;
    VkImage all_images[max_swapchain_images] = {};
    std::uint32_t fetched = image_count;
    vkGetSwapchainImagesKHR(device, swapchain, &fetched, all_images);
    image_count = fetched;
    std::fprintf(stderr, "[hw] swapchain: %u images (asked for %u, surface minimum %u), present mode %d\n", image_count,
                 wanted_images, caps.minImageCount, static_cast<int>(mode));
    for (std::uint32_t i = 0; i < image_count; ++i) {
      images[i] = all_images[i];
      image_layouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
      VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      vkCreateSemaphore(device, &semaphore_info, nullptr, &render_finished[i]);
    }
    if (uses_pictures() && !create_pictures()) {
      error = "could not create the presentation pictures";
      return false;
    }
    if (hud_wanted && !hud_layer) {
      const char *scale = std::getenv("GT4_HW_SCALE");
      hud_layer = std::make_unique<gt4::hw::hud::Layer>(physical, device, frame_slots, window, report.device_name, scale != nullptr ? scale : "", hud_mode);
    }
    resize_pending = false;
    return true;
  }

  // Waits for the old swapchain's presents, destroys it, creates the new one (chapter 13.6).
  bool recreate_swapchain(std::string &error) {
    pacer_drain();
    device_wait_idle();
    destroy_swapchain();
    stats.swapchain_recreations++;
    return create_swapchain(error);
  }

  void record_black(VkCommandBuffer cmd, std::uint32_t index) {
    VkImageMemoryBarrier to_clear{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_clear.oldLayout = draw_layouts[index];
    to_clear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_clear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_clear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_clear.image = draw_images[index];
    to_clear.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_clear.srcAccessMask = 0;
    to_clear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_clear);
    VkClearColorValue black{};
    vkCmdClearColorImage(cmd, draw_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                         &to_clear.subresourceRange);
    VkImageMemoryBarrier to_present = to_clear;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = draw_final_layout;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_present);
    draw_layouts[index] = draw_final_layout;
  }

  // Records one presentation: the swapchain image is cleared to black, then the composed display frame
  // is blitted into the centred rectangle that keeps its aspect ratio (chapter 12.5; the fragment output
  // stage that replaces this blit is M6).
  void record_frame(VkCommandBuffer cmd, std::uint32_t index, const hwgs::writeback::FrameOutput &frame) {
    (void)frame;
    record_black_then(cmd, index, [&] {
      VkImageMemoryBarrier to_copy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      to_copy.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      to_copy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_copy.image = upload.image;
      to_copy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, upload.levels, 0, 1};
      to_copy.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &to_copy);
      if (shownOnGpu && gpuImage) {
        // IN-364: the renderer's display image (written by its compose kernel in an earlier submission on this
        // queue) is copied into the upload image (same 32-bit texel size, RGBA8 bytes).
        VkMemoryBarrier composed{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        composed.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        composed.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &composed,
                             0, nullptr, 0, nullptr);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {upload.width, upload.height, 1};
        gt4_hwd_vkCmdCopyImage(cmd, gpuImage, VK_IMAGE_LAYOUT_GENERAL, upload.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      } else {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {upload.width, upload.height, 1};
        gt4_hwd_vkCmdCopyBufferToImage(cmd, upload.buffer, upload.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                       &region);
      }
      to_copy.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      to_copy.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      to_copy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      to_copy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &to_copy);
      // Chapter 13.6 (fixed 16:9, the presentation contract of the default renderer and gt4-gs): the composed
      // frame (640 x 448 for GT4, an anamorphic 16:9 picture with the game's forced widescreen, TASK-079) is
      // stretched into the largest centred 16:9 rectangle. Keeping the frame's own 640:448 ratio showed the
      // 16:9 game squeezed into 4:3 with side bars (owner test 3). TASK-080: when the race view was projected for
      // another display ratio (ultrawide, 32:9, 16:10) the game asks for the fill and the frame goes into the
      // largest centred rectangle of that ratio instead, as the default renderer and gt4-gs do (owner test 6 showed
      // the widened race squeezed into the 16:9 pillarbox).
      const std::uint64_t aspect = display_aspect.load(std::memory_order_relaxed);
      const bool fill = display_aspect_fill(aspect);
      const PresentRect rect = present_rect_for(extent.width, extent.height, fill, display_aspect_ratio(aspect));
      stats.present_fill = fill ? 1u : 0u;
      stats.present_fill_millionths = static_cast<std::uint32_t>(aspect >> 32);
      const std::uint32_t dw = rect.width, dh = rect.height;
      stats.present_left = rect.left;
      stats.present_top = rect.top;
      stats.present_width = rect.width;
      stats.present_height = rect.height;
      // IN-375, IN-406: halve with 2x2 box blits while the halved level still covers the destination rectangle, then
      // one bilinear reduction (present_box_halvings).
      std::uint32_t level = 0;
      std::uint32_t lw = upload.width, lh = upload.height;
      const std::uint32_t halvings = present_box_halvings(upload.width, upload.height, dw, dh, upload.levels);
      while (level < halvings) {
        VkImageBlit half{};
        half.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        half.srcOffsets[1] = {static_cast<std::int32_t>(lw), static_cast<std::int32_t>(lh), 1};
        half.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level + 1, 0, 1};
        half.dstOffsets[1] = {static_cast<std::int32_t>(lw >> 1), static_cast<std::int32_t>(lh >> 1), 1};
        gt4_hwd_vkCmdBlitImage(cmd, upload.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, upload.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &half, VK_FILTER_LINEAR);
        VkImageMemoryBarrier toSource = to_copy;
        toSource.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level + 1, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &toSource);
        ++level;
        lw >>= 1;
        lh >>= 1;
      }
      stats.present_downscale_levels = level;
      stats.present_source_width = upload.width;
      stats.present_source_height = upload.height;
      VkImageBlit blit{};
      blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
      blit.srcOffsets[1] = {static_cast<std::int32_t>(lw), static_cast<std::int32_t>(lh), 1};
      blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      // TASK-179: under a mirrored or half-turned pre-transform the blit reverses those axes (the identity: left, top to
      // left + dw, top + dh as before).
      const BlitDestination destination = present_blit_destination(rect, extent.width, extent.height, present_transform);
      blit.dstOffsets[0] = {destination.x0, destination.y0, 0};
      blit.dstOffsets[1] = {destination.x1, destination.y1, 1};
      gt4_hwd_vkCmdBlitImage(cmd, upload.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, draw_images[index],
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
      if (hud_layer && present_transform.upright()) {
        hud_layer->record(cmd, static_cast<std::uint32_t>(submissions % frame_slots), draw_images[index], swapchain_format, extent);
      }
    });
  }

  // Transition to TRANSFER_DST, clear to black, run `body`, transition to PRESENT_SRC.
  template <typename Body> void record_black_then(VkCommandBuffer cmd, std::uint32_t index, Body body) {
    VkImageMemoryBarrier to_clear{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_clear.oldLayout = draw_layouts[index];
    to_clear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_clear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_clear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_clear.image = draw_images[index];
    to_clear.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_clear.srcAccessMask = 0;
    to_clear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_clear);
    VkClearColorValue black{};
    vkCmdClearColorImage(cmd, draw_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                         &to_clear.subresourceRange);
    body();
    VkImageMemoryBarrier to_present = to_clear;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = draw_final_layout;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_present);
    draw_layouts[index] = draw_final_layout;
  }

  // The commands of the second queue for one presentation: the swapchain image takes the picture.
  void record_show(std::uint32_t slot, std::uint32_t index) {
    VkCommandBuffer show = show_commands[slot];
    vkResetCommandBuffer(show, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(show, &begin);
    VkImageMemoryBarrier to_copy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_copy.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_copy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_copy.image = images[index];
    to_copy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_copy.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(show, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_copy);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {extent.width, extent.height, 1};
    gt4_hwd_vkCmdCopyImage(show, pictures[slot], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images[index],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier to_present = to_copy;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = 0;
    vkCmdPipelineBarrier(show, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_present);
    vkEndCommandBuffer(show);
  }

  // IN-516: on the virtual display of a game stream, with vsync, a presentation holds its queue until the display takes the
  // frame (with VK_PRESENT_MODE_FIFO_LATEST_READY_KHR too), and everything submitted after it waits on that queue: GS thread
  // waits of about 11 ms per VBlank in the showroom on the owner's desktop, with waits of one or two whole refreshes (17.7 and
  // 33 ms) in races, the spikes in the frametime graph; without vsync 0.9 ms. The real monitor at 144 Hz showed none of this.
  // So the rendering queue draws the frame into a window-sized picture, and a second queue of the
  // family waits for the display, copies the picture and presents. Nothing here waits for the display: when the second
  // queue is two presentations behind, or no swapchain image is free, this frame is not shown. A device with one queue
  // keeps the path below. GT4_HW_PRESENT_QUEUE=0: that path (A/B).
  void present_on_own_queue(const hwgs::writeback::FrameOutput *frame) {
    const std::uint32_t slot = static_cast<std::uint32_t>(submissions % frame_slots);
    if (submissions >= presents_in_flight) {
      const std::uint32_t older = static_cast<std::uint32_t>((submissions - presents_in_flight) % frame_slots);
      if (gs_waits([&] { return vkWaitForFences(device, 1, &fences[older], VK_TRUE, 0); }) != VK_SUCCESS) {
        ++presents_dropped;
        return;
      }
    }
    gs_waits([&] { return vkWaitForFences(device, 1, &fences[slot], VK_TRUE, UINT64_MAX); });  // older than `older`: already done
    std::uint32_t index = 0;
    const VkResult acquired = gs_waits([&] { return acquire_image(0, acquire_semaphores[slot], index); });
    note_present_result(acquired, Rebuild::acquire_out_of_date);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
      ++presents_failed;
      return;
    }
    if (acquired == VK_NOT_READY || acquired == VK_TIMEOUT) {
      ++presents_dropped;
      return;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {  // suboptimal: the image is acquired (TASK-145)
      ++presents_failed;
      return;
    }
    vkResetFences(device, 1, &fences[slot]);
    gs_waits([&] { return vkWaitForFences(device, 1, &draw_fences[slot], VK_TRUE, UINT64_MAX); });
    vkResetFences(device, 1, &draw_fences[slot]);

    // The rendering queue: the picture.
    VkCommandBuffer cmd = command_buffers[slot];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    draw_images = pictures;
    draw_layouts = picture_layouts;
    draw_final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const auto upload_start = std::chrono::steady_clock::now();
    if (frame && frame->valid && ensure_upload(frame->width, frame->height)) {
      if (!shownOnGpu) {
        // The staging buffer is reused across presents: the previous picture's copy finishes first.
        const std::uint32_t previous = (slot + frame_slots - 1u) % frame_slots;
        gs_waits([&] { return vkWaitForFences(device, 1, &draw_fences[previous], VK_TRUE, UINT64_MAX); });
        std::memcpy(upload.mapped, frame->pixels.data(), frame->pixels.size() * sizeof(std::uint32_t));
      }
      record_frame(cmd, slot, *frame);
    } else {
      record_black(cmd, slot);
    }
    uploadUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - upload_start).count();
    vkEndCommandBuffer(cmd);
    VkSubmitInfo draw{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    draw.commandBufferCount = 1;
    draw.pCommandBuffers = &cmd;
    draw.signalSemaphoreCount = 1;
    draw.pSignalSemaphores = &picture_ready[slot];
    gs_waits([&] { return submit_to(queue, draw, draw_fences[slot], QueueCaller::gs_thread); });

    // The second queue: waits for the swapchain image, copies the picture, presents.
    record_show(slot, index);
    VkCommandBuffer show = show_commands[slot];
    const VkSemaphore waits[2] = {acquire_semaphores[slot], picture_ready[slot]};
    const VkPipelineStageFlags wait_stages[2] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 2;
    submit.pWaitSemaphores = waits;
    submit.pWaitDstStageMask = wait_stages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &show;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_finished[index];
    gs_waits([&] { return submit_to(present_queue, submit, fences[slot], QueueCaller::gs_thread); });
    ++submissions;
    if (submissions % 600 == 0 && presents_dropped != presents_dropped_reported) {
      std::fprintf(stderr, "[hw] presentations not shown (the display was behind): %llu\n", static_cast<unsigned long long>(presents_dropped));
      presents_dropped_reported = presents_dropped;
    }

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_finished[index];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &index;
    const VkResult result = gs_waits([&] { return queue_present(present_queue, present); });
    note_present_result(result, Rebuild::present_out_of_date);
    if (frame == nullptr) stats.black_presents++;  // presented without a composed image (chapter 12.4)
  }

  // TASK-082: with frames being left out, pictures arrive at uneven times, and showing them from the GS thread would hold up the
  // next one. So the GS thread only draws the picture, and this thread shows it at its place (hw_present_spacing.hpp), on the second
  // queue of the family. Only then: at 60 frames per second the path above stays as it is.
  // TASK-174: on the shared queue this thread shows every picture: at once (`spaced` false), or at its place with frames left out.
  struct PacedPicture {
    std::uint32_t slot;
    std::chrono::steady_clock::time_point arrival;
    bool spaced;
    // TASK-090 host clock: the exact steady-clock time to show it, in microseconds since the epoch (0: use `spaced`).
    std::int64_t show_us = 0;
  };
  std::thread pacer_thread;
  std::mutex pacer_mutex;
  std::condition_variable pacer_changed;
  std::deque<PacedPicture> pacer_queue;
  bool pacer_busy = false, pacer_stop = false;
  std::uint32_t pacer_slots_held = 0;  // TASK-174: one bit per slot handed over and not finished (hw_shared_present.hpp)
  gt4::hw::PresentSpacing pacer_spacing;
  static constexpr std::size_t kPacedPictures = 2;
  static constexpr std::uint32_t kNoSlot = UINT32_MAX;

  // TASK-174: on the shared queue a present that finds the display's queue full blocks inside vkQueuePresentKHR while it holds
  // queue_lock, and the GS thread's next submission waits as long: 6 to 22 ms per present on the RX 9060 XT's Windows driver in
  // FIFO, in hw_gs_smoke with frames coming faster than the display takes them (Hypothesis, to check on the Steam Deck: Mesa's X11
  // swapchain presents from a thread of its own in FIFO and waits in the acquire, which the presentation thread takes, so its
  // present does not block). The presentation report shows it (gs_lock_longest_ms, presenter_longest_ms).
  // GT4_HW_PRESENT_SHARED_WAIT=<n> (diagnostic, off by default) has the presentation thread first wait, outside the lock, until no
  // more than n - 1 of its presentations are still on their way to the display (shown times of VK_KHR_present_wait2, from the
  // frametime thread; at most kShownWaitLimit, so a lost shown time never stops the pictures). Not the default: a presentation took
  // 12 to 14 ms to be shown in a window at 144 Hz (two refreshes), so n = 1 halved the frames shown when the display was the limit
  // (589 against 1,056 of 1,200 at 144 frames per second) and n = 2 cost a quarter, while they bounded the GS thread's longest wait
  // for the lock at 1.2 and 2.8 ms (20 ms without). At 60 Hz with two refreshes to the display, n = 1 would hold 30 frames a second.
  const std::uint64_t shared_presents_on_the_way = [] {
    const char *text = std::getenv("GT4_HW_PRESENT_SHARED_WAIT");
    return text == nullptr ? std::uint64_t{0} : static_cast<std::uint64_t>(std::strtoull(text, nullptr, 10));
  }();
  static constexpr auto kShownWaitLimit = std::chrono::milliseconds(50);
  std::atomic<std::uint64_t> presenter_display_wait_ns{0};  // presentation thread: this wait and the acquire, session total

  void wait_for_previous_shown() {
    if (!shares_queue() || shared_presents_on_the_way == 0 || !present_wait_enabled || wait_for_present == nullptr) return;
    if (!frametime_thread.joinable()) return;  // no presentation with an id yet
    // Ids are given in queue_present_timed, on this thread here.
    if (present_ids < shared_presents_on_the_way) return;
    const std::uint64_t previous = present_ids - (shared_presents_on_the_way - 1);
    std::unique_lock<std::mutex> lock(frametime_mutex);
    frametime_changed.wait_for(lock, kShownWaitLimit, [&] { return frametime_done_id >= previous || frametime_stop; });
  }

  void pacer_worker() {
    std::uint32_t finished_slot = kNoSlot;
    for (;;) {
      PacedPicture picture{};
      std::int64_t whenUs = 0;
      {
        std::unique_lock<std::mutex> lock(pacer_mutex);
        pacer_busy = false;
        if (finished_slot != kNoSlot) pacer_slots_held &= ~(1u << finished_slot);  // its last queue call returned
        pacer_changed.notify_all();
        pacer_changed.wait(lock, [&] { return pacer_stop || !pacer_queue.empty(); });
        if (pacer_queue.empty()) return;
        picture = pacer_queue.front();
        pacer_queue.pop_front();
        pacer_busy = true;
        finished_slot = picture.slot;
        if (picture.show_us != 0) {
          whenUs = picture.show_us;
        } else if (picture.spaced) {
          whenUs = pacer_spacing.place(std::chrono::duration_cast<std::chrono::microseconds>(picture.arrival.time_since_epoch()).count(),
                                       !pacer_queue.empty());
        }
      }
      if (picture.spaced || picture.show_us != 0) {
        const auto when = std::chrono::steady_clock::time_point(std::chrono::microseconds(whenUs));
        // The system's sleep is too coarse for this: the last stretch spins.
        std::this_thread::sleep_until(when - std::chrono::milliseconds(2));
        while (std::chrono::steady_clock::now() < when) std::this_thread::yield();
      }
      show_paced_picture(picture.slot);
    }
  }

  // Nothing may be on its way while the swapchain changes, or when the other path takes over.
  void pacer_drain() {
    if (!pacer_thread.joinable()) return;
    std::unique_lock<std::mutex> lock(pacer_mutex);
    pacer_changed.wait(lock, [&] { return pacer_queue.empty() && !pacer_busy; });
  }

  // Shows what is still queued, then ends the thread.
  void pacer_close() {
    if (!pacer_thread.joinable()) return;
    {
      const std::lock_guard<std::mutex> lock(pacer_mutex);
      pacer_stop = true;
    }
    pacer_changed.notify_all();
    pacer_thread.join();
  }

  // On the presentation thread: waits for a swapchain image and shows the picture of `slot`. Its semaphore was signalled by the
  // drawing, so it is waited on even when nothing can be shown. On the shared queue every queue call here holds queue_lock
  // (submit_to, present_locked); the acquire does not, so the GS thread's submissions go on while this thread waits for the display.
  void show_paced_picture(std::uint32_t slot) {
    vkWaitForFences(device, 1, &fences[slot], VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &fences[slot]);
    const auto display_wait_start = std::chrono::steady_clock::now();
    wait_for_previous_shown();
    std::uint32_t index = 0;
    const VkResult acquired = acquire_image(100000000, acquire_semaphores[slot], index);
    presenter_display_wait_ns += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - display_wait_start).count());
    note_present_result(acquired, Rebuild::acquire_out_of_date);
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {  // suboptimal: the image is acquired (TASK-145)
      if (acquired == VK_TIMEOUT || acquired == VK_NOT_READY) {
        ++presents_dropped;  // no swapchain image within 100 ms: the display was behind
      } else {
        ++presents_failed;
      }
      const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      VkSubmitInfo release{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      release.waitSemaphoreCount = 1;
      release.pWaitSemaphores = &picture_ready[slot];
      release.pWaitDstStageMask = &stage;
      submit_to(present_queue, release, fences[slot], QueueCaller::presentation_thread);
      return;
    }
    record_show(slot, index);
    const VkSemaphore waits[2] = {acquire_semaphores[slot], picture_ready[slot]};
    const VkPipelineStageFlags wait_stages[2] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 2;
    submit.pWaitSemaphores = waits;
    submit.pWaitDstStageMask = wait_stages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &show_commands[slot];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_finished[index];
    submit_to(present_queue, submit, fences[slot], QueueCaller::presentation_thread);
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_finished[index];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &index;
    const VkResult result = queue_present(present_queue, present);
    note_present_result(result, Rebuild::present_out_of_date);
  }

  // On the GS thread: draws the frame into the picture of `slot` on the rendering queue (signals the slot's picture_ready and
  // draw_fences). The caller made sure nothing still reads that picture.
  void draw_picture(std::uint32_t slot, const hwgs::writeback::FrameOutput *frame) {
    gs_waits([&] { return vkWaitForFences(device, 1, &draw_fences[slot], VK_TRUE, UINT64_MAX); });
    vkResetFences(device, 1, &draw_fences[slot]);
    VkCommandBuffer cmd = command_buffers[slot];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    draw_images = pictures;
    draw_layouts = picture_layouts;
    draw_final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const auto upload_start = std::chrono::steady_clock::now();
    if (frame && frame->valid && ensure_upload(frame->width, frame->height)) {
      if (!shownOnGpu) {
        // The staging buffer is reused across presents: the previous picture's copy finishes first.
        const std::uint32_t previous = (slot + frame_slots - 1u) % frame_slots;
        gs_waits([&] { return vkWaitForFences(device, 1, &draw_fences[previous], VK_TRUE, UINT64_MAX); });
        std::memcpy(upload.mapped, frame->pixels.data(), frame->pixels.size() * sizeof(std::uint32_t));
      }
      record_frame(cmd, slot, *frame);
    } else {
      record_black(cmd, slot);
    }
    uploadUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - upload_start).count();
    vkEndCommandBuffer(cmd);
    VkSubmitInfo draw{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    draw.commandBufferCount = 1;
    draw.pCommandBuffers = &cmd;
    draw.signalSemaphoreCount = 1;
    draw.pSignalSemaphores = &picture_ready[slot];
    gs_waits([&] { return submit_to(queue, draw, draw_fences[slot], QueueCaller::gs_thread); });
  }

  // On the GS thread: the drawn picture of `slot` goes to the presentation thread.
  void hand_over(std::uint32_t slot, bool spaced) {
    {
      const std::lock_guard<std::mutex> lock(pacer_mutex);
      pacer_queue.push_back(PacedPicture{slot, std::chrono::steady_clock::now(), spaced, 0});
      pacer_slots_held |= 1u << slot;
    }
    pacer_changed.notify_all();
  }

  // TASK-090 host clock: the drawn picture of `slot` goes to the presentation thread, to be shown at `show_us`.
  void hand_over_at(std::uint32_t slot, std::int64_t show_us) {
    {
      const std::lock_guard<std::mutex> lock(pacer_mutex);
      pacer_queue.push_back(PacedPicture{slot, std::chrono::steady_clock::now(), false, show_us});
      pacer_slots_held |= 1u << slot;
    }
    pacer_changed.notify_all();
  }

  void report_presents_dropped() {
    if (submissions % 600 == 0 && presents_dropped != presents_dropped_reported) {
      std::fprintf(stderr, "[hw] presentations not shown (the display was behind): %llu\n", static_cast<unsigned long long>(presents_dropped));
      presents_dropped_reported = presents_dropped;
    }
  }

  // Under pacer_mutex, on the GS thread: may the picture of `slot` be drawn and handed over (hw_shared_present.hpp)? The slot's
  // fence is asked with timeout 0, and only when the presentation thread no longer holds the slot.
  bool may_hand_over(std::uint32_t slot, bool spaced) {
    const gt4::hw::PresenterLoad load{pacer_queue.size(), pacer_busy, pacer_slots_held};
    const auto copy_done = [&] {
      return gs_waits([&] { return vkWaitForFences(device, 1, &fences[slot], VK_TRUE, 0); }) == VK_SUCCESS;
    };
    return gt4::hw::hand_over_decision(load, slot, spaced, kPacedPictures, presents_in_flight, copy_done) ==
           gt4::hw::HandOverVerdict::draw;
  }

  // On the GS thread, second queue (TASK-082): draws the picture and hands it over. The display being behind costs a picture here,
  // before any drawing: two pictures waiting for their place, or (TASK-174 review) the copy of the slot's last picture, on the
  // second queue, not run yet. Before that check the drawing could overwrite a picture the second queue had still to copy.
  void present_paced(const hwgs::writeback::FrameOutput *frame) {
    if (!pacer_thread.joinable()) pacer_thread = std::thread([this] { pacer_worker(); });
    const std::uint32_t slot = static_cast<std::uint32_t>(submissions % frame_slots);
    {
      const std::lock_guard<std::mutex> lock(pacer_mutex);
      pacer_spacing.note_arrival(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
      if (!may_hand_over(slot, true)) {
        ++presents_dropped;
        return;
      }
    }
    draw_picture(slot, frame);
    ++submissions;
    hand_over(slot, true);
    if (frame == nullptr) stats.black_presents++;
  }

  // TASK-174: the path of IN-516 on a device with one queue in its graphics family (RADV on the Steam Deck, GitHub issue #204).
  // There the GS thread used to acquire the swapchain image (waiting as long as the display needed) and present, and those waits
  // pushed drawn frames past 16.7 ms. Now the GS thread draws the frame into the slot's picture on the rendering queue and hands it
  // to the presentation thread, which waits for a swapchain image, copies the picture into it and presents (show_paced_picture), on
  // the same queue: every queue call of both threads, the renderer's submissions included, holds queue_lock. Nothing here waits for
  // the display: when the presentation thread already holds its pictures (hand_over_decision), or the copy of the slot's last picture
  // has not run on the GPU yet, this frame is not shown, and counted. `spaced`: frames left out (TASK-082), shown at their place.
  void present_on_shared_queue(const hwgs::writeback::FrameOutput *frame, bool spaced) {
    if (!pacer_thread.joinable()) pacer_thread = std::thread([this] { pacer_worker(); });
    const std::uint32_t slot = static_cast<std::uint32_t>(submissions % frame_slots);
    {
      const std::lock_guard<std::mutex> lock(pacer_mutex);
      if (spaced) {
        pacer_spacing.note_arrival(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
      }
      // The slot's fence is signalled once the copy of its last picture ran (and, on one queue, everything submitted before it, the
      // slot's last drawing included).
      if (!may_hand_over(slot, spaced)) {
        ++presents_dropped;
        return;
      }
    }
    draw_picture(slot, frame);
    ++submissions;
    hand_over(slot, spaced);
    report_presents_dropped();
    if (frame == nullptr) stats.black_presents++;
  }

  // TASK-090 host clock, on the GS thread: draws `frame`'s picture into a slot of the rendering queue and hands it to
  // the presentation thread for an exact show time. The presenter shows at most kHostClockQueueLimit pictures; when it is
  // behind, or the slot's previous picture is still with it, this tick is not shown and counted. Nothing waits for the
  // display here, so the guest VBlank path keeps its cost.
  void present_host_clock_path(const hwgs::writeback::FrameOutput *frame, std::int64_t show_us) {
    if (!pacer_thread.joinable()) pacer_thread = std::thread([this] { pacer_worker(); });
    // Any slot whose picture the presentation thread already finished is usable; the round-robin start spreads the
    // pictures of one guest frame over the slots instead of always taking the first free one.
    std::uint32_t slot = kNoSlot;
    {
      const std::lock_guard<std::mutex> lock(pacer_mutex);
      const gt4::hw::PresenterLoad load{pacer_queue.size(), pacer_busy, pacer_slots_held};
      const std::size_t on_the_way = load.queued + (load.busy ? 1u : 0u);
      if (on_the_way < kHostClockQueueLimit) {
        for (std::uint32_t attempt = 0; attempt < frame_slots; ++attempt) {
          const std::uint32_t candidate = static_cast<std::uint32_t>((submissions + attempt) % frame_slots);
          if (((load.slots_held >> candidate) & 1u) != 0) continue;
          if (gs_waits([&] { return vkWaitForFences(device, 1, &fences[candidate], VK_TRUE, 0); }) != VK_SUCCESS)
            continue;
          slot = candidate;
          break;
        }
      }
      if (slot == kNoSlot) {
        ++presents_dropped;
        ++stats.host_clock_dropped;
        return;
      }
    }
    draw_picture(slot, frame);
    ++submissions;
    hand_over_at(slot, show_us);
    ++stats.host_clock_pictures;
    report_presents_dropped();
    if (frame == nullptr) stats.black_presents++;
  }

  // One presentation per guest VBlank: the composed display frame, or black without one (chapter 12.5).
  void present_frame(const hwgs::writeback::FrameOutput *frame) {
    if (!swapchain) return;
    ++present_calls;
    if (latency_control_on) {
      if (const std::int32_t shift = latency_control.take_grid_shift_us()) {
        gt4::pacing::grid_shift_us.fetch_add(shift, std::memory_order_relaxed);
        std::fprintf(stderr, "[hw] latency control: frames miss the flip, grid %+.1f ms at call %llu\n", shift / 1000.0,
                     static_cast<unsigned long long>(present_calls));
      }
    }
    // TASK-082: with frames being left out the queue does not grow: one frame in, at most one out.
    const bool frames_left_out = gt4::pacing::frame_skip_recent.load(std::memory_order_relaxed);
    if (latency_control_on && latency_control.leave_out() && !frames_left_out) {
      std::fprintf(stderr, "[hw] latency control: a presentation left out at call %llu (%llu so far), %.1f ms to the display, target %.1f ms\n",
                   static_cast<unsigned long long>(present_calls), static_cast<unsigned long long>(latency_control.left_out()),
                   latency_control.last_lag_ms(), latency_control.target_ms());
      ++present_requests;
      ++presents_left_out;
      return;
    }
    ++present_requests;
    const auto step_start = std::chrono::steady_clock::now();
    present_on_path(frame, frames_left_out);
    gs_present_step_us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - step_start).count();
  }

  void present_on_path(const hwgs::writeback::FrameOutput *frame, bool frames_left_out) {
    if (shares_queue()) {
      present_on_shared_queue(frame, frames_left_out);
      return;
    }
    if (present_queue && frames_left_out) {
      present_paced(frame);
      return;
    }
    pacer_drain();
    if (present_queue) {
      present_on_own_queue(frame);
      return;
    }
    present_on_rendering_queue(frame);
  }

  // The path of a device with one queue before TASK-174 (GT4_HW_PRESENT_SHARED_QUEUE=0, GT4_HW_PRESENT_QUEUE=0): the GS thread
  // waits for a swapchain image, draws into it and presents, on the rendering queue.
  void present_on_rendering_queue(const hwgs::writeback::FrameOutput *frame) {
    const std::uint32_t slot = static_cast<std::uint32_t>(submissions % frame_slots);
    // At most presents_in_flight submissions outstanding: wait for the one issued two ago.
    if (submissions >= presents_in_flight) {
      const std::uint32_t older = static_cast<std::uint32_t>((submissions - presents_in_flight) % frame_slots);
      gs_waits([&] { return vkWaitForFences(device, 1, &fences[older], VK_TRUE, UINT64_MAX); });
    }
    gs_waits([&] { return vkWaitForFences(device, 1, &fences[slot], VK_TRUE, UINT64_MAX); });

    std::uint32_t index = 0;
    const VkResult acquired = gs_waits([&] { return acquire_image(UINT64_MAX, acquire_semaphores[slot], index); });
    note_present_result(acquired, Rebuild::acquire_out_of_date);
    // Out of date: nothing was submitted, so the slot fence stays signalled and the slot stays reusable. Suboptimal: the
    // image is acquired and still presentable (TASK-145).
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
      ++presents_failed;
      return;
    }
    vkResetFences(device, 1, &fences[slot]);

    VkCommandBuffer cmd = command_buffers[slot];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    const auto upload_start = std::chrono::steady_clock::now();
    if (frame && frame->valid && ensure_upload(frame->width, frame->height)) {
      if (!shownOnGpu) {
        // The staging buffer is reused across presents: the previous present's copy finishes first (its fence;
        // IN-355: not the whole device, which would also wait for the frame the renderer just submitted).
        if (submissions > 0) {
          const std::uint32_t previous = static_cast<std::uint32_t>((submissions - 1) % frame_slots);
          gs_waits([&] { return vkWaitForFences(device, 1, &fences[previous], VK_TRUE, UINT64_MAX); });
        }
        std::memcpy(upload.mapped, frame->pixels.data(), frame->pixels.size() * sizeof(std::uint32_t));
      }
      record_frame(cmd, index, *frame);
    } else {
      record_black(cmd, index);
    }
    uploadUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - upload_start).count();
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &acquire_semaphores[slot];
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_finished[index];
    gs_waits([&] { return submit_to(queue, submit, fences[slot], QueueCaller::gs_thread); });
    ++submissions;

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_finished[index];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &index;
    const VkResult result = gs_waits([&] { return queue_present(queue, present); });
    note_present_result(result, Rebuild::present_out_of_date);
    if (frame == nullptr) stats.black_presents++;  // presented without a composed image (chapter 12.4)
  }
};

// 06.3 `normal` half-pixel mode: m = s + 0.2 (s even), s + 0.3 (s odd with bit 1 clear), s + 0.1 (otherwise).
static float scale_offset_normal(std::uint32_t scale) {
  if (scale % 2u == 0u) return static_cast<float>(scale) + 0.2f;
  if ((scale & 2u) == 0u) return static_cast<float>(scale) + 0.3f;
  return static_cast<float>(scale) + 0.1f;
}

Backend::Backend() : impl_(std::make_unique<Impl>()) {}
Backend::~Backend() = default;

bool Backend::init(const Options &options, std::string &error) {
  Impl &s = *impl_;
  if (s.initialised) {
    error = "HWR backend already initialised";
    return false;
  }
  s.options = options;
  try {
    s.present_mode = gt4::pacing::parse_present_mode(std::getenv("GT4_PRESENT_MODE"));
  } catch (const std::invalid_argument &problem) {
    error = std::string("GT4_PRESENT_MODE: ") + problem.what();
    return false;
  }
  if (!s.create_instance(error)) return false;
  if (!s.choose_device(error)) {
    s.shutdown();
    return false;
  }
  if (!s.create_device(error)) {
    s.shutdown();
    return false;
  }
  // M4: the GS front end and the write-back drawer run on this device (no silent fallback on failure).
  s.renderer = std::make_unique<hwgs::writeback::Renderer>();
  hwgs::gpu::VulkanHandles handles{s.instance, s.physical, s.device, s.queue, s.queue_family};
  handles.pixelInterlock = s.pixel_interlock;
  handles.independentBlend = s.independent_blend;
  handles.dualSourceBlend = true;  // a required feature of this backend
  handles.queueLock = s.shares_queue() ? &s.queue_lock : nullptr;  // TASK-174: the renderer's submissions share the queue
  handles.profile = s.profile;      // TASK-179
  {
    // M12 (06.1, IN-371): GT4_HW_SCALE clamped to the device ceiling s_max = max(1, floor(maxImageDimension2D /
    // 1280)); the 06.3 half-pixel mode sets the vertex shift factor (native: s, normal: m, off: 1).
    const std::uint32_t ceiling = std::max<std::uint32_t>(1u, s.report.max_image_dimension_2d / 1280u);
    std::uint32_t scale = options.scale;
    if (scale > ceiling) {
      std::fprintf(stderr, "[hw] GT4_HW_SCALE=%u is above this device's ceiling %u; using %u\n", scale, ceiling,
                   ceiling);
      scale = ceiling;
    }
    float shift = static_cast<float>(scale);
    if (options.half_pixel == HalfPixel::off) {
      shift = 1.0f;
    } else if (options.half_pixel == HalfPixel::normal) {
      shift = scale_offset_normal(scale);
    }
    s.renderer->SetScale(scale, shift);
  }
  if (!s.renderer->Init(handles, error)) {
    s.shutdown();
    return false;
  }
  // TASK-090 M4: the debug-only live layer. A device or format refusal disables it and is reported; the game runs on.
  if (gt4::coregs3::present::active()) {
    gt4::hw::coregs3::PresentConfig config;
    config.instance = s.instance;
    config.physical = s.physical;
    config.device = s.device;
    config.queue = s.queue;
    config.queue_family = s.queue_family;
    config.depth_clamp_enabled = s.depth_clamp_enabled;
    s.coregs3_present = std::make_unique<gt4::hw::coregs3::PresentLayer>();
    std::string present_error;
    if (!s.coregs3_present->init(config, present_error)) {
      std::fprintf(stderr, "[coregs3] live present refused: %s\n", present_error.c_str());
      s.coregs3_present.reset();
    } else {
      // TASK-090 2D overlay split: the present layer wants the frame's 3D image and its 2D layer separately. A no-op
      // for every other mode (the faithful renderers never set it), and GT4_COREGS3_2D_OVERLAY=0 is the A/B.
      s.renderer->SetCoregs3Overlay(true);
    }
  }
  // TASK-090 host clock: above 60 Hz the window presents each due presentation tick at the requested display rate. It
  // needs the pictures path (the presentation thread shows a picture at its scheduled time); GT4_COREGS3_HOST_CLOCK=0 is
  // the A/B switch that keeps the once-per-guest-VBlank present.
  if (s.coregs3_present) {
    const char *host_clock_text = std::getenv("GT4_COREGS3_HOST_CLOCK");
    const bool host_clock_disabled = host_clock_text != nullptr && std::strcmp(host_clock_text, "0") == 0;
    const gt4::coregs3::present::TimelineConfig &timeline = gt4::coregs3::present::timeline_config();
    s.coregs3_host_clock = gt4::coregs3::present::host_clock_enabled() && !host_clock_disabled &&
                           timeline.selected_tick == UINT32_MAX && s.uses_pictures();
    if (timeline.hz > 60) {
      if (s.coregs3_host_clock) {
        std::fprintf(stderr,
                     "[coregs3] host clock: on, %u Hz presentation (%zu pictures in flight, one guest frame of lag)\n",
                     timeline.hz, Impl::kHostClockQueueLimit);
      } else {
        std::fprintf(stderr, "[coregs3] host clock: off (%s); the window presents once per guest VBlank\n",
                     host_clock_disabled ? "GT4_COREGS3_HOST_CLOCK=0"
                                         : (timeline.selected_tick != UINT32_MAX
                                                ? "GT4_COREGS3_PRESENT_TICK diagnostic"
                                                : "the presentation path has no picture queues"));
      }
    }
  }
  // Output stage options (13.4 GT4_HW_DEINTERLACE, 13.2 GT4_HW_ANTIBLUR).
  hwgs::writeback::OutputSettings output;
  switch (options.deinterlace.algorithm) {
    case DeinterlaceAlgorithm::automatic: output.deinterlace = hwgs::display::DeinterlaceMode::Auto; break;
    case DeinterlaceAlgorithm::off: output.deinterlace = hwgs::display::DeinterlaceMode::Off; break;
    case DeinterlaceAlgorithm::weave: output.deinterlace = hwgs::display::DeinterlaceMode::Weave; break;
    case DeinterlaceAlgorithm::bob: output.deinterlace = hwgs::display::DeinterlaceMode::Bob; break;
    case DeinterlaceAlgorithm::blend: output.deinterlace = hwgs::display::DeinterlaceMode::Blend; break;
    case DeinterlaceAlgorithm::adaptive: output.deinterlace = hwgs::display::DeinterlaceMode::Adaptive; break;
  }
  output.bottomFieldFirst = options.deinterlace.bottom_field_first;
  output.antiBlur = options.antiblur;
  s.renderer->SetOutputSettings(output);
  // GT4_HW_SYNC_MODE (12.8): `cache` keeps resident targets on the GPU (M7, IN-340), `writeback` is the
  // reference mode.
  s.renderer->SetSyncMode(options.sync_mode == SyncMode::cache);
  // GT4_HW_GLM (12.8, chapter 04.9): textures over GPU-newer targets decode from the GPU memory mirror.
  s.renderer->SetGlm(options.glm);
  // GT4_HW_GPU_TIMING (12.8): GPU timestamps per category, reported on the GT4_HW_PROFILE_EVERY line.
  s.renderer->SetGpuTiming(options.gpu_timing);
  s.renderer->SetDecodeReuse(options.decode_reuse);
  // 13.5 GT4_HW_POSTFX=fxaa (IN-376): progressive presentation frames (races) pass through the edge-only FXAA.
  s.renderer->SetFxaa((options.postfx & postfx_fxaa) != 0);
  // IN-433: GT4_HW_FILTER=forced / forced_no_sprite (core 2).
  s.renderer->SetForcedBilinear(options.filter == Filter::forced ? 1u : options.filter == Filter::forced_no_sprite ? 2u : 0u);
  s.renderer->SetClutDefer(options.clut_defer);
  // IN-383 (parity plan item 2): GT4's tone-sequence strip batches run as one dispatch per page; GT4_HW_VERIFY_SHUFFLE
  // runs both paths and compares the written pages (09.4).
  s.renderer->SetToneHle(options.tone_hle);
  s.renderer->SetVerifyShuffle(options.verify_shuffle);
  // IN-358 diagnostic (not a 12.8 option): GT4_HW_CHUNKS_PER_SUBMIT=<n> mid-frame submission interval (0 = off).
  if (const char *chunks = std::getenv("GT4_HW_CHUNKS_PER_SUBMIT")) {
    s.renderer->SetChunksPerSubmission(static_cast<std::uint32_t>(std::strtoul(chunks, nullptr, 10)));
  }
  s.initialised = true;
  return true;
}

void Backend::set_stream_event_handler(StreamEventHandler handler, void *context) {
  impl_->stream_handler = handler;
  impl_->stream_context = context;
}

bool Backend::prepare_shaders(const std::function<void(std::size_t, std::size_t)> &progress, std::string &error) {
  return impl_->renderer->PrepareCore2Shaders(progress, error);
}

bool Backend::gif_transfer(int path, const std::uint8_t *bytes, std::size_t byte_count) {
  if (path < 1 || path > 3) return false;
  impl_->stats.gif_bytes += byte_count;
  return impl_->renderer && impl_->renderer->GifTransfer(static_cast<std::uint32_t>(path), bytes, byte_count);
}

bool Backend::gif_busy(int path) const {
  return path >= 1 && path <= 3 && impl_->renderer &&
         impl_->renderer->GifBusy(static_cast<std::uint32_t>(path));
}

void Backend::reset_gif_paths() {
  if (impl_->renderer) impl_->renderer->GifReset(0x7u);
}

bool Backend::write_privileged(std::uint32_t address, std::uint64_t value) {
  if (address < privileged_base || address > privileged_end || (address & 7u) != 0) {
    impl_->stats.faults++;
    return false;
  }
  impl_->privileged[(address - privileged_base) / 8] = value;
  impl_->stats.privileged_writes++;
  // Display registers feed the composed frame (chapter 12.4); other addresses are the donor's.
  if (impl_->renderer) impl_->renderer->WritePrivileged(address, value);
  return true;
}

std::uint64_t Backend::read_privileged(std::uint32_t address) const {
  if (address < privileged_base || address > privileged_end || (address & 7u) != 0) return 0;
  return impl_->privileged[(address - privileged_base) / 8];
}

void Backend::reset_context() {
  if (impl_->renderer) impl_->renderer->ResetGs();
}

void Backend::clear_vram() {
  if (impl_->renderer) impl_->renderer->ClearVram();
}

std::size_t Backend::read_transfer_fifo(void *destination, std::size_t qwords) {
  // LOCAL->HOST data of the most recent TRXDIR = 1 transfer (chapter 10.2); zeros beyond it.
  const std::size_t bytes = impl_->renderer ? impl_->renderer->ReadFifo(destination, qwords * 16) : 0;
  std::memset(static_cast<std::uint8_t *>(destination) + bytes, 0, qwords * 16 - bytes);
  return qwords;
}

const std::uint8_t *Backend::map_vram(std::uint32_t offset, std::uint32_t size) {
  if (!impl_->renderer || static_cast<std::uint64_t>(offset) + size > 4u * 1024u * 1024u) return nullptr;
  impl_->renderer->Flush();  // coherent with the recorded work (chapter 12.3)
  return impl_->renderer->Slm() + offset;
}

void Backend::flush() {
  impl_->stats.flushes++;
  if (impl_->renderer) impl_->renderer->Flush();  // VBlank commit and batch flush (chapter 02.8)
  // TASK-090 live world: the frame-end GS local-memory base over the measured world range, the same role the `.gt4vram`
  // vblank record plays offline. The per-job records take precedence; a page no job window covers falls back to this
  // image (computed after the flush, so the shadow is current). Only in the live world mode.
  if (impl_->renderer && gt4::coregs3::live_world::active()) {
    gt4::coregs3::live_world::note_vram_base(impl_->stats.flushes, 0x2BC0u, 0x1440u, impl_->renderer->Coregs3Slm());
  }
}

void Backend::coregs3_snapshot(std::uint32_t kind, std::uint32_t pass, std::uint32_t label, std::uint64_t vblank,
                               const gt4::coregs3::vram::Window *windows, std::uint32_t window_count) {
  if (impl_->renderer) impl_->renderer->Coregs3Snapshot(kind, pass, label, vblank, windows, window_count);
}

void Backend::coregs3_live_textures(std::uint64_t job, std::uint64_t vblank,
                                    const gt4::coregs3::vram::Window *windows, std::uint32_t window_count) {
  // The GS worker owns the shadow local memory at this point (the copy runs before the job's records execute), so its
  // view is current without a flush; `Slm()` never writes or submits work.
  if (!impl_->renderer) return;
  gt4::coregs3::live_world::note_job_textures(job, vblank, windows, window_count, impl_->renderer->Coregs3Slm());
}

void Backend::next_frame() { impl_->stats.frames++; }

void Backend::coregs3_guest_frame() { impl_->coregs3_guest_frame(); }

void Backend::set_display_field(bool odd) {
  if (impl_->renderer) impl_->renderer->SetDisplayField(odd);
}

bool Backend::has_presentation_thread() const { return impl_->uses_pictures(); }

void Backend::set_age_targets(bool age) {
  if (impl_->renderer) impl_->renderer->SetAgeTargets(age);
}

void Backend::wait_idle() {
  // The presentation thread first: vkDeviceWaitIdle uses every queue of the device, the one it presents on included.
  impl_->pacer_drain();
  impl_->device_wait_idle();
}

bool Backend::scanout(ScanoutRequest &request) {
  request.width = 0;
  request.height = 0;
  request.pixels.clear();
  if (!impl_->renderer) return false;
  // 12.9: `vram` (default) is the reference image the donor's decode matches; `scanout` is the presentation
  // image (field offset and deinterlace). The deinterlacer is stateful, so a windowed session reuses the
  // image it presented this VBlank instead of composing the presentation a second time.
  Impl &s = *impl_;
  if (s.options.dump_source == DumpSource::upscaled) {
    // IN-372: the GPU presentation compose at the internal scale (progressive frames); other frames fall back to
    // the CPU presentation compose at 1x.
    hwgs::writeback::GpuFrame gpuFrame;
    if (s.renderer->ComposeFrameGpu(gpuFrame)) {
      // TASK-090 M4: headless runs never reach end_frame, so the live present layer is drawn here, after the compose
      // submission and before the readback (same queue, submission order).
      s.overlay_coregs3(reinterpret_cast<VkImage>(gpuFrame.image), gpuFrame.width, gpuFrame.height);
      // TASK-090 2D overlay split: composite the frame's 2D layer before the readback reads the image.
      s.renderer->CompositeCoregs3Overlay(gpuFrame);
      if (s.renderer->ReadGpuFrame(gpuFrame, request.pixels)) {
        request.width = gpuFrame.width;
        request.height = gpuFrame.height;
        return true;
      }
    }
  }
  if (s.options.dump_source == DumpSource::scanout) {
    if (s.window && s.presentedVblank == s.stats.flushes && s.frame.valid) {
      request.width = s.frame.width;
      request.height = s.frame.height;
      request.pixels = s.frame.pixels;
      return true;
    }
  }
  const hwgs::writeback::ComposeKind kind = s.options.dump_source != DumpSource::vram
                                                ? hwgs::writeback::ComposeKind::Presentation
                                                : hwgs::writeback::ComposeKind::Reference;
  hwgs::writeback::FrameOutput frame = impl_->renderer->Compose(kind);
  if (!frame.valid) return false;  // no known video mode: no image (chapter 12.4)
  request.width = frame.width;
  request.height = frame.height;
  request.pixels = std::move(frame.pixels);
  return true;
}

bool Backend::attach_window(void *hwnd, std::string &error) {
  Impl &s = *impl_;
  if (!s.initialised) {
    error = "attach_window before init";
    return false;
  }
  s.window = static_cast<HWND>(hwnd);
  VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
  info.hinstance = GetModuleHandleW(nullptr);
  info.hwnd = s.window;
  const VkResult result = vkCreateWin32SurfaceKHR(s.instance, &info, nullptr, &s.surface);
  if (result != VK_SUCCESS) {
    error = std::string("vkCreateWin32SurfaceKHR failed: ") + vk_result_text(result);
    return false;
  }
  VkBool32 supported = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(s.physical, s.queue_family, s.surface, &supported);
  if (!supported) {
    error = "graphics queue cannot present to this surface";
    return false;
  }
  if (!s.create_swapchain(error)) return false;
  // IN-355: with a window the presentation shows the previous VBlank's display, downloaded asynchronously, so
  // the CPU never waits for the frame it just submitted. GT4_HW_ASYNC_PRESENT=0 (diagnostic, not a 12.8 option)
  // composes synchronously.
  const char *async = std::getenv("GT4_HW_ASYNC_PRESENT");
  if (s.renderer) s.renderer->SetAsyncPresentation(!(async && std::string(async) == "0"));
  return true;
}

void Backend::detach_window() {
  Impl &s = *impl_;
  if (!s.device) return;
  if (s.renderer) s.renderer->SetAsyncPresentation(false);
  s.pacer_drain();
  s.device_wait_idle();
  s.destroy_swapchain();
  if (s.surface) vkDestroySurfaceKHR(s.instance, s.surface, nullptr);
  s.surface = VK_NULL_HANDLE;
  s.window = nullptr;
}

void Backend::resize_window(std::uint32_t width, std::uint32_t height) {
  // The window procedure passes a size message on only when the client size changed (TASK-145); the new swapchain
  // takes the size from the surface.
  (void)width;
  (void)height;
  impl_->request_rebuild(Impl::Rebuild::window_size);
}

void Backend::present(PresentPhase phase) {
  // One guest VBlank on the default policy: the frame end and one present slot (chapter 12.5 parity). Above 60 Hz the
  // CoreGS 3.0 host clock hands every due presentation tick to the presenter in end_frame, each at its own display
  // period; the scheduler slot would then show the last picture again out of schedule, so it is skipped.
  if (end_frame(phase)) {
    if (!impl_->host_clock_presented) present_slot();
  }
}

bool Backend::end_frame(PresentPhase phase) {
  Impl &s = *impl_;
  s.stats.presents++;
  if (!s.window) return false; // headless: counted, nothing displayed
  // TASK-145: a suboptimal swapchain is rebuilt only when the surface size differs from it.
  if (!s.resize_pending && s.swapchain && s.suboptimal_seen.exchange(false)) {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s.physical, s.surface, &caps);
    const bool size_changed = caps.currentExtent.width != 0xFFFFFFFFu &&
                              (caps.currentExtent.width != s.extent.width || caps.currentExtent.height != s.extent.height);
    if (size_changed) {
      s.request_rebuild(Impl::Rebuild::suboptimal_size);
    } else if (++s.suboptimal_kept <= 3 || s.suboptimal_kept % 100 == 0) {
      std::fprintf(stderr, "[hw] swapchain suboptimal at the same size %ux%u, kept (%llu so far)\n", s.extent.width,
                   s.extent.height, static_cast<unsigned long long>(s.suboptimal_kept));
    }
  }
  if (s.resize_pending || !s.swapchain) {
    static constexpr const char *kReasons[] = {"first or after a minimised window", "window size changed",
                                               "acquire out of date", "present out of date",
                                               "suboptimal at a new surface size"};
    const int reason = s.rebuild_reason.exchange(static_cast<int>(Impl::Rebuild::none));
    if (s.swapchain) {
      std::fprintf(stderr, "[hw] swapchain rebuilt: %s (was %ux%u)\n", kReasons[std::clamp(reason, 0, 4)], s.extent.width,
                   s.extent.height);
    }
    std::string error;
    if (!s.recreate_swapchain(error)) {
      s.resize_pending = false;
      return false;
    }
  }
  if (!s.swapchain || !s.renderer) return false;

  // Frame end (chapter 12.5) with the present decision separated from it (M6 interface, IN-331): compose
  // the presentation image once into the reusable buffer and hand its stamp to the scheduler.
  s.renderer->SetDisplayField(phase == PresentPhase::field_odd);
  {
    // IN-432: core 2 resamples the frame to the rectangle the present below shows it in.
    const std::uint64_t aspect = s.display_aspect.load(std::memory_order_relaxed);
    const PresentRect rect = present_rect_for(s.extent.width, s.extent.height, display_aspect_fill(aspect),
                                              display_aspect_ratio(aspect));
    s.renderer->SetPresentSize(rect.width, rect.height);
  }
  hwgs::writeback::GpuFrame gpuFrame;
  if (s.renderer->ComposeFrameGpu(gpuFrame)) {
    // IN-364: composed on the GPU; the present copies the display image (no CPU pixels, no upload).
    s.frameOnGpu = true;
    s.composedFrame = gpuFrame;
    s.gpuImage = reinterpret_cast<VkImage>(gpuFrame.image);
    s.frame.valid = true;
    s.frame.width = gpuFrame.width;
    s.frame.height = gpuFrame.height;
    s.frame.pixels.clear();
  } else {
    s.frameOnGpu = false;
    s.composedFrame = {};
    s.renderer->ComposeFrame(s.frame, hwgs::writeback::ComposeKind::Presentation);
  }
  // TASK-090 M4: the debug-only live layer, after the compose submission and before the present copies the image. The
  // host clock (above 60 Hz) draws and hands over one picture per due presentation tick; below it, the whole batch is
  // drawn and the normal once-per-VBlank present shows its newest tick.
  s.host_clock_presented = false;
  if (s.frameOnGpu) {
    if (s.coregs3_host_clock) {
      // The presenter's copies read the renderer's display image (record_frame's GPU path), which the draw_tick calls
      // here update for every presentation tick.
      s.shownOnGpu = true;
      s.host_clock_presented = s.overlay_coregs3_host_clock(s.gpuImage, s.frame.width, s.frame.height);
    } else {
      s.overlay_coregs3(s.gpuImage, s.frame.width, s.frame.height);
      // TASK-090 2D overlay split: the 2D layer over the image the world just drew into, before the presenter copies it.
      s.composite_coregs3_overlay();
    }
  }
  s.presentedVblank = s.stats.flushes;
  s.composeTotalUs += s.renderer->Timings().composeUs;
  ++s.composedFrames;
  hwgs::present::FrameStamp stamp;
  stamp.guestFrame = ++s.guestFrames;
  stamp.phase = phase == PresentPhase::field_odd    ? hwgs::present::Phase::FieldOdd
                : phase == PresentPhase::field_even ? hwgs::present::Phase::FieldEven
                                                    : hwgs::present::Phase::Progressive;
  stamp.hasImage = s.frame.valid;
  s.scheduler.SubmitFrame(stamp);
  return true;
}

void Backend::present_slot() {
  // One host present opportunity: the scheduler decides new, held or nothing. A host clock running faster
  // than the guest (VRR, 120/144 Hz; TASK-082/TASK-045) calls this more often than end_frame(); it must be
  // called from the thread that owns the queue (today the EE thread at VBlank).
  Impl &s = *impl_;
  if (!s.window || !s.swapchain || !s.renderer) return;
  const hwgs::present::Action action = s.scheduler.Tick();
  if (action == hwgs::present::Action::none) return;  // holding: the display keeps what it has
  if (action == hwgs::present::Action::presentNew) {
    // Keep the presented image: a later hold re-presents exactly these pixels.
    s.shownFrame.valid = s.frame.valid;
    s.shownFrame.width = s.frame.width;
    s.shownFrame.height = s.frame.height;
    s.shownFrame.pixels.assign(s.frame.pixels.begin(), s.frame.pixels.end());
    s.shownOnGpu = s.frameOnGpu;
  }
  const auto start = std::chrono::steady_clock::now();
  s.present_frame(s.shownFrame.valid ? &s.shownFrame : nullptr);
  const std::chrono::duration<double, std::micro> elapsed = std::chrono::steady_clock::now() - start;
  s.presentUs = elapsed.count();
}

RenderCounters Backend::render_counters() const {
  RenderCounters out;
  if (!impl_->renderer) return out;
  const hwgs::writeback::Counters &c = impl_->renderer->GetCounters();
  out.batches = c.batches;
  out.primitives = c.primitives;
  out.primitives_skipped_texture = c.primitivesSkippedTexture;
  out.batches_empty = c.batchesEmpty;
  out.textures = c.texturesDecoded;
  out.texels = c.texelsDecoded;
  out.gpu_draws = c.gpuDraws;
  out.writeback_pixels = c.writebackPixels;
  out.flat_deindexed = c.flatDeindexedPrimitives;
  out.sprites = c.spritesExpanded;
  out.presentation_frames = c.presentationFrames;
  out.blank_frames = c.blankFrames;
  out.deinterlaced_frames = c.deinterlacedFrames;
  out.interlace_offset_frames = c.interlaceOffsetFrames;
  out.merge_alpha_from_texel = c.mergeAlphaFromTexel;
  out.writeback_depth_words = c.writebackDepthWords;
  out.fifo_overruns = c.fifoOverruns;
  const hwgs::writeback::PassTimings &totals = impl_->renderer->TimingTotals();
  out.translate_us = totals.translateUs;
  out.execute_us = totals.executeUs;
  out.decode_us = c.textureDecodeUs;
  out.cache_writeback_us = c.cacheWritebackUs;
  out.cache_load_us = c.cacheLoadUs;
  out.gif_us = c.gifUs;
  out.upload_us = c.uploadUs;
  out.textures_cached = c.texturesCached;
  out.cache_writebacks = c.cacheWritebacks;
  out.cache_loads = c.cacheLoads;
  out.gpu_chunks = c.gpuChunks;
  const hwgs::gpu::GpuStats &gpu = impl_->renderer->GpuCounters();
  out.gpu_submissions = gpu.submissions;
  out.gpu_barriers = gpu.barriers;
  out.gpu_render_passes = gpu.renderPasses;
  out.gpu_upload_bytes = gpu.uploadBytes;
  out.gpu_readback_bytes = gpu.readbackBytes;
  out.gpu_scaled_image_bytes = gpu.scaledImageBytes;
  out.gpu_busy_ns = gpu.gpuBusyNs;
  for (int i = 0; i < 8; ++i) out.gpu_ns[i] = gpu.gpuNs[i];
  out.gpu_early_submissions = gpu.earlySubmissions;
  out.vblanks_interlaced = c.vblanksInterlaced;
  out.vblanks_progressive = c.vblanksProgressive;
  out.scale = impl_->renderer->Scale();
  out.scaled_draws = gpu.scaledDraws;
  out.scaled_passes = gpu.scaledPasses;
  out.scaled_refreshes = gpu.scaledRefreshes;
  out.scaled_refresh_pixels = gpu.scaledRefreshPixels;
  out.scaled_composes = c.scaledComposedFrames;
  out.scaled_circuits_from_slots = c.scaledCircuitsFromSlots;
  out.scaled_circuits_from_glm = c.scaledCircuitsFromGlm;
  out.scaled_circuits_from_shadows = c.scaledCircuitsFromShadows;
  out.scaled_shadow_saves = c.scaledShadowSaves;
  out.tone_runs = c.toneRuns;
  out.tone_carries = c.toneCarries;
  out.tone_strip_pages = c.toneStripPages;
  out.tone_strip_batches = c.toneStripBatches;
  out.tone_verify_mismatch_bytes = c.toneVerifyMismatchBytes;
  out.gpu_composed_frames = c.gpuComposedFrames;
  for (int i = 0; i < 4; ++i) out.gpu_compose_skips[i] = c.gpuComposeSkips[i];
  for (int i = 0; i < 9; ++i) out.writeback_reasons[i] = c.cacheWritebackByReason[i];
  return out;
}

bool Backend::has_window() const { return impl_->window != nullptr; }

void Backend::set_display_aspect(bool fill, std::uint32_t ratio_millionths) {
  impl_->display_aspect.store(pack_display_aspect(fill, ratio_millionths), std::memory_order_relaxed);
}

const Options &Backend::options() const { return impl_->options; }

const DeviceReport &Backend::device_report() const { return impl_->report; }

Stats Backend::stats() const {
  Stats out = impl_->stats;
  if (impl_->renderer) {
    // Averages per frame: a single last-frame number is too noisy to compare with the roadmap budget.
    const hwgs::writeback::PassTimings &totals = impl_->renderer->TimingTotals();
    const std::uint64_t frames = impl_->renderer->TimedFrames();
    if (frames != 0) {
      out.translate_us = totals.translateUs / static_cast<double>(frames);
      out.execute_us = totals.executeUs / static_cast<double>(frames);
    }
    if (impl_->composedFrames != 0) {
      out.compose_us = impl_->composeTotalUs / static_cast<double>(impl_->composedFrames);
    }
    out.auxiliary_frames = impl_->renderer->GetCounters().auxiliaryFrames;
  }
  out.held_presents = impl_->scheduler.presentedHeld();
  out.dropped_frames = impl_->scheduler.dropped();
  out.upload_us = impl_->uploadUs;
  out.present_us = impl_->presentUs;
  // TASK-174.
  const Impl &s = *impl_;
  out.present_path = Impl::present_path_name(s.present_path);
  out.present_requests = s.present_requests;
  out.presents_shown = s.presents_shown.load(std::memory_order_relaxed);
  out.presents_not_shown = s.presents_dropped.load(std::memory_order_relaxed);
  out.presents_failed = s.presents_failed.load(std::memory_order_relaxed);
  out.presents_left_out = s.presents_left_out;
  out.gs_present_step_us = s.gs_present_step_us;
  out.gs_present_wait_us = s.gs_present_wait_us;
  const gt4::hw::QueueLock::Totals gsLock = s.queue_lock.totals(Impl::QueueCaller::gs_thread);
  const gt4::hw::QueueLock::Totals presenterLock = s.queue_lock.totals(Impl::QueueCaller::presentation_thread);
  out.gs_queue_lock_wait_us = static_cast<double>(gsLock.wait_ns) / 1000.0;
  out.gs_queue_lock_longest_wait_us = static_cast<double>(gsLock.longest_wait_ns) / 1000.0;
  out.gs_queue_calls = gsLock.calls;
  out.presenter_queue_lock_wait_us = static_cast<double>(presenterLock.wait_ns) / 1000.0;
  out.presenter_queue_call_us = static_cast<double>(presenterLock.call_ns) / 1000.0;
  out.presenter_queue_longest_call_us = static_cast<double>(presenterLock.longest_call_ns) / 1000.0;
  out.presenter_display_wait_us = static_cast<double>(s.presenter_display_wait_ns.load(std::memory_order_relaxed)) / 1000.0;
  return out;
}

} // namespace gt4::hw
