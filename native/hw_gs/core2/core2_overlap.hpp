// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// TASK-184: the exact "may these two primitives share an output pixel" test of core 2's no-interlock path. Pure
// arithmetic, no Vulkan, so a fixture can check it against brute force.
//
// The no-interlock path (IN-466, IN-471) splits a batch into one draw per primitive and records a fragment barrier
// whenever a primitive "may share a pixel" with an earlier one since the last barrier: two read-modify-writes of the same
// frame or depth word need an order, and only the barrier gives one through storage images. Every false "may share" is a
// barrier the GPU did not need. The test below answers the question the hardware answers, in two steps:
//
//  1. the output-pixel box: the pixels the primitive can cover on the raster grid the GPU samples (a pixel is covered
//     when its centre at k + 0.5 lies inside the primitive; the grid maps a 12.4 guest coordinate c to
//     scale * c/16 + 0.5 * shift), clipped by the primitive's own scissor. This separates primitives whose boxes meet in
//     native units but whose pixels never do (a scissor clip, or shapes that fall on different pixels).
//  2. for two axis-aligned right triangles (the two halves of every GT4 sprite, and the shape of everything drawn as a
//     quad) a per-row interval test with the top-left fill rule: a pixel centre belongs to the shape on its left or top
//     edge and not on its right or bottom edge, so two quads that share an edge tile exactly and share no pixel. Two
//     general triangles keep the separating-axis test of IN-471 (a pair shares no pixel when one of the six edges has the
//     other triangle entirely outside it).
//
// Both steps only ever *widen* what a primitive covers (1/320 of a pixel of slack for the float rounding of an edge; the
// diagonal is tested closed), so a "share no pixel" answer never hides a real pair: the pictures stay bit-identical and
// only the barriers the GPU did not need disappear.
#pragma once

#include <cmath>
#include <cstdint>

