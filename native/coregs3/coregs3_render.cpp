// TASK-090 CoreGS 3.0: the offline reference rasteriser. See coregs3_render.hpp for what it does and does not do.
#include "coregs3_render.hpp"

#include "coregs3_lighting.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace gt4::coregs3 {
namespace {

struct ClipVertex {
  float x = 0, y = 0, w = 0;  // screen position and view depth (1/w is the perspective weight)
  float r = 1, g = 1, b = 1, a = 1;
  bool valid = false;
};

// The object-space path keeps its own row-major camera: it never uses the captured matrix, so it does not go through
// the measured VU1 convention. The arithmetic is unchanged from the first offline pictures.
ClipVertex project(const MeshVertex &vertex, const DrawMatrix &matrix, const RenderOptions &options) {
  ClipVertex out;
  const float x = vertex.position[0], y = vertex.position[1], z = vertex.position[2];
  const float cx = matrix.row[0][0] * x + matrix.row[0][1] * y + matrix.row[0][2] * z + matrix.row[0][3];
  const float cy = matrix.row[1][0] * x + matrix.row[1][1] * y + matrix.row[1][2] * z + matrix.row[1][3];
  const float cw = matrix.row[3][0] * x + matrix.row[3][1] * y + matrix.row[3][2] * z + matrix.row[3][3];
  if (!(cw > 1e-6f) || !std::isfinite(cw)) return out;  // behind the eye or degenerate: the CPU reference clips it
  const float inv = 1.0f / cw;
  const float ndc_x = cx * inv;
  const float ndc_y = cy * inv;
  out.x = (ndc_x * 0.5f + 0.5f) * static_cast<float>(options.width);
  out.y = options.flip_y ? (0.5f - ndc_y * 0.5f) * static_cast<float>(options.height)
                         : (ndc_y * 0.5f + 0.5f) * static_cast<float>(options.height);
  out.w = cw;
  out.r = vertex.color[0] / 255.0f;
  out.g = vertex.color[1] / 255.0f;
  out.b = vertex.color[2] / 255.0f;
  out.a = vertex.color[3] / 255.0f;
  out.valid = std::isfinite(out.x) && std::isfinite(out.y);
  return out;
}

std::uint8_t clamp_level(float value) {
  const float scaled = value * 255.0f + 0.5f;
  if (!(scaled > 0.0f)) return 0;
  if (scaled >= 255.0f) return 255;
  return static_cast<std::uint8_t>(scaled);
}

// Screen coordinates are finite but, after near-plane clipping, a vertex can project thousands of pixels outside the
// viewport. Converting an out-of-int-range float to int is undefined host behaviour, so saturate before the cast. For
// every value the old direct cast handled (|value| well under 2^31) the result is identical, including the case of a
// triangle wholly to the right of the viewport (`min_x` stays one past the last column, so the bounds test still
// counts it as clipped).
int pixel_low(float value, int last) {
  if (!(value >= 0.0f)) return 0;
  if (value >= static_cast<float>(last) + 1.0f) return last + 1;
  return static_cast<int>(std::floor(value));
}

int pixel_high(float value, int last) {
  if (!(value >= 0.0f)) return 0;
  if (value > static_cast<float>(last)) return last;
  return static_cast<int>(std::ceil(value));
}

int pixel_centre(float value) {
  constexpr int limit = 1 << 20;  // far outside any supported viewport, far inside int
  if (!(value >= static_cast<float>(-limit))) return -limit;
  if (value >= static_cast<float>(limit)) return limit;
  return static_cast<int>(value);
}

void draw_points(const Mesh &mesh, const std::vector<ClipVertex> &projected, const RenderOptions &options,
                 std::vector<std::uint8_t> &rgba, std::vector<std::uint8_t> &mask, std::vector<float> &depth,
                 RenderStats &stats) {
  const std::uint32_t width = options.width, height = options.height;
  for (std::size_t index = 0; index < mesh.vertices.size(); ++index) {
    const ClipVertex &vertex = projected[index];
    if (!vertex.valid) continue;
    const int centre_x = pixel_centre(vertex.x);
    const int centre_y = pixel_centre(vertex.y);
    const int radius = static_cast<int>(options.point_size);
    for (int dy = -radius; dy <= radius; ++dy)
      for (int dx = -radius; dx <= radius; ++dx) {
        const int x = centre_x + dx, y = centre_y + dy;
        if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) continue;
        const std::size_t pixel = static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x);
        const float depth_value = vertex.w;
        if (options.depth_test && depth_value > depth[pixel]) continue;
        depth[pixel] = depth_value;
        rgba[pixel * 4 + 0] = clamp_level(vertex.r);
        rgba[pixel * 4 + 1] = clamp_level(vertex.g);
        rgba[pixel * 4 + 2] = clamp_level(vertex.b);
        rgba[pixel * 4 + 3] = 255;
        mask[pixel] = 1;
      }
  }
  stats.triangles_drawn = static_cast<std::uint32_t>(mesh.vertices.size());
}

