// TASK-090 CoreGS 3.0: the measured car-pass texture sampler. See coregs3_sampler.hpp for the subset and the refusals.
#include "coregs3_sampler.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gt4::coregs3 {
namespace {

constexpr std::int32_t kChannelMax = 255;
constexpr std::int32_t kCoordinateLimit = 1000000000;  // ToTexel104's saturation bound

std::int32_t clamp_channel(std::int32_t value) {
  return std::clamp(value, 0, kChannelMax);
}

// (T * C) >> 7 with 0x80 meaning one, saturating at 255: the GS MODULATE product.
std::int32_t scaled_product(std::int32_t texel, std::int32_t fragment) {
  return std::min(kChannelMax, (texel * fragment) >> 7);
}

std::uint32_t bits(std::uint64_t value, unsigned low, unsigned high) {
  const std::uint64_t mask = high - low == 63 ? ~0ull : ((1ull << (high - low + 1)) - 1ull);
  return static_cast<std::uint32_t>((value >> low) & mask);
}

// One texel of the decoded rectangle. A coordinate outside the rectangle the decoder produced is a refusal, never a
// zero fill: the sidecar may simply not cover the wrapped texel.
bool fetch_texel(const SampledTexture &texture, std::int32_t x, std::int32_t y, std::uint8_t out[4]) noexcept {
  if (x < 0 || y < 0) return false;
  const std::uint32_t ux = static_cast<std::uint32_t>(x);
  const std::uint32_t uy = static_cast<std::uint32_t>(y);
  if (ux < texture.x0 || uy < texture.y0 || ux - texture.x0 >= texture.width || uy - texture.y0 >= texture.height)
    return false;
  if (texture.rgba == nullptr) return false;
  const std::size_t index =
      (static_cast<std::size_t>(uy - texture.y0) * texture.width + (ux - texture.x0)) * 4u;
  std::memcpy(out, texture.rgba + index, 4u);
  return true;
}

std::uint8_t texel_channel(const std::uint8_t rgba[4], std::uint32_t lane) { return rgba[lane]; }

}  // namespace

SampleState measured_sample_state() noexcept {
  SampleState state;
  // TEST: ATE 1, ATST GREATER 6, AREF 32, AFAIL KEEP 0, DATE 0, DATM 1, ZTE 1, ZTST GEQUAL 2.
  state.test = 1ull | (6ull << 1) | (32ull << 4) | (0ull << 12) | (0ull << 14) | (1ull << 15) | (1ull << 16) |
               (2ull << 17);
  // ALPHA: A Cs, B Cd, C As, D Cd, FIX 0.
  state.alpha = 0ull | (1ull << 2) | (0ull << 4) | (1ull << 6);
  state.fbmsk = 0;
  state.zmsk = 0;
  // The measured base car PRIM: type 4 strip, IIP, TME and FGE on, ABE off (the tap's PRIM 0x3C tag for the base draws;
  // the reflection pass's 0x7C carries ABE on and is attached from its own tapped record).
  state.prim = 0x3Cu;
  state.fog_colour = 0;
  return state;
}

const char *sample_status_name(SampleStatus status) noexcept {
  switch (status) {
    case SampleStatus::ok: return "ok";
    case SampleStatus::refused_psm: return "refused_psm";
    case SampleStatus::refused_tfx: return "refused_tfx";
    case SampleStatus::refused_filter: return "refused_filter";
    case SampleStatus::refused_region: return "refused_region";
    case SampleStatus::refused_tcc: return "refused_tcc";
    case SampleStatus::refused_mip: return "refused_mip";
  }
  return "unknown";
}

std::int32_t texel_coordinate_10_4(float uv, std::uint32_t log2_size) noexcept {
  const double scaled = static_cast<double>(uv) * static_cast<double>(1u << log2_size);
  const double value = std::floor(scaled * 16.0);
  if (!(value == value)) return 0;  // NaN: a defined coordinate, the caller's draw is already non-finite
  if (value > static_cast<double>(kCoordinateLimit)) return kCoordinateLimit;
  if (value < -static_cast<double>(kCoordinateLimit)) return -kCoordinateLimit;
  return static_cast<std::int32_t>(value);
}

