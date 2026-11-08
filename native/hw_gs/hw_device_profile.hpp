// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// TASK-179 (Android plan stage A1): the capability probe of CoreGS 2.0. Written from the Vulkan specification, clean room
// (decision 0070).
//
// One small record of what the chosen GPU is and has, filled once at start (vulkan_loader::probe_device_profile), logged
// in one line and handed to the renderer with the device (gpu::VulkanHandles::profile), so that later stages can key
// settings off the vendor and device class instead of the single "integrated GPU" test of today. Only the structure is
// new: `integrated_defaults` is today's rule (VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) and nothing reads `device_class`
// yet. Also the sizing of core 2's host-visible rings from the heaps (plan_core2_rings).
//
// Pure arithmetic and strings, no Vulkan: the numbers that come from Vulkan are copied in as plain integers (the
// physical device type has the VkPhysicalDeviceType values, the transform bits the VkSurfaceTransformFlagBitsKHR ones).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace gt4::hw {

// PCI vendor ids (and the Khronos ids of vendors without one) that Vulkan reports in VkPhysicalDeviceProperties::vendorID.
constexpr std::uint32_t kVendorAmd = 0x1002;
constexpr std::uint32_t kVendorNvidia = 0x10DE;
constexpr std::uint32_t kVendorIntel = 0x8086;
constexpr std::uint32_t kVendorArm = 0x13B5;        // Mali
constexpr std::uint32_t kVendorQualcomm = 0x5143;   // Adreno
constexpr std::uint32_t kVendorImagination = 0x1010;  // PowerVR
constexpr std::uint32_t kVendorSamsung = 0x144D;    // Xclipse
constexpr std::uint32_t kVendorApple = 0x106B;
constexpr std::uint32_t kVendorMesa = 0x10005;      // VK_VENDOR_ID_MESA (llvmpipe, lavapipe)

// VkPhysicalDeviceType values.
constexpr std::uint32_t kDeviceTypeOther = 0;
constexpr std::uint32_t kDeviceTypeIntegrated = 1;
constexpr std::uint32_t kDeviceTypeDiscrete = 2;
constexpr std::uint32_t kDeviceTypeVirtual = 3;
constexpr std::uint32_t kDeviceTypeCpu = 4;

inline const char *vendor_name(std::uint32_t vendor_id) {
  switch (vendor_id) {
  case kVendorAmd: return "AMD";
  case kVendorNvidia: return "NVIDIA";
  case kVendorIntel: return "Intel";
  case kVendorArm: return "ARM";
  case kVendorQualcomm: return "Qualcomm";
  case kVendorImagination: return "Imagination";
  case kVendorSamsung: return "Samsung";
  case kVendorApple: return "Apple";
  case kVendorMesa: return "Mesa";
  default: return "unknown";
  }
}

// What kind of GPU it is, for settings that depend on it.
//   discrete: its own memory (VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU).
//   integrated: an immediate-mode GPU sharing the system memory, from a desktop vendor (AMD, Intel, NVIDIA): the class
//     today's "integrated GPU" defaults were tuned on (an Intel HD 620).
//   tiler_or_unknown: everything else: the tile-based mobile GPUs (Adreno, Mali, PowerVR, Apple, Xclipse until measured)
//     and software, virtual or unknown devices. Nothing is tuned for this class yet (Android plan stages A4, A5).
enum class DeviceClass : std::uint8_t { discrete, integrated, tiler_or_unknown };

inline const char *device_class_name(DeviceClass device_class) {
  switch (device_class) {
  case DeviceClass::discrete: return "discrete";
  case DeviceClass::integrated: return "integrated";
  case DeviceClass::tiler_or_unknown: return "tiler-or-unknown";
  }
  return "tiler-or-unknown";
}

inline bool desktop_vendor(std::uint32_t vendor_id) {
  return vendor_id == kVendorAmd || vendor_id == kVendorIntel || vendor_id == kVendorNvidia;
}

inline DeviceClass classify_device(std::uint32_t vendor_id, std::uint32_t device_type) {
  if (device_type == kDeviceTypeDiscrete) return DeviceClass::discrete;
  if (device_type == kDeviceTypeIntegrated && desktop_vendor(vendor_id)) return DeviceClass::integrated;
  return DeviceClass::tiler_or_unknown;
}

// Today's rule for core 2's integrated-GPU defaults (IN-431, IN-476, IN-481, IN-494, IN-496, IN-517 and TASK-134): the
// device type alone. Kept exactly; a phone GPU that reports itself integrated still gets them until stage A4 keys them
// off the class instead.
inline bool integrated_defaults(std::uint32_t device_type) { return device_type == kDeviceTypeIntegrated; }