void rasterise_triangle(const ClipVertex &a, const ClipVertex &b, const ClipVertex &c, const RenderOptions &options,
                        float flat_r, float flat_g, float flat_b, std::vector<std::uint8_t> &rgba,
                        std::vector<std::uint8_t> &mask, std::vector<float> &depth, RenderStats &stats) {
  const std::uint32_t width = options.width, height = options.height;
  const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
  if (area == 0.0f) return;
  const int last_x = static_cast<int>(width) - 1;
  const int last_y = static_cast<int>(height) - 1;
  const int min_x = pixel_low(std::min({a.x, b.x, c.x}), last_x);
  const int max_x = pixel_high(std::max({a.x, b.x, c.x}), last_x);
  const int min_y = pixel_low(std::min({a.y, b.y, c.y}), last_y);
  const int max_y = pixel_high(std::max({a.y, b.y, c.y}), last_y);
  if (min_x > max_x || min_y > max_y) {
    stats.triangles_clipped += 1;
    return;
  }
  bool drew = false;
  const float inv_area = 1.0f / area;
  for (int y = min_y; y <= max_y; ++y) {
    for (int x = min_x; x <= max_x; ++x) {
      const float px = static_cast<float>(x) + 0.5f;
      const float py = static_cast<float>(y) + 0.5f;
      const float w0 = ((b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x)) * inv_area;
      const float w1 = ((c.x - b.x) * (py - b.y) - (c.y - b.y) * (px - b.x)) * inv_area;
      const float w2 = ((a.x - c.x) * (py - c.y) - (a.y - c.y) * (px - c.x)) * inv_area;
      if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
      // e0 (across A-B) weighs C, e1 (across B-C) weighs A, e2 (across C-A) weighs B.
      const float alpha = w1, beta = w2, gamma = w0;
      // Perspective-correct view depth at the pixel.
      const float inv_w = alpha / a.w + beta / b.w + gamma / c.w;
      if (!(inv_w > 0.0f)) continue;
      const float depth_value = 1.0f / inv_w;
      const std::size_t pixel = static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x);
      // The GS tests with GEQUAL against a reversed Z: a later primitive wins at equal depth, which is what keeps the
      // overlapping parts of one car from z-fighting in the offline picture.
      if (options.depth_test && !(depth_value <= depth[pixel])) continue;
      // The GS interpolates Gouraud colours linearly in screen space; perspective correction is only for the texture
      // coordinates (STQ), which this reference does not use yet.
      const float red = options.flat_shading ? flat_r : alpha * a.r + beta * b.r + gamma * c.r;
      const float green = options.flat_shading ? flat_g : alpha * a.g + beta * b.g + gamma * c.g;
      const float blue = options.flat_shading ? flat_b : alpha * a.b + beta * b.b + gamma * c.b;
      depth[pixel] = depth_value;
      rgba[pixel * 4 + 0] = clamp_level(red);
      rgba[pixel * 4 + 1] = clamp_level(green);
      rgba[pixel * 4 + 2] = clamp_level(blue);
      rgba[pixel * 4 + 3] = 255;
      mask[pixel] = 1;
      drew = true;
    }
  }
  if (drew) stats.triangles_drawn += 1;
}

