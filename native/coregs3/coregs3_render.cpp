// TASK-090 CoreGS 3.0: the offline reference rasteriser. See coregs3_render.hpp for what it does and does not do.
#include "coregs3_render.hpp"

#include "coregs3_lighting.hpp"
#include "coregs3_reflection.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace gt4::coregs3 {
namespace {

// The GEQUAL depth window of the reflection pass. The base and reflection draws of one car part are two captured
// shape calls; their float projections agree to a few 1e-4 relative units where the GS's 24-bit Z is identical, so an
// exact float compare could reject a fragment the GS accepts. The relative window is ~30 GS Z LSBs at the measured
// depth and far below the car's own depth separation.
constexpr float kReflectionDepthTolerance = 1.0e-3f;

struct ClipVertex {
  float x = 0, y = 0, w = 0;  // screen position and view depth (1/w is the perspective weight)
  float r = 1, g = 1, b = 1, a = 1;
  float u = 0, v = 0;   // model UV (perspective-correct interpolation; the VU1's Q cancels in S/Q)
  bool has_uv = false;
  // The reflection pass's per-vertex attributes: the pinned texgen S,T (perspective-correct, like the model UV) and
  // the tapped vertex fog (interpolated linearly, like the Gouraud colours). `has_reflection` is true when the renderer
  // computed a coordinate for this vertex. The world path (TASK-090 track M1) reuses `fog`/`has_fog` for its tapped
  // per-vertex fog.
  float rs = 0, rt = 0;
  float fog = 255.0f;
  bool has_fog = false;
  // TASK-090 texture mip: the explicit shader LOD of this vertex (chapter 03.6), interpolated linearly like the GPU
  // sampler the reference uses. A vertex without a tapped Q keeps `has_lod` false and the sampler its level-0 fallback.
  float lod = 0.0f;
  bool has_lod = false;
  // TASK-090 texture mip: the tapped STQ Q. The GS interpolates S, T and Q linearly in screen space and divides, so the
  // UV is lerp(u*Q)/lerp(Q), not the 1/w perspective-correct form unless Q is the 1/w weight (measured: it is not).
  float q = 1.0f;
  bool has_q = false;
  bool has_reflection = false;
  bool valid = false;
};

float float_bits(std::uint32_t bits) {
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof value);
  return value;
}