struct HeapInfo {
  std::uint64_t size = 0;    // VkMemoryHeap::size
  std::uint64_t budget = 0;  // VK_EXT_memory_budget heapBudget at the probe; 0 without the extension
  std::uint64_t usage = 0;   // VK_EXT_memory_budget heapUsage at the probe
  bool device_local = false;
};

// ---- TASK-184: the ordering and attachment-read capabilities of the device --------------------------
//
// Core 2's no-interlock path (IN-466, IN-471) orders two primitives that may share a pixel with a fragment barrier,
// because a storage image read-modify-write has no order of its own. Three families of extensions can replace that with
// hardware order or make an attachment readable in place:
//   - rasterization-order attachment access (VK_EXT_ and VK_ARM_ spellings): a subpass that declares it, plus pipelines
//     that declare it, order the attachment accesses of overlapping primitives by primitive order, which is the only way
//     a fragment shader may read a colour attachment it also writes (VK_EXT_attachment_feedback_loop_layout, or
//     VK_KHR_dynamic_rendering_local_read with dynamic rendering, say how the image is laid out for that read);
//   - VK_EXT_shader_tile_image: reads the tile's pixels from the fragment shader without a layout at all (Apple, some
//     Mali), the cheapest form of the same idea;
//   - VK_EXT_fragment_shader_interlock: the pixel interlock core 2 already uses where it exists (this record keeps the
//     four bits for the log next to the others).
// Every field is what the device reported at the probe, so game.log answers which of the paths a phone can take. Nothing
// here changes a picture; only the renderer's choice of path reads it.
struct OrderingCapabilities {
  bool raster_order_ext = false;  // VK_EXT_rasterization_order_attachment_access in the device's extension list
  bool raster_order_arm = false;  // VK_ARM_rasterization_order_attachment_access
  bool raster_order_color_supported = false, raster_order_color_enabled = false;
  bool raster_order_depth_supported = false, raster_order_depth_enabled = false;
  bool raster_order_stencil_supported = false, raster_order_stencil_enabled = false;
  bool dynamic_rendering_local_read = false;  // VK_KHR_dynamic_rendering_local_read, extension and feature
  bool attachment_feedback_loop_layout = false;
  bool attachment_feedback_loop_dynamic_state = false;
  bool shader_tile_image = false;
  bool shader_tile_image_color = false, shader_tile_image_depth = false, shader_tile_image_stencil = false;
  bool fragment_shader_interlock = false;
  bool interlock_sample = false, interlock_pixel = false, interlock_shading_rate = false;

  // A colour attachment may be declared ordered by the subpass and its pipelines, and the device was created with it.
  bool color_ordered() const { return raster_order_color_supported && raster_order_color_enabled; }
};

struct DeviceProfile {
  bool probed = false;  // false: the device's creator did not run the probe (every field below is a default)
  std::string name;
  std::uint32_t vendor_id = 0, device_id = 0, device_type = kDeviceTypeOther;
  std::uint32_t api_version = 0, driver_version = 0;
  DeviceClass device_class = DeviceClass::tiler_or_unknown;
  bool integrated_defaults = false;
  // TASK-182 step 3: a memory type that is both device-local and host-visible, i.e. the memory the CPU and the GPU both
  // use without a copy (an APU or a phone). The class above says what the device is; this says how its memory behaves.
  bool unified_memory = false;
  // TASK-184: the ordering and attachment-read extensions and their feature bits (see OrderingCapabilities above).
  OrderingCapabilities ordering;

  // Features that drawing at a scale above 1x relies on. `*_supported` is what the device has, `*_enabled` what the
  // device was created with (the creator sets it); the renderer keys its paths off `*_enabled`.
  bool large_points_supported = false, large_points_enabled = false;  // gl_PointSize = scale (hw_vertex.vert)
  float point_size_max = 1.0f, point_size_granularity = 0.0f;
  bool wide_lines_supported = false;  // reported only: every pipeline draws its lines 1.0 wide, so it is never needed
  float line_width_max = 1.0f;
  bool storage_image_array_dynamic_indexing_supported = false;  // hw_deinterlace_scaled.comp: history[index]
  bool storage_image_array_dynamic_indexing_enabled = false;

  // Memory: every heap, with its budget where VK_EXT_memory_budget exists (enabled on the device when it does).
  bool memory_budget = false;
  std::uint32_t heap_count = 0;
  std::array<HeapInfo, 16> heaps{};