// Computes the flat-shading colour of one triangle from its model-space face normal and the draw's linear part. The
// matrix is the row-major form of the measured columns, so this is the same normal rotation as the first pictures.
void flat_color(const Mesh &mesh, const DrawMatrix &matrix, const RenderOptions &options, std::size_t first,
                std::size_t second, std::size_t third, float &red, float &green, float &blue) {
  red = green = blue = 1.0f;
  if (!options.flat_shading) return;
  const MeshVertex &va = mesh.vertices[first];
  const MeshVertex &vb = mesh.vertices[second];
  const MeshVertex &vc = mesh.vertices[third];
  const float e1[3] = {vb.position[0] - va.position[0], vb.position[1] - va.position[1],
                       vb.position[2] - va.position[2]};
  const float e2[3] = {vc.position[0] - va.position[0], vc.position[1] - va.position[1],
                       vc.position[2] - va.position[2]};
  const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                      e1[0] * e2[1] - e1[1] * e2[0]};
  const float vn[3] = {matrix.row[0][0] * n[0] + matrix.row[0][1] * n[1] + matrix.row[0][2] * n[2],
                       matrix.row[1][0] * n[0] + matrix.row[1][1] * n[1] + matrix.row[1][2] * n[2],
                       matrix.row[2][0] * n[0] + matrix.row[2][1] * n[1] + matrix.row[2][2] * n[2]};
  const float length = std::sqrt(vn[0] * vn[0] + vn[1] * vn[1] + vn[2] * vn[2]);
  float shade = 1.0f;
  if (length > 1e-12f) {
    const float lx = options.light_direction[0], ly = options.light_direction[1], lz = options.light_direction[2];
    const float ll = std::sqrt(lx * lx + ly * ly + lz * lz);
    const float cosine = ll > 0.0f ? std::fabs((vn[0] * lx + vn[1] * ly + vn[2] * lz) / (length * ll)) : 0.0f;
    shade = 0.18f + 0.82f * cosine;
  }
  red = 0.5f * (va.color[0] + vb.color[0] + vc.color[0]) / 255.0f * shade;
  green = 0.5f * (va.color[1] + vb.color[1] + vc.color[1]) / 255.0f * shade;
  blue = 0.5f * (va.color[2] + vb.color[2] + vc.color[2]) / 255.0f * shade;
  if (!mesh.has_colors) {
    red = green = blue = static_cast<float>(options.fallback_color[0]) / 255.0f * shade;
  }
}

// The object-space path: the caller projected every vertex with the generated camera, and this loop is byte for byte
// the original triangle rasteriser.
void rasterise_projected(const Mesh &mesh, const std::vector<ClipVertex> &projected, const DrawMatrix &matrix,
                         const RenderOptions &options, std::vector<std::uint8_t> &rgba,
                         std::vector<std::uint8_t> &mask, std::vector<float> &depth, RenderStats &stats) {
  if (options.points) {
    draw_points(mesh, projected, options, rgba, mask, depth, stats);
    return;
  }
  for (std::size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
    const ClipVertex &a = projected[mesh.indices[index + 0]];
    const ClipVertex &b = projected[mesh.indices[index + 1]];
    const ClipVertex &c = projected[mesh.indices[index + 2]];
    stats.triangles += 1;
    if (!a.valid || !b.valid || !c.valid) {
      stats.triangles_clipped += 1;
      continue;
    }
    float red = 1.0f, green = 1.0f, blue = 1.0f;
    flat_color(mesh, matrix, options, mesh.indices[index + 0], mesh.indices[index + 1], mesh.indices[index + 2], red,
               green, blue);
    rasterise_triangle(a, b, c, options, red, green, blue, rgba, mask, depth, stats);
  }
}

// Clip-space vertex with the attributes the rasteriser interpolates across a clip. Only the near plane (w) and the
// colours need to be kept: clipping creates new vertices along a model-space edge, where every attribute is linear.
struct ClipVertexIn {
  ClipPosition clip;
  float r = 1, g = 1, b = 1, a = 1;
};