// TASK-090 track M1: the world matrix source rule lives in coregs3_render.hpp (WorldMatrixTracker) so the fixture can
// test it directly; the renderer only consumes it.


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
  out.u = vertex.uv[0];
  out.v = vertex.uv[1];
  out.has_uv = vertex.has_uv;
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
                        float flat_r, float flat_g, float flat_b, const TextureBinding *binding,
                        std::vector<std::uint8_t> &rgba, std::vector<std::uint8_t> &mask, std::vector<float> &depth,
                        RenderStats &stats, std::vector<std::uint8_t> *textured_mask, bool world, bool apply_fog,
                        bool write_depth, bool wet_reflection = false,
                        std::vector<std::uint8_t> *wet_reflection_mask = nullptr) {
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
  // The measured sampler path needs the model UV on every corner. An item whose channel has no UV keeps the lit colour.
  const bool can_sample = binding != nullptr && binding->texture != nullptr && a.has_uv && b.has_uv && c.has_uv;
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
      const std::size_t pixel = static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x);
      if (can_sample && textured_mask != nullptr) (*textured_mask)[pixel] = 1;
      // Perspective-correct view depth at the pixel.
      const float inv_w = alpha / a.w + beta / b.w + gamma / c.w;
      if (!(inv_w > 0.0f)) continue;
      const float depth_value = 1.0f / inv_w;
      // The GS tests with GEQUAL against a reversed Z: a later primitive wins at equal depth, which is what keeps the
      // overlapping parts of one car from z-fighting in the offline picture. The sky pass writes no comparable depth
      // (its own projection's w is not the course's view depth), so it neither tests nor writes depth here.
      if (options.depth_test && write_depth && !(depth_value <= depth[pixel])) {
        if (wet_reflection) stats.world_reflection_depth_failed += 1;
        continue;
      }
      // The GS interpolates Gouraud colours linearly in screen space; perspective correction is only for the texture
      // coordinates (STQ).
      const float red = options.flat_shading ? flat_r : alpha * a.r + beta * b.r + gamma * c.r;
      const float green = options.flat_shading ? flat_g : alpha * a.g + beta * b.g + gamma * c.g;
      const float blue = options.flat_shading ? flat_b : alpha * a.b + beta * b.b + gamma * c.b;
      // The vertex alpha feeds MODULATE (the GS texture function reads C's alpha). The untextured branch below still
      // writes the opaque alpha the pre-texture picture always had.
      const float vertex_alpha = options.flat_shading ? c.a : alpha * a.a + beta * b.a + gamma * c.a;
      std::array<std::uint8_t, 4> fragment{clamp_level(red), clamp_level(green), clamp_level(blue),
                                           clamp_level(vertex_alpha)};
      bool textured = false;
      bool depth_masked = false;
      // TASK-090 texture-mip: the material record of this fragment (filled when a texture sampled it) and the writer the
      // tool's --sample-dump reads at each colour write.
      std::array<std::uint8_t, 4> sample_vertex = fragment;
      std::array<std::uint8_t, 4> sample_texel{};
      std::uint32_t sample_level = 0;
      std::uint8_t sample_flags = 0;
      std::uint8_t sample_fog = 255;
      std::uint32_t sample_fog_colour = 0;
      const auto record_sample = [&](std::size_t pixel_index) {
        if (options.sample_dump == nullptr || (sample_flags & 1u) == 0 || binding == nullptr) return;
        if (pixel_index >= options.sample_dump->pixels.size()) return;
        TextureSampleDump::Pixel &record = options.sample_dump->pixels[pixel_index];
        record.flags = sample_flags;
        record.level = static_cast<std::uint8_t>(sample_level);
        std::memcpy(record.texel, sample_texel.data(), 4u);
        std::memcpy(record.vertex, sample_vertex.data(), 4u);
        std::memcpy(record.fragment, fragment.data(), 4u);
        record.fog = sample_fog;
        record.fog_colour[0] = static_cast<std::uint8_t>(sample_fog_colour & 0xFFu);
        record.fog_colour[1] = static_cast<std::uint8_t>((sample_fog_colour >> 8) & 0xFFu);
        record.fog_colour[2] = static_cast<std::uint8_t>((sample_fog_colour >> 16) & 0xFFu);
        record.page = static_cast<std::uint16_t>(binding->texture->tbp0);
      };
      if (can_sample) {
        const SampleState &state = binding->state;
        // The GS interpolates S, T and Q linearly in screen space and divides (chapter 01.5). The tapped Q is not the
        // 1/w weight the old form assumed: the measured Q*w spread inside a triangle reaches 92x, so the
        // perspective-correct form samples different texels than the reference. The world path uses the GS's own
        // arithmetic; paths without a tapped Q (the car pass) keep the perspective-correct form byte for byte.
        float u = (alpha * a.u / a.w + beta * b.u / b.w + gamma * c.u / c.w) / inv_w;
        float v = (alpha * a.v / a.w + beta * b.v / b.w + gamma * c.v / c.w) / inv_w;
        if (world && !options.world_no_mip && a.has_q && b.has_q && c.has_q) {
          const float q[3] = {a.q, b.q, c.q};
          const float uu[3] = {a.u, b.u, c.u};
          const float vv[3] = {a.v, b.v, c.v};
          stq_texture_coordinate(alpha, beta, gamma, q, uu, vv, u, v);
        }
        std::array<std::uint8_t, 4> sampled;
        // TASK-090 texture mip: a complete chain with a tapped LOD samples through the explicit-LOD path; anything else
        // keeps the level-0 subset and is counted.
        const bool mip = binding->texture->mip_ready && a.has_lod && b.has_lod && c.has_lod;
        const float lod = mip ? alpha * a.lod + beta * b.lod + gamma * c.lod : 0.0f;
        const SampleStatus sample_status =
            mip ? sample_texture_lod(*binding->texture, u, v, lod, binding->mip_base_level, fragment, sampled,
                                     &sample_texel, &sample_level)
                : sample_texture(*binding->texture, u, v, fragment, sampled, &sample_texel);
        if (mip) {
          sample_flags |= 4u;
          stats.world_mip_pixels += 1;
          stats.world_mip_levels[std::min<std::uint32_t>(sample_level, 7u)] += 1;
          stats.world_mip_base_levels[std::min<std::uint32_t>(binding->mip_base_level, 7u)] += 1;
        } else if (binding->texture->mip_ready) {
          stats.world_mip_missing_lod += 1;
        }
        if (sample_status != SampleStatus::ok) {
          // A per-pixel refusal (a wrapped texel outside the decoded rectangle): keep the lit colour and count it. The
          // static state was validated before rasterising, so nothing else can reach this branch.
          stats.sample_refused_pixels += 1;
          if (wet_reflection) stats.world_reflection_sample_refused += 1;
        } else {
          fragment = sampled;
          textured = true;
          sample_flags |= 1u;
          stats.textured_pixels += 1;
          // The world path (TASK-090 track M1) applies the tapped per-vertex fog toward FOGCOL before the alpha test,
          // exactly the GS pixel order the reflection pass uses. The sky pass's measured state has no fog and is
          // skipped by the caller; a fragment whose item carried no tapped fog is counted and left unfogged.
          if (world && apply_fog) {
            if (a.has_fog && b.has_fog && c.has_fog) {
              const float fog = alpha * a.fog + beta * b.fog + gamma * c.fog;
              const std::uint32_t fog_level =
                  fog <= 0.0f ? 0u : fog >= 255.0f ? 255u : static_cast<std::uint32_t>(fog + 0.5f);
              fragment = apply_vertex_fog(fragment, fog_level, state.fog_colour);
              sample_flags |= 2u;
              sample_fog = static_cast<std::uint8_t>(fog_level);
              sample_fog_colour = state.fog_colour;
              stats.world_fog_pixels += 1;
            } else {
              stats.world_fog_missing += 1;
            }
          }
          // Alpha test before the destination alpha test and the depth test: AFAIL KEEP writes neither colour nor
          // depth. The world path evaluates DATE itself, so it uses the ATE/ATST/AREF part alone.
          const AlphaTestResult test = world
                                           ? (options.world_ignore_alpha_test ? AlphaTestResult::pass
                                                                              : alpha_test_ate_only(state.test, fragment[3]))
                                           : alpha_test(state.test, fragment[3]);
          if (test == AlphaTestResult::fail) {
            stats.alpha_test_failed += 1;
            if (world) {
              // The measured AFAIL modes of the selected course items (TEST bits 12-13): 1 FBONLY writes colour and
              // keeps depth, 2 ZBONLY writes depth and keeps colour, 3 RGBONLY writes RGB and keeps the old alpha. The
              // fail path bypasses the depth test, exactly the GS test order (alpha test before the Z test). Counted.
              const std::uint32_t afail = static_cast<std::uint32_t>((state.test >> 12) & 3u);
              if (afail == 2) {
                depth[pixel] = depth_value;
                stats.world_afail_depth += 1;
                continue;
              }
              if (afail == 1 || afail == 3) {
                if (afail == 3) fragment[3] = rgba[pixel * 4 + 3];
                for (std::uint32_t lane = 0; lane < 4; ++lane) rgba[pixel * 4 + lane] = fragment[lane];
                mask[pixel] = 1;
                record_sample(pixel);
                stats.world_afail_written += 1;
                drew = true;
                continue;
              }
            }
            continue;
          }
          if (test != AlphaTestResult::pass) {
            stats.sample_refused_pixels += 1;
            continue;
          }
          if (world) {
            // Destination alpha test (TEST.DATE/DATM), against the framebuffer alpha of the selected draws. A pixel no
            // selected draw has written carries the offline background's alpha; that is counted, never assumed.
            const std::uint64_t test_register = state.test;
            if (((test_register >> 14) & 1u) != 0) {
              if (mask[pixel] == 0) stats.world_date_missing += 1;
              const bool expected = ((test_register >> 15) & 1u) != 0;
              const bool msb = (rgba[pixel * 4 + 3] & 0x80u) != 0;
              if (msb != expected) {
                stats.world_date_failed += 1;
                continue;
              }
            }
          }
          if (prim_blend_enabled(state.prim)) {
            if (world) {
              // The tapped ABE equation, evaluated as the GS does; the sky's (A Cs, B Cd, C As, D Cs) form is outside
              // the car pass's normal/additive families but inside blend_fragment's selector model.
              const std::array<std::uint8_t, 4> destination{rgba[pixel * 4 + 0], rgba[pixel * 4 + 1],
                                                            rgba[pixel * 4 + 2], rgba[pixel * 4 + 3]};
              std::array<std::uint8_t, 4> blended;
              blend_fragment(decode_blend_equation(state.alpha), fragment, destination, blended);
              fragment = blended;
              stats.world_blended_pixels += 1;
            } else {
              const BlendFamily family = blend_family(state.alpha);
              if (family == BlendFamily::other) {
                stats.sample_refused_pixels += 1;
                continue;
              }
              const std::array<std::uint8_t, 4> destination{rgba[pixel * 4 + 0], rgba[pixel * 4 + 1],
                                                            rgba[pixel * 4 + 2], rgba[pixel * 4 + 3]};
              std::array<std::uint8_t, 4> blended;
              blend_fragment(decode_blend_equation(state.alpha), fragment, destination, blended);
              fragment = blended;
              stats.blended_pixels += 1;
              if (family == BlendFamily::additive) stats.additive_pixels += 1;
            }
          }
          if (state.fbmsk != 0) {
            for (std::uint32_t lane = 0; lane < 4; ++lane) {
              const std::uint32_t keep = (state.fbmsk >> (8u * lane)) & 0xFFu;
              fragment[lane] = static_cast<std::uint8_t>((rgba[pixel * 4 + lane] & keep) |
                                                         (fragment[lane] & ~keep));
            }
            stats.fbmsk_pixels += 1;
          }
          depth_masked = state.zmsk != 0;
          if (depth_masked) stats.depth_masked_pixels += 1;
          if (wet_reflection) {
            // Name the FRAME.FBMSK form this fragment wrote under: the captured pass's main-view draws write destination
            // alpha only (0x00FFFFFF), the sphere-mapper draws write RGB only (0xFF000000). Anything else stays counted
            // in neither lane.
            if (state.fbmsk == 0x00FFFFFFu) stats.world_reflection_alpha_only += 1;
            else if (state.fbmsk == 0xFF000000u) stats.world_reflection_rgb_only += 1;
          }
        }
      }
      if (write_depth && !depth_masked) depth[pixel] = depth_value;
      if (!textured) {
        // The pre-texture picture: the lit/base colour with an opaque alpha, byte for byte as before this milestone.
        rgba[pixel * 4 + 0] = clamp_level(red);
        rgba[pixel * 4 + 1] = clamp_level(green);
        rgba[pixel * 4 + 2] = clamp_level(blue);
        rgba[pixel * 4 + 3] = 255;
      } else {
        for (std::uint32_t lane = 0; lane < 4; ++lane) rgba[pixel * 4 + lane] = fragment[lane];
        record_sample(pixel);
      }
      mask[pixel] = 1;
      if (wet_reflection) {
        stats.world_reflection_pixels += 1;
        if (wet_reflection_mask != nullptr) (*wet_reflection_mask)[pixel] = 1;
      }
      drew = true;
    }
  }
  if (drew) stats.triangles_drawn += 1;
}