std::int32_t wrap_texel(std::int32_t texel, std::uint32_t log2_size, std::uint32_t mode, std::uint32_t minimum,
                        std::uint32_t maximum) noexcept {
  const std::int32_t last = static_cast<std::int32_t>((1u << log2_size) - 1u);
  switch (mode & 3u) {
    case 0: return texel & last;  // REPEAT
    case 1: return std::clamp(texel, 0, last);  // CLAMP
    case 2: return std::clamp(texel, static_cast<std::int32_t>(minimum), static_cast<std::int32_t>(maximum));
    case 3:  // REGION_REPEAT: MINU is the mask, MAXU the fixed bits
      return (texel & static_cast<std::int32_t>(minimum)) | static_cast<std::int32_t>(maximum);
  }
  return texel;
}

SampleStatus sample_texture(const SampledTexture &texture, float u, float v,
                            const std::array<std::uint8_t, 4> &vertex, std::array<std::uint8_t, 4> &out) noexcept {
  if (texture.psm != 0x14u && texture.psm != 0x13u && texture.psm != 0x00u) return SampleStatus::refused_psm;
  if (texture.tfx != 0u) return SampleStatus::refused_tfx;
  if (texture.mxl != 0u) return SampleStatus::refused_mip;
  if (texture.mmag != texture.mmin) return SampleStatus::refused_filter;
  if (texture.mmag > 1u) return SampleStatus::refused_filter;
  if (texture.width == 0 || texture.height == 0 || texture.rgba == nullptr) return SampleStatus::refused_region;
  if (texture.tw > 10u || texture.th > 10u) return SampleStatus::refused_region;

  const std::int32_t s = texel_coordinate_10_4(u, texture.tw);
  const std::int32_t t = texel_coordinate_10_4(v, texture.th);

  std::uint8_t texel[4] = {0, 0, 0, 0};
  if (texture.mmag == 0u) {
    // Nearest: floor of the 10.4 coordinate, then the axis wrap.
    const std::int32_t x = wrap_texel(s >> 4, texture.tw, texture.wms, texture.minu, texture.maxu);
    const std::int32_t y = wrap_texel(t >> 4, texture.th, texture.wmt, texture.minv, texture.maxv);
    if (!fetch_texel(texture, x, y, texel)) return SampleStatus::refused_region;
  } else {
    // Bilinear: (u - 0.5) to a floor base plus 4-bit weights, the four taps wrapped independently, sum >> 8.
    const std::int32_t ss = s - 8;
    const std::int32_t st = t - 8;
    const std::int32_t base_x = ss >> 4;
    const std::int32_t base_y = st >> 4;
    const std::int32_t fx = ss & 15;
    const std::int32_t fy = st & 15;
    std::uint8_t c00[4], c10[4], c01[4], c11[4];
    if (!fetch_texel(texture,
                     wrap_texel(base_x, texture.tw, texture.wms, texture.minu, texture.maxu),
                     wrap_texel(base_y, texture.th, texture.wmt, texture.minv, texture.maxv), c00) ||
        !fetch_texel(texture,
                     wrap_texel(base_x + 1, texture.tw, texture.wms, texture.minu, texture.maxu),
                     wrap_texel(base_y, texture.th, texture.wmt, texture.minv, texture.maxv), c10) ||
        !fetch_texel(texture,
                     wrap_texel(base_x, texture.tw, texture.wms, texture.minu, texture.maxu),
                     wrap_texel(base_y + 1, texture.th, texture.wmt, texture.minv, texture.maxv), c01) ||
        !fetch_texel(texture,
                     wrap_texel(base_x + 1, texture.tw, texture.wms, texture.minu, texture.maxu),
                     wrap_texel(base_y + 1, texture.th, texture.wmt, texture.minv, texture.maxv), c11))
      return SampleStatus::refused_region;
    const std::int32_t w00 = (16 - fx) * (16 - fy);
    const std::int32_t w10 = fx * (16 - fy);
    const std::int32_t w01 = (16 - fx) * fy;
    const std::int32_t w11 = fx * fy;
    for (std::uint32_t lane = 0; lane < 4; ++lane) {
      const std::int32_t mixed = (static_cast<std::int32_t>(c00[lane]) * w00 +
                                  static_cast<std::int32_t>(c10[lane]) * w10 +
                                  static_cast<std::int32_t>(c01[lane]) * w01 +
                                  static_cast<std::int32_t>(c11[lane]) * w11) >> 8;
      texel[lane] = static_cast<std::uint8_t>(clamp_channel(mixed));
    }
  }

  // MODULATE with the 8-bit integer product. TCC = 0 keeps the vertex alpha.
  out[0] = static_cast<std::uint8_t>(scaled_product(texel_channel(texel, 0), vertex[0]));
  out[1] = static_cast<std::uint8_t>(scaled_product(texel_channel(texel, 1), vertex[1]));
  out[2] = static_cast<std::uint8_t>(scaled_product(texel_channel(texel, 2), vertex[2]));
  out[3] = texture.tcc != 0u ? static_cast<std::uint8_t>(scaled_product(texel_channel(texel, 3), vertex[3]))
                             : vertex[3];
  return SampleStatus::ok;
}