namespace hwgs::core2::overlap {

// A primitive's coverage in the two spaces the decision needs: 12.4 native units (the edges) and output pixels (the
// raster grid).
struct Shape {
  std::int32_t x[3] = {0, 0, 0}, y[3] = {0, 0, 0};
  // The vertex bounds in 12.4 units. A line or a point keeps its three slots holding one vertex, so the bounds are
  // widened by its own slack when the caller builds it.
  std::int32_t minx = 0, maxx = 0, miny = 0, maxy = 0;
  bool triangle = false;
  // The output pixels the primitive can cover, [px0, px1) x [py0, py1), already clipped by its scissor. Empty
  // (px1 <= px0) means it covers nothing and waits for nobody.
  std::int32_t px0 = 0, py0 = 0, px1 = 0, py1 = 0;
  // 0: a box (a line or a point), 1: a general triangle, 2: an axis-aligned right triangle.
  std::uint8_t kind = 0;
};

// The vertex bounds, for a caller that has only filled the vertices.
inline void set_bounds(Shape &shape) {
  shape.minx = shape.maxx = shape.x[0];
  shape.miny = shape.maxy = shape.y[0];
  const int count = shape.triangle ? 3 : 1;
  for (int i = 1; i < count; ++i) {
    if (shape.x[i] < shape.minx) shape.minx = shape.x[i];
    if (shape.x[i] > shape.maxx) shape.maxx = shape.x[i];
    if (shape.y[i] < shape.miny) shape.miny = shape.y[i];
    if (shape.y[i] > shape.maxy) shape.maxy = shape.y[i];
  }
}

// The slack of the coverage model along an edge, in output pixels: 1/320, more than the float rounding of an edge at the
// largest scale the game runs and far less than half a pixel, so the model is a superset of what the rasteriser covers.
constexpr double kEdgeSlack = 1.0 / 320.0;

// A 12.4 native coordinate on the output raster grid (the mapping core2 uses for the vertex stage and the fill coverage).
inline double raster_x(std::int32_t native, double scale, double shift) {
  return scale * (static_cast<double>(native) / 16.0) + 0.5 * shift;
}

// The covered output pixel columns [px0, px1) of a 12.4 coordinate span, before the scissor: a pixel is covered when its
// centre k + 0.5 lies in [low, high), and the slack widens the interval so no covered pixel is left out. The rows use the
// same mapping, so one function serves both axes.
inline void pixel_span(std::int32_t native0, std::int32_t native1, double scale, double shift, std::int32_t &first,
                       std::int32_t &end) {
  const double low = raster_x(native0 < native1 ? native0 : native1, scale, shift);
  const double high = raster_x(native0 < native1 ? native1 : native0, scale, shift);
  first = static_cast<std::int32_t>(std::ceil(low - 0.5 - kEdgeSlack));
  end = static_cast<std::int32_t>(std::ceil(high - 0.5 + kEdgeSlack));
}

// The three vertices of the shape as the corners of an axis-aligned box, with the right angle at (`rx`, `ry`), or
// `false` when they are not (a general triangle, or a degenerate one).
struct RightCorners {
  bool valid = false;
  std::int32_t rx = 0, ry = 0;  // the right-angle corner
  std::int32_t tx = 0, ty = 0;  // the tips of the horizontal and the vertical leg
};

inline bool present_vertex(const Shape &shape, std::int32_t x, std::int32_t y) {
  for (int i = 0; i < 3; ++i) {
    if (shape.x[i] == x && shape.y[i] == y) return true;
  }
  return false;
}

inline RightCorners right_corners(const Shape &shape) {
  RightCorners out;
  if (!shape.triangle) return out;
  std::int32_t minx = shape.x[0], maxx = shape.x[0], miny = shape.y[0], maxy = shape.y[0];
  for (int i = 1; i < 3; ++i) {
    if (shape.x[i] < minx) minx = shape.x[i];
    if (shape.x[i] > maxx) maxx = shape.x[i];
    if (shape.y[i] < miny) miny = shape.y[i];
    if (shape.y[i] > maxy) maxy = shape.y[i];
  }
  if (minx == maxx || miny == maxy) return out;
  // Every vertex must be a corner of the box: a triangle with a vertex in the middle of an edge (a slanted triangle
  // whose box it fills only partly) is not this shape, and treating it as one would decide its pixels wrongly.
  for (int i = 0; i < 3; ++i) {
    const bool at_x = shape.x[i] == minx || shape.x[i] == maxx;
    const bool at_y = shape.y[i] == miny || shape.y[i] == maxy;
    if (!at_x || !at_y) return out;
    for (int j = i + 1; j < 3; ++j) {
      if (shape.x[i] == shape.x[j] && shape.y[i] == shape.y[j]) return out;  // two vertices on one corner: no area
    }
  }
  // The right angle is at the corner diagonally opposite the one the three vertices leave out.
  if (!present_vertex(shape, maxx, maxy)) {
    out.rx = minx;
    out.ry = miny;
  } else if (!present_vertex(shape, minx, maxy)) {
    out.rx = maxx;
    out.ry = miny;
  } else if (!present_vertex(shape, maxx, miny)) {
    out.rx = minx;
    out.ry = maxy;
  } else if (!present_vertex(shape, minx, miny)) {
    out.rx = maxx;
    out.ry = maxy;
  } else {
    return out;  // four corners: not a triangle
  }
  out.tx = out.rx == minx ? maxx : minx;
  out.ty = out.ry == miny ? maxy : miny;
  if (!present_vertex(shape, out.tx, out.ry) || !present_vertex(shape, out.rx, out.ty)) return out;
  out.valid = true;
  return out;
}

// Whether an edge of `a` has all of `b` on its outer side (IN-471, unchanged): a vertex of `b` that is also a vertex of
// `a` may lie on the edge, because identical vertices snap identically and the fill rule gives each sample of a shared
// edge to one triangle; any other vertex needs a gap of 1/16 native pixel, more than the vertex stage's rounding can
// close.
inline bool edge_separates(const Shape &a, const Shape &b) {
  for (int i = 0; i < 3; ++i) {
    const int j = (i + 1) % 3, k = (i + 2) % 3;
    const std::int64_t ex = a.x[j] - a.x[i], ey = a.y[j] - a.y[i];
    const std::int64_t third = ex * (a.y[k] - a.y[i]) - ey * (a.x[k] - a.x[i]);
    if (third == 0) return true;  // no area: covers nothing
    const std::int64_t sign = third > 0 ? 1 : -1;
    const std::int64_t gap = (ex < 0 ? -ex : ex) + (ey < 0 ? -ey : ey);
    bool outside = true;
    for (int m = 0; m < 3 && outside; ++m) {
      const bool sharedVertex = (b.x[m] == a.x[0] && b.y[m] == a.y[0]) || (b.x[m] == a.x[1] && b.y[m] == a.y[1]) ||
                                (b.x[m] == a.x[2] && b.y[m] == a.y[2]);
      outside = sign * (ex * (b.y[m] - a.y[i]) - ey * (b.x[m] - a.x[i])) <= (sharedVertex ? 0 : -gap);
    }
    if (outside) return true;
  }
  return false;
}

// An axis-aligned right triangle on the raster grid: a vertical leg at `x_fixed`, rows [y_lo, y_hi), and a diagonal
// x = slope * y + intercept. On every row inside the band the covered columns are the half-open interval of the two ends,
// which is the top-left fill rule: the left/top end belongs to the shape, the right/bottom end does not, so two shapes
// that share an edge share exactly no pixel.
struct RightTriangle {
  bool valid = false;
  bool diagonal_is_low = false;  // the diagonal end of a row is the interval's low end
  double x_fixed = 0.0;
  double slope = 0.0, intercept = 0.0;
  double y_lo = 0.0, y_hi = 0.0;
};

inline RightTriangle right_triangle_of(const RightCorners &corners, double scale, double shift) {
  RightTriangle out;
  if (!corners.valid) return out;
  const double x_fixed = raster_x(corners.rx, scale, shift);
  const double y_a = raster_x(corners.ry, scale, shift);  // the legs' row, where the diagonal starts
  const double y_b = raster_x(corners.ty, scale, shift);  // the vertical leg's end
  const double x_b = raster_x(corners.tx, scale, shift);  // the diagonal's column on the legs' row
  if (y_a == y_b) return out;
  out.valid = true;
  out.x_fixed = x_fixed;
  out.y_lo = y_a < y_b ? y_a : y_b;
  out.y_hi = y_a < y_b ? y_b : y_a;
  out.slope = (x_fixed - x_b) / (y_b - y_a);
  out.intercept = x_b - out.slope * y_a;
  const double mid = 0.5 * (out.y_lo + out.y_hi);
  out.diagonal_is_low = (out.slope * mid + out.intercept) < x_fixed;
  return out;
}

// One shape's row interval [lo, hi) as two linear functions of y.
struct Interval {
  double lo_slope = 0.0, lo_intercept = 0.0, hi_slope = 0.0, hi_intercept = 0.0;
};

inline Interval interval_of(const RightTriangle &r) {
  Interval out;
  if (r.diagonal_is_low) {
    out.lo_slope = r.slope;
    out.lo_intercept = r.intercept;
    out.hi_intercept = r.x_fixed;
  } else {
    out.lo_intercept = r.x_fixed;
    out.hi_slope = r.slope;
    out.hi_intercept = r.intercept;
  }
  return out;
}

// A row overlap narrower than this many output pixels is a shared edge, not a shared pixel: the fill rule gives a sample
// that lies exactly on the edge two shapes have in common to one of them. It is also far above the double rounding of the
// two parameterisations of one line (about 1e-13 px here), which is what the two halves of a quad produce.
constexpr double kTouchEpsilon = 1.0e-6;

// Whether `a * y < b` holds for some y in [lo, hi) with the overlap margin, and the sub-interval where it does.
inline bool solve_less(double a, double b, double lo, double hi, double margin, double &out_lo, double &out_hi) {
  const double target = b - margin;
  if (a > 0.0) {
    out_lo = lo;
    out_hi = target / a;
  } else if (a < 0.0) {
    out_lo = target / a;
    out_hi = hi;
  } else {
    if (!(0.0 < target)) return false;
    out_lo = lo;
    out_hi = hi;
  }
  if (out_lo < lo) out_lo = lo;
  if (out_hi > hi) out_hi = hi;
  return out_hi > out_lo;
}

// do the two right triangles share a pixel: "lo_a < hi_b and lo_b < hi_a" for some row of the common band, solved in
// closed form (each of the two conditions is linear in y, because one end of a right triangle's row is the vertical leg
// and the other the diagonal). Both conditions carry the overlap margin, so two shapes that only touch along an edge or a
// corner answer "no": their pixels are the fill rule's, and it gives every shared sample to one of them.
inline bool right_triangles_may_share(const RightTriangle &a, const RightTriangle &b) {
  if (!a.valid || !b.valid) return true;  // unknown: wait
  const double band_lo = a.y_lo > b.y_lo ? a.y_lo : b.y_lo;
  const double band_hi = a.y_hi < b.y_hi ? a.y_hi : b.y_hi;
  if (!(band_hi - band_lo > kTouchEpsilon)) return false;  // no shared row
  const Interval ia = interval_of(a), ib = interval_of(b);
  double lo1 = 0.0, hi1 = 0.0, lo2 = 0.0, hi2 = 0.0;
  if (!solve_less(ia.lo_slope - ib.hi_slope, ib.hi_intercept - ia.lo_intercept, band_lo, band_hi, kTouchEpsilon, lo1, hi1)) {
    return false;
  }
  if (!solve_less(ib.lo_slope - ia.hi_slope, ia.hi_intercept - ib.lo_intercept, band_lo, band_hi, kTouchEpsilon, lo2, hi2)) {
    return false;
  }
  const double lo = lo1 > lo2 ? lo1 : lo2;
  const double hi = hi1 < hi2 ? hi1 : hi2;
  // The band is continuous and the rasteriser samples rows, so a band narrower than the pixel grid can still return
  // "share" here; that is conservative, and the pixel boxes have already removed the cases with no row at all.
  return hi - lo > kTouchEpsilon;
}

// The decision, with the raster grid the right-triangle row test needs: does `b` need an order against `a`?
inline bool may_share_pixel(const Shape &a, const Shape &b, double scale, double shift) {
  if (a.px1 <= a.px0 || a.py1 <= a.py0 || b.px1 <= b.px0 || b.py1 <= b.py0) return false;  // covers nothing
  if (a.px1 <= b.px0 || b.px1 <= a.px0 || a.py1 <= b.py0 || b.py1 <= a.py0) return false;  // no pixel in common
  if (!a.triangle || !b.triangle) return true;  // lines and points are tested as boxes (conservative)
  if (a.kind == 2 && b.kind == 2) {
    const RightTriangle ra = right_triangle_of(right_corners(a), scale, shift);
    const RightTriangle rb = right_triangle_of(right_corners(b), scale, shift);
    if (ra.valid && rb.valid) return right_triangles_may_share(ra, rb);
  }
  return !edge_separates(a, b) && !edge_separates(b, a);
}

// Fills a shape's pixel box from its bounds, on the raster grid of the pass, clipped by the scissor and the target. The
// kind is set here so a caller cannot forget it.
inline void set_pixel_box(Shape &shape, std::uint32_t scale, float shift, std::int32_t left, std::int32_t top,
                          std::int32_t right, std::int32_t bottom) {
  const double s = static_cast<double>(scale), sh = static_cast<double>(shift);
  shape.kind = shape.triangle ? (right_corners(shape).valid ? std::uint8_t{2} : std::uint8_t{1}) : std::uint8_t{0};
  pixel_span(shape.minx, shape.maxx, s, sh, shape.px0, shape.px1);
  pixel_span(shape.miny, shape.maxy, s, sh, shape.py0, shape.py1);
  if (shape.px0 < left) shape.px0 = left;
  if (shape.py0 < top) shape.py0 = top;
  if (shape.px1 > right) shape.px1 = right;
  if (shape.py1 > bottom) shape.py1 = bottom;
  if (shape.px1 < shape.px0) shape.px1 = shape.px0;
  if (shape.py1 < shape.py0) shape.py1 = shape.py0;
}

}  // namespace hwgs::core2::overlap