// One fragment of the measured body/paint reflection pass (round 20). It is deliberately a separate function from
// rasterise_triangle: the pre-reflection picture keeps its exact arithmetic, and this pass carries the state the tap
// measured for the PRIM 0x7C draw:
//   * the environment map sampled at the pinned texgen S,T (perspective-correct, like the model UV) and MODULATE'd with
//     the lit vertex colour (the round-18 lighting output);
//   * the GS fog toward FOGCOL (black in the measured pass) with the tapped per-vertex fog;
//   * the alpha test (ATE/ATST/AREF, AFAIL KEEP) before the destination alpha test (TEST.DATE/DATM against the
//     framebuffer alpha the base draws wrote), then the GEQUAL depth test, exactly the GS pixel order;
//   * the additive ALPHA equation `(Cs-0)*Ad+Cd` (COLCLAMP = 1), FBMSK, and ZBUF.ZMSK (the measured pass writes no
//     depth); the fragment alpha As is what the pass writes to the alpha lane.
void rasterise_reflection_triangle(const ClipVertex &a, const ClipVertex &b, const ClipVertex &c,
                                   const RenderOptions &options, const TextureBinding &binding,
                                   std::vector<std::uint8_t> &rgba, std::vector<std::uint8_t> &mask,
                                   std::vector<float> &depth, RenderStats &stats,
                                   std::vector<std::uint8_t> *reflection_mask) {
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
  const SampleState &state = binding.state;
  const bool date_enabled = (state.test & (1ull << 14)) != 0;
  const bool date_expected = (state.test & (1ull << 15)) != 0;
  const bool can_sample = binding.texture != nullptr && a.has_reflection && b.has_reflection && c.has_reflection;
  const float inv_area = 1.0f / area;
  bool drew = false;
  for (int y = min_y; y <= max_y; ++y) {
    for (int x = min_x; x <= max_x; ++x) {
      const float px = static_cast<float>(x) + 0.5f;
      const float py = static_cast<float>(y) + 0.5f;
      const float w0 = ((b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x)) * inv_area;
      const float w1 = ((c.x - b.x) * (py - b.y) - (c.y - b.y) * (px - b.x)) * inv_area;
      const float w2 = ((a.x - c.x) * (py - c.y) - (a.y - c.y) * (px - c.x)) * inv_area;
      if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
      const float alpha = w1, beta = w2, gamma = w0;
      const std::size_t pixel = static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x);
      const float inv_w = alpha / a.w + beta / b.w + gamma / c.w;
      if (!(inv_w > 0.0f)) continue;
      const float depth_value = 1.0f / inv_w;
      // The lit vertex colour is MODULATE's C (the round-18 lighting output, the measured reflection's RGBAQ input).
      const float red = alpha * a.r + beta * b.r + gamma * c.r;
      const float green = alpha * a.g + beta * b.g + gamma * c.g;
      const float blue = alpha * a.b + beta * b.b + gamma * c.b;
      const float vertex_alpha = alpha * a.a + beta * b.a + gamma * c.a;
      const std::array<std::uint8_t, 4> fragment{clamp_level(red), clamp_level(green), clamp_level(blue),
                                                 clamp_level(vertex_alpha)};
      if (!can_sample) {
        // A vertex with no coordinate: the pass writes nothing here, and the item is counted as a refusal by the
        // caller before it reaches this function.
        stats.reflection_sample_refused += 1;
        continue;
      }
      const float s = (alpha * a.rs / a.w + beta * b.rs / b.w + gamma * c.rs / c.w) / inv_w;
      const float t = (alpha * a.rt / a.w + beta * b.rt / b.w + gamma * c.rt / c.w) / inv_w;
      std::array<std::uint8_t, 4> sampled;
      if (sample_texture(*binding.texture, s, t, fragment, sampled) != SampleStatus::ok) {
        stats.reflection_sample_refused += 1;
        continue;
      }
      // Fog: the tapped per-vertex value, interpolated like the Gouraud colours, mixed toward FOGCOL (black measured).
      const float fog = alpha * a.fog + beta * b.fog + gamma * c.fog;
      const std::uint32_t fog_level =
          fog <= 0.0f ? 0u : fog >= 255.0f ? 255u : static_cast<std::uint32_t>(fog + 0.5f);
      if (!binding.vertex_fog.empty()) {
        sampled = apply_vertex_fog(sampled, fog_level, state.fog_colour);
      } else {
        // No tapped fog for this item: the measured pass's FOGCOL black with fog 255 is the identity, so the colour is
        // kept and the fallback is counted.
        stats.reflection_fog_missing += 1;
      }
      // Alpha test before the destination alpha test, exactly the GS pixel order; AFAIL KEEP writes nothing.
      const AlphaTestResult test = alpha_test_ignore_date(state.test, sampled[3]);
      if (test == AlphaTestResult::fail) {
        stats.reflection_alpha_failed += 1;
        continue;
      }
      if (test != AlphaTestResult::pass) {
        stats.reflection_sample_refused += 1;
        continue;
      }
      // Destination alpha test: DATM selects the MSB the framebuffer must carry. A failure writes neither colour nor
      // depth (the GS intersects the write enables). A pixel no captured draw has written carries the offline
      // background's alpha where the real frame carries the track pass: that is counted, never silently assumed.
      if (date_enabled) {
        if (mask[pixel] == 0) stats.reflection_alpha_missing += 1;
        const bool msb = (rgba[pixel * 4 + 3] & 0x80u) != 0;
        if (msb != date_expected) {
          stats.reflection_date_failed += 1;
          continue;
        }
      }
      // ZTST GEQUAL against the reversed Z: nearer or equal wins. The reflection is the same surface the base draw
      // wrote, so the float projections can differ by a few 1e-4 relative units; the GS's 24-bit Z is identical there.
      if (options.depth_test && !(depth_value <= depth[pixel] * (1.0f + kReflectionDepthTolerance))) {
        stats.reflection_depth_failed += 1;
        continue;
      }
      std::array<std::uint8_t, 4> result = sampled;
      const BlendFamily family = blend_family(state.alpha);
      if (prim_blend_enabled(state.prim) && family == BlendFamily::additive) {
        const std::array<std::uint8_t, 4> destination{rgba[pixel * 4 + 0], rgba[pixel * 4 + 1],
                                                      rgba[pixel * 4 + 2], rgba[pixel * 4 + 3]};
        blend_fragment(decode_blend_equation(state.alpha), sampled, destination, result);
        stats.reflection_blended_pixels += 1;
      }
      if (state.fbmsk != 0) {
        for (std::uint32_t lane = 0; lane < 4; ++lane) {
          const std::uint32_t keep = (state.fbmsk >> (8u * lane)) & 0xFFu;
          result[lane] = static_cast<std::uint8_t>((rgba[pixel * 4 + lane] & keep) | (result[lane] & ~keep));
        }
        stats.reflection_fbmsk_pixels += 1;
      }
      for (std::uint32_t lane = 0; lane < 4; ++lane) rgba[pixel * 4 + lane] = result[lane];
      if (state.zmsk != 0) {
        stats.reflection_depth_masked_pixels += 1;
      } else {
        depth[pixel] = depth_value;
      }
      mask[pixel] = 1;
      if (reflection_mask != nullptr) (*reflection_mask)[pixel] = 1;
      stats.reflection_pixels += 1;
      drew = true;
    }
  }
  if (drew) stats.triangles_drawn += 1;
}

