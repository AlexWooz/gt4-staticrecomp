// CTest hw_device_profile: TASK-179, the capability profile's class rule and log line, and the sizing of core 2's
// host-visible rings from the heap (hw_device_profile.hpp), host only.
#include "hw_device_profile.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace gt4::hw;

namespace {

void check(bool condition, const char *what) {
  if (condition) return;
  std::fprintf(stderr, "hw_device_profile: %s\n", what);
  std::exit(1);
}

constexpr std::uint64_t MiB = std::uint64_t{1} << 20;
constexpr std::uint64_t GiB = std::uint64_t{1} << 30;

RingRequest request(std::uint32_t shares, std::uint64_t heap, std::uint64_t budget) {
  RingRequest r;
  r.shares = shares;
  r.heap_size = heap;
  r.heap_budget = budget;
  r.other = 256 * 1024;  // core 2's delayed-reduction buffer
  return r;
}

bool today(const RingPlan &plan, std::uint32_t shares) {
  return plan.shares == shares && plan.staging == 32 * MiB * shares && plan.vertex == 8 * MiB * shares &&
         plan.uniform == 2 * MiB * shares && plan.readback == 64 * MiB && !plan.reduced;
}

}  // namespace

int main() {
  // ---- The class rule, and today's integrated-GPU rule kept as it is ----
  check(classify_device(kVendorAmd, kDeviceTypeDiscrete) == DeviceClass::discrete, "AMD discrete");
  check(classify_device(kVendorNvidia, kDeviceTypeDiscrete) == DeviceClass::discrete, "NVIDIA discrete");
  check(classify_device(kVendorIntel, kDeviceTypeIntegrated) == DeviceClass::integrated, "Intel integrated");
  check(classify_device(kVendorAmd, kDeviceTypeIntegrated) == DeviceClass::integrated, "AMD APU");
  check(classify_device(kVendorQualcomm, kDeviceTypeIntegrated) == DeviceClass::tiler_or_unknown, "Adreno is a tiler");
  check(classify_device(kVendorArm, kDeviceTypeIntegrated) == DeviceClass::tiler_or_unknown, "Mali is a tiler");
  check(classify_device(kVendorImagination, kDeviceTypeIntegrated) == DeviceClass::tiler_or_unknown, "PowerVR is a tiler");
  check(classify_device(kVendorMesa, kDeviceTypeCpu) == DeviceClass::tiler_or_unknown, "a software device is unknown");
  check(classify_device(kVendorNvidia, kDeviceTypeVirtual) == DeviceClass::tiler_or_unknown, "a virtual device is unknown");
  check(integrated_defaults(kDeviceTypeIntegrated), "the integrated defaults follow the device type");
  check(!integrated_defaults(kDeviceTypeDiscrete) && !integrated_defaults(kDeviceTypeOther), "only the integrated type");
  check(std::string(vendor_name(kVendorQualcomm)) == "Qualcomm" && std::string(vendor_name(0x1234)) == "unknown", "vendor names");

  // ---- Points: one pixel always, larger only with largePoints and within the device's largest size ----
  DeviceProfile profile;
  check(profile.points_of_size(1.0f) && !profile.points_of_size(2.0f), "no largePoints: one-pixel points only");
  profile.large_points_supported = true;
  check(!profile.points_of_size(3.0f), "supported but not enabled is not enough");
  profile.large_points_enabled = true;
  profile.point_size_max = 64.0f;
  check(profile.points_of_size(12.0f) && !profile.points_of_size(65.0f), "enabled, up to the largest size");

  // ---- The log line ----
  profile.probed = true;
  profile.name = "Test GPU";
  profile.vendor_id = kVendorAmd;
  profile.device_type = kDeviceTypeDiscrete;
  profile.device_class = DeviceClass::discrete;
  profile.api_version = (1u << 22) | (4u << 12) | 335u;
  profile.memory_budget = true;
  profile.heap_count = 2;
  profile.heaps[0] = HeapInfo{16 * GiB, 15 * GiB, 1 * GiB, true};
  profile.heaps[1] = HeapInfo{32 * GiB, 30 * GiB, 2 * GiB, false};
  const std::string line = profile_line(profile);
  check(line.find("Test GPU, vendor AMD (0x1002)") == 0, "the line starts with the name and vendor");
  check(line.find("class discrete") != std::string::npos, "the line names the class");
  check(line.find("Vulkan 1.4.335") != std::string::npos, "the line names the Vulkan version");
  check(line.find("large points yes (max 64.0, enabled)") != std::string::npos, "the line names largePoints");
  check(line.find("heaps 0:local 16384 MiB (budget 15360 MiB, used 1024 MiB), 1:host 32768 MiB") != std::string::npos,
        "the line lists the heaps");
  check(largest_heap(profile, true) == 0 && largest_heap(profile, false) == 1, "largest heaps");
  check(line.find("device-local host-visible memory no") != std::string::npos,
        "a discrete profile has no device-local host-visible type");

  // ---- TASK-182 step 3: the K rule's device default (row 2 of STUDY-ASYNC-READBACK 9.1) ----
  {
    DeviceProfile gpu;  // a discrete desktop GPU: K stays 0 until its own A/B
    gpu.device_class = DeviceClass::discrete;
    gpu.device_type = kDeviceTypeDiscrete;
    check(!k_rule_default_on(gpu) && k_rule_default_batches(gpu) == 0, "discrete: the rule stays off");
    check(std::string(k_rule_default_reason(gpu)) == "discrete GPU", "discrete: the reason");

    DeviceProfile phone;  // Adreno, Mali, PowerVR, Apple, Xclipse: tile-based
    phone.device_class = DeviceClass::tiler_or_unknown;
    phone.device_type = kDeviceTypeIntegrated;
    phone.vendor_id = kVendorQualcomm;
    check(k_rule_default_on(phone) && k_rule_default_batches(phone) == kRuleDefaultBatches, "a tiler: K = 8");
    check(std::string(k_rule_default_reason(phone)) == "tile-based or unknown GPU", "a tiler: the reason");

    DeviceProfile software;  // lavapipe and the like: no better data than the other tilers
    software.device_class = DeviceClass::tiler_or_unknown;
    software.device_type = kDeviceTypeCpu;
    check(k_rule_default_on(software), "a software device: K = 8");

    DeviceProfile apu;  // an integrated GPU sharing system memory: unified
    apu.device_class = DeviceClass::integrated;
    apu.device_type = kDeviceTypeIntegrated;
    apu.unified_memory = true;
    check(k_rule_default_on(apu) && k_rule_default_batches(apu) == kRuleDefaultBatches, "an APU with unified memory: K = 8");
    check(std::string(k_rule_default_reason(apu)) == "unified memory", "an APU: the reason");

    DeviceProfile integratedOwn;  // integrated by type, but its memory is its own (not the case today, kept honest)
    integratedOwn.device_class = DeviceClass::integrated;
    integratedOwn.device_type = kDeviceTypeIntegrated;
    integratedOwn.unified_memory = false;
    integratedOwn.integrated_defaults = false;
    check(!k_rule_default_on(integratedOwn), "an integrated GPU without unified memory keeps K = 0");
    check(std::string(k_rule_default_reason(integratedOwn)) == "integrated GPU with its own memory", "its reason");
    integratedOwn.integrated_defaults = true;  // today's integrated default rule (device type) still counts
    check(k_rule_default_on(integratedOwn), "the integrated device-type default is enough");

    DeviceProfile rebar;  // a discrete GPU with a host-visible device-local type (resizable BAR): the class wins
    rebar.device_class = DeviceClass::discrete;
    rebar.device_type = kDeviceTypeDiscrete;
    rebar.unified_memory = true;
    check(!k_rule_default_on(rebar), "a discrete GPU stays off even if it reports unified memory");
  }
  {
    DeviceProfile apuLine = profile;
    apuLine.unified_memory = true;
    const std::string text = profile_line(apuLine);
    check(text.find("device-local host-visible memory yes") != std::string::npos,
          "the line names the device-local host-visible type");
  }

  // ---- Rings: today's sizes wherever they fit in a quarter of the heap's budget ----
  check(today(plan_core2_rings(request(16, 16 * GiB, 15 * GiB)), 16), "a desktop keeps today's 736 MiB");
  check(plan_core2_rings(request(16, 16 * GiB, 15 * GiB)).total() == 736 * MiB, "736 MiB on a discrete GPU");
  check(today(plan_core2_rings(request(8, 4 * GiB, 0)), 8), "an integrated GPU without a budget keeps today's 400 MiB");
  check(plan_core2_rings(request(8, 4 * GiB, 0)).total() == 400 * MiB, "400 MiB on an integrated GPU");
  check(std::string(plan_core2_rings(request(8, 4 * GiB, 0)).basis) == "heap size", "the heap size without a budget");
  check(std::string(plan_core2_rings(request(8, 4 * GiB, 3 * GiB)).basis) == "heap budget", "the budget when there is one");
  check(plan_core2_rings(request(8, 4 * GiB, 3 * GiB)).allowance == 768 * MiB, "a quarter of the budget");
  check(plan_core2_rings(request(8, 2 * GiB, 3 * GiB)).allowance == 512 * MiB, "the budget is never above the heap");
  check(today(plan_core2_rings(request(16, 0, 0)), 16), "an unknown heap keeps today's sizes");
  {
    RingRequest fixed = request(16, 256 * MiB, 0);
    fixed.fixed = true;
    check(today(plan_core2_rings(fixed), 16), "GT4_CORE2_RINGS=fixed keeps today's sizes on any heap");
  }
  {  // 2 GiB of budget: 512 MiB allowed; 64 MiB of readback leaves 10 shares of 42 MiB.
    const RingPlan plan = plan_core2_rings(request(16, 6 * GiB, 2 * GiB));
    check(plan.reduced && plan.shares == 10 && plan.readback == 64 * MiB, "2 GiB: 10 shares");
    check(plan.staging == 320 * MiB && plan.vertex == 80 * MiB && plan.uniform == 20 * MiB, "2 GiB: ring sizes");
    check(plan.total() <= plan.allowance, "2 GiB: within the allowance");
  }
  {  // A forced budget of 256 MiB: 64 MiB allowed, below the floor of two shares and 16 MiB of readback.
    RingRequest forced = request(16, 16 * GiB, 15 * GiB);
    forced.forced_budget = 256 * MiB;
    const RingPlan plan = plan_core2_rings(forced);
    check(std::string(plan.basis) == "forced budget", "the forced budget wins");
    check(plan.shares == kRingMinimumShares && plan.readback == kReadbackMinimumBytes, "the floor: two shares, 16 MiB");
  }
  {  // 600 MiB of allowance: 13 shares would leave too little readback; 12 shares and 64 MiB fit.
    const RingPlan plan = plan_core2_rings(request(16, 8 * GiB, 2400 * MiB));
    check(plan.shares == 12 && plan.readback == 64 * MiB && plan.total() <= plan.allowance, "2400 MiB: 12 shares");
  }
  {  // Two shares fit but not the whole readback: it shrinks, in whole MiB, not below 16.
    RingRequest small = request(16, 1 * GiB, 0);
    small.forced_budget = 512 * MiB;  // 128 MiB allowed: two shares are 84 MiB, 43 MiB left (minus the 256 KiB)
    const RingPlan plan = plan_core2_rings(small);
    check(plan.shares == 2 && plan.readback == 43 * MiB && plan.total() <= plan.allowance, "128 MiB: two shares, 43 MiB");
  }
  // Every budget: never above today's sizes, never below the floor, within the allowance whenever the floor allows.
  for (std::uint64_t budget = 16 * MiB; budget <= 24 * GiB; budget += 48 * MiB) {
    for (std::uint32_t shares : {8u, 16u, 24u}) {
      const RingPlan plan = plan_core2_rings(request(shares, 32 * GiB, budget));
      check(plan.shares >= kRingMinimumShares && plan.shares <= shares, "shares between the floor and today's");
      check(plan.readback >= kReadbackMinimumBytes && plan.readback <= 64 * MiB, "readback between 16 and 64 MiB");
      check(plan.staging == 32 * MiB * plan.shares, "a share keeps its size");
      const bool atFloor = plan.shares == kRingMinimumShares && plan.readback == kReadbackMinimumBytes;
      check(atFloor || plan.total() + 256 * 1024 <= plan.allowance, "within the allowance above the floor");
      check(plan.reduced == !today(plan, shares), "reduced exactly when below today's sizes");
    }
  }
  // ---- TASK-184: the ordering capabilities, mocked, and the raster-order decision they produce ----
  {
    DeviceProfile none;  // the S23's Adreno 740, as its own extension list reads: none of these
    check(!raster_order_default_on(none), "no extension: the raster-order flags stay off");
    check(std::string(raster_order_reason(none)) == "no rasterization-order attachment access", "and say why");
    const std::string text = ordering_line(none);
    check(text.find("rasterization-order attachment access: EXT no, ARM no, colour no (no), depth no (no), stencil no (no)") == 0,
          "the ordering line starts with the three ordering bits, reported and enabled");
    check(text.find("dynamic rendering local read no") != std::string::npos, "the line names local read");
    check(text.find("attachment feedback loop layout no, dynamic state no") != std::string::npos, "the line names the loops");
    check(text.find("shader tile image no (colour no, depth no, stencil no)") != std::string::npos, "the line names tile images");
    check(text.find("fragment shader interlock no (sample no, pixel no, shading rate no)") != std::string::npos,
          "the line names the interlock bits");

    DeviceProfile present;  // the extension is in the list but the feature bit is not offered
    present.ordering.raster_order_ext = true;
    check(!raster_order_default_on(present), "extension without the feature: off");
    check(std::string(raster_order_reason(present)) == "the extension is present but the feature is not", "and say why");

    DeviceProfile created;  // reported and enabled at device creation: the path turns on by itself
    created.ordering.raster_order_ext = true;
    created.ordering.raster_order_color_supported = true;
    created.ordering.raster_order_color_enabled = true;
    created.ordering.raster_order_depth_supported = true;
    created.ordering.raster_order_depth_enabled = true;
    check(raster_order_default_on(created), "extension, feature and enabled: on by itself");
    check(created.ordering.color_ordered(), "the colour bit is the one the subpass flag needs");
    check(std::string(raster_order_reason(created)) == "the device has rasterizationOrderColorAttachmentAccess",
          "and say why");
    const std::string orderedText = ordering_line(created);
    check(orderedText.find("colour yes (yes), depth yes (yes)") != std::string::npos,
          "the line shows supported and enabled side by side");

    DeviceProfile arm;  // the ARM spelling alone is enough (the three bits are the same bits)
    arm.ordering.raster_order_arm = true;
    arm.ordering.raster_order_color_supported = true;
    arm.ordering.raster_order_color_enabled = true;
    check(raster_order_default_on(arm), "the ARM spelling counts too");
    check(ordering_line(arm).find("EXT no, ARM yes") != std::string::npos, "and the line keeps the spelling apart");

    DeviceProfile reported;  // reported by the device but the device was created without it (a refused feature)
    reported.ordering.raster_order_ext = true;
    reported.ordering.raster_order_color_supported = true;
    check(!raster_order_default_on(reported), "reported but not enabled: off, never a validation error");

    // The flags the path declares for each of those profiles (the renderer static_asserts the bit values against Vulkan).
    const RasterOrderFlags noFlags = raster_order_flags(none.ordering);
    check(noFlags.subpass == 0 && noFlags.colorBlend == 0 && !noFlags.any(), "no capability, no flags");
    const RasterOrderFlags ordered = raster_order_flags(created.ordering);
    check(ordered.subpass == kRasterOrderSubpassColor && ordered.colorBlend == kRasterOrderColorBlendFlag && ordered.any(),
          "colour access: the subpass and the pipeline both carry the colour bit");
    check(raster_order_flags(reported.ordering).subpass == 0, "supported but not enabled: no flags");
  }
  std::printf("hw_device_profile: ok\n");
  return 0;
}
