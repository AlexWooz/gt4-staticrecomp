// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from the TASK-026 specification (docs/hw-renderer-spec), clean room per decision 0069.
// Implements chapter 12.3 (backend interface) at milestone M0: every operation exists,
// the GS data path is a stub, and the device, swapchain and present path are real.
//
// This header is Vulkan-free. Only the translation units under native/hw_gs/ that
// implement the backend include Vulkan headers.
#pragma once

#include "hw_gs_options.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gt4::hw {

// Guest-side stream events, delivered inline during gif_transfer (chapter 12.3).
enum class StreamEvent { signal, finish, label };

// Returning true asks the caller to flush.
using StreamEventHandler = bool (*)(void *context, StreamEvent event, std::uint32_t value);

// Display phase requested at a guest VBlank (chapter 12.5).
enum class PresentPhase { field_even, field_odd, progressive };

// scanout(): the output image as host RGBA8 (red in the low byte), at internal resolution.
struct ScanoutRequest {
  PresentPhase phase = PresentPhase::progressive;
  std::uint32_t width = 0;   // filled by the backend
  std::uint32_t height = 0;  // filled by the backend
  std::vector<std::uint32_t> pixels;
};

// Counters for the F8 report (chapter 12.7). M0 fills the subset that exists.
struct Stats {
  std::uint64_t frames = 0;             // next_frame() calls
  std::uint64_t gif_bytes = 0;          // bytes accepted by gif_transfer
  std::uint64_t privileged_writes = 0;  // accepted by write_privileged
  std::uint64_t flushes = 0;            // flush() calls
  std::uint64_t presents = 0;           // present() calls (window or headless)
  std::uint64_t black_presents = 0;     // presents that displayed a black frame
  std::uint64_t swapchain_recreations = 0;
  std::uint64_t faults = 0;             // out-of-range reads and writes

  // M6 interface: presentation decisions, auxiliary passes and the per-pass CPU budget of the last
  // frame (docs/COREGS2-ROADMAP.md requirements 1, 2 and 4). GPU timestamps are a later step; there is
  // no field here that could be mistaken for one.
  std::uint64_t held_presents = 0;      // re-presented images (no new frame this VBlank)
  std::uint64_t dropped_frames = 0;     // frame ends replaced before they reached the display
  std::uint64_t auxiliary_frames = 0;   // render-only passes that restored guest memory
  double translate_us = 0;              // batch translation, last frame
  double execute_us = 0;                // GPU drawer submission, last frame
  double compose_us = 0;                // display composition, last frame
  double upload_us = 0;                 // composed image upload, last present
  double present_us = 0;                // swapchain acquire, submit and present, last present
  // Destination rectangle of the last presented image inside the swapchain (chapter 13.6: the largest centred 16:9
  // rectangle; GT4 draws its forced 16:9 picture anamorphically into 640 x 448, TASK-079), or with the TASK-080 fill
  // the largest centred rectangle of the race view's display ratio (present_fill = 1).
  std::uint32_t present_left = 0, present_top = 0, present_width = 0, present_height = 0;
  std::uint32_t present_fill = 0;
  std::uint32_t present_fill_millionths = 0;  // the fill target ratio x 1e6 (0 = the whole window)
  std::uint32_t present_downscale_levels = 0;  // IN-375: 2x2 box steps before the final blit (upscaled frames)
  std::uint32_t present_source_width = 0, present_source_height = 0;  // the presented frame (scaled size)

  // TASK-174: how frames reach the display and what that costs the GS thread (session totals; docs/PRESENTATION-ONE-QUEUE.md).
  const char *present_path = "";          // "rendering_queue", "own_queue" (IN-516) or "shared_queue" (TASK-174)
  std::uint64_t present_requests = 0;     // presentations asked of the path; each ends as one of the four below
  std::uint64_t presents_shown = 0;       // vkQueuePresentKHR returned VK_SUCCESS or VK_SUBOPTIMAL_KHR
  std::uint64_t presents_not_shown = 0;   // the presentation side was behind: no free slot, picture or swapchain image in time
  std::uint64_t presents_failed = 0;      // an acquire or present error (out of date, surface lost)
  std::uint64_t presents_left_out = 0;    // left out by the display latency control (IN-521)
  double gs_present_step_us = 0;          // GS thread: the whole presentation step
  double gs_present_wait_us = 0;          // GS thread: inside its fence waits, acquire, queue submit and present calls there
  double gs_queue_lock_wait_us = 0;       // GS thread: waiting for the shared queue's lock (all its submissions)
  double gs_queue_lock_longest_wait_us = 0;  // GS thread: its longest single wait for that lock
  std::uint64_t gs_queue_calls = 0;       // GS thread: queue calls under the shared queue's lock
  double presenter_queue_lock_wait_us = 0;  // presentation thread: waiting for the shared queue's lock
  double presenter_queue_call_us = 0;       // presentation thread: inside its submit and present calls on the shared queue
  double presenter_queue_longest_call_us = 0;  // presentation thread: its longest single call there (what the GS thread may wait)
  double presenter_display_wait_us = 0;  // presentation thread: waiting for the display (acquire, previous presentation shown)
};