// Computes the flat-shading colour of one triangle from its model-space face normal and the draw's linear part. The
// matrix is the row-major form of the uploaded 0xC1 block; its linear rows differ from a metre transform's by one
// positive (or negative) uniform scale, which the normalisation below removes, so this is the same normal rotation as
// the measured transform.
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
    // The object-space diagnostic is a geometry view: it never samples the frame's texture table.
    rasterise_triangle(a, b, c, options, red, green, blue, nullptr, rgba, mask, depth, stats, nullptr, false, false,
                       true);
  }
}

// Clip-space vertex with the attributes the rasteriser interpolates across a clip. Only the near plane (w), the
// colours and the model UV need to be kept: clipping creates new vertices along a model-space edge, where every
// attribute is linear.
struct ClipVertexIn {
  ClipPosition clip;
  float r = 1, g = 1, b = 1, a = 1;
  float u = 0, v = 0;
  bool has_uv = false;
  float rs = 0, rt = 0;  // reflection texgen S,T (perspective attribute, interpolated like the model UV)
  float fog = 255.0f;    // reflection/world vertex fog (linear, like the Gouraud colours)
  bool has_fog = false;
  float lod = 0.0f;      // TASK-090 texture mip: the explicit shader LOD (linear, like the Gouraud colours)
  bool has_lod = false;
  float q = 1.0f;        // TASK-090 texture mip: the tapped STQ Q (linear, the GS's own STQ interpolation)
  bool has_q = false;
  bool has_reflection = false;
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
      cut.u = current.u + (next.u - current.u) * t;
      cut.v = current.v + (next.v - current.v) * t;
      cut.has_uv = current.has_uv && next.has_uv;
      cut.rs = current.rs + (next.rs - current.rs) * t;
      cut.rt = current.rt + (next.rt - current.rt) * t;
      cut.fog = current.fog + (next.fog - current.fog) * t;
      cut.has_fog = current.has_fog && next.has_fog;
      cut.lod = current.lod + (next.lod - current.lod) * t;
      cut.has_lod = current.has_lod && next.has_lod;
      cut.q = current.q + (next.q - current.q) * t;
      cut.has_q = current.has_q && next.has_q;
      cut.has_reflection = current.has_reflection && next.has_reflection;
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

// Applies the measured per-item VU1 lighting to a freshly built mesh, in place. Every vertex is shaded with its own
// item's program and block (coregs3_lighting.hpp, shade_mesh_items): the capture's light state and selector are per
// item, and a draw can mix programs. The flat-shading diagnostic keeps its fixed headlight and its base colours, so it
// is never lit by this function.
void apply_lighting(const CapturedDraw &draw, Mesh &mesh, const RenderOptions &options, RenderStats &stats) {
  const std::uint32_t vertex_count = static_cast<std::uint32_t>(mesh.vertices.size());
  if (options.flat_shading) {
    stats.draws_fallback += 1;
    stats.vertices_fallback += vertex_count;
    return;
  }
  LightCounters counters;
  shade_mesh_items(draw, mesh, counters, nullptr);
  stats.lighting.add(counters);
  stats.draws_lit += static_cast<std::uint32_t>(counters.draws_lit);
  stats.draws_fallback += static_cast<std::uint32_t>(counters.draws_fallback);
  stats.vertices_lit += static_cast<std::uint32_t>(counters.vertices_lit);
  stats.vertices_fallback += static_cast<std::uint32_t>(counters.vertices_fallback);
}

// The per-vertex inputs of the reflection pass, computed once per mesh before rasterising: the pinned texgen S,T (the
// MeshVertex position and normal through the item's own block) and the tapped vertex fog (indexed by the vertex's
// position inside its item).
struct ReflectionVertex {
  float s = 0.0f;
  float t = 0.0f;
  float fog = 255.0f;
  bool valid = false;
};

// The screen path: the measured VU1 transform (coregs3_vertex.hpp) followed by clip-space rejection and near-plane
// clipping. No 12.4 wrapped coordinate exists anywhere in this path, so the rasteriser can only see a true projection.
void rasterise_clip_space(const Mesh &mesh, const CapturedDraw &draw, const DrawMatrix &matrix,
                          const RenderOptions &options, const std::vector<TextureBinding> *bindings,
                          const std::vector<std::uint8_t> *reflection_items, std::vector<std::uint8_t> &rgba,
                          std::vector<std::uint8_t> &mask, std::vector<float> &depth, RenderStats &stats,
                          std::vector<std::uint8_t> *textured_mask, std::vector<std::uint8_t> *reflection_mask) {
  const float width = static_cast<float>(options.width);
  const float height = static_cast<float>(options.height);
  std::vector<ProjectionResult> projected(mesh.vertices.size());
  for (std::size_t index = 0; index < mesh.vertices.size(); ++index) {
    const MeshVertex &vertex = mesh.vertices[index];
    ModelPosition position = {vertex.position[0], vertex.position[1], vertex.position[2]};
    const VertexColumns *columns = nullptr;
    if (options.world_pass) {
      // TASK-090 track M1: the world path multiplies the vertex's captured units by the captured upload columns. A
      // MODE 2 family carries the full int32 lane (the exact value ITOF0 converts); a V3-16 family without MODE 2
      // carries the raw signed halfword; a V3-32 family carries the stored float.
      switch (vertex.encoding) {
      case PositionEncoding::integer_32:
        if (vertex.has_lane_position)
          position = {static_cast<float>(vertex.lane_position[0]), static_cast<float>(vertex.lane_position[1]),
                      static_cast<float>(vertex.lane_position[2])};
        stats.world_vertices_integer_32 += 1;
        break;
      case PositionEncoding::integer_16:
        if (vertex.has_raw_position)
          position = {static_cast<float>(vertex.raw_position[0]), static_cast<float>(vertex.raw_position[1]),
                      static_cast<float>(vertex.raw_position[2])};
        stats.world_vertices_integer_16 += 1;
        break;
      case PositionEncoding::float_32:
        stats.world_vertices_float_32 += 1;
        break;
      }
      columns = &matrix.upload;
    } else {
      // The car path: the VU1 multiplies the raw 16-bit integer of a V3-16 channel and the stored float of a V3-32
      // channel; the metre column set for each encoding differs by kModelPositionUnitsPerMetre (coregs3_vertex.hpp).
      columns = mesh_position_encoding(vertex) == PositionEncoding::integer_16 ? &matrix.integer_positions
                                                                             : &matrix.float_positions;
    }
    projected[index] = project_vertex(*columns, position, width, height, options.flip_y);
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
      point.u = source.uv[0];
      point.v = source.uv[1];
      point.has_uv = source.has_uv;
      point.valid = true;
    }
    draw_points(mesh, points, options, rgba, mask, depth, stats);
    return;
  }

