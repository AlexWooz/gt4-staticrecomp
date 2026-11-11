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
                            stats.written_pixels += 1;
                            stats.alpha_written += 1;
                          });
  }
  return true;
}

std::int32_t gs_dither_entry(std::uint64_t dimx, std::uint32_t x, std::uint32_t y, std::uint32_t scale) noexcept {
  if (scale == 0) scale = 1;
  const std::uint32_t sx = x / scale;
  const std::uint32_t sy = y / scale;
  const std::uint32_t entry = 4u * (sy & 3u) + (sx & 3u);
  const std::uint32_t field = static_cast<std::uint32_t>((dimx >> (4u * entry)) & 7u);
  return field >= 4u ? static_cast<std::int32_t>(field) - 8 : static_cast<std::int32_t>(field);
}

std::uint8_t ct16_channel(std::int32_t value, bool clamp_enabled) noexcept {
  if (clamp_enabled) {
    if (value < 0) value = 0;
    if (value > 255) value = 255;
  } else {
    value &= 0xFF;
  }
  return static_cast<std::uint8_t>(((value >> 3) & 0x1F) << 3);
}

namespace {

// One presented channel, selected by the GT4_CORGS3_DITHER mode (coregs3_dither.hpp). `faithful` is the captured CT16
// scanout conversion (dither entry already added to `value`); `off` keeps the modulated 8-bit colour and applies only
// COLCLAMP, because the dither and the five-bit truncation are the scanout model the modern path removes.
std::uint8_t present_channel(std::int32_t value, bool clamp_enabled, DitherMode dither) noexcept {
  if (dither == DitherMode::faithful) return ct16_channel(value, clamp_enabled);
  if (clamp_enabled) {
    if (value < 0) value = 0;
    if (value > 255) value = 255;
  } else {
    value &= 0xFF;
  }
  return static_cast<std::uint8_t>(value);
}

std::uint32_t low_rgba(const GsDecodeState &state) { return static_cast<std::uint32_t>(state.rgba & 0xFFFFFFFFu); }

std::array<std::uint8_t, 4> rgba_bytes(std::uint32_t rgba) {
  return {static_cast<std::uint8_t>(rgba & 0xFFu), static_cast<std::uint8_t>((rgba >> 8) & 0xFFu),
          static_cast<std::uint8_t>((rgba >> 16) & 0xFFu), static_cast<std::uint8_t>((rgba >> 24) & 0xFFu)};
}

// The measured full-screen draws carry one constant RGBAQ across every vertex. A run that varies is refused.
bool constant_vertex_colour(const GsDecodeBatch &draw, std::array<std::uint8_t, 4> &colour, std::string &error) {
  if (draw.verts.empty()) {
    error = "the draw has no vertices";
    return false;
  }
  const std::uint32_t first = draw.verts.front().rgba;
  for (const GsDecodeVertex &vertex : draw.verts) {
    if (vertex.rgba != first) {
      error = "the draw's vertex colours are not constant; the measured full-screen signature does not cover it";
      return false;
    }
  }
  colour = rgba_bytes(first);
  return true;
}

// The measured full-screen draws are made of contiguous strips: each sprite pair covers one strip, and the union of
// the batch covers the captured 640x448 screen. The clears verify the geometry; the frame sample and the present also
// verify that the UV rectangle spans the screen 1:1 (the page and the offline image share the grid at the 3x scale, so
// the source texel of an output pixel is the same pixel).
bool batch_covers_capture_screen(const GsDecodeBatch &draw, std::uint32_t capture_width,
                                 std::uint32_t capture_height, bool textured, std::string &error) {
  if (draw.min_x > 0.5f || draw.min_y > 0.5f || draw.max_x < static_cast<float>(capture_width) - 0.5f ||
      draw.max_y < static_cast<float>(capture_height) - 0.5f) {
    error = "the draw's vertex extent does not cover the captured screen";
    return false;
  }
  if (!textured) return true;
  float min_u = 1e30f, max_u = -1e30f, min_v = 1e30f, max_v = -1e30f;
  for (const GsDecodeVertex &vertex : draw.verts) {
    if (!vertex.has_uv) {
      error = "the draw's vertices carry no UV; the measured full-screen sample needs one";
      return false;
    }
    min_u = std::min(min_u, vertex.u);
    max_u = std::max(max_u, vertex.u);
    min_v = std::min(min_v, vertex.v);
    max_v = std::max(max_v, vertex.v);
  }
  if (min_u < -0.5f || min_v < -0.5f || max_u > static_cast<float>(capture_width) + 0.5f ||
      max_v > static_cast<float>(capture_height) + 0.5f || min_u > 0.5f || min_v > 0.5f ||
      max_u < static_cast<float>(capture_width) - 0.5f || max_v < static_cast<float>(capture_height) - 0.5f) {
    error = "the draw's UV rectangle does not span the captured screen 1:1";
    return false;
  }
  return true;
}

std::int32_t scaled_product(std::int32_t texel, std::int32_t colour) {
  const std::int32_t value = (texel * colour) >> 7;
  return value > 255 ? 255 : value;
}

std::uint8_t fbmsk_lane(std::uint32_t fbmsk, std::uint32_t lane) {
  return static_cast<std::uint8_t>((fbmsk >> (8u * lane)) & 0xFFu);
}

}  // namespace