// Renderer counters of chapter 12.7, flattened so the backend header stays free of the write-back
// renderer's internals. They explain a missing draw: a skipped texture, an empty batch after culling.
struct RenderCounters {
  std::uint64_t batches = 0;
  std::uint64_t primitives = 0;
  std::uint64_t primitives_skipped_texture = 0;
  std::uint64_t batches_empty = 0;
  std::uint64_t textures = 0;
  std::uint64_t texels = 0;
  std::uint64_t gpu_draws = 0;
  std::uint64_t writeback_pixels = 0;
  std::uint64_t flat_deindexed = 0;
  std::uint64_t sprites = 0;
  // Output stage (13.7) and V-01-12.
  std::uint64_t presentation_frames = 0;
  std::uint64_t blank_frames = 0;
  std::uint64_t deinterlaced_frames = 0;
  std::uint64_t interlace_offset_frames = 0;
  std::uint64_t merge_alpha_from_texel = 0;
  std::uint64_t writeback_depth_words = 0;
  std::uint64_t fifo_overruns = 0;
  // Session totals for interval profiles (GT4_HW_PROFILE_EVERY): CPU microseconds and work counts.
  double translate_us = 0.0;
  double execute_us = 0.0;
  double decode_us = 0.0;
  double cache_writeback_us = 0.0;
  double cache_load_us = 0.0;
  double gif_us = 0.0;     // the whole GS stream (front end, translate, execute, transfers)
  double upload_us = 0.0;  // EE->GS uploads
  std::uint64_t gpu_busy_ns = 0;            // GT4_HW_GPU_TIMING: GPU busy time (first to last timestamp)
  std::uint64_t gpu_ns[8] = {};             // pass, decode, clut, swizzle, upload, download, scaled, refresh
  std::uint64_t gpu_early_submissions = 0;  // mid-frame submissions (IN-358)
  std::uint64_t vblanks_interlaced = 0;     // SMODE2.INT = 1 (480i)
  std::uint64_t vblanks_progressive = 0;    // SMODE2.INT = 0 (480p)
  // M12 (IN-371/IN-372): the internal scale, draws and passes repeated at scale, scaled-image refreshes from 1x
  // words, scaled GPU composes and their circuits read from a slot's scaled image or from the GLM at 1x.
  std::uint32_t scale = 1;
  std::uint64_t scaled_draws = 0;
  std::uint64_t scaled_passes = 0;
  std::uint64_t scaled_refreshes = 0;
  std::uint64_t scaled_refresh_pixels = 0;
  std::uint64_t scaled_composes = 0;
  std::uint64_t scaled_circuits_from_slots = 0;
  std::uint64_t scaled_circuits_from_glm = 0;
  std::uint64_t scaled_circuits_from_shadows = 0;  // IN-377: read from a display shadow
  std::uint64_t scaled_shadow_saves = 0;
  std::uint64_t tone_runs = 0;     // IN-379
  std::uint64_t tone_carries = 0;
  std::uint64_t tone_strip_pages = 0;      // IN-383 (parity plan item 2): pages written by one dispatch
  std::uint64_t tone_strip_batches = 0;    // strip batches those dispatches replaced
  std::uint64_t tone_verify_mismatch_bytes = 0;  // GT4_HW_VERIFY_SHUFFLE: bytes that differ from the batches
  std::uint64_t gpu_composed_frames = 0;
  std::uint64_t gpu_compose_skips[4] = {};  // IN-384: why a VBlank fell back to the 1x CPU presentation    // presentation frames composed on the GPU (IN-364)
  std::uint64_t gpu_render_passes = 0;      // IN-367 efficiency counters
  std::uint64_t gpu_upload_bytes = 0;
  std::uint64_t gpu_readback_bytes = 0;
  std::uint64_t gpu_scaled_image_bytes = 0;  // IN-384: device memory of every scaled presentation image
  std::uint64_t textures_cached = 0;
  std::uint64_t cache_writebacks = 0;
  std::uint64_t cache_loads = 0;
  std::uint64_t gpu_chunks = 0;
  std::uint64_t gpu_submissions = 0;
  std::uint64_t gpu_barriers = 0;
  std::uint64_t writeback_reasons[9] = {};  // other, texture, CLUT, transfer, display, eviction, load overlap,
                                            // fallback batch, CPU sprite
};

