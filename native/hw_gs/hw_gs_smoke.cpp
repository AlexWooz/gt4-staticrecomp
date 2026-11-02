// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from the TASK-026 specification (docs/hw-renderer-spec), clean room per decision 0069.
// Headless driver for the HWR backend at milestone M0 (chapters 12.2, 12.3, 12.6, 13.6).
//
//   hw_gs_smoke --print-options           renderer and GT4_HW_* effective values, then the JSON object
//   hw_gs_smoke [--frames N] [--window]   with GT4_RENDERER=hw: N guest frames of black output;
//                                         --window also attaches a hidden window (real swapchain)
//
// Exit codes: 0 success (donor selected: nothing to run), 1 backend failure, 2 usage or option error.

#include "hw_gs_dispatch.hpp"
#include "hw_gs_present_rect.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

gt4::hw::EnvironmentLookup process_environment() {
  return [](const char *name) -> const char * { return std::getenv(name); };
}

int usage() {
  std::fprintf(stderr, "usage: hw_gs_smoke --print-options | [--frames N] [--window] [--display | --display-640] "
                       "[--client W H] [--fill RATIO] [--resize-toggles N]\n");
  return 2;
}

// Hidden window for the swapchain: it is created but never shown.
HWND create_hidden_window() {
  WNDCLASSW cls{};
  cls.lpfnWndProc = DefWindowProcW;
  cls.hInstance = GetModuleHandleW(nullptr);
  cls.lpszClassName = L"GT4HwrSmokeWindow";
  RegisterClassW(&cls);
  return CreateWindowExW(0, cls.lpszClassName, L"GT4 HWR smoke", WS_OVERLAPPEDWINDOW, 0, 0, 640, 360,
                         nullptr, nullptr, cls.hInstance, nullptr);
}

void print_options(const gt4::hw::Options &options, gt4::hw::Renderer renderer) {
  std::printf("GT4_RENDERER=%s\n", gt4::hw::renderer_name(renderer));
  for (const auto &line : gt4::hw::describe_options(options)) std::printf("%s\n", line.c_str());
  std::printf("json %s\n", gt4::hw::options_json(options).c_str());
}

void print_device(const gt4::hw::DeviceReport &report) {
  std::printf("device=%s vendor=0x%04X amd=%d max_image_2d=%u max_compute_x=%u\n", report.device_name.c_str(),
              report.vendor_id, report.amd ? 1 : 0, report.max_image_dimension_2d,
              report.max_compute_workgroup_count_x);
  std::string found, missing;
  for (const auto &item : report.optional_found) found += " " + item;
  for (const auto &item : report.optional_missing) missing += " " + item;
  std::printf("optional_found:%s\noptional_missing:%s\n", found.c_str(), missing.c_str());
}

} // namespace

