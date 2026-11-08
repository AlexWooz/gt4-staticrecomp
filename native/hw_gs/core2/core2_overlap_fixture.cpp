// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// TASK-184: the output-pixel overlap test of core 2's no-interlock path (core2_overlap.hpp) against brute force.
//
// The test decides whether a primitive must wait for an earlier one. A wrong "no shared pixel" drops a barrier the picture
// needs, so the fixture checks the property that matters: whenever the test says the two primitives share no pixel, a
// brute-force scan of the pixels both boxes cover finds no pixel centre inside both triangles either. It also reports how
// often the test says "share" although brute force finds no shared pixel (a false positive: one barrier the GPU did not
// need, which is only a cost), and pins the cases that matter for GT4: the two halves of a sprite, adjacent sprites, and
// a scissor that removes a primitive's pixels entirely.
#include "core2_overlap.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using hwgs::core2::overlap::Shape;

// A tiny deterministic generator: the same cases on every run and every machine.
struct Random {
  std::uint32_t state = 0x9E3779B9u;
  std::uint32_t next() {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
  }
  std::int32_t range(std::int32_t low, std::int32_t high) {  // inclusive
    return low + static_cast<std::int32_t>(next() % static_cast<std::uint32_t>(high - low + 1));
  }
};

Shape triangle(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1, std::int32_t x2, std::int32_t y2) {
  Shape shape;
  shape.triangle = true;
  shape.x[0] = x0;
  shape.y[0] = y0;
  shape.x[1] = x1;
  shape.y[1] = y1;
  shape.x[2] = x2;
  shape.y[2] = y2;
  hwgs::core2::overlap::set_bounds(shape);
  return shape;
}

// The signed perpendicular distance from a point to the line through a and b, in 12.4 units.
double edge_distance(const Shape &shape, int edge, double px, double py) {
  const int i = edge, j = (edge + 1) % 3;
  const double ex = shape.x[j] - shape.x[i], ey = shape.y[j] - shape.y[i];
  const double length = std::sqrt(ex * ex + ey * ey);
  if (length == 0.0) return 0.0;
  return (ex * (py - shape.y[i]) - ey * (px - shape.x[i])) / length;
}

// Whether the pixel centre is strictly inside the triangle. Strict, with a tiny epsilon for the double arithmetic: a
// centre exactly on an edge belongs to one of the two triangles that share it by the fill rule, so only the interior is
// the property the barrier test must never lose.
bool covers(const Shape &shape, double scale, double shift, std::int32_t px, std::int32_t py) {
  if (!shape.triangle) return false;  // lines and points are not tested by brute force here
  const double cx = px + 0.5, cy = py + 0.5;
  // The centre back in 12.4 units, so the distances below are in the same space as the edges.
  const double native_x = (cx - 0.5 * shift) * 16.0 / scale;
  const double native_y = (cy - 0.5 * shift) * 16.0 / scale;
  const double eps = 1.0e-9;
  bool all_positive = true, all_negative = true;
  for (int i = 0; i < 3; ++i) {
    const double d = edge_distance(shape, i, native_x, native_y);
    if (!(d > eps)) all_positive = false;
    if (!(d < -eps)) all_negative = false;
  }
  return all_positive || all_negative;
}

long false_positives = 0, shared_pairs = 0, disjoint_pairs = 0;
int failures = 0;