  // The reflection pass's per-vertex data: the pinned texgen S,T and the tapped fog. An item is reflected only when
  // every one of its mesh vertices has a coordinate; a vertex outside the measured block/input form falls the whole
  // item back to the lit colour and is counted once (never a per-pixel silent default).
  std::vector<ReflectionVertex> reflection(mesh.vertices.size());
  std::vector<std::uint8_t> reflected;
  std::vector<std::uint32_t> first_vertex(draw.item_count, 0xFFFFFFFFu);
  for (std::size_t index = 0; index < mesh.vertices.size(); ++index) {
    const std::uint32_t item = mesh.vertices[index].item;
    if (item < first_vertex.size() && first_vertex[item] == 0xFFFFFFFFu)
      first_vertex[item] = static_cast<std::uint32_t>(index);
  }
  // TASK-090 track M1: the world path's tapped per-vertex fog, indexed by the vertex's position inside its item (the
  // tapped record emits one fog byte per vertex in the same order as the mesh's vertex list).
  std::vector<float> world_fog(mesh.vertices.size(), 255.0f);
  std::vector<std::uint8_t> world_fog_known(mesh.vertices.size(), 0);
  // TASK-090 texture mip: the explicit per-vertex LOD the table computed (empty for every non-mip binding).
  std::vector<float> world_lod(mesh.vertices.size(), 0.0f);
  std::vector<std::uint8_t> world_lod_known(mesh.vertices.size(), 0);
  // TASK-090 texture mip diagnostic: the tapped STQ Q, to check Q * clip.w is constant across a triangle.
  std::vector<float> world_q(mesh.vertices.size(), 0.0f);
  std::vector<std::uint8_t> world_q_known(mesh.vertices.size(), 0);
  if (options.world_pass && bindings != nullptr) {
    for (std::size_t index = 0; index < mesh.vertices.size(); ++index) {
      const std::uint32_t item = mesh.vertices[index].item;
      if (item >= bindings->size() || item >= first_vertex.size()) continue;
      const std::uint32_t local = static_cast<std::uint32_t>(index) - first_vertex[item];
      const std::vector<std::uint8_t> &fog = (*bindings)[item].vertex_fog;
      if (local < fog.size()) {
        world_fog[index] = static_cast<float>(fog[local]);
        world_fog_known[index] = 1;
      }
      const std::vector<float> &lod = (*bindings)[item].vertex_lod;
      if (local < lod.size()) {
        world_lod[index] = lod[local];
        world_lod_known[index] = 1;
      }
      const std::vector<float> &q = (*bindings)[item].vertex_q;
      if (local < q.size()) {
        world_q[index] = q[local];
        world_q_known[index] = 1;
      }
    }
  }
  if (options.reflections && reflection_items != nullptr && bindings != nullptr) {
    reflected = *reflection_items;
    for (std::uint32_t item = 0; item < reflected.size(); ++item) {
      if (reflected[item] == 0) continue;
      bool item_ok = true;
      for (std::size_t index = 0; index < mesh.vertices.size(); ++index) {
        const MeshVertex &vertex = mesh.vertices[index];
        if (vertex.item != item) continue;
        ReflectionCoordinate coordinate;
        if (reflection_st(item < draw.item_count ? draw.items[item] : CapturedItem{}, vertex, coordinate) !=
            ReflectionStatus::ok) {
          item_ok = false;
          stats.reflection_refused_st += 1;
          break;
        }
        ReflectionVertex &data = reflection[index];
        data.s = coordinate.s;
        data.t = coordinate.t;
        data.valid = true;
        const std::vector<std::uint8_t> &fog = (*bindings)[item].vertex_fog;
        const std::uint32_t local = static_cast<std::uint32_t>(index) - first_vertex[item];
        if (local < fog.size()) data.fog = static_cast<float>(fog[local]);
      }
      if (!item_ok) {
        reflected[item] = 0;
        stats.items_reflection_refused += 1;
      }
    }
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
    input[0].u = va.uv[0];
    input[0].v = va.uv[1];
    input[0].has_uv = va.has_uv;
    input[1].r = vb.color[0] / 255.0f;
    input[1].g = vb.color[1] / 255.0f;
    input[1].b = vb.color[2] / 255.0f;
    input[1].a = vb.color[3] / 255.0f;
    input[1].u = vb.uv[0];
    input[1].v = vb.uv[1];
    input[1].has_uv = vb.has_uv;
    input[2].r = vc.color[0] / 255.0f;
    input[2].g = vc.color[1] / 255.0f;
    input[2].b = vc.color[2] / 255.0f;
    input[2].a = vc.color[3] / 255.0f;
    input[2].u = vc.uv[0];
    input[2].v = vc.uv[1];
    input[2].has_uv = vc.has_uv;
    // The triangle's item decides whether this is the reflection pass; a mesh never mixes items inside a triangle.
    const std::uint32_t item = va.item;
    const bool reflection_pass =
        reflected.size() > item && reflected[item] != 0 && reflection[ia].valid && reflection[ib].valid &&
        reflection[ic].valid;
    if (reflection_pass) {
      input[0].rs = reflection[ia].s;
      input[0].rt = reflection[ia].t;
      input[0].fog = reflection[ia].fog;
      input[0].has_reflection = true;
      input[1].rs = reflection[ib].s;
      input[1].rt = reflection[ib].t;
      input[1].fog = reflection[ib].fog;
      input[1].has_reflection = true;
      input[2].rs = reflection[ic].s;
      input[2].rt = reflection[ic].t;
      input[2].fog = reflection[ic].fog;
      input[2].has_reflection = true;
    }
    if (options.world_pass) {
      input[0].fog = world_fog[ia];
      input[0].has_fog = world_fog_known[ia] != 0;
      input[1].fog = world_fog[ib];
      input[1].has_fog = world_fog_known[ib] != 0;
      input[2].fog = world_fog[ic];
      input[2].has_fog = world_fog_known[ic] != 0;
      input[0].lod = world_lod[ia];
      input[0].has_lod = world_lod_known[ia] != 0;
      input[1].lod = world_lod[ib];
      input[1].has_lod = world_lod_known[ib] != 0;
      input[2].lod = world_lod[ic];
      input[2].has_lod = world_lod_known[ic] != 0;
      input[0].q = world_q[ia];
      input[0].has_q = world_q_known[ia] != 0;
      input[1].q = world_q[ib];
      input[1].has_q = world_q_known[ib] != 0;
      input[2].q = world_q[ic];
      input[2].has_q = world_q_known[ic] != 0;
      // The UV interpolation assumes Q = k / w. Measure it: |Q * w| must be constant across the triangle.
      if (world_q_known[ia] != 0 && world_q_known[ib] != 0 && world_q_known[ic] != 0) {
        const double qa = std::fabs(static_cast<double>(world_q[ia]) * pa.clip.w);
        const double qb = std::fabs(static_cast<double>(world_q[ib]) * pb.clip.w);
        const double qc = std::fabs(static_cast<double>(world_q[ic]) * pc.clip.w);
        const double low = std::min({qa, qb, qc});
        const double high = std::max({qa, qb, qc});
        if (low > 1.0e-30 && high < 1.0e30) {
          stats.world_qw_triangles += 1;
          const double ratio = high / low;
          stats.world_qw_ratio_min = std::min(stats.world_qw_ratio_min, ratio);
          stats.world_qw_ratio_max = std::max(stats.world_qw_ratio_max, ratio);
        }
      }
    }
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
      screen[corner].u = polygon[corner].u;
      screen[corner].v = polygon[corner].v;
      screen[corner].has_uv = polygon[corner].has_uv;
      screen[corner].rs = polygon[corner].rs;
      screen[corner].rt = polygon[corner].rt;
      screen[corner].fog = polygon[corner].fog;
      screen[corner].has_fog = polygon[corner].has_fog;
      screen[corner].lod = polygon[corner].lod;
      screen[corner].has_lod = polygon[corner].has_lod;
      screen[corner].q = polygon[corner].q;
      screen[corner].has_q = polygon[corner].has_q;
      screen[corner].has_reflection = polygon[corner].has_reflection;
      screen[corner].valid = true;
    }
    if (!projectable) {
      stats.triangles_clipped += 1;
      continue;
    }
    float red = 1.0f, green = 1.0f, blue = 1.0f;
    flat_color(mesh, matrix, options, ia, ib, ic, red, green, blue);
    // The item of the triangle's first vertex selects its texture binding. A mesh never mixes items inside a triangle
    // (the strip tables partition per item), so one lookup is exact.
    const TextureBinding *binding = nullptr;
    if (bindings != nullptr) {
      const std::uint32_t item = mesh.vertices[ia].item;
      if (item < bindings->size()) binding = &(*bindings)[item];
    }
    for (std::size_t corner = 1; corner + 1 < count; ++corner) {
      if (reflection_pass && binding != nullptr)
        rasterise_reflection_triangle(screen[0], screen[corner], screen[corner + 1], options, *binding, rgba, mask,
                                      depth, stats, reflection_mask);
      else {
        // The sky pass's measured record has no fog; every other world draw applies the tapped per-vertex fog. The sky's
        // own projection w is not comparable with the course's view depth (a separate skybox matrix), so it is drawn as
        // the background: it writes no depth and the course always tests against the cleared depth, exactly the GS's
        // far-plane sky equivalent (H, recorded in the report).
        const bool apply_fog =
            options.world_pass && !(options.world_sky_no_fog && draw.pass == kPassSky);
        const bool write_depth = !(options.world_pass && draw.pass == kPassSky);
        // TASK-090 wet reflection: the reflection pass's main-view items are a normal world draw (their state's FBMSK,
        // blend, fog and ZBUF apply); the flags only name the pass in the counters and the mask.
        const bool wet_reflection = options.world_pass && draw.pass == kPassReflection;
        rasterise_triangle(screen[0], screen[corner], screen[corner + 1], options, red, green, blue, binding, rgba,
                           mask, depth, stats, textured_mask, options.world_pass, apply_fog, write_depth, wet_reflection,
                           wet_reflection ? reflection_mask : nullptr);
      }
    }
  }
}

}  // namespace