AlphaTestResult alpha_test(std::uint64_t test, std::uint32_t fragment_alpha) noexcept {
  // DATE (bit 14) is the destination alpha test: a caller that does not implement it refuses rather than silently
  // passing. The reflection pass calls alpha_test_ignore_date and evaluates DATE itself.
  if (bits(test, 14, 14) != 0u) return AlphaTestResult::refused_date;
  return alpha_test_ignore_date(test, fragment_alpha);
}

AlphaTestResult alpha_test_ignore_date(std::uint64_t test, std::uint32_t fragment_alpha) noexcept {
  if (bits(test, 0, 0) == 0u) return AlphaTestResult::pass;  // ATE off
  if (bits(test, 12, 13) != 0u) return AlphaTestResult::refused_fail_mode;  // only AFAIL KEEP is reproduced here
  return alpha_test_ate_only(test, fragment_alpha);
}

AlphaTestResult alpha_test_ate_only(std::uint64_t test, std::uint32_t fragment_alpha) noexcept {
  if (bits(test, 0, 0) == 0u) return AlphaTestResult::pass;  // ATE off
  const std::uint32_t reference = bits(test, 4, 11);
  const std::uint32_t alpha = fragment_alpha & 0xFFu;
  switch (bits(test, 1, 3)) {
    case 0: return AlphaTestResult::fail;  // NEVER
    case 1: return AlphaTestResult::pass;  // ALWAYS
    case 2: return alpha < reference ? AlphaTestResult::pass : AlphaTestResult::fail;
    case 3: return alpha <= reference ? AlphaTestResult::pass : AlphaTestResult::fail;
    case 4: return alpha == reference ? AlphaTestResult::pass : AlphaTestResult::fail;
    case 5: return alpha >= reference ? AlphaTestResult::pass : AlphaTestResult::fail;
    case 6: return alpha > reference ? AlphaTestResult::pass : AlphaTestResult::fail;
    case 7: return alpha != reference ? AlphaTestResult::pass : AlphaTestResult::fail;
  }
  return AlphaTestResult::fail;
}

const char *alpha_test_result_name(AlphaTestResult result) noexcept {
  switch (result) {
    case AlphaTestResult::pass: return "pass";
    case AlphaTestResult::fail: return "fail";
    case AlphaTestResult::refused_date: return "refused_date";
    case AlphaTestResult::refused_fail_mode: return "refused_fail_mode";
  }
  return "unknown";
}

BlendEquation decode_blend_equation(std::uint64_t alpha_register) noexcept {
  BlendEquation equation;
  equation.a = bits(alpha_register, 0, 1);
  equation.b = bits(alpha_register, 2, 3);
  equation.c = bits(alpha_register, 4, 5);
  equation.d = bits(alpha_register, 6, 7);
  equation.fix = bits(alpha_register, 32, 39);
  return equation;
}

BlendFamily blend_family(std::uint64_t alpha_register) noexcept {
  const BlendEquation equation = decode_blend_equation(alpha_register);
  // Normal: A Cs, B Cd, C As, D Cd.
  if (equation.a == 0u && equation.b == 1u && equation.c == 0u && equation.d == 1u) return BlendFamily::normal;
  // Additive as measured on the car pass: A Cs, B zero, C Ad, D Cd.
  if (equation.a == 0u && equation.b == 2u && equation.c == 1u && equation.d == 1u) return BlendFamily::additive;
  return BlendFamily::other;
}

const char *blend_family_name(BlendFamily family) noexcept {
  switch (family) {
    case BlendFamily::normal: return "normal";
    case BlendFamily::additive: return "additive";
    case BlendFamily::other: return "other";
  }
  return "unknown";
}

