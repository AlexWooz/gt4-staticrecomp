// TASK-090 CoreGS 3.0, phase S2: the offline wet-road composite. See coregs3_wet_composite.hpp.
#include "coregs3_wet_composite.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "coregs3_sampler.hpp"

namespace gt4::coregs3 {
namespace {

std::uint8_t rgba_lane(const GsDecodeState &state, std::uint32_t lane) {
  return static_cast<std::uint8_t>((state.rgba >> (8u * lane)) & 0xFFu);
}

bool is_sprite_pair(const GsDecodeVertex &latch, const GsDecodeVertex &draw) {
  return !latch.drawing && draw.drawing;
}

// Rasterises one captured sprite rectangle into the output image. `screen` maps a captured 640x448 pixel coordinate to
// the output pixel coordinate (the offline image may be 3x). The caller supplies the per-pixel operation.
template <typename Pixel>
void for_each_sprite_pixel(const GsDecodeVertex &a, const GsDecodeVertex &b, std::uint32_t width, std::uint32_t height,
                           std::uint32_t capture_width, std::uint32_t capture_height, Pixel &&pixel) {
  const float sx = static_cast<float>(width) / static_cast<float>(capture_width);
  const float sy = static_cast<float>(height) / static_cast<float>(capture_height);
  float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x);
  float y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
  const int min_x = std::max(0, static_cast<int>(std::floor(x0 * sx)));
  const int max_x = std::min(static_cast<int>(width) - 1, static_cast<int>(std::ceil(x1 * sx)) - 1);
  const int min_y = std::max(0, static_cast<int>(std::floor(y0 * sy)));
  const int max_y = std::min(static_cast<int>(height) - 1, static_cast<int>(std::ceil(y1 * sy)) - 1);
  if (min_x > max_x || min_y > max_y) return;
  const float u0 = a.u, u1 = b.u, v0 = a.v, v1 = b.v;
  const float span_x = x1 - x0;
  const float span_y = y1 - y0;
  for (int y = min_y; y <= max_y; ++y) {
    const float py = (static_cast<float>(y) + 0.5f) / sy;
    if (py < y0 || py >= y1) continue;
    const float ty = span_y > 0.0f ? (py - y0) / span_y : 0.0f;
    for (int x = min_x; x <= max_x; ++x) {
      const float px = (static_cast<float>(x) + 0.5f) / sx;
      if (px < x0 || px >= x1) continue;
      const float tx = span_x > 0.0f ? (px - x0) / span_x : 0.0f;
      const std::size_t index = static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x);
      pixel(index, u0 + (u1 - u0) * tx, v0 + (v1 - v0) * ty);
    }
  }
}

}  // namespace