int main(int argc, char **argv) {
  bool print_only = false;
  bool use_window = false;
  bool display = false;  // --display: a 64 x 64 NTSC display on circuit 1, so presents take the GPU compose (IN-364)
  bool wideDisplay = false;  // --display-640: the same with GT4's 640 x 448 race display (IN-375 downscale steps)
  std::uint64_t frames = 4200;
  std::uint64_t resize_toggles = 0;
  // TASK-080 (owner test 6): --client W H sizes the hidden window's client area (an ultrawide 3440 x 1440, say);
  // --fill R asks for the race-view fill at display ratio R (0 = the whole window), as the game's presentation sink does.
  std::uint32_t client_width = 0, client_height = 0;
  bool fill = false;
  double fill_ratio = 0.0;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--print-options") {
      print_only = true;
    } else if (arg == "--window") {
      use_window = true;
    } else if (arg == "--display") {
      display = true;
    } else if (arg == "--display-640") {
      display = true;
      wideDisplay = true;
    } else if (arg == "--frames" && i + 1 < argc) {
      frames = std::strtoull(argv[++i], nullptr, 10);
    } else if (arg == "--resize-toggles" && i + 1 < argc) {
      resize_toggles = std::strtoull(argv[++i], nullptr, 10);
      use_window = true;
    } else if (arg == "--client" && i + 2 < argc) {
      client_width = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
      client_height = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
      use_window = true;
    } else if (arg == "--fill" && i + 1 < argc) {
      fill = true;
      fill_ratio = std::strtod(argv[++i], nullptr);
    } else {
      return usage();
    }
  }

  std::unique_ptr<gt4::hw::Dispatch> dispatch;
  try {
    dispatch = std::make_unique<gt4::hw::Dispatch>(process_environment());
  } catch (const gt4::hw::OptionError &problem) {
    std::fprintf(stderr, "hw_gs_smoke: %s\n", problem.what());
    return 2;
  }

  if (print_only) {
    print_options(dispatch->options(), dispatch->renderer());
    return 0;
  }
  // donor (unset) runs nothing here; parallel and hw go through start(), which refuses parallel.
  if (dispatch->renderer() == gt4::hw::Renderer::donor) {
    std::printf("GT4_RENDERER=donor: HWR not selected, nothing to run\n");
    return 0;
  }

  print_options(dispatch->options(), dispatch->renderer());
  std::string error;
  if (!dispatch->start(error)) {
    std::fprintf(stderr, "hw_gs_smoke: %s\n", error.c_str());
    return 1;
  }
  gt4::hw::Backend &backend = *dispatch->backend();
  print_device(backend.device_report());

  HWND window = nullptr;
  if (use_window) {
    window = create_hidden_window();
    if (window && client_width && client_height) {
      RECT outer{0, 0, static_cast<LONG>(client_width), static_cast<LONG>(client_height)};
      AdjustWindowRect(&outer, WS_OVERLAPPEDWINDOW, FALSE);
      SetWindowPos(window, nullptr, 0, 0, outer.right - outer.left, outer.bottom - outer.top,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (!window || !backend.attach_window(window, error)) {
      std::fprintf(stderr, "hw_gs_smoke: window: %s\n", error.empty() ? "CreateWindow failed" : error.c_str());
      return 1;
    }
  }

  const std::uint32_t fill_millionths =
      fill && fill_ratio > 0.0 ? static_cast<std::uint32_t>(std::floor(fill_ratio * 1e6 + 0.5)) : 0u;
  if (fill) backend.set_display_aspect(true, fill_millionths);
  if (display) {
    backend.write_privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));             // PMODE: RC1, MMOD, ALP 255
    backend.write_privileged(0x12000010u, std::uint64_t{2} << 13);                       // SMODE1: NTSC
    if (wideDisplay) {
      backend.write_privileged(0x12000070u, std::uint64_t{10} << 9);                     // DISPFB1: FBW 10, CT32
      backend.write_privileged(0x12000080u, (std::uint64_t{3} << 23) | (std::uint64_t{2559} << 32) | (std::uint64_t{447} << 44));  // 640 x 448
    } else {
      backend.write_privileged(0x12000070u, std::uint64_t{1} << 9);                      // DISPFB1: FBW 1, CT32
      backend.write_privileged(0x12000080u, (std::uint64_t{63} << 32) | (std::uint64_t{63} << 44));  // 64 x 64
    }
  }
  for (std::uint64_t frame = 0; frame < frames; ++frame) {
    backend.next_frame();
    backend.present(gt4::hw::PresentPhase::progressive);
  }
  backend.flush();
  backend.wait_idle();

  // Fullscreen-toggle stall check (TASK-027 M0b): each toggle changes the window's client size and sets the
  // resize flag, exactly as Alt+Enter and F11 do; the swapchain is rebuilt before the next present. Every
  // toggle is timed from the flag to the end of that present. Limit: 100 ms (the owner's stall criterion).
  if (resize_toggles > 0) {
    double worst_ms = 0.0;
    for (std::uint64_t toggle = 0; toggle < resize_toggles; ++toggle) {
      const std::uint32_t client_width = toggle % 2 == 0 ? 800u : 640u;
      const std::uint32_t client_height = client_width * 9u / 16u;
      RECT outer{}, client{};
      GetWindowRect(window, &outer);
      GetClientRect(window, &client);
      const int frame_width = (outer.right - outer.left) - (client.right - client.left);
      const int frame_height = (outer.bottom - outer.top) - (client.bottom - client.top);
      SetWindowPos(window, nullptr, 0, 0, static_cast<int>(client_width) + frame_width,
                   static_cast<int>(client_height) + frame_height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
      const auto started = std::chrono::steady_clock::now();
      backend.resize_window(client_width, client_height);
      backend.next_frame();
      backend.present(gt4::hw::PresentPhase::progressive);
      backend.wait_idle();
      const double elapsed_ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
      if (elapsed_ms > worst_ms) worst_ms = elapsed_ms;
    }
    const gt4::hw::Stats toggled = backend.stats();
    std::printf("resize_toggles=%llu max_toggle_ms=%.2f swapchain_recreations=%llu\n",
                static_cast<unsigned long long>(resize_toggles), worst_ms,
                static_cast<unsigned long long>(toggled.swapchain_recreations));
    if (worst_ms > 100.0) {
      std::fprintf(stderr, "hw_gs_smoke: a fullscreen-style toggle stalled for %.2f ms (limit 100 ms)\n", worst_ms);
      if (use_window) {
        backend.detach_window();
        DestroyWindow(window);
      }
      dispatch->stop();
      return 1;
    }
  }

  const gt4::hw::Stats stats = backend.stats();
  std::printf("frames=%llu presents=%llu black_presents=%llu gif_bytes=%llu swapchain_recreations=%llu faults=%llu\n",
              static_cast<unsigned long long>(stats.frames), static_cast<unsigned long long>(stats.presents),
              static_cast<unsigned long long>(stats.black_presents), static_cast<unsigned long long>(stats.gif_bytes),
              static_cast<unsigned long long>(stats.swapchain_recreations),
              static_cast<unsigned long long>(stats.faults));
  std::printf("gpu_composed=%llu\n", static_cast<unsigned long long>(backend.render_counters().gpu_composed_frames));
  if (use_window) {
    // 13.6: the image goes into a 16:9 rectangle (within one pixel of rounding), never the frame's own ratio.
    const std::uint64_t w = stats.present_width, h = stats.present_height;
    const bool widescreen = w > 0 && h > 0 && (w * 9 >= h * 16 ? w * 9 - h * 16 : h * 16 - w * 9) <= 16;
    std::printf("present_rect=%u,%u %ux%u present_16x9=%d\n", stats.present_left, stats.present_top,
                stats.present_width, stats.present_height, widescreen ? 1 : 0);
    // TASK-080: the rectangle must be exactly the one for this client area and fill request.
    RECT client{};
    GetClientRect(window, &client);
    const std::uint64_t packed = gt4::hw::pack_display_aspect(fill, fill_millionths);
    const gt4::hw::PresentRect expected =
        gt4::hw::present_rect_for(static_cast<std::uint32_t>(client.right), static_cast<std::uint32_t>(client.bottom),
                                  gt4::hw::display_aspect_fill(packed), gt4::hw::display_aspect_ratio(packed));
    const bool exact = expected.left == stats.present_left && expected.top == stats.present_top &&
                       expected.width == stats.present_width && expected.height == stats.present_height;
    std::printf("client=%ldx%ld fill=%u present_expected=%d\n", client.right, client.bottom, stats.present_fill,
                exact ? 1 : 0);
    std::printf("present_downscale_levels=%u scale=%u source=%ux%u\n", stats.present_downscale_levels,
                backend.render_counters().scale, stats.present_source_width, stats.present_source_height);
  }

  if (use_window) {
    backend.detach_window();
    DestroyWindow(window);
  }
  dispatch->stop();
  return 0;
}