// Cuts a triangle against clip.w >= kMinClipW (Sutherland-Hodgman on one plane). Returns 3 or 4 vertices, or 0 when
// the triangle is completely behind the eye. Intersection w is written exactly as kMinClipW so the subsequent
// projection classifies the new vertex as valid instead of rounding it back across the near plane.
std::size_t clip_near_plane(const ClipVertexIn in[3], ClipVertexIn out[4]) noexcept {
  std::size_t count = 0;
  for (std::size_t index = 0; index < 3; ++index) {
    const ClipVertexIn &current = in[index];
    const ClipVertexIn &next = in[(index + 1) % 3];
    const bool current_inside = current.clip.w >= kMinClipW;
    const bool next_inside = next.clip.w >= kMinClipW;
    if (current_inside) out[count++] = current;
    if (current_inside != next_inside) {
      // One endpoint is inside the plane and the other outside, so the denominator cannot be zero.
      const float t = (kMinClipW - current.clip.w) / (next.clip.w - current.clip.w);
      ClipVertexIn cut;
      cut.clip.x = current.clip.x + (next.clip.x - current.clip.x) * t;
      cut.clip.y = current.clip.y + (next.clip.y - current.clip.y) * t;
      cut.clip.z = current.clip.z + (next.clip.z - current.clip.z) * t;
      cut.clip.w = kMinClipW;
      cut.r = current.r + (next.r - current.r) * t;
      cut.g = current.g + (next.g - current.g) * t;
      cut.b = current.b + (next.b - current.b) * t;
      cut.a = current.a + (next.a - current.a) * t;
      out[count++] = cut;
    }
  }
  return count;
}

bool clip_position_finite(const ClipPosition &clip) noexcept {
  return std::isfinite(clip.x) && std::isfinite(clip.y) && std::isfinite(clip.z) && std::isfinite(clip.w);
}

// True when the triangle cannot contribute: it is wholly behind the eye plane, or wholly in front of it and wholly
// outside one of the four side planes of the clip volume. The second test is an optimisation only; the rasteriser
// would otherwise walk a huge bounding box and reject every pixel.
bool triangle_outside_clip_volume(const ClipPosition &a, const ClipPosition &b, const ClipPosition &c) noexcept {
  if (a.w < kMinClipW && b.w < kMinClipW && c.w < kMinClipW) return true;
  if (!(a.w >= kMinClipW && b.w >= kMinClipW && c.w >= kMinClipW)) return false;
  if (a.x < -a.w && b.x < -b.w && c.x < -c.w) return true;
  if (a.x > a.w && b.x > b.w && c.x > c.w) return true;
  if (a.y < -a.w && b.y < -b.w && c.y < -c.w) return true;
  if (a.y > a.w && b.y > b.w && c.y > c.w) return true;
  return false;
}

// Applies the measured VU1 diffuse lighting (coregs3_lighting.hpp) to a freshly built mesh, in place: a lit vertex's
// colour becomes the RGBA the VU1 fragment would write. A draw keeps the base-colour fallback, and is counted, when it
// has no measured light block, no normals or no colour channel; a vertex also falls back when it lacks the normal or the
// raw position the formula needs. The flat-shading diagnostic keeps its fixed headlight and its base colours, so it is
// never lit by this function.
void apply_lighting(const CapturedDraw &draw, Mesh &mesh, const RenderOptions &options, RenderStats &stats) {
  const std::uint32_t vertex_count = static_cast<std::uint32_t>(mesh.vertices.size());
  if (options.flat_shading) {
    stats.draws_fallback += 1;
    stats.vertices_fallback += vertex_count;
    return;
  }
  LightBlock block;
  if (decode_light_block(draw, block) != LightBlockStatus::ok || !mesh.has_normals || !mesh.has_colors) {
    stats.draws_fallback += 1;
    stats.vertices_fallback += vertex_count;
    return;
  }
  std::uint32_t lit = 0;
  for (MeshVertex &vertex : mesh.vertices) {
    std::array<std::uint8_t, 4> rgba{};
    if (shade_vertex(block, vertex, rgba)) {
      vertex.color = rgba;
      ++lit;
    }
  }
  stats.vertices_lit += lit;
  stats.vertices_fallback += vertex_count - lit;
  if (lit != 0) stats.draws_lit += 1;
  if (lit == 0) stats.draws_fallback += 1;
}