  // True when points of `size` pixels can be drawn as points (else core 2 draws them as two triangles).
  bool points_of_size(float size) const { return size <= 1.0f || (large_points_enabled && size <= point_size_max); }
};

inline const char *yes_no(bool value) { return value ? "yes" : "no"; }

// The one line the probe adds to game.log, next to the device profile.
inline std::string ordering_line(const DeviceProfile &p) {
  const OrderingCapabilities &c = p.ordering;
  char text[512];
  std::snprintf(text, sizeof text,
                "rasterization-order attachment access: EXT %s, ARM %s, colour %s (%s), depth %s (%s), stencil %s (%s); "
                "dynamic rendering local read %s; attachment feedback loop layout %s, dynamic state %s; "
                "shader tile image %s (colour %s, depth %s, stencil %s); "
                "fragment shader interlock %s (sample %s, pixel %s, shading rate %s)",
                yes_no(c.raster_order_ext), yes_no(c.raster_order_arm), yes_no(c.raster_order_color_supported),
                yes_no(c.raster_order_color_enabled), yes_no(c.raster_order_depth_supported),
                yes_no(c.raster_order_depth_enabled), yes_no(c.raster_order_stencil_supported),
                yes_no(c.raster_order_stencil_enabled), yes_no(c.dynamic_rendering_local_read),
                yes_no(c.attachment_feedback_loop_layout), yes_no(c.attachment_feedback_loop_dynamic_state),
                yes_no(c.shader_tile_image), yes_no(c.shader_tile_image_color), yes_no(c.shader_tile_image_depth),
                yes_no(c.shader_tile_image_stencil), yes_no(c.fragment_shader_interlock), yes_no(c.interlock_sample),
                yes_no(c.interlock_pixel), yes_no(c.interlock_shading_rate));
  return text;
}

// TASK-184, GT4_CORE2_RASTER_ORDER: whether the colour-attachment passes declare rasterization order. Auto: on when the
// device was created with the colour feature (nothing reads it back today, so it changes no picture, only the flags the
// subpass and its pipelines carry). An explicit 0 turns it off for the A/B, an explicit 1 asks for it and is refused
// where the device cannot.
inline bool raster_order_default_on(const DeviceProfile &p) { return p.ordering.color_ordered(); }

inline const char *raster_order_reason(const DeviceProfile &p) {
  if (p.ordering.color_ordered()) return "the device has rasterizationOrderColorAttachmentAccess";
  if (p.ordering.raster_order_ext || p.ordering.raster_order_arm) return "the extension is present but the feature is not";
  return "no rasterization-order attachment access";
}

// The Vulkan flag bits the rasterization-order path sets, as plain integers so that a host-only fixture can pin the
// mapping without the Vulkan headers. core2_gpu.cpp static_asserts each of them against the real enum value, so a header
// that renumbers them breaks the build and never the picture.
constexpr std::uint32_t kRasterOrderSubpassColor = 0x00000010u;    // VK_SUBPASS_DESCRIPTION_..._COLOR_ACCESS_BIT_EXT
constexpr std::uint32_t kRasterOrderColorBlendFlag = 0x00000001u;  // VK_PIPELINE_COLOR_BLEND_STATE_CREATE_..._BIT_EXT

struct RasterOrderFlags {
  std::uint32_t subpass = 0;     // VkSubpassDescription::flags of the colour-attachment passes
  std::uint32_t colorBlend = 0;  // VkPipelineColorBlendStateCreateInfo::flags of their pipelines
  bool any() const { return subpass != 0; }
};

// What the colour-attachment path declares, given what the device reported and was created with. Only the colour bit is
// used: the path's fragment stage reads no attachment back, so the depth and stencil ordering bits would carry no
// guarantee anything depends on today (the probe still reports them, and the attachment-read variant of the path will
// need the colour bit this sets).
inline RasterOrderFlags raster_order_flags(const OrderingCapabilities &c) {
  RasterOrderFlags flags;
  if (!c.color_ordered()) return flags;
  flags.subpass = kRasterOrderSubpassColor;
  flags.colorBlend = kRasterOrderColorBlendFlag;
  return flags;
}

// The largest heap with (device_local) or without device-local memory; -1 when there is none.
inline int largest_heap(const DeviceProfile &profile, bool device_local) {
  int best = -1;
  for (std::uint32_t i = 0; i < profile.heap_count && i < profile.heaps.size(); ++i) {
    if (profile.heaps[i].device_local != device_local) continue;
    if (best < 0 || profile.heaps[i].size > profile.heaps[static_cast<std::size_t>(best)].size) best = static_cast<int>(i);
  }
  return best;
}