PositionEncoding mesh_position_encoding(const MeshVertex &vertex) noexcept {
  return vertex.has_raw_position ? PositionEncoding::integer_16 : PositionEncoding::float_32;
}

const char *world_matrix_source_name(WorldMatrixSource source) noexcept {
  switch (source) {
  case WorldMatrixSource::packet: return "packet";
  case WorldMatrixSource::vu0: return "vu0";
  case WorldMatrixSource::last_packet: return "last_packet";
  default: return "missing";
  }
}

WorldMatrixSource world_matrix_columns(const CapturedDraw &draw, bool has_last, const VertexColumns &last,
                                       VertexColumns &columns) noexcept {
  const float cmd_value = float_bits(draw.cmd_value);
  const float pgl_value = float_bits(draw.pgl[8]);
  const std::uint32_t valid_byte = (draw.pgl[0] >> 24) & 0xFFu;
  const std::uint32_t flag_byte = draw.pgl[0] & 0xFFu;
  const bool uploaded = valid_byte == 0 || pgl_value != cmd_value;
  WorldMatrixSource source = WorldMatrixSource::missing;
  if (uploaded) {
    if (draw.matrix_packet_valid != 0) {
      std::array<std::array<std::uint32_t, 4>, 4> raw{};
      for (std::size_t column = 0; column < 4; ++column)
        for (std::size_t lane = 0; lane < 4; ++lane) raw[column][lane] = draw.matrix_packet[1 + column][lane];
      columns = vertex_columns_from_raw(raw);
      source = WorldMatrixSource::packet;
    } else {
      columns = vertex_columns_from_vu0_upload(draw.vf, cmd_value);
      source = WorldMatrixSource::vu0;
    }
  } else if (flag_byte == 0) {
    // The draw did not rewrite the VU0 matrix, so the captured pre-call vf24..27 block is the carried block the VU1
    // reads (the measured course draws 36..82). No upload ran, so the columns carry `value`.
    columns = vertex_columns_from_vu0_upload(draw.vf, cmd_value);
    source = WorldMatrixSource::vu0;
  } else if (has_last) {
    columns = last;
    source = WorldMatrixSource::last_packet;
  } else {
    columns = vertex_columns_from_vu0_upload(draw.vf, cmd_value);
    source = WorldMatrixSource::vu0;
  }
  if (!vertex_columns_finite(columns)) source = WorldMatrixSource::missing;
  return source;
}