bool apply_full_screen_clear(const GsDecodeBatch &draw, std::vector<std::uint8_t> &rgba, std::uint32_t width,
                             std::uint32_t height, std::uint32_t capture_width, std::uint32_t capture_height,
                             WetCompositeStats &stats, std::string &error) {
  const GsDecodeState &state = draw.state;
  if (rgba.size() != static_cast<std::size_t>(width) * height * 4u || width == 0 || height == 0) {
    error = "the clear destination has the wrong size";
    return false;
  }
  if (state.prim_type() != 6 || state.prim_tme() != 0 || state.prim_abe() || state.test_ate() != 0) {
    error = "the draw is not a measured full-screen clear (sprite, TME/ABE/ATE off)";
    return false;
  }
  std::array<std::uint8_t, 4> colour{};
  if (!constant_vertex_colour(draw, colour, error)) return false;
  if (!batch_covers_capture_screen(draw, capture_width, capture_height, false, error)) return false;
  const std::uint32_t fbmsk = state.frame_fbmsk();
  for (std::size_t index = 0; index + 1 < draw.verts.size(); index += 2) {
    const GsDecodeVertex &latch = draw.verts[index];
    const GsDecodeVertex &corner = draw.verts[index + 1];
    if (!is_sprite_pair(latch, corner)) {
      error = "the clear vertex run is not a sequence of latch/draw sprite pairs";
      return false;
    }
    stats.sprites += 1;
    for_each_sprite_pixel(latch, corner, width, height, capture_width, capture_height,
                          [&](std::size_t pixel, float, float) {
                            bool changed = false;
                            bool written = false;
                            for (std::uint32_t lane = 0; lane < 4; ++lane) {
                              const std::uint8_t mask = fbmsk_lane(fbmsk, lane);
                              const std::uint8_t merged =
                                  static_cast<std::uint8_t>((rgba[pixel * 4 + lane] & mask) | (colour[lane] & ~mask));
                              if (merged != rgba[pixel * 4 + lane]) changed = true;
                              if (mask != 0xFFu) written = true;
                              rgba[pixel * 4 + lane] = merged;
                            }
                            stats.pixels += 1;
                            stats.changed_pixels += changed ? 1u : 0u;
                            stats.written_pixels += written ? 1u : 0u;
                            stats.alpha_written += fbmsk_lane(fbmsk, 3) != 0xFFu ? 1u : 0u;
                          });
  }
  return true;
}