void blend_fragment(const BlendEquation &equation, const std::array<std::uint8_t, 4> &source,
                    const std::array<std::uint8_t, 4> &destination, std::array<std::uint8_t, 4> &out) noexcept {
  const std::int32_t as = source[3];
  const std::int32_t ad = destination[3];
  std::int32_t factor = 0;
  switch (equation.c) {
    case 0: factor = as; break;
    case 1: factor = ad; break;
    default: factor = static_cast<std::int32_t>(equation.fix); break;
  }
  for (std::uint32_t lane = 0; lane < 3; ++lane) {
    const std::int32_t cs = source[lane];
    const std::int32_t cd = destination[lane];
    const std::int32_t a = equation.a == 0u ? cs : equation.a == 1u ? cd : 0;
    const std::int32_t b = equation.b == 0u ? cs : equation.b == 1u ? cd : 0;
    const std::int32_t d = equation.d == 0u ? cs : equation.d == 1u ? cd : 0;
    const std::int32_t blended = (((a - b) * factor) >> 7) + d;  // arithmetic shift of a signed product
    out[lane] = static_cast<std::uint8_t>(clamp_channel(blended));  // COLCLAMP = 1 (the measured default)
  }
  out[3] = static_cast<std::uint8_t>(clamp_channel(as));  // the GS writes As to the alpha lane
}

BindingStatus check_binding(const TextureBinding &binding) noexcept {
  if (binding.texture == nullptr) return BindingStatus::no_texture;
  const SampledTexture &texture = *binding.texture;
  if (texture.psm != 0x14u && texture.psm != 0x13u && texture.psm != 0x00u) return BindingStatus::refused_psm;
  if (texture.tfx != 0u) return BindingStatus::refused_tfx;
  if (texture.mxl != 0u) return BindingStatus::refused_mip;
  if (texture.mmag != texture.mmin || texture.mmag > 1u) return BindingStatus::refused_filter;
  if (bits(binding.state.test, 14, 14) != 0u) return BindingStatus::refused_date;
  if (bits(binding.state.test, 0, 0) != 0u && bits(binding.state.test, 12, 13) != 0u)
    return BindingStatus::refused_alpha_fail;
  if (prim_blend_enabled(binding.state.prim) && blend_family(binding.state.alpha) == BlendFamily::other)
    return BindingStatus::refused_blend;
  return BindingStatus::ok;
}

BindingStatus check_world_binding(const TextureBinding &binding) noexcept {
  if (binding.texture == nullptr) return BindingStatus::no_texture;
  const SampledTexture &texture = *binding.texture;
  if (texture.psm != 0x14u && texture.psm != 0x13u && texture.psm != 0x00u) return BindingStatus::refused_psm;
  if (texture.tfx != 0u) return BindingStatus::refused_tfx;
  if (texture.mxl != 0u) return BindingStatus::refused_mip;
  if (texture.mmag != texture.mmin || texture.mmag > 1u) return BindingStatus::refused_filter;
  // DATE is implemented by the world rasteriser (destination alpha test against the framebuffer alpha), and all four
  // AFAIL modes are implemented there (KEEP, FBONLY, ZBONLY, RGBONLY), so neither refuses the binding.
  return BindingStatus::ok;
}

const char *binding_status_name(BindingStatus status) noexcept {
  switch (status) {
    case BindingStatus::ok: return "ok";
    case BindingStatus::no_texture: return "no_texture";
    case BindingStatus::refused_psm: return "refused_psm";
    case BindingStatus::refused_tfx: return "refused_tfx";
    case BindingStatus::refused_filter: return "refused_filter";
    case BindingStatus::refused_blend: return "refused_blend";
    case BindingStatus::refused_date: return "refused_date";
    case BindingStatus::refused_alpha_fail: return "refused_alpha_fail";
    case BindingStatus::refused_mip: return "refused_mip";
  }
  return "unknown";
}

std::uint32_t frame_fbmsk(std::uint64_t frame_register) noexcept { return bits(frame_register, 32, 63); }
std::uint32_t zbuf_zmsk(std::uint64_t zbuf_register) noexcept { return bits(zbuf_register, 32, 32); }
// PRIM bits (native/hw_gs/frontend/hw_gs_types.hpp primbit): ABE is bit 6 and TME bit 4. The earlier reading used bit
// 0 for ABE; every measured car PRIM (base 0x3C, reflection 0x7C) is even, so the mistake was invisible until the
// additive reflection pass needed the difference between them.
bool prim_blend_enabled(std::uint32_t prim) noexcept { return ((prim >> 6) & 1u) != 0u; }
bool prim_texture_enabled(std::uint32_t prim) noexcept { return ((prim >> 4) & 1u) != 0u; }

}  // namespace gt4::coregs3