// The screen path: the measured VU1 transform (coregs3_vertex.hpp) followed by clip-space rejection and near-plane
// clipping. No 12.4 wrapped coordinate exists anywhere in this path, so the rasteriser can only see a true projection.
void rasterise_clip_space(const Mesh &mesh, const DrawMatrix &matrix, const RenderOptions &options,
                          std::vector<std::uint8_t> &rgba, std::vector<std::uint8_t> &mask,
                          std::vector<float> &depth, RenderStats &stats) {
  const float width = static_cast<float>(options.width);
  const float height = static_cast<float>(options.height);
  std::vector<ProjectionResult> projected(mesh.vertices.size());
  for (std::size_t index = 0; index < mesh.vertices.size(); ++index) {
    const ModelPosition position = {mesh.vertices[index].position[0], mesh.vertices[index].position[1],
                                    mesh.vertices[index].position[2]};
    projected[index] = project_vertex(matrix.columns, position, width, height, options.flip_y);
  }

  if (options.points) {
    std::vector<ClipVertex> points(mesh.vertices.size());
    for (std::size_t index = 0; index < mesh.vertices.size(); ++index) {
      const ProjectionResult &result = projected[index];
      if (result.status != ProjectionStatus::ok) continue;
      ClipVertex &point = points[index];
      point.x = result.screen.x;
      point.y = result.screen.y;
      point.w = result.clip.w;
      const MeshVertex &source = mesh.vertices[index];
      point.r = source.color[0] / 255.0f;
      point.g = source.color[1] / 255.0f;
      point.b = source.color[2] / 255.0f;
      point.a = source.color[3] / 255.0f;
      point.valid = true;
    }
    draw_points(mesh, points, options, rgba, mask, depth, stats);
    return;
  }

  for (std::size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
    const std::uint32_t ia = mesh.indices[index + 0];
    const std::uint32_t ib = mesh.indices[index + 1];
    const std::uint32_t ic = mesh.indices[index + 2];
    stats.triangles += 1;
    const ProjectionResult &pa = projected[ia];
    const ProjectionResult &pb = projected[ib];
    const ProjectionResult &pc = projected[ic];
    // A non-finite clip position cannot be clipped or projected safely; it is dropped, never replaced by a guess.
    if (!clip_position_finite(pa.clip) || !clip_position_finite(pb.clip) || !clip_position_finite(pc.clip)) {
      stats.triangles_clipped += 1;
      continue;
    }
    if (triangle_outside_clip_volume(pa.clip, pb.clip, pc.clip)) {
      stats.triangles_clipped += 1;
      continue;
    }
    ClipVertexIn input[3];
    input[0].clip = pa.clip;
    input[1].clip = pb.clip;
    input[2].clip = pc.clip;
    const MeshVertex &va = mesh.vertices[ia];
    const MeshVertex &vb = mesh.vertices[ib];
    const MeshVertex &vc = mesh.vertices[ic];
    input[0].r = va.color[0] / 255.0f;
    input[0].g = va.color[1] / 255.0f;
    input[0].b = va.color[2] / 255.0f;
    input[0].a = va.color[3] / 255.0f;
    input[1].r = vb.color[0] / 255.0f;
    input[1].g = vb.color[1] / 255.0f;
    input[1].b = vb.color[2] / 255.0f;
    input[1].a = vb.color[3] / 255.0f;
    input[2].r = vc.color[0] / 255.0f;
    input[2].g = vc.color[1] / 255.0f;
    input[2].b = vc.color[2] / 255.0f;
    input[2].a = vc.color[3] / 255.0f;
    ClipVertexIn polygon[4];
    const std::size_t count = clip_near_plane(input, polygon);
    if (count < 3) {
      stats.triangles_clipped += 1;
      continue;
    }
    ClipVertex screen[4];
    bool projectable = true;
    for (std::size_t corner = 0; corner < count; ++corner) {
      const ProjectionResult result =
          project_clip_position(polygon[corner].clip, width, height, options.flip_y);
      if (result.status != ProjectionStatus::ok) {
        projectable = false;
        break;
      }
      screen[corner].x = result.screen.x;
      screen[corner].y = result.screen.y;
      screen[corner].w = result.clip.w;
      screen[corner].r = polygon[corner].r;
      screen[corner].g = polygon[corner].g;
      screen[corner].b = polygon[corner].b;
      screen[corner].a = polygon[corner].a;
      screen[corner].valid = true;
    }
    if (!projectable) {
      stats.triangles_clipped += 1;
      continue;
    }
    float red = 1.0f, green = 1.0f, blue = 1.0f;
    flat_color(mesh, matrix, options, ia, ib, ic, red, green, blue);
    for (std::size_t corner = 1; corner + 1 < count; ++corner)
      rasterise_triangle(screen[0], screen[corner], screen[corner + 1], options, red, green, blue, rgba, mask, depth,
                         stats);
  }
}

}  // namespace