bool apply_frame_sample(const GsDecodeBatch &draw, std::vector<std::uint8_t> &rgba, std::uint32_t width,
                        std::uint32_t height, std::uint32_t capture_width, std::uint32_t capture_height,
                        WetCompositeStats &stats, std::string &error) {
  const GsDecodeState &state = draw.state;
  if (rgba.size() != static_cast<std::size_t>(width) * height * 4u || width == 0 || height == 0) {
    error = "the frame sample destination has the wrong size";
    return false;
  }
  if (state.prim_type() != 6 || state.prim_tme() != 1 || !state.prim_abe() || state.tex0_tbp0() != 0x1A40u ||
      state.tex0_psm() != 0 || state.tex0_tfx() != 0 || state.frame_fbp() != 210 || state.zbuf_zmsk() != 0) {
    error = "the draw is not the measured full-screen frame sample (page 210, TME+ABE, TFX 0)";
    return false;
  }
  const BlendEquation equation = decode_blend_equation(state.alpha);
  if (equation.a != 0 || equation.b != 1 || equation.c != 0 || equation.d != 1) {
    error = "the frame sample's ALPHA equation is not the normal blend (Cs - Cd) * As + Cd";
    return false;
  }
  std::array<std::uint8_t, 4> vertex{};
  if (!constant_vertex_colour(draw, vertex, error)) return false;
  if (!batch_covers_capture_screen(draw, capture_width, capture_height, true, error)) return false;
  const std::uint32_t fbmsk = state.frame_fbmsk();
  const std::vector<std::uint8_t> source = rgba;  // the sampled frame is the image as it stood before the draw
  for (std::size_t index = 0; index + 1 < draw.verts.size(); index += 2) {
    const GsDecodeVertex &latch = draw.verts[index];
    const GsDecodeVertex &corner = draw.verts[index + 1];
    if (!is_sprite_pair(latch, corner)) {
      error = "the frame sample vertex run is not a sequence of latch/draw sprite pairs";
      return false;
    }
    stats.sprites += 1;
    for_each_sprite_pixel(latch, corner, width, height, capture_width, capture_height,
                          [&](std::size_t pixel, float, float) {
                            // The page is the offline image itself: at the 3x scale the present/sample is 1:1, so the
                            // source texel of an output pixel is the same pixel (the page and the draw share the
                            // grid). The captured UV rectangle is verified to cover the screen, not used to remap.
                            const std::size_t at = pixel;
                            std::array<std::uint8_t, 4> sampled{};
                            for (std::uint32_t lane = 0; lane < 3; ++lane)
                              sampled[lane] = static_cast<std::uint8_t>(
                                  scaled_product(source[at * 4 + lane], vertex[lane]));
                            sampled[3] = state.tex0_tcc() != 0
                                             ? static_cast<std::uint8_t>(scaled_product(source[at * 4 + 3], vertex[3]))
                                             : vertex[3];
                            std::array<std::uint8_t, 4> destination{rgba[pixel * 4 + 0], rgba[pixel * 4 + 1],
                                                                   rgba[pixel * 4 + 2], rgba[pixel * 4 + 3]};
                            std::array<std::uint8_t, 4> blended{};
                            blend_fragment(equation, sampled, destination, blended);
                            const double old_luma =
                                destination[0] * 0.299 + destination[1] * 0.587 + destination[2] * 0.114;
                            const double new_luma = blended[0] * 0.299 + blended[1] * 0.587 + blended[2] * 0.114;
                            stats.added_luma += new_luma - old_luma;
                            bool changed = false;
                            bool written = false;
                            for (std::uint32_t lane = 0; lane < 4; ++lane) {
                              const std::uint8_t mask = fbmsk_lane(fbmsk, lane);
                              const std::uint8_t merged = static_cast<std::uint8_t>(
                                  (rgba[pixel * 4 + lane] & mask) | (blended[lane] & ~mask));
                              if (merged != rgba[pixel * 4 + lane]) changed = true;
                              if (mask != 0xFFu) written = true;
                              rgba[pixel * 4 + lane] = merged;
                            }
                            stats.pixels += 1;
                            stats.sampled_pixels += 1;
                            stats.changed_pixels += changed ? 1u : 0u;
                            stats.written_pixels += written ? 1u : 0u;
                            stats.alpha_written += fbmsk_lane(fbmsk, 3) != 0xFFu ? 1u : 0u;
                          });
  }
  return true;
}