// Compares one pair with brute force and checks the exactness direction.
void check_pair(const Shape &a, const Shape &b, double scale, double shift, const char *what) {
  const bool decision = hwgs::core2::overlap::may_share_pixel(a, b, scale, shift);
  bool brute = false;
  const std::int32_t x0 = a.px0 > b.px0 ? a.px0 : b.px0;
  const std::int32_t x1 = a.px1 < b.px1 ? a.px1 : b.px1;
  const std::int32_t y0 = a.py0 > b.py0 ? a.py0 : b.py0;
  const std::int32_t y1 = a.py1 < b.py1 ? a.py1 : b.py1;
  for (std::int32_t py = y0; py < y1 && !brute; ++py) {
    for (std::int32_t px = x0; px < x1 && !brute; ++px) {
      brute = covers(a, scale, shift, px, py) && covers(b, scale, shift, px, py);
    }
  }
  if (!decision && brute) {
    ++failures;
    if (failures <= 10) {
      std::printf("  FAIL (%s, scale %.0f): the test says no shared pixel, brute force finds one: "
                  "a=(%d,%d)(%d,%d)(%d,%d) b=(%d,%d)(%d,%d)(%d,%d) boxes %d,%d..%d,%d / %d,%d..%d,%d\n",
                  what, scale, a.x[0], a.y[0], a.x[1], a.y[1], a.x[2], a.y[2], b.x[0], b.y[0], b.x[1], b.y[1],
                  b.x[2], b.y[2], a.px0, a.py0, a.px1, a.py1, b.px0, b.py0, b.px1, b.py1);
    }
  }
  if (brute) ++shared_pairs;
  if (decision && !brute) ++false_positives;
  if (!decision) ++disjoint_pairs;
}

// A sprite as the front end draws it: two triangles of one axis-aligned quad, `diagonal` 0 or 1.
void sprite_triangles(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1, int diagonal, Shape out[2]) {
  if (diagonal == 0) {
    out[0] = triangle(x0, y0, x1, y0, x0, y1);
    out[1] = triangle(x1, y0, x1, y1, x0, y1);
  } else {
    out[0] = triangle(x0, y0, x1, y0, x1, y1);
    out[1] = triangle(x0, y0, x1, y1, x0, y1);
  }
  for (int i = 0; i < 2; ++i) hwgs::core2::overlap::set_pixel_box(out[i], 1, 1.0f, 0, 0, 640, 448);
}

}  // namespace

