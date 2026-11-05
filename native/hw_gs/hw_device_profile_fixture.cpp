// CTest hw_device_profile: TASK-179, the capability profile's class rule and log line (hw_device_profile.hpp), host only.
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

constexpr std::uint64_t GiB = std::uint64_t{1} << 30;

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

  std::printf("hw_device_profile: ok\n");
  return 0;
}