WorldMatrixSource WorldMatrixTracker::for_draw(const CapturedDraw &draw, VertexColumns &columns) {
  const WorldMatrixSource source = world_matrix_columns(draw, has_last_, last_, columns);
  if (source == WorldMatrixSource::packet) {
    last_ = columns;
    has_last_ = true;
  }
  return source;
}

DrawMatrix draw_matrix(const CapturedDraw &draw) {
  DrawMatrix matrix;
  // The capture stores the Cmd's float `value` as raw bits; the guest upload scales the linear columns by it, and the
  // metre transform also converts the position encoding's units (coregs3_vertex.hpp, docs/COREGS3-HOOKS.md §10.2).
  Float32 cmd_value = 0.0f;
  std::memcpy(&cmd_value, &draw.cmd_value, sizeof cmd_value);
  matrix.upload = vertex_columns_from_vu0_upload(draw.vf, cmd_value);
  matrix.integer_positions = vertex_columns_from_vu0_metres(draw.vf, cmd_value, PositionEncoding::integer_16);
  matrix.float_positions = vertex_columns_from_vu0_metres(draw.vf, cmd_value, PositionEncoding::float_32);
  // The row-major form is the uploaded 0xC1 matrix: rows 0-2 are the shape-scaled linear columns, row 3 the
  // translation. The flat-shading normal rotation and the timeline use it.
  const VertexRows rows = vertex_rows(matrix.upload);
  for (std::uint32_t row = 0; row < 4; ++row)
    for (std::uint32_t lane = 0; lane < 4; ++lane) matrix.row[row][lane] = rows[row][lane];
  const bool rows_used = matrix.row[0][0] != 0.0f || matrix.row[0][1] != 0.0f || matrix.row[1][0] != 0.0f ||
                         matrix.row[1][1] != 0.0f || matrix.row[2][0] != 0.0f || matrix.row[2][1] != 0.0f ||
                         matrix.row[3][3] != 0.0f;
  matrix.valid = vertex_columns_finite(matrix.upload) && vertex_columns_finite(matrix.integer_positions) &&
                 vertex_columns_finite(matrix.float_positions) && rows_used;
  return matrix;
}

