// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from the TASK-026 specification (docs/hw-renderer-spec), clean room per decision 0069.
// CTest hw_present_rect: the HWR 16:9 rectangle (chapter 13.6) against the shared
// gt4::vulkan::presentation_rect used by the paraLLEl/donor host (chapter 12.5).

#include "../native_vulkan.hpp"
#include "hw_gs_present_rect.hpp"

#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;

bool same(const gt4::hw::PresentRect &a, const gt4::vulkan::PresentationRect &b) {
  return a.left == b.left && a.top == b.top && a.width == b.width && a.height == b.height;
}

void expect_rect(std::uint32_t width, std::uint32_t height, gt4::hw::PresentRect expected) {
  const auto got = gt4::hw::present_rect_widescreen(width, height);
  const auto shared = gt4::vulkan::presentation_rect(width, height, gt4::vulkan::Aspect::widescreen);
  if (got.left != expected.left || got.top != expected.top || got.width != expected.width ||
      got.height != expected.height || !same(got, shared)) {
    ++failures;
    std::fprintf(stderr, "FAIL: %ux%u gave (%u,%u,%u,%u) expected (%u,%u,%u,%u)\n", width, height,
                 got.left, got.top, got.width, got.height, expected.left, expected.top, expected.width,
                 expected.height);
  }
}

} // namespace