bool apply_wet_composite(const GsDecodeBatch &draw, const std::uint8_t *source_rgba, std::uint32_t source_width,
                         std::uint32_t source_height, std::vector<std::uint8_t> &rgba, std::uint32_t width,
                         std::uint32_t height, std::uint32_t capture_width, std::uint32_t capture_height,
                         WetCompositeStats &stats, std::string &error,
                         std::vector<std::uint8_t> *covered_mask, std::vector<std::uint8_t> *alpha_mask) {
  const GsDecodeState &state = draw.state;
  if (source_rgba == nullptr || width == 0 || height == 0 || capture_width == 0 || capture_height == 0) {
    error = "the composite needs a source texture and a non-zero viewport";
    return false;
  }
  if (rgba.size() != static_cast<std::size_t>(width) * height * 4u) {
    error = "the composite destination has the wrong size";
    return false;
  }
  if (covered_mask != nullptr) covered_mask->assign(static_cast<std::size_t>(width) * height, 0);
  if (alpha_mask != nullptr) alpha_mask->assign(static_cast<std::size_t>(width) * height, 0);
  if (state.prim_type() != 6 || state.prim_tme() != 1 || state.tex0_psm() != 0 || state.tex0_tcc() != 1 ||
      state.tex0_tfx() != 0 || state.tex1_mmag() != 1 || state.tex1_mmin() != 1 || state.tex1_mxl() != 0 ||
      state.clamp_wms() != 2 || state.clamp_wmt() != 2 || source_width != (1u << state.tex0_tw()) ||
      source_height != (1u << state.tex0_th())) {
    error = "the draw is not the measured full-screen composite (PRIM/TEX0/TEX1/CLAMP state)";
    return false;
  }
  const BlendEquation equation = decode_blend_equation(state.alpha);
  if (equation.a != 0 || equation.b != 2 || equation.c != 1 || equation.d != 1) {
    error = "the composite's ALPHA equation is not (Cs - 0) * Ad + Cd";
    return false;
  }
  if (state.test_ate() != 0) {
    error = "the composite carries an alpha test; the measured draw has none";
    return false;
  }
  if (state.frame_fbmsk() != 0) {
    error = "the composite does not write all four framebuffer lanes";
    return false;
  }
  SampledTexture texture;
  texture.psm = state.tex0_psm();
  texture.tw = state.tex0_tw();
  texture.th = state.tex0_th();
  texture.tcc = state.tex0_tcc();
  texture.tfx = state.tex0_tfx();
  texture.mxl = 0;
  texture.mmag = state.tex1_mmag();
  texture.mmin = state.tex1_mmin();
  texture.wms = state.clamp_wms();
  texture.wmt = state.clamp_wmt();
  texture.minu = state.clamp_minu();
  texture.maxu = state.clamp_maxu();
  texture.minv = state.clamp_minv();
  texture.maxv = state.clamp_maxv();
  texture.width = source_width;
  texture.height = source_height;
  texture.x0 = 0;
  texture.y0 = 0;
  texture.rgba = source_rgba;
  const std::array<std::uint8_t, 4> vertex{rgba_lane(state, 0), rgba_lane(state, 1), rgba_lane(state, 2),
                                          rgba_lane(state, 3)};
  for (std::size_t index = 0; index + 1 < draw.verts.size(); index += 2) {
    const GsDecodeVertex &latch = draw.verts[index];
    const GsDecodeVertex &corner = draw.verts[index + 1];
    if (!is_sprite_pair(latch, corner) || !latch.has_uv || !corner.has_uv) {
      error = "the composite vertex run is not a sequence of latch/draw sprite pairs with UVs";
      return false;
    }
    stats.sprites += 1;
    for_each_sprite_pixel(latch, corner, width, height, capture_width, capture_height,
                          [&](std::size_t pixel, float u, float v) {
                            std::array<std::uint8_t, 4> sampled;
                            if (sample_texture(texture, u, v, vertex, sampled) != SampleStatus::ok) {
                              stats.refused_pixels += 1;
                              stats.pixels += 1;
                              return;
                            }
                            const std::array<std::uint8_t, 4> destination{
                                rgba[pixel * 4 + 0], rgba[pixel * 4 + 1], rgba[pixel * 4 + 2], rgba[pixel * 4 + 3]};
                            const std::uint8_t old_alpha = destination[3];
                            std::array<std::uint8_t, 4> blended;
                            blend_fragment(equation, sampled, destination, blended);
                            const double old_luma = destination[0] * 0.299 + destination[1] * 0.587 +
                                                    destination[2] * 0.114;
                            const double new_luma =
                                blended[0] * 0.299 + blended[1] * 0.587 + blended[2] * 0.114;
                            if (blended[0] != destination[0] || blended[1] != destination[1] ||
                                blended[2] != destination[2])
                              stats.changed_pixels += 1;
                            stats.added_luma += new_luma - old_luma;
                            if (covered_mask != nullptr) (*covered_mask)[pixel] = 1;
                            if (alpha_mask != nullptr && old_alpha != 0) (*alpha_mask)[pixel] = old_alpha;
                            stats.pixels += 1;
                            stats.sampled_pixels += 1;
                            if (old_alpha != 0) stats.masked_pixels += 1;
                            for (std::uint32_t lane = 0; lane < 4; ++lane) rgba[pixel * 4 + lane] = blended[lane];
                            stats.alpha_written += 1;
                          });
  }
  return true;
}

bool apply_full_screen_multiply(const GsDecodeBatch &draw, std::vector<std::uint8_t> &rgba, std::uint32_t width,
                                std::uint32_t height, std::uint32_t capture_width, std::uint32_t capture_height,
                                WetCompositeStats &stats, std::string &error) {
  const GsDecodeState &state = draw.state;
  if (rgba.size() != static_cast<std::size_t>(width) * height * 4u || width == 0 || height == 0) {
    error = "the multiply destination has the wrong size";
    return false;
  }
  const BlendEquation equation = decode_blend_equation(state.alpha);
  if (state.prim_type() != 6 || state.prim_tme() != 0 || !state.prim_abe() || equation.a != 1 ||
      equation.b != 2 || equation.c != 2 || equation.d != 2 || state.frame_fbmsk() != 0) {
    error = "the draw is not a full-screen (Cd * FIX) multiply";
    return false;
  }
  const std::uint32_t fix = equation.fix;
  const std::uint8_t alpha = rgba_lane(state, 3);
  for (std::size_t index = 0; index + 1 < draw.verts.size(); index += 2) {
    const GsDecodeVertex &latch = draw.verts[index];
    const GsDecodeVertex &corner = draw.verts[index + 1];
    if (!is_sprite_pair(latch, corner)) {
      error = "the multiply vertex run is not a sequence of latch/draw sprite pairs";
      return false;
    }
    stats.sprites += 1;
    for_each_sprite_pixel(latch, corner, width, height, capture_width, capture_height,
                          [&](std::size_t pixel, float, float) {
                            std::int32_t values[3];
                            for (std::uint32_t lane = 0; lane < 3; ++lane) {
                              const std::int32_t cd = rgba[pixel * 4 + lane];
                              values[lane] = ((cd - 0) * static_cast<std::int32_t>(fix)) >> 7;
                              if (values[lane] < 0) values[lane] = 0;
                              if (values[lane] > 255) values[lane] = 255;
                            }
                            for (std::uint32_t lane = 0; lane < 3; ++lane)
                              rgba[pixel * 4 + lane] = static_cast<std::uint8_t>(values[lane]);
                            rgba[pixel * 4 + 3] = alpha;
                            stats.pixels += 1;
                            stats.alpha_written += 1;
                          });
  }
  return true;
}

}  // namespace gt4::coregs3