inline std::string mib_text(std::uint64_t bytes) { return std::to_string(bytes >> 20) + " MiB"; }

// The one log line of the probe.
inline std::string profile_line(const DeviceProfile &p) {
  char text[512];
  auto yes = [](bool value) { return value ? "yes" : "no"; };
  std::snprintf(text, sizeof text,
                "%s, vendor %s (0x%04X) device 0x%04X, class %s, device-local host-visible memory %s, integrated defaults %s, "
                "Vulkan %u.%u.%u; "
                "large points %s (max %.1f, %s), wide lines %s (max %.1f, not used), "
                "storage image array dynamic indexing %s (%s); memory budget %s",
                p.name.c_str(), vendor_name(p.vendor_id), p.vendor_id, p.device_id, device_class_name(p.device_class),
                p.unified_memory ? "yes" : "no", p.integrated_defaults ? "on" : "off", p.api_version >> 22,
                (p.api_version >> 12) & 0x3FFu, p.api_version & 0xFFFu, yes(p.large_points_supported),
                static_cast<double>(p.point_size_max),
                p.large_points_enabled ? "enabled" : "points drawn as quads above 1x", yes(p.wide_lines_supported),
                static_cast<double>(p.line_width_max), yes(p.storage_image_array_dynamic_indexing_supported),
                p.storage_image_array_dynamic_indexing_enabled ? "enabled" : "constant-index shader",
                p.memory_budget ? "yes" : "no");
  std::string line = text;
  for (std::uint32_t i = 0; i < p.heap_count && i < p.heaps.size(); ++i) {
    const HeapInfo &heap = p.heaps[i];
    line += (i == 0 ? ", heaps " : ", ") + std::to_string(i) + (heap.device_local ? ":local " : ":host ") + mib_text(heap.size);
    if (p.memory_budget) line += " (budget " + mib_text(heap.budget) + ", used " + mib_text(heap.usage) + ")";
  }
  return line;
}

// ---- TASK-182 step 3: the K rule's device default (row 2 of STUDY-ASYNC-READBACK 9.1) -----------------------------
//
// The K rule (GT4_CORE2_SUBMIT_AFTER_WAIT) submits again K draw batches after a waited readback, so the GPU is not left
// idle while the GS worker records its way back to the next submission. That bubble is the product of two things: how
// long the CPU records before the next submission, and whether the GPU had queued work at all. It is largest on the
// devices with a slow CPU against a fast GPU and a bursty drain: the tile-based mobile GPUs (Adreno, Mali, PowerVR,
// Apple, Xclipse) and the unified-memory parts (an APU, where the GPU's work competes with the CPU for the same memory).
// On a discrete desktop GPU the measured wall gain is small on a fast CPU (TASK-178 9.3: -0.9..-2.4 ms in the medians
// under another agent's load, but the least disturbed pair differed by 0.07 ms), so K stays 0 there unless a device's own
// A/B says otherwise.
//
//   tiler_or_unknown: on (K = 8): the tile-based mobile GPUs, and software/virtual devices, which have no better data.
//   integrated:       on (K = 8) when the memory is unified (a memory type that is device-local and host-visible, or the
//                     integrated device type GT4's APU defaults already follow); off otherwise.
//   discrete:         off (K = 0).
inline bool k_rule_default_on(const DeviceProfile &profile) {
  if (profile.device_class == DeviceClass::discrete) return false;
  if (profile.device_class == DeviceClass::tiler_or_unknown) return true;
  return profile.unified_memory || profile.integrated_defaults;
}

constexpr std::uint32_t kRuleDefaultBatches = 8;  // the study's suggestion for a tiler (submitEvery is 32 there)

inline std::uint32_t k_rule_default_batches(const DeviceProfile &profile) {
  return k_rule_default_on(profile) ? kRuleDefaultBatches : 0u;
}

// Why the rule is on or off there, for the [core2] submissions line of game.log.
inline const char *k_rule_default_reason(const DeviceProfile &profile) {
  if (profile.device_class == DeviceClass::discrete) return "discrete GPU";
  if (profile.device_class == DeviceClass::tiler_or_unknown) return "tile-based or unknown GPU";
  if (profile.unified_memory) return "unified memory";
  return "integrated GPU with its own memory";
}