DrawMatrix draw_matrix(const CapturedDraw &draw) {
  DrawMatrix matrix;
  matrix.columns = vertex_columns_from_vu0(draw.vf);
  // The equivalent row-major form: row r holds component r of every column, i.e. matrix[r][c] = column[c][r]. The
  // flat-shading normal rotation and the --ndc diagnostic use this form; the measured projection uses the columns
  // directly. The columns are UNSCALED: the 0xC1 packet rows are vf * cmd.value, but the metre positions already
  // carry the 1/4096 that cancels `value` (docs/COREGS3-HOOKS.md §10.2).
  for (std::uint32_t row = 0; row < 4; ++row)
    for (std::uint32_t lane = 0; lane < 4; ++lane) matrix.row[row][lane] = matrix.columns.column[lane][row];
  const bool rows_used = matrix.row[0][0] != 0.0f || matrix.row[0][1] != 0.0f || matrix.row[1][0] != 0.0f ||
                         matrix.row[1][1] != 0.0f || matrix.row[2][0] != 0.0f || matrix.row[2][1] != 0.0f ||
                         matrix.row[3][3] != 0.0f;
  matrix.valid = vertex_columns_finite(matrix.columns) && rows_used;
  return matrix;
}

bool render_frame(const FrameSnapshot &frame, const RenderOptions &options, std::vector<std::uint8_t> &rgba,
                  std::vector<std::uint8_t> &mask, RenderStats &stats, std::string &error) {
  (void)error;
  const std::uint32_t width = options.width, height = options.height;
  if (width == 0 || height == 0) return false;
  rgba.assign(static_cast<std::size_t>(width) * height * 4u, 0);
  mask.assign(static_cast<std::size_t>(width) * height, 0);
  for (std::size_t index = 0; index < static_cast<std::size_t>(width) * height; ++index) {
    rgba[index * 4 + 0] = options.background[0];
    rgba[index * 4 + 1] = options.background[1];
    rgba[index * 4 + 2] = options.background[2];
    rgba[index * 4 + 3] = options.background[3];
  }
  std::vector<float> depth(static_cast<std::size_t>(width) * height, std::numeric_limits<float>::infinity());

  stats = RenderStats{};
  MeshOptions mesh_options;
  mesh_options.position_scale = options.position_scale;
  mesh_options.topology = options.topology;
  mesh_options.max_edge = options.max_edge;

  if (options.object_space) {
    // Object-space mode: build every mesh first, frame the union of their vertices, and draw with that one matrix.
    std::vector<Mesh> meshes;
    meshes.reserve(frame.draws.size());
    float low[3] = {1e30f, 1e30f, 1e30f};
    float high[3] = {-1e30f, -1e30f, -1e30f};
    for (const CapturedDraw &draw : frame.draws) {
      stats.draws += 1;
      Mesh mesh;
      std::string ignored;
      if (!build_mesh(frame.packets.data(), frame.packets.size(), draw, mesh_options, mesh, ignored)) {
        stats.shapes_refused += 1;
        meshes.emplace_back();
        continue;
      }
      stats.shapes_drawn += 1;
      stats.vertices += static_cast<std::uint32_t>(mesh.vertices.size());
      // The object-space diagnostic framing does not use the captured draw's light block: it keeps the base colours
      // and its own generated camera, so every vertex here is a counted fallback.
      stats.draws_fallback += 1;
      stats.vertices_fallback += static_cast<std::uint32_t>(mesh.vertices.size());
      for (const MeshVertex &vertex : mesh.vertices)
        for (std::uint32_t axis = 0; axis < 3; ++axis) {
          low[axis] = std::min(low[axis], vertex.position[axis]);
          high[axis] = std::max(high[axis], vertex.position[axis]);
        }
      meshes.push_back(std::move(mesh));
    }
    if (!(high[0] >= low[0])) return false;
    const float centre[3] = {(low[0] + high[0]) * 0.5f, (low[1] + high[1]) * 0.5f, (low[2] + high[2]) * 0.5f};
    float radius = 0.5f * std::max({high[0] - low[0], high[1] - low[1], high[2] - low[2]});
    if (!(radius > 0.0f)) radius = 1.0f;
    const float yaw = options.object_yaw * 3.14159265358979f / 180.0f;
    const float pitch = options.object_pitch * 3.14159265358979f / 180.0f;
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    const float scale = 1.8f / radius;
    // Yaw about Y, then pitch about X: rows of the object-to-view rotation, times the fit scale. w stays 1.
    DrawMatrix object_matrix;
    object_matrix.row[0][0] = cy * scale;
    object_matrix.row[0][2] = sy * scale;
    object_matrix.row[1][0] = -sy * sp * scale;
    object_matrix.row[1][1] = cp * scale;
    object_matrix.row[1][2] = cy * sp * scale;
    object_matrix.row[2][0] = sy * cp;
    object_matrix.row[2][1] = sp;
    object_matrix.row[2][2] = -cy * cp;
    object_matrix.row[3][3] = 1.0f;
    for (std::uint32_t row = 0; row < 3; ++row)
      object_matrix.row[row][3] = -(object_matrix.row[row][0] * centre[0] + object_matrix.row[row][1] * centre[1] +
                                   object_matrix.row[row][2] * centre[2]);
    object_matrix.valid = true;
    for (const Mesh &mesh : meshes) {
      std::vector<ClipVertex> projected(mesh.vertices.size());
      for (std::size_t index = 0; index < mesh.vertices.size(); ++index)
        projected[index] = project(mesh.vertices[index], object_matrix, options);
      rasterise_projected(mesh, projected, object_matrix, options, rgba, mask, depth, stats);
    }
    for (std::uint8_t value : mask) stats.covered_pixels += value != 0 ? 1u : 0u;
    return true;
  }

  for (const CapturedDraw &draw : frame.draws) {
    stats.draws += 1;
    const DrawMatrix matrix = draw_matrix(draw);
    if (!matrix.valid) {
      stats.shapes_refused += 1;
      if (stats.refusals.size() < 32) stats.refusals.push_back("draw " + std::to_string(draw.sequence) + ": no matrix");
      continue;
    }
    Mesh mesh;
    std::string mesh_error;
    if (!build_mesh(frame.packets.data(), frame.packets.size(), draw, mesh_options, mesh, mesh_error)) {
      stats.shapes_refused += 1;
      if (stats.refusals.size() < 32)
        stats.refusals.push_back("draw " + std::to_string(draw.sequence) + ": " + mesh_error);
      continue;
    }
    stats.shapes_drawn += 1;
    stats.vertices += static_cast<std::uint32_t>(mesh.vertices.size());
    apply_lighting(draw, mesh, options, stats);
    rasterise_clip_space(mesh, matrix, options, rgba, mask, depth, stats);
  }
  for (std::uint8_t value : mask) stats.covered_pixels += value != 0 ? 1u : 0u;
  return true;
}

}  // namespace gt4::coregs3