bool apply_present_ct16(const GsDecodeBatch &draw, const std::vector<std::uint8_t> &rgba, std::uint32_t width,
                        std::uint32_t height, std::uint32_t capture_width, std::uint32_t capture_height,
                        DitherMode dither, std::vector<std::uint8_t> &display, WetCompositeStats &stats,
                        std::string &error) {
  const GsDecodeState &state = draw.state;
  if (rgba.size() != static_cast<std::size_t>(width) * height * 4u || width == 0 || height == 0) {
    error = "the present source has the wrong size";
    return false;
  }
  const bool display_target =
      (state.frame_fbp() == 140 || state.frame_fbp() == 0) && state.frame_fbw() == 10 && state.frame_psm() == 2;
  if (state.prim_type() != 6 || state.prim_tme() != 1 || state.prim_abe() || state.tex0_tbp0() != 0x1A40u ||
      state.tex0_psm() != 0 || state.tex0_tfx() != 0 || !display_target || state.frame_fbmsk() != 0) {
    error = "the draw is not the measured CT16 present (page 210 -> a CT16 display buffer, FBW 10, no blend)";
    return false;
  }
  if (state.test_ate() != 0) {
    error = "the present carries an alpha test; the measured draw has none";
    return false;
  }
  std::array<std::uint8_t, 4> vertex{};
  if (!constant_vertex_colour(draw, vertex, error)) return false;
  if (!batch_covers_capture_screen(draw, capture_width, capture_height, true, error)) return false;
  display.assign(static_cast<std::size_t>(width) * height * 4u, 0);
  const std::uint32_t scale_x = capture_width != 0 ? width / capture_width : 1;
  const std::uint32_t scale_y = capture_height != 0 ? height / capture_height : 1;
  const std::uint32_t scale = std::max(scale_x, scale_y);
  const bool clamp_enabled = state.colclamp != 0;
  const bool dither_on = dither == DitherMode::faithful && state.dthe_on();
  for (std::size_t index = 0; index + 1 < draw.verts.size(); index += 2) {
    const GsDecodeVertex &latch = draw.verts[index];
    const GsDecodeVertex &corner = draw.verts[index + 1];
    if (!is_sprite_pair(latch, corner)) {
      error = "the present vertex run is not a sequence of latch/draw sprite pairs";
      return false;
    }
    stats.sprites += 1;
    for_each_sprite_pixel(latch, corner, width, height, capture_width, capture_height,
                          [&](std::size_t pixel, float, float) {
                            // The page being presented is the offline image itself (1:1 at the 3x scale).
                            const std::size_t at = pixel;
                            const std::uint32_t out_x = static_cast<std::uint32_t>(pixel % width);
                            const std::uint32_t out_y = static_cast<std::uint32_t>(pixel / width);
                            const std::int32_t dither_value =
                                dither_on ? gs_dither_entry(state.dimx, out_x, out_y, scale) : 0;
                            std::array<std::uint8_t, 3> rgb{};
                            for (std::uint32_t lane = 0; lane < 3; ++lane) {
                              const std::int32_t modulated = scaled_product(rgba[at * 4 + lane], vertex[lane]);
                              rgb[lane] = present_channel(modulated + dither_value, clamp_enabled, dither);
                            }
                            const std::int32_t alpha_source =
                                state.tex0_tcc() != 0 ? scaled_product(rgba[at * 4 + 3], vertex[3]) : vertex[3];
                            const bool alpha_bit = (alpha_source & 0x80) != 0;
                            bool changed = false;
                            for (std::uint32_t lane = 0; lane < 3; ++lane) {
                              if (display[pixel * 4 + lane] != rgb[lane]) changed = true;
                              display[pixel * 4 + lane] = rgb[lane];
                            }
                            const std::uint8_t alpha = alpha_bit ? 0xFFu : 0x00u;
                            if (display[pixel * 4 + 3] != alpha) changed = true;
                            display[pixel * 4 + 3] = alpha;
                            stats.pixels += 1;
                            stats.sampled_pixels += 1;
                            stats.changed_pixels += changed ? 1u : 0u;
                            stats.written_pixels += 1;
                            stats.alpha_written += 1;
                          });
  }
  return true;
}

}  // namespace gt4::coregs3