// ---- Core 2's host-visible rings, sized from the device (TASK-179) -------------------------------------------------
//
// Core 2 maps four rings: staging, vertex and uniform with one share per recording in flight, and the readback buffer.
// Today's sizes come from constants: per share 32 MiB staging, 8 MiB vertex and 2 MiB uniform, 8 or 16 shares (8 on an
// integrated GPU, 16 on a discrete one), and 64 MiB of readback: 400 or 736 MiB. Those stay the sizes, and the cap,
// wherever they fit in a quarter of the heap they live in (its VK_EXT_memory_budget budget when the device reports one,
// else its size). Where they do not, the number of shares goes down first (a share keeps its size, so no request that
// fits today stops fitting; fewer recordings are in flight), to two at least; then the readback buffer, to 16 MiB at
// least (a whole 2048 x 2048 1x reduction; larger display reads go in bands). A phone's low-memory killer acts fast.

constexpr std::uint64_t kRingMiB = std::uint64_t{1} << 20;
constexpr std::uint32_t kRingMinimumShares = 2;
constexpr std::uint64_t kReadbackMinimumBytes = 16 * kRingMiB;

struct RingRequest {
  std::uint64_t heap_size = 0;    // the heap of the ring memory type
  std::uint64_t heap_budget = 0;  // its budget (VK_EXT_memory_budget), 0 when unknown
  std::uint64_t forced_budget = 0;  // GT4_CORE2_RINGS=<MiB>: a budget to plan for instead (tests on a large desktop)
  bool fixed = false;               // GT4_CORE2_RINGS=fixed: today's sizes whatever the heap
  std::uint32_t shares = 16;        // today's share count
  std::uint64_t staging_share = 32 * kRingMiB, vertex_share = 8 * kRingMiB, uniform_share = 2 * kRingMiB;
  std::uint64_t readback = 64 * kRingMiB;
  std::uint64_t other = 0;  // other mapped buffers in the same heap (counted, never resized)
};

struct RingPlan {
  std::uint32_t shares = 0;
  std::uint64_t staging = 0, vertex = 0, uniform = 0, readback = 0;
  std::uint64_t allowance = 0;  // a quarter of the budget planned for (0 with `fixed`)
  bool reduced = false;         // below today's sizes
  const char *basis = "";       // "fixed", "forced budget", "heap budget" or "heap size"

  std::uint64_t total() const { return staging + vertex + uniform + readback; }
};

inline RingPlan plan_core2_rings(const RingRequest &r) {
  RingPlan plan;
  const std::uint64_t per_share = r.staging_share + r.vertex_share + r.uniform_share;
  auto fill = [&](std::uint32_t shares, std::uint64_t readback) {
    plan.shares = shares;
    plan.staging = r.staging_share * shares;
    plan.vertex = r.vertex_share * shares;
    plan.uniform = r.uniform_share * shares;
    plan.readback = readback;
  };
  fill(r.shares, r.readback);
  if (r.fixed) {
    plan.basis = "fixed";
    return plan;
  }
  std::uint64_t base = r.heap_size;
  plan.basis = "heap size";
  if (r.forced_budget != 0) {
    base = r.forced_budget;
    plan.basis = "forced budget";
  } else if (r.heap_budget != 0) {
    base = r.heap_size != 0 ? std::min(r.heap_budget, r.heap_size) : r.heap_budget;
    plan.basis = "heap budget";
  }
  if (base == 0) {  // nothing known about the heap: today's sizes
    plan.basis = "unknown heap";
    return plan;
  }
  plan.allowance = base / 4;
  const std::uint64_t room = plan.allowance > r.other ? plan.allowance - r.other : 0;
  if (plan.total() <= room) return plan;
  plan.reduced = true;
  const std::uint64_t for_shares = room > r.readback ? room - r.readback : 0;
  const std::uint64_t fitting = per_share != 0 ? for_shares / per_share : r.shares;
  const std::uint32_t shares = static_cast<std::uint32_t>(
      std::clamp<std::uint64_t>(fitting, kRingMinimumShares, std::max<std::uint64_t>(r.shares, kRingMinimumShares)));
  std::uint64_t readback = r.readback;
  const std::uint64_t share_bytes = per_share * shares;
  if (share_bytes + readback > room) {
    const std::uint64_t left = room > share_bytes ? room - share_bytes : 0;
    readback = std::clamp<std::uint64_t>(left / kRingMiB * kRingMiB, std::min(kReadbackMinimumBytes, r.readback), r.readback);
  }
  fill(shares, readback);
  return plan;
}

}  // namespace gt4::hw