int main() {
  using namespace hwgs::core2::overlap;
  const std::uint32_t scales[] = {1, 2, 3, 12};

  // 1. The two halves of one sprite share no pixel, at every scale and both diagonals: the fill rule gives the samples
  //    on the shared diagonal to one of them.
  for (std::uint32_t scale : scales) {
    for (int diagonal = 0; diagonal < 2; ++diagonal) {
      Shape halves[2];
      sprite_triangles(0, 0, 64, 48, diagonal, halves);
      for (int i = 0; i < 2; ++i) set_pixel_box(halves[i], scale, 1.0f, 0, 0, 640, 448);
      if (may_share_pixel(halves[0], halves[1], static_cast<double>(scale), 1.0)) {
        ++failures;
        std::printf("  FAIL: the halves of a sprite (diagonal %d) share a pixel at %ux\n", diagonal, scale);
      }
      check_pair(halves[0], halves[1], static_cast<double>(scale), 1.0, "sprite halves");
    }
  }

  // 2. Sprites that tile side by side or stacked share no pixel either (the case a vertex-space test cannot separate).
  for (std::uint32_t scale : scales) {
    Shape left[2], right[2], above[2], below[2];
    sprite_triangles(0, 0, 64, 48, 0, left);
    sprite_triangles(64, 0, 128, 48, 0, right);
    sprite_triangles(0, 0, 64, 48, 0, above);
    sprite_triangles(0, 48, 64, 96, 1, below);
    for (int i = 0; i < 2; ++i) {
      set_pixel_box(left[i], scale, 1.0f, 0, 0, 640, 448);
      set_pixel_box(right[i], scale, 1.0f, 0, 0, 640, 448);
      set_pixel_box(above[i], scale, 1.0f, 0, 0, 640, 448);
      set_pixel_box(below[i], scale, 1.0f, 0, 0, 640, 448);
    }
    for (int i = 0; i < 2; ++i) {
      for (int j = 0; j < 2; ++j) {
        if (may_share_pixel(left[i], right[j], static_cast<double>(scale), 1.0)) {
          ++failures;
          std::printf("  FAIL: two sprites side by side share a pixel at %ux\n", scale);
        }
        if (may_share_pixel(above[i], below[j], static_cast<double>(scale), 1.0)) {
          ++failures;
          std::printf("  FAIL: two sprites stacked share a pixel at %ux\n", scale);
        }
        check_pair(left[i], right[j], static_cast<double>(scale), 1.0, "side by side");
        check_pair(above[i], below[j], static_cast<double>(scale), 1.0, "stacked");
      }
    }
  }

  // 3. A scissor that removes the primitive's pixels makes the answer "no" whatever the geometry says.
  {
    Shape a = triangle(0, 0, 16, 0, 0, 16);
    Shape b = triangle(0, 0, 16, 0, 0, 16);
    set_pixel_box(a, 1, 1.0f, 100, 100, 200, 200);
    set_pixel_box(b, 1, 1.0f, 100, 100, 200, 200);
    if (may_share_pixel(a, b, 1.0, 1.0)) {
      ++failures;
      std::printf("  FAIL: a primitive clipped away by its scissor still shares a pixel\n");
    }
    if (a.px0 < 100 || a.py0 < 100 || a.px1 > 200 || a.py1 > 200) {
      ++failures;
      std::printf("  FAIL: the pixel box %d,%d..%d,%d is not inside its scissor\n", a.px0, a.py0, a.px1, a.py1);
    }
  }

  // 4. Random sprite pairs and random general triangles against brute force, at every scale.
  Random random;
  for (std::uint32_t scale : scales) {
    const double s = static_cast<double>(scale);
    for (int n = 0; n < 400; ++n) {
      const std::int32_t x0 = random.range(0, 150), y0 = random.range(0, 150);
      const std::int32_t w = random.range(1, 40), h = random.range(1, 40);
      Shape first[2], second[2];
      sprite_triangles(x0 * 16, y0 * 16, (x0 + w) * 16, (y0 + h) * 16, n & 1, first);
      const std::int32_t x1 = x0 + random.range(-w, w + 8), y1 = y0 + random.range(-h, h + 8);
      const std::int32_t w1 = random.range(1, 40), h1 = random.range(1, 40);
      sprite_triangles(x1 * 16, y1 * 16, (x1 + w1) * 16, (y1 + h1) * 16, (n >> 1) & 1, second);
      for (int i = 0; i < 2; ++i) {
        set_pixel_box(first[i], scale, 1.0f, 0, 0, 1920, 1080);
        set_pixel_box(second[i], scale, 1.0f, 0, 0, 1920, 1080);
      }
      for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) check_pair(first[i], second[j], s, 1.0, "random sprites");
      }
    }
    // General triangles: a vertex anywhere in the box, which is what the vertex-space test still decides.
    for (int n = 0; n < 200; ++n) {
      Shape a = triangle(random.range(0, 100) * 16, random.range(0, 100) * 16, random.range(0, 100) * 16,
                         random.range(0, 100) * 16, random.range(0, 100) * 16, random.range(0, 100) * 16);
      Shape b = triangle(random.range(0, 100) * 16, random.range(0, 100) * 16, random.range(0, 100) * 16,
                         random.range(0, 100) * 16, random.range(0, 100) * 16, random.range(0, 100) * 16);
      set_pixel_box(a, scale, 1.0f, 0, 0, 1920, 1080);
      set_pixel_box(b, scale, 1.0f, 0, 0, 1920, 1080);
      check_pair(a, b, s, 1.0, "random general triangles");
    }
  }

  std::printf("hw_core2_overlap: %ld pairs share a pixel, %ld share none, %ld false positives (%.1f%% of the shares)\n",
              shared_pairs, disjoint_pairs, false_positives,
              shared_pairs + false_positives == 0 ? 0.0
                                                  : 100.0 * static_cast<double>(false_positives) /
                                                        static_cast<double>(shared_pairs + false_positives));
  std::printf("hw_core2_overlap: %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