bool render_frame(const FrameSnapshot &frame, const RenderOptions &options, std::vector<std::uint8_t> &rgba,
                  std::vector<std::uint8_t> &mask, RenderStats &stats, std::string &error,
                  std::vector<std::uint8_t> *textured_mask, std::vector<std::uint8_t> *reflection_mask) {
  (void)error;
  const std::uint32_t width = options.width, height = options.height;
  if (width == 0 || height == 0) return false;
  rgba.assign(static_cast<std::size_t>(width) * height * 4u, 0);
  mask.assign(static_cast<std::size_t>(width) * height, 0);
  if (textured_mask != nullptr) textured_mask->assign(static_cast<std::size_t>(width) * height, 0);
  if (reflection_mask != nullptr) reflection_mask->assign(static_cast<std::size_t>(width) * height, 0);
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
  mesh_options.world_channels = options.world_pass;
  WorldMatrixTracker world_tracker;

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

  bool wet_alpha_cleared = false;
  for (std::size_t draw_index = 0; draw_index < frame.draws.size(); ++draw_index) {
    const CapturedDraw &draw = frame.draws[draw_index];
    stats.draws += 1;
    if (options.world_pass && options.clear_alpha_before_wet_reflection && draw.pass == kPassReflection &&
        !wet_alpha_cleared) {
      // TASK-090 S2: the captured full-screen alpha clear that precedes the wet-road reflection mask. It is applied at
      // the stream position the capture measured (after the course draws, before the pass 7 draws).
      for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(width) * height; ++pixel)
        rgba[pixel * 4 + 3] = 0;
      wet_alpha_cleared = true;
      stats.world_reflection_alpha_clear += 1;
    }
    DrawMatrix matrix = draw_matrix(draw);
    if (options.world_pass) {
      // TASK-090 track M1: the selected world draws use the captured upload rule, not the car path's carried-vf
      // transform. The source of each draw's matrix is counted; a draw with no usable matrix is refused, never guessed.
      VertexColumns columns;
      const WorldMatrixSource source = world_tracker.for_draw(draw, columns);
      switch (source) {
      case WorldMatrixSource::packet: stats.world_matrices_packet += 1; break;
      case WorldMatrixSource::vu0: stats.world_matrices_vu0 += 1; break;
      case WorldMatrixSource::last_packet: stats.world_matrices_last_packet += 1; break;
      default: stats.world_matrices_missing += 1; break;
      }
      if (source == WorldMatrixSource::missing) {
        stats.shapes_refused += 1;
        if (stats.refusals.size() < 32)
          stats.refusals.push_back("draw " + std::to_string(draw.sequence) + ": no world matrix");
        continue;
      }
      matrix = DrawMatrix{};
      matrix.upload = columns;
      matrix.integer_positions = columns;
      matrix.float_positions = columns;
      const VertexRows rows = vertex_rows(columns);
      for (std::uint32_t row = 0; row < 4; ++row)
        for (std::uint32_t lane = 0; lane < 4; ++lane) matrix.row[row][lane] = rows[row][lane];
      matrix.valid = true;
      stats.world_draws += 1;
      if (draw.pass == kPassSky) stats.world_sky_depth_background += 1;
    }
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
    if (options.world_pass && draw.pass == kPassReflection) {
      // TASK-090 wet reflection: the pass's own census, so the report can never confuse its fragments with the course's.
      stats.world_reflection_draws += 1;
      stats.world_reflection_vertices += static_cast<std::uint32_t>(mesh.vertices.size());
      for (std::uint32_t item = 0; item < draw.item_count; ++item)
        if (draw.items[item].packet_bytes != 0) stats.world_reflection_items += 1;
    }
    apply_lighting(draw, mesh, options, stats);
    // Resolve and statically validate the draw's texture bindings. A binding outside the measured subset keeps the lit
    // colour and is counted; the rasteriser only sees a binding whose texture it may sample.
    std::vector<TextureBinding> usable;
    std::vector<std::uint8_t> reflection_items;
    const std::vector<TextureBinding> *bindings = nullptr;
    if (options.textures != nullptr) {
      const std::vector<TextureBinding> *source = options.textures->for_draw(draw.sequence);
      if (source != nullptr) {
        usable = *source;
        reflection_items.assign(draw.item_count, 0u);
        std::vector<bool> present(draw.item_count, false);
        for (const MeshVertex &vertex : mesh.vertices)
          if (vertex.item < present.size()) present[vertex.item] = true;
        for (std::uint32_t item = 0; item < usable.size(); ++item) {
          if (!present[item]) continue;
          if (options.world_pass) {
            // The world path's checks: DATE is implemented by the rasteriser and any evaluable ALPHA equation is applied.
            switch (check_world_binding(usable[item])) {
              case BindingStatus::ok: stats.items_textured += 1; break;
              case BindingStatus::no_texture:
                stats.items_untextured += 1;
                stats.world_items_untextured += 1;
                break;
              case BindingStatus::refused_psm:
                usable[item].texture = nullptr;
                stats.items_refused_psm += 1;
                stats.items_untextured += 1;
                stats.world_items_untextured += 1;
                break;
              case BindingStatus::refused_tfx:
                usable[item].texture = nullptr;
                stats.items_refused_tfx += 1;
                stats.items_untextured += 1;
                stats.world_items_untextured += 1;
                break;
              case BindingStatus::refused_filter:
                usable[item].texture = nullptr;
                stats.items_refused_filter += 1;
                stats.items_untextured += 1;
                stats.world_items_untextured += 1;
                break;
              case BindingStatus::refused_mip:
                usable[item].texture = nullptr;
                stats.items_refused_mip += 1;
                stats.items_untextured += 1;
                stats.world_items_untextured += 1;
                break;
              case BindingStatus::refused_blend:
                usable[item].texture = nullptr;
                stats.items_refused_blend += 1;
                stats.items_untextured += 1;
                stats.world_items_untextured += 1;
                break;
              case BindingStatus::refused_date:
                usable[item].texture = nullptr;
                stats.items_refused_date += 1;
                stats.items_untextured += 1;
                stats.world_items_untextured += 1;
                break;
              case BindingStatus::refused_alpha_fail:
                usable[item].texture = nullptr;
                stats.items_refused_alpha_fail += 1;
                stats.items_untextured += 1;
                stats.world_items_untextured += 1;
                break;
            }
            continue;
          }
          // The body/paint reflection pass carries its own measured subset (coregs3_reflection.hpp): the destination
          // alpha test and the additive equation are implemented there, so it is validated separately and never by
          // check_binding (which refuses DATE).
          if (options.reflections && usable[item].reflection) {
            switch (check_reflection_binding(usable[item])) {
              case ReflectionBindingStatus::ok:
                reflection_items[item] = 1;
                stats.items_reflected += 1;
                break;
              case ReflectionBindingStatus::no_texture:
                stats.items_reflection_untextured += 1;
                stats.items_untextured += 1;
                break;
              case ReflectionBindingStatus::refused_psm:
                stats.reflection_refused_psm += 1;
                stats.items_reflection_refused += 1;
                stats.items_untextured += 1;
                break;
              case ReflectionBindingStatus::refused_tfx:
                stats.reflection_refused_tfx += 1;
                stats.items_reflection_refused += 1;
                stats.items_untextured += 1;
                break;
              case ReflectionBindingStatus::refused_filter:
                stats.reflection_refused_filter += 1;
                stats.items_reflection_refused += 1;
                stats.items_untextured += 1;
                break;
              case ReflectionBindingStatus::refused_mip:
                stats.reflection_refused_mip += 1;
                stats.items_reflection_refused += 1;
                stats.items_untextured += 1;
                break;
              case ReflectionBindingStatus::refused_alpha_fail:
                stats.reflection_refused_alpha_fail += 1;
                stats.items_reflection_refused += 1;
                stats.items_untextured += 1;
                break;
              case ReflectionBindingStatus::refused_blend:
                stats.reflection_refused_blend += 1;
                stats.items_reflection_refused += 1;
                stats.items_untextured += 1;
                break;
              case ReflectionBindingStatus::refused_depth:
                stats.reflection_refused_depth += 1;
                stats.items_reflection_refused += 1;
                stats.items_untextured += 1;
                break;
            }
            continue;
          }
          switch (check_binding(usable[item])) {
            case BindingStatus::ok: stats.items_textured += 1; break;
            case BindingStatus::no_texture: stats.items_untextured += 1; break;
            case BindingStatus::refused_psm:
              usable[item].texture = nullptr;
              stats.items_refused_psm += 1;
              stats.items_untextured += 1;
              break;
            case BindingStatus::refused_tfx:
              usable[item].texture = nullptr;
              stats.items_refused_tfx += 1;
              stats.items_untextured += 1;
              break;
            case BindingStatus::refused_filter:
              usable[item].texture = nullptr;
              stats.items_refused_filter += 1;
              stats.items_untextured += 1;
              break;
            case BindingStatus::refused_mip:
              usable[item].texture = nullptr;
              stats.items_refused_mip += 1;
              stats.items_untextured += 1;
              break;
            case BindingStatus::refused_blend:
              usable[item].texture = nullptr;
              stats.items_refused_blend += 1;
              stats.items_untextured += 1;
              break;
            case BindingStatus::refused_date:
              usable[item].texture = nullptr;
              stats.items_refused_date += 1;
              stats.items_untextured += 1;
              break;
            case BindingStatus::refused_alpha_fail:
              usable[item].texture = nullptr;
              stats.items_refused_alpha_fail += 1;
              stats.items_untextured += 1;
              break;
          }
        }
        bindings = &usable;
      }
    }
    rasterise_clip_space(mesh, draw, matrix, options, bindings, reflection_items.empty() ? nullptr : &reflection_items,
                         rgba, mask, depth, stats, textured_mask, reflection_mask);
  }
  for (std::uint8_t value : mask) stats.covered_pixels += value != 0 ? 1u : 0u;
  return true;
}

}  // namespace gt4::coregs3