// Result of the feature report (chapter 12.6).
struct DeviceReport {
  std::string device_name;
  std::uint32_t vendor_id = 0;
  bool amd = false;
  std::vector<std::string> missing_required;  // refusal list when non-empty
  std::vector<std::string> optional_found;
  std::vector<std::string> optional_missing;
  std::uint32_t max_image_dimension_2d = 0;  // also the 06.1 scale ceiling (/ 1280)
  std::uint32_t max_compute_workgroup_count_x = 0;
};

class Backend {
public:
  Backend();
  ~Backend();
  Backend(const Backend &) = delete;
  Backend &operator=(const Backend &) = delete;

  // Creates instance, device, queues, allocator-free persistent state. Never presents.
  // On failure `error` names the missing requirement and the function returns false.
  bool init(const Options &options, std::string &error);

  // IN-507: compiles what the CoreGS 2.0 shader cache lacks (`progress` gets done and total, only when there is work).
  // TASK-204: `recorded` receives how many of those variants the sessions recorded (variants.bin). TASK-206: the list is
  // expanded over the session's scale (GT4_HW_SCALE) and `added` receives how many keys that added beyond the 1x shapes.
  bool prepare_shaders(const std::function<void(std::size_t, std::size_t)> &progress, std::string &error,
                       std::size_t *recorded = nullptr, std::size_t *added = nullptr);

  void set_stream_event_handler(StreamEventHandler handler, void *context);

  // Feeds raw GIF qwords for path 1, 2 or 3. Transfers may split at any qword boundary.
  // M0 stub: counts bytes and returns true.
  bool gif_transfer(int path, const std::uint8_t *bytes, std::size_t byte_count);
  bool gif_busy(int path) const;
  void reset_gif_paths();

  // Stores display-register writes in 0x12000000..0x12001FFF. Returns false outside that range.
  bool write_privileged(std::uint32_t address, std::uint64_t value);
  std::uint64_t read_privileged(std::uint32_t address) const;

  void reset_context();
  void clear_vram();

  // Copies LOCAL->HOST data. M0 returns zeros and counts a fault.
  std::size_t read_transfer_fifo(void *destination, std::size_t qwords);

  // CPU view of GS memory coherent with recorded work. M0 returns nullptr.
  const std::uint8_t *map_vram(std::uint32_t offset, std::uint32_t size);

  void flush();       // submits recorded work, never waits
  void next_frame(); // headless frame boundary
  // Field of the VBlank being closed (CSR.FIELD from the VSYNC record; chapters 12.4, 13.3, 13.4). Set before
  // present() and before a scanout dump so the field offset and the deinterlacer use this VBlank's field.
  void set_display_field(bool odd);
  void set_age_targets(bool age);  // TASK-082, core 2: off for a VBlank with no drawing on purpose
  // TASK-082, TASK-174: pictures are shown from their own thread (a second queue of the family, or the rendering queue shared with
  // the presentation thread), so the frame spacing with frames left out happens there and not on the GS thread.
  bool has_presentation_thread() const;
  void wait_idle();

  // Builds the display image as present would. M0: no image, returns false (black).
  bool scanout(ScanoutRequest &request);

  // Swapchain on an existing HWND (passed as void* to keep this header Win32-free).
  bool attach_window(void *hwnd, std::string &error);
  void detach_window();
  // Resize and fullscreen only set a flag; recreation happens before the next present.
  void resize_window(std::uint32_t width, std::uint32_t height);
  // TASK-080 display-aspect fill (the hw form of protocol 1.3 CONTROL display_aspect): with `fill` the next presents
  // put the picture into the largest centred rectangle of `ratio_millionths` / 1e6 (0 = the whole window) instead of
  // the 16:9 rectangle. Any thread; effective from the next present.
  void set_display_aspect(bool fill, std::uint32_t ratio_millionths);
  // One call per guest VBlank: end_frame(phase) then one present_slot(). Headless (no window) only counts.
  void present(PresentPhase phase);
  // The two halves of present() (IN-331 follow-up, roadmap requirement 1): end_frame composes the guest
  // frame and submits its stamp to the present scheduler (false when nothing can be shown: headless, no
  // swapchain); present_slot is one host present opportunity (new image, held image or nothing). A host
  // clock faster than the guest calls present_slot() more often; it must run on the thread owning the queue.
  bool end_frame(PresentPhase phase);
  void present_slot();

  // True while a window (swapchain) is attached; headless runs advance frames themselves.
  bool has_window() const;

  const Options &options() const;
  const DeviceReport &device_report() const;
  Stats stats() const;
  RenderCounters render_counters() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace gt4::hw
