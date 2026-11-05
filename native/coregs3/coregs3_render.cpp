// TASK-090 CoreGS 3.0: the offline reference rasteriser. See coregs3_render.hpp for what it does and does not do.
#include "coregs3_render.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace gt4::coregs3 {
namespace {

float as_float(std::uint32_t bits) {
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof value);
  return value;
}

struct ClipVertex {
  float x = 0, y = 0, w = 0;  // screen position and view depth (1/w is the perspective weight)
  float r = 1, g = 1, b = 1, a = 1;
  bool valid = false;
};

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

void rasterise(const Mesh &mesh, const DrawMatrix &matrix, const RenderOptions &options,
               std::vector<std::uint8_t> &rgba, std::vector<std::uint8_t> &mask, std::vector<float> &depth,
               RenderStats &stats) {
  const std::uint32_t width = options.width, height = options.height;
  std::vector<ClipVertex> projected(mesh.vertices.size());
  for (std::size_t index = 0; index < mesh.vertices.size(); ++index)
    projected[index] = project(mesh.vertices[index], matrix, options);

  if (options.points) {
    for (std::size_t index = 0; index < mesh.vertices.size(); ++index) {
      const ClipVertex &vertex = projected[index];
      if (!vertex.valid) continue;
      const int centre_x = static_cast<int>(vertex.x);
      const int centre_y = static_cast<int>(vertex.y);
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
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
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area == 0.0f) continue;
    int min_x = static_cast<int>(std::floor(std::min({a.x, b.x, c.x})));
    int max_x = static_cast<int>(std::ceil(std::max({a.x, b.x, c.x})));
    int min_y = static_cast<int>(std::floor(std::min({a.y, b.y, c.y})));
    int max_y = static_cast<int>(std::ceil(std::max({a.y, b.y, c.y})));
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x > static_cast<int>(width) - 1) max_x = static_cast<int>(width) - 1;
    if (max_y > static_cast<int>(height) - 1) max_y = static_cast<int>(height) - 1;
    if (min_x > max_x || min_y > max_y) {
      stats.triangles_clipped += 1;
      continue;
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
        if (options.depth_test && !(depth_value < depth[pixel])) continue;
        // The GS interpolates Gouraud colours linearly in screen space; perspective correction is only for the texture
        // coordinates (STQ), which this reference does not use yet.
        const float red = alpha * a.r + beta * b.r + gamma * c.r;
        const float green = alpha * a.g + beta * b.g + gamma * c.g;
        const float blue = alpha * a.b + beta * b.b + gamma * c.b;
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
}

}  // namespace

DrawMatrix draw_matrix(const CapturedDraw &draw) {
  DrawMatrix matrix;
  for (std::uint32_t row = 0; row < 4; ++row) {
    for (std::uint32_t lane = 0; lane < 4; ++lane) {
      matrix.row[row][lane] = as_float(draw.vf[row][lane]);
      if (!std::isfinite(matrix.row[row][lane])) matrix.valid = false;
    }
  }
  // func_0049E548 multiplies rows 0-2 by the shape's `value`; a value of 0 uploads the raw rows.
  const float scale = as_float(draw.cmd_value);
  if (scale != 0.0f) {
    for (std::uint32_t row = 0; row < 3; ++row)
      for (std::uint32_t lane = 0; lane < 4; ++lane) matrix.row[row][lane] *= scale;
  }
  const bool rows_used = matrix.row[0][0] != 0.0f || matrix.row[0][1] != 0.0f || matrix.row[1][0] != 0.0f ||
                         matrix.row[1][1] != 0.0f || matrix.row[2][0] != 0.0f || matrix.row[2][1] != 0.0f ||
                         matrix.row[3][3] != 0.0f;
  matrix.valid = rows_used;
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
  mesh_options.flip_v = options.flip_v;
  mesh_options.topology = options.topology;

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
    for (const Mesh &mesh : meshes) rasterise(mesh, object_matrix, options, rgba, mask, depth, stats);
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
    rasterise(mesh, matrix, options, rgba, mask, depth, stats);
  }
  for (std::uint8_t value : mask) stats.covered_pixels += value != 0 ? 1u : 0u;
  return true;
}

}  // namespace gt4::coregs3