int main() {
  // Known vectors (the 1280x1024 case is the letterbox example of chapter 13.6).
  expect_rect(1920, 1080, {0, 0, 1920, 1080});
  expect_rect(1280, 1024, {0, 152, 1280, 720});
  expect_rect(960, 720, {0, 90, 960, 540});
  expect_rect(2560, 1080, {320, 0, 1920, 1080});
  expect_rect(0, 0, {0, 0, 0, 0});
  expect_rect(1, 1, {0, 0, 1, 1});

  // Overflow guard and a sweep: the HWR rectangle must equal the shared one everywhere.
  const std::uint32_t samples[] = {1, 2, 3, 7, 16, 255, 640, 1023, 1024, 1366, 1920, 2160, 4096, 65535, UINT32_MAX};
  for (std::uint32_t width : samples) {
    for (std::uint32_t height : samples) {
      const auto got = gt4::hw::present_rect_widescreen(width, height);
      const auto shared = gt4::vulkan::presentation_rect(width, height, gt4::vulkan::Aspect::widescreen);
      if (!same(got, shared)) {
        ++failures;
        std::fprintf(stderr, "FAIL: sweep %ux%u differs from presentation_rect\n", width, height);
        return 1;
      }
      if (got.width && got.height && got.left + got.width > width) {
        ++failures;
        std::fprintf(stderr, "FAIL: sweep %ux%u rectangle leaves the surface\n", width, height);
        return 1;
      }
    }
  }

  // TASK-080 display-aspect fill (owner test 6, ultrawide): the HWR rectangle equals the default renderer's
  // presentation_rect_ratio for 16:9, 16:10, 21:9, the owner's 3440 x 1440 (21.5:9) and 32:9, in 16:9, ultrawide and
  // super-ultrawide windows, and the ratio carried in millionths (the protocol form) gives the same rectangle.
  {
    const double ratios[] = {16.0 / 9.0, 16.0 / 10.0, 21.0 / 9.0, 3440.0 / 1440.0, 32.0 / 9.0};
    const std::uint32_t windows[][2] = {{1920, 1080}, {3440, 1440}, {2560, 1080}, {5120, 1440}, {1280, 1024}, {1, 1}};
    for (double ratio : ratios) {
      for (const auto &window : windows) {
        const auto got = gt4::hw::present_rect_ratio(window[0], window[1], ratio);
        const auto shared = gt4::vulkan::presentation_rect_ratio(window[0], window[1], ratio);
        const std::uint64_t packed =
            gt4::hw::pack_display_aspect(true, static_cast<std::uint32_t>(ratio * 1e6 + 0.5));
        const auto carried = gt4::hw::present_rect_for(window[0], window[1], gt4::hw::display_aspect_fill(packed),
                                                       gt4::hw::display_aspect_ratio(packed));
        // The protocol form rounds the ratio to millionths, which can move a .5 rounding by one pixel (32:9 in
        // 3440 x 1440: 967.5 rows): within one pixel of the exact rectangle, still centred.
        const auto within_one = [](std::uint32_t a, std::uint32_t b) { return a + 1 >= b && b + 1 >= a; };
        const bool carried_close = within_one(carried.left, shared.left) && within_one(carried.top, shared.top) &&
                                   within_one(carried.width, shared.width) && within_one(carried.height, shared.height);
        if (!same(got, shared) || !carried_close) {
          ++failures;
          std::fprintf(stderr, "FAIL: fill %.4f in %ux%u gave (%u,%u,%u,%u) / (%u,%u,%u,%u), shared (%u,%u,%u,%u)\n",
                       ratio, window[0], window[1], got.left, got.top, got.width, got.height, carried.left, carried.top,
                       carried.width, carried.height, shared.left, shared.top, shared.width, shared.height);
        }
      }
    }
    struct Known {
      std::uint32_t width, height;
      double ratio;
      gt4::hw::PresentRect expected;
    };
    const Known known[] = {
        {3440, 1440, 3440.0 / 1440.0, {0, 0, 3440, 1440}},  // the owner's monitor: the whole screen
        {3440, 1440, 21.0 / 9.0, {40, 0, 3360, 1440}},      // 21:9 in 21.5:9: thin side bars
        {3440, 1440, 16.0 / 9.0, {440, 0, 2560, 1440}},     // a 16:9 fill is the pillarbox
        {3440, 1440, 32.0 / 9.0, {0, 236, 3440, 968}},      // 32:9 letterboxed, never stretched
        {5120, 1440, 32.0 / 9.0, {0, 0, 5120, 1440}},       // super-ultrawide: the whole screen
        {1920, 1080, 21.0 / 9.0, {0, 128, 1920, 823}},      // 21:9 in a 16:9 window: letterboxed
    };
    for (const Known &k : known) {
      const auto got = gt4::hw::present_rect_ratio(k.width, k.height, k.ratio);
      if (got.left != k.expected.left || got.top != k.expected.top || got.width != k.expected.width ||
          got.height != k.expected.height) {
        ++failures;
        std::fprintf(stderr, "FAIL: fill %.4f in %ux%u gave (%u,%u,%u,%u) expected (%u,%u,%u,%u)\n", k.ratio, k.width,
                     k.height, got.left, got.top, got.width, got.height, k.expected.left, k.expected.top,
                     k.expected.width, k.expected.height);
      }
    }
    // Without the fill (menus, a 16:9 player) the rectangle stays the 16:9 one of chapter 13.6; a fill target of 0 is
    // the protocol's "whole window".
    const auto off = gt4::hw::present_rect_for(3440, 1440, false, 21.0 / 9.0);
    const auto whole = gt4::hw::present_rect_for(3440, 1440, true, 0.0);
    if (!same(off, gt4::vulkan::presentation_rect(3440, 1440, gt4::vulkan::Aspect::widescreen)) || whole.left != 0 ||
        whole.top != 0 || whole.width != 3440 || whole.height != 1440) {
      ++failures;
      std::fprintf(stderr, "FAIL: no-fill or whole-window rectangle\n");
    }
  }

  // IN-406: box halvings before the final bilinear blit. The final blit must reduce, never magnify a halved level.
  {
    struct Halving {
      std::uint32_t src_w, src_h, dst_w, dst_h, max_levels, expected;
    };
    const Halving known[] = {
        {1920, 1344, 1920, 1080, 4, 0},  // 3x in a 1080p window (owner test 13): one bilinear reduction, no halving
        {3840, 2688, 1920, 1080, 4, 1},  // 6x in 1080p: one box halving to 1920 x 1344, then a 0.8x reduction
        {3840, 2688, 2434, 1369, 4, 0},  // 6x in the owner's 3440 x 1369 window: one 0.63x bilinear reduction
        {7680, 5376, 1920, 1080, 4, 2},  // 12x in 1080p: two halvings to 1920 x 1344
        {7680, 5376, 1920, 1080, 2, 1},  // capped by the image's mip levels
        {1280, 896, 1920, 1080, 4, 0},   // 2x in 1080p: a magnification, nothing to halve
        {3840, 2688, 0, 0, 4, 0},        // empty rectangle
    };
    for (const Halving &h : known) {
      const std::uint32_t got = gt4::hw::present_box_halvings(h.src_w, h.src_h, h.dst_w, h.dst_h, h.max_levels);
      const std::uint32_t lw = h.src_w >> got, lh = h.src_h >> got;
      const bool magnifies = h.dst_w && h.dst_h && got > 0 && (lw < h.dst_w || lh < h.dst_h);
      if (got != h.expected || magnifies) {
        ++failures;
        std::fprintf(stderr, "FAIL: halvings %ux%u into %ux%u (levels %u) gave %u expected %u\n", h.src_w, h.src_h,
                     h.dst_w, h.dst_h, h.max_levels, got, h.expected);
      }
    }
  }

  if (failures == 0) std::printf("hw_present_rect: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
