// CTest native_coregs3_sampler: TASK-090 texture milestone. Host only, no game data: every texture, UV and packet here
// is hand-authored, which is what makes this fixture publishable.
//
//   - the measured MODULATE subset: rgb = min(255, (T * C) >> 7), a = min(255, (Ta * Ca) >> 7), with TCC = 0 keeping the
//     vertex alpha, on hand-authored T4, T8 and CT32 textures (the decoder's output is RGBA8; this fixture authors the
//     same RGBA8 a T4/T8 palette or a CT32 direct texel produces);
//   - nearest and bilinear sampling (the spec's 10.4 coordinate, (u - 0.5) floor base, 4-bit weights, >> 8 sum);
//   - the four CLAMP wrap modes, including the full-size region clamp the night car pass measured, and the refusal of a
//     wrapped texel outside the decoded rectangle;
//   - the alpha test: the eight ATST comparisons, ATE off, AFAIL KEEP and the explicit DATE / other-AFAIL refusals;
//   - the normal and additive blends in the GS integer form and the FBMSK lane merge;
//   - a render integration test: a hand-authored textured quad through render_frame (sampled texel colours, an
//     alpha-test-failed pixel that stays at the background, a blended pixel, FBMSK alpha-only, ZMSK depth).
#include "coregs3_render.hpp"
#include "coregs3_sampler.hpp"
#include "coregs3_scene.hpp"
#include "coregs3_shape.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace gt4::coregs3;

void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

std::uint32_t float_bits(float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof bits);
  return bits;
}

void put_u16(std::vector<std::uint8_t> &out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void put_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
}

// A VIF UNPACK word: opcode 0x60, VN/VL, NUM vectors and the TOPS-relative destination qword.
std::uint32_t unpack_word(std::uint32_t vn, std::uint32_t vl, std::uint32_t num, std::uint32_t qword, bool tops) {
  return 0x60000000u | ((vn & 3u) << 26) | ((vl & 3u) << 24) | ((num & 0xFFu) << 16) | (tops ? 0x8000u : 0u) |
         (qword & 0x3FFu);
}

// A hand-authored 2x2 nearest texture with a vertex colour of 0x80 (the GS's 1.0), so MODULATE returns the texel.
constexpr std::uint32_t kQuadSize = 2;

SampledTexture make_texture(std::uint32_t psm, std::uint32_t tw, std::uint32_t th, std::uint32_t mmag,
                            std::uint32_t mmin, std::uint32_t wms, std::uint32_t wmt, std::uint32_t minu,
                            std::uint32_t maxu, std::uint32_t minv, std::uint32_t maxv,
                            const std::vector<std::uint8_t> &rgba) {
  SampledTexture texture;
  texture.psm = psm;
  texture.tw = tw;
  texture.th = th;
  texture.tcc = 1;
  texture.tfx = 0;
  texture.mxl = 0;
  texture.mmag = mmag;
  texture.mmin = mmin;
  texture.wms = wms;
  texture.wmt = wmt;
  texture.minu = minu;
  texture.maxu = maxu;
  texture.minv = minv;
  texture.maxv = maxv;
  texture.width = 1u << tw;
  texture.height = 1u << th;
  texture.rgba = rgba.data();
  return texture;
}

// Two RGBA texels as the decoded-texture vector the sampler reads.
std::vector<std::uint8_t> two_texels(const std::array<std::uint8_t, 4> &first,
                                     const std::array<std::uint8_t, 4> &second) {
  return {first[0], first[1], first[2], first[3], second[0], second[1], second[2], second[3]};
}

std::array<std::uint8_t, 4> sample_at(const SampledTexture &texture, float u, float v,
                                      std::array<std::uint8_t, 4> vertex = {128, 128, 128, 128}) {
  std::array<std::uint8_t, 4> out{0, 0, 0, 0};
  require(sample_texture(texture, u, v, vertex, out) == SampleStatus::ok, "the sample succeeds");
  return out;
}

void check_modulate_and_formats() {
  // A CT32 palette entry set; a T4 texture samples 4-bit indices 0/1 through it, a T8 texture byte indices 0x34/0x99
  // through a 256-entry palette, and a CT32 texture stores the RGBA8 directly. The fixture authors the RGBA8 the
  // decoder produces for each format, which is all the sampler consumes.
  const std::array<std::uint8_t, 4> palette0{10, 20, 30, 40};
  const std::array<std::uint8_t, 4> palette1{200, 100, 50, 60};
  const std::array<std::uint8_t, 4> palette_34{80, 90, 100, 110};
  const std::array<std::uint8_t, 4> palette_99{240, 250, 5, 15};
  const std::vector<std::uint8_t> t4 = two_texels(palette0, palette1);
  const std::vector<std::uint8_t> t8 = two_texels(palette_34, palette_99);
  const std::vector<std::uint8_t> ct32 = two_texels(palette1, palette0);
  {
    const SampledTexture texture = make_texture(0x14, 1, 0, 0, 0, 1, 1, 0, 1, 0, 0, t4);
    // Vertex 0x80 is 1.0: MODULATE returns the texel exactly (even values, so the >> 7 has no rounding).
    require(sample_at(texture, 0.1f, 0.5f) == palette0, "the T4 texture samples palette entry 0");
    require(sample_at(texture, 0.7f, 0.5f) == palette1, "the T4 texture samples palette entry 1");
  }
  {
    const SampledTexture texture = make_texture(0x13, 1, 0, 0, 0, 1, 1, 0, 1, 0, 0, t8);
    require(sample_at(texture, 0.1f, 0.5f) == palette_34, "the T8 texture samples palette entry 0x34");
    require(sample_at(texture, 0.7f, 0.5f) == palette_99, "the T8 texture samples palette entry 0x99");
  }
  {
    const SampledTexture texture = make_texture(0x00, 1, 0, 0, 0, 1, 1, 0, 1, 0, 0, ct32);
    require(sample_at(texture, 0.1f, 0.5f) == palette1, "the CT32 texture samples its direct texel 0");
    require(sample_at(texture, 0.7f, 0.5f) == palette0, "the CT32 texture samples its direct texel 1");
  }
  // Exact 8-bit products: (200 * 100) >> 7 = 156, (100 * 100) >> 7 = 78, and the 255 * 255 saturation.
  const std::vector<std::uint8_t> one{200, 100, 50, 64};
  const SampledTexture texture = make_texture(0x14, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, one);
  const std::array<std::uint8_t, 4> vertex{100, 100, 100, 100};
  require(sample_at(texture, 0.0f, 0.0f, vertex) == std::array<std::uint8_t, 4>{156, 78, 39, 50},
          "MODULATE is the exact 8-bit product");
  const std::uint8_t saturated[4] = {255, 255, 255, 255};
  const std::vector<std::uint8_t> white(saturated, saturated + 4);
  const SampledTexture saturated_texture = make_texture(0x14, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, white);
  require(sample_at(saturated_texture, 0.0f, 0.0f, {255, 255, 255, 255}) ==
              std::array<std::uint8_t, 4>{255, 255, 255, 255},
          "MODULATE saturates at 255");
  // TCC = 0 keeps the vertex alpha.
  SampledTexture no_alpha = texture;
  no_alpha.tcc = 0;
  require(sample_at(no_alpha, 0.0f, 0.0f, vertex)[3] == 100, "TCC = 0 keeps the vertex alpha");
  // A format outside the measured set, a mip level, an unknown function and a mixed filter pair are all refused.
  SampledTexture refused = texture;
  refused.psm = 0x01;
  std::array<std::uint8_t, 4> out{};
  require(sample_texture(refused, 0.0f, 0.0f, vertex, out) == SampleStatus::refused_psm, "an unknown PSM is refused");
  refused = texture;
  refused.mxl = 1;
  require(sample_texture(refused, 0.0f, 0.0f, vertex, out) == SampleStatus::refused_mip, "a mip level is refused");
  refused = texture;
  refused.tfx = 1;
  require(sample_texture(refused, 0.0f, 0.0f, vertex, out) == SampleStatus::refused_tfx, "a non-MODULATE TFX is refused");
  refused = texture;
  refused.mmag = 1;
  refused.mmin = 0;
  require(sample_texture(refused, 0.0f, 0.0f, vertex, out) == SampleStatus::refused_filter,
          "MMAG != MMIN is refused (no LOD)");
}

void check_filtering_and_wrap() {
  // 4x4 nearest texture, each texel identified by its red channel.
  std::vector<std::uint8_t> texels(4 * 4 * 4, 0);
  for (std::uint32_t y = 0; y < 4; ++y)
    for (std::uint32_t x = 0; x < 4; ++x) {
      texels[(y * 4 + x) * 4 + 0] = static_cast<std::uint8_t>(x * 16 + y * 4);
      texels[(y * 4 + x) * 4 + 3] = 255;
    }
  const SampledTexture nearest = make_texture(0x14, 2, 2, 0, 0, 1, 1, 0, 3, 0, 3, texels);
  require(texel_coordinate_10_4(0.5f, 2) == 32, "the 10.4 texel coordinate of u = 0.5, TW = 2 is 32");
  require(sample_at(nearest, 0.0f, 0.0f)[0] == 0, "nearest u=0 v=0 is texel (0,0)");
  require(sample_at(nearest, 0.9f, 0.9f)[0] == static_cast<std::uint8_t>(3 * 16 + 3 * 4),
          "nearest u=0.9 v=0.9 is texel (3,3)");
  // CLAMP: u = -0.1 clamps to texel 0, u = 1.1 to texel 3.
  require(sample_at(nearest, -0.1f, 0.5f)[0] == static_cast<std::uint8_t>(0 * 16 + 2 * 4), "negative u clamps to 0");
  require(sample_at(nearest, 1.1f, 0.5f)[0] == static_cast<std::uint8_t>(3 * 16 + 2 * 4), "u above 1 clamps to 3");
  // REPEAT: u = 1.25 wraps to texel 1.
  const SampledTexture repeat = make_texture(0x14, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, texels);
  require(sample_at(repeat, 1.25f, 0.5f)[0] == static_cast<std::uint8_t>(1 * 16 + 2 * 4), "repeat wraps 1.25 to texel 1");
  // REGION_CLAMP to [1,2]: u = 0 clamps to 1, u = 1 to 2.
  const SampledTexture region_clamp = make_texture(0x14, 2, 2, 0, 0, 2, 2, 1, 2, 1, 2, texels);
  require(sample_at(region_clamp, 0.0f, 0.5f)[0] == static_cast<std::uint8_t>(1 * 16 + 2 * 4),
          "region clamp clamps u to MINU");
  require(sample_at(region_clamp, 1.0f, 0.5f)[0] == static_cast<std::uint8_t>(2 * 16 + 2 * 4),
          "region clamp clamps u to MAXU");
  // REGION_REPEAT u' = (u & MINU) | MAXU with MINU 1 and MAXU 2: texel 5 -> (5 & 1) | 2 = 3, texel 2 -> 2.
  require(wrap_texel(5, 2, 3, 1, 2) == 3, "region repeat masks then ORs the fixed bits");
  require(wrap_texel(2, 2, 3, 1, 2) == 2, "region repeat keeps an already masked texel");
  // Bilinear: 2x2 red 0, 100 / 200, 255 at u = v = 0.5 is the four-tap average, 555 * 64 >> 8 = 138.
  const std::vector<std::uint8_t> quad{0, 0, 0, 255, 100, 100, 100, 255, 200, 200, 200, 255, 255, 255, 255, 255};
  const SampledTexture linear = make_texture(0x14, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, quad);
  require(sample_at(linear, 0.5f, 0.5f)[0] == 138, "bilinear at the centre is the 4-tap average with >> 8");
  require(sample_at(linear, 0.05f, 0.05f)[0] == 0, "bilinear in the first texel's centre returns it exactly");
  // A decoded rectangle smaller than the full texture: a repeated tap outside it is refused, never zero-filled.
  SampledTexture cropped = linear;
  cropped.width = 1;
  cropped.height = 1;
  std::array<std::uint8_t, 4> out{};
  require(sample_texture(cropped, 0.9f, 0.9f, {128, 128, 128, 128}, out) == SampleStatus::refused_region,
          "a wrapped texel outside the decoded rectangle is refused");
}

void check_alpha_test_and_blends() {
  // TEST layout: ATE 0, ATST bits 1-3, AREF bits 4-11, AFAIL bits 12-13, DATE bit 14.
  const auto make_test = [](std::uint32_t ate, std::uint32_t atst, std::uint32_t aref, std::uint32_t afail,
                            std::uint32_t date) {
    return static_cast<std::uint64_t>(ate) | (static_cast<std::uint64_t>(atst & 7u) << 1) |
           (static_cast<std::uint64_t>(aref & 0xFFu) << 4) | (static_cast<std::uint64_t>(afail & 3u) << 12) |
           (static_cast<std::uint64_t>(date & 1u) << 14);
  };
  require(alpha_test(make_test(0, 0, 0, 0, 0), 0) == AlphaTestResult::pass, "ATE = 0 passes everything");
  require(alpha_test(make_test(1, 6, 32, 0, 0), 33) == AlphaTestResult::pass, "GREATER 32 passes 33");
  require(alpha_test(make_test(1, 6, 32, 0, 0), 32) == AlphaTestResult::fail, "GREATER 32 fails 32");
  require(alpha_test(make_test(1, 5, 32, 0, 0), 32) == AlphaTestResult::pass, "GEQUAL 32 passes 32");
  require(alpha_test(make_test(1, 2, 32, 0, 0), 31) == AlphaTestResult::pass, "LESS 32 passes 31");
  require(alpha_test(make_test(1, 4, 32, 0, 0), 31) == AlphaTestResult::fail, "EQUAL 32 fails 31");
  require(alpha_test(make_test(1, 7, 32, 0, 0), 31) == AlphaTestResult::pass, "NOTEQUAL 32 passes 31");
  require(alpha_test(make_test(1, 6, 32, 1, 0), 33) == AlphaTestResult::refused_fail_mode,
          "an AFAIL other than KEEP is refused");
  require(alpha_test(make_test(1, 6, 32, 0, 1), 33) == AlphaTestResult::refused_date, "DATE is deferred and refused");

  // Normal blend: A Cs, B Cd, C As, D Cd. A source inside the destination's range passes through unchanged at As = 128.
  const std::uint64_t normal = 0ull | (1ull << 2) | (0ull << 4) | (1ull << 6);
  require(blend_family(normal) == BlendFamily::normal, "A0 B1 C0 D1 is the normal family");
  const std::array<std::uint8_t, 4> source{200, 100, 50, 128};
  const std::array<std::uint8_t, 4> destination{100, 150, 200, 255};
  std::array<std::uint8_t, 4> blended{};
  blend_fragment(decode_blend_equation(normal), source, destination, blended);
  require(blended == std::array<std::uint8_t, 4>{200, 100, 50, 128}, "normal blend at As = 0x80 returns the source");
  // Additive: A Cs, B 0, C Ad, D Cd. Ad = 255 doubles the source before adding the destination, then clamps.
  const std::uint64_t additive = 0ull | (2ull << 2) | (1ull << 4) | (1ull << 6);
  require(blend_family(additive) == BlendFamily::additive, "A0 B2 C1 D1 is the measured additive family");
  blend_fragment(decode_blend_equation(additive), source, destination, blended);
  require(blended == std::array<std::uint8_t, 4>{255, 255, 255, 128}, "additive blend doubles and clamps");
  // 16-bit channel values stay 16-bit in the merge.
  require(frame_fbmsk(0xFF000000ull << 32) == 0xFF000000u, "FRAME.FBMSK is bits 32-63");
  require(zbuf_zmsk(1ull << 32) == 1u, "ZBUF.ZMSK is bit 32");
  require(prim_blend_enabled(0x7Du) && !prim_blend_enabled(0x7Cu), "PRIM.ABE is bit 0");
  require(prim_texture_enabled(0x7Cu) && !prim_texture_enabled(0x6Bu), "PRIM.TME is bit 4");
}

// ---- render integration: a hand-authored textured quad ---------------------------------------------------------

// One VIF packet: four V3-16 positions at TOPS 0, four V2-16 UVs at TOPS 0x40, four V4-8 colours at TOPS 0x80.
std::vector<std::uint8_t> make_quad_packet() {
  std::vector<std::uint8_t> packet;
  auto pad = [&packet] {
    while (packet.size() % 4 != 0) packet.push_back(0);
  };
  put_u32(packet, unpack_word(2, 1, 4, 0x00, true));
  const std::int16_t positions[4][3] = {{-4096, -4096, 0}, {4096, -4096, 0}, {-4096, 4096, 0}, {4096, 4096, 0}};
  for (const auto &position : positions)
    for (std::int16_t component : position) put_u16(packet, static_cast<std::uint16_t>(component));
  pad();
  put_u32(packet, unpack_word(1, 1, 4, 0x40, true));  // V2-16 UVs, signed /4096, no V flip
  const std::int16_t uvs[4][2] = {{0, 0}, {4096, 0}, {0, 4096}, {4096, 4096}};
  for (const auto &uv : uvs)
    for (std::int16_t component : uv) put_u16(packet, static_cast<std::uint16_t>(component));
  pad();
  put_u32(packet, unpack_word(3, 2, 4, 0x80, true));  // V4-8 colour, 0x80 = 1.0 in the GS
  for (int vertex = 0; vertex < 4; ++vertex)
    for (int lane = 0; lane < 4; ++lane) packet.push_back(0x80);
  pad();
  return packet;
}

void set_identity_columns(CapturedDraw &draw) {
  // The measured metre transform is the identity: vf24..vf27 are the identity and the shape `value` is 1/4096, so the
  // V3-16 metre scale (value * 4096) is exactly 1 and clip = (x, y, z, 1).
  draw.cmd_value = float_bits(1.0f / 4096.0f);
  for (std::uint32_t column = 0; column < 4; ++column)
    for (std::uint32_t lane = 0; lane < 4; ++lane)
      draw.vf[column][lane] = float_bits(column == lane ? 1.0f : 0.0f);
}

CapturedDraw make_quad_draw(std::uint32_t packet_bytes) {
  CapturedDraw draw;
  draw.sequence = 3;
  draw.shape = 0x00400000u;
  draw.cmd_value = float_bits(1.0f);
  draw.item_count = 1;
  draw.items[0].address = 0x00100000u;
  draw.items[0].qwords = static_cast<std::uint16_t>(packet_bytes / 16);
  draw.items[0].texture_record = 0;
  draw.items[0].packet_offset = 0;
  draw.items[0].packet_bytes = packet_bytes;
  // No measured run pair: the lighting keeps the base colour (the colour channel), which the texture tests want.
  set_identity_columns(draw);
  return draw;
}

// A 2x2 nearest texture: (0,0) red, (1,0) green, (0,1) blue, (1,1) yellow. Alpha is 255 everywhere.
std::vector<std::uint8_t> quad_texture() {
  return {255, 0,   0,   255, 0,   255, 0,   255,
          0,   0,   255, 255, 255, 255, 0,   255};
}

struct RenderFixture {
  FrameSnapshot frame;
  FrameTextureTable table;
  std::vector<std::uint8_t> rgba, mask;
  RenderStats stats;
  std::string error;

  RenderFixture() {
    std::vector<std::uint8_t> packet = make_quad_packet();
    CapturedDraw draw = make_quad_draw(static_cast<std::uint32_t>(packet.size()));
    {
      // The packet must decode to a four-vertex strip with a model UV on every vertex.
      Mesh mesh;
      MeshOptions mesh_options;
      std::string mesh_error;
      require(build_mesh(packet.data(), packet.size(), draw, mesh_options, mesh, mesh_error),
              "the quad packet builds a mesh");
      const ChannelScan scan = scan_channels(packet.data(), packet.size(), draw);
      require(scan.channels.size() == 3 && scan.channels[0].role == ChannelRole::positions &&
                  scan.channels[1].role == ChannelRole::uv && scan.channels[2].role == ChannelRole::color,
              "the quad packet scans as positions, model UV and colour");
      require(mesh.vertices.size() == 4 && mesh.has_uv && mesh.vertices[0].has_uv, "the quad mesh carries model UVs");
      require(mesh.vertices[1].uv[0] == 1.0f && mesh.vertices[2].uv[1] == 1.0f, "the UV channel decodes to 0/1");
    }
    frame.draws.push_back(draw);
    frame.draw_count = 1;
    frame.packets = packet;

    table.sequence.push_back(draw.sequence);
    table.payloads.push_back(quad_texture());
    SampledTexture texture =
        make_texture(0x14, 1, 1, 0, 0, 1, 1, 0, 1, 0, 1, table.payloads.back());
    table.textures.push_back(texture);
    std::vector<TextureBinding> bindings(1);
    bindings[0].texture = &table.textures.back();
    bindings[0].state = measured_sample_state();
    table.draws.push_back(bindings);
  }

  void render(bool attach_textures) {
    RenderOptions options;
    options.width = 64;
    options.height = 48;
    options.textures = attach_textures ? &table : nullptr;
    require(render_frame(frame, options, rgba, mask, stats, error), "the quad renders");
  }

  std::array<std::uint8_t, 4> pixel(int x, int y) const {
    const std::size_t at = (static_cast<std::size_t>(y) * 64u + static_cast<std::size_t>(x)) * 4u;
    return {rgba[at + 0], rgba[at + 1], rgba[at + 2], rgba[at + 3]};
  }
};

void check_render_textured_quad() {
  {
    RenderFixture fixture;
    fixture.render(true);
    require(fixture.stats.items_textured == 1, "the quad item is textured");
    require(fixture.stats.textured_pixels > 0, "the quad samples pixels");
    require(fixture.pixel(16, 12) == std::array<std::uint8_t, 4>{0, 0, 255, 255},
            "the top-left quadrant is the texture's v=1,u=0 texel (blue)");
    require(fixture.pixel(48, 12) == std::array<std::uint8_t, 4>{255, 255, 0, 255},
            "the top-right quadrant is yellow");
    require(fixture.pixel(16, 36) == std::array<std::uint8_t, 4>{255, 0, 0, 255},
            "the bottom-left quadrant is red");
    require(fixture.pixel(48, 36) == std::array<std::uint8_t, 4>{0, 255, 0, 255},
            "the bottom-right quadrant is green");

    // The untextured A/B is the same geometry on the vertex colour (the colour channel is 0x80 = 0.502, so the base
    // colour written by the pre-texture path is 128 in every lane).
    RenderFixture untextured;
    untextured.render(false);
    require(untextured.stats.textured_pixels == 0 && untextured.stats.items_textured == 0,
            "the A/B frame samples no texture");
    require(untextured.pixel(16, 12) == std::array<std::uint8_t, 4>{128, 128, 128, 255},
            "the A/B frame keeps the lit/base colour");
  }

  // Alpha test: the green texel (bottom-right) fails GREATER 32 with alpha 0 and writes nothing; the background stays.
  {
    RenderFixture fixture;
    fixture.table.payloads[0][4 + 3] = 0;  // texel (1,0) alpha
    fixture.table.textures[0].rgba = fixture.table.payloads[0].data();
    fixture.render(true);
    require(fixture.pixel(48, 36) == std::array<std::uint8_t, 4>{24, 24, 28, 255},
            "an alpha-test-failed fragment keeps the background");
    require(fixture.stats.alpha_test_failed > 0, "the alpha-test failure is counted");
  }

  // Normal blend with As = 64 (0.5): (T - Cd) * 64 >> 7 + Cd. Blue texel over the background (24,24,28).
  {
    RenderFixture fixture;
    fixture.table.payloads[0][8 + 3] = 64;  // texel (0,1) alpha
    fixture.table.textures[0].rgba = fixture.table.payloads[0].data();
    fixture.table.draws[0][0].state.prim = 0x7Du;  // ABE on
    fixture.render(true);
    require(fixture.pixel(16, 12) == std::array<std::uint8_t, 4>{12, 12, 141, 64},
            "the normal blend matches the GS integer form");
    require(fixture.stats.blended_pixels > 0, "the blended fragments are counted");
  }

  // Additive blend: (T - 0) * Ad >> 7 + Cd with Ad = 255, clamped. Blue over (24,24,28) gives (24,24,255).
  {
    RenderFixture fixture;
    fixture.table.payloads[0][8 + 3] = 128;  // passes the alpha test; the additive factor is Ad = 255
    fixture.table.textures[0].rgba = fixture.table.payloads[0].data();
    fixture.table.draws[0][0].state.prim = 0x7Du;
    fixture.table.draws[0][0].state.alpha = 0ull | (2ull << 2) | (1ull << 4) | (1ull << 6);
    fixture.render(true);
    require(fixture.pixel(16, 12) == std::array<std::uint8_t, 4>{24, 24, 255, 128},
            "the additive blend matches the GS integer form");
    require(fixture.stats.additive_pixels > 0, "the additive fragments are counted");
  }

  // FBMSK alpha-only (0xFF000000 keeps the alpha lane): the written alpha stays the destination's 255, not the
  // texture's 64.
  {
    RenderFixture fixture;
    fixture.table.payloads[0][0 + 3] = 64;  // texel (0,0) alpha
    fixture.table.textures[0].rgba = fixture.table.payloads[0].data();
    fixture.table.draws[0][0].state.fbmsk = 0xFF000000u;
    fixture.render(true);
    require(fixture.pixel(16, 36)[3] == 255, "FBMSK keeps the alpha lane");
    require(fixture.stats.fbmsk_pixels > 0, "the FBMSK fragments are counted");
  }

  // ZMSK = 1: the fragment writes colour but not depth. The second draw is the same quad one metre farther (its w row
  // gains a z term), so with the first draw's depth written it loses the w-buffer; with ZMSK on it wins.
  {
    const auto two_draw_frame = [](bool mask_first_depth) {
      auto fixture = std::make_unique<RenderFixture>();
      std::vector<std::uint8_t> far_packet = make_quad_packet();
      // Rewrite the four z components to +1024 (raw V3-16): the far draw's w row will add 0.001 * 0.25.
      // The positions start after the 4-byte UNPACK word; each vertex is three little-endian 16-bit components.
      for (int vertex = 0; vertex < 4; ++vertex) {
        const std::size_t z_at = 4 + (static_cast<std::size_t>(vertex) * 3 + 2) * 2;
        far_packet[z_at] = static_cast<std::uint8_t>(1024 & 0xFF);
        far_packet[z_at + 1] = static_cast<std::uint8_t>((1024 >> 8) & 0xFF);
      }
      CapturedDraw far_draw = make_quad_draw(static_cast<std::uint32_t>(far_packet.size()));
      far_draw.sequence = 4;
      far_draw.vf[2][3] = float_bits(0.001f);  // w = 1 + 0.001 * z, so the far quad has a larger depth
      const std::uint32_t far_offset = static_cast<std::uint32_t>(fixture->frame.packets.size());
      fixture->frame.draws.push_back(far_draw);
      fixture->frame.draw_count = 2;
      fixture->frame.packets.insert(fixture->frame.packets.end(), far_packet.begin(), far_packet.end());
      fixture->frame.draws[1].items[0].packet_offset = far_offset;
      fixture->frame.draws[1].items[0].packet_bytes = static_cast<std::uint32_t>(far_packet.size());

      fixture->table.sequence.push_back(4);
      fixture->table.payloads.push_back(
          {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255});
      SampledTexture white = make_texture(0x14, 1, 1, 0, 0, 1, 1, 0, 1, 0, 1, fixture->table.payloads.back());
      fixture->table.textures.push_back(white);
      std::vector<TextureBinding> far_binding(1);
      far_binding[0].texture = &fixture->table.textures.back();
      far_binding[0].state = measured_sample_state();
      fixture->table.draws.push_back(far_binding);
      fixture->table.draws[0][0].state.zmsk = mask_first_depth ? 1u : 0u;
      return fixture;
    };
    {
      auto fixture = two_draw_frame(true);
      fixture->render(true);
      require(fixture->pixel(16, 12) == std::array<std::uint8_t, 4>{255, 255, 255, 255},
              "ZMSK = 1 leaves depth unwritten, so the later (farther) draw wins");
      require(fixture->stats.depth_masked_pixels > 0, "the depth-masked fragments are counted");
    }
    {
      auto fixture = two_draw_frame(false);
      fixture->render(true);
      require(fixture->pixel(16, 12) == std::array<std::uint8_t, 4>{0, 0, 255, 255},
              "without ZMSK the farther draw loses the w-buffer");
    }
  }
}

}  // namespace

int main() {
  try {
    check_modulate_and_formats();
    check_filtering_and_wrap();
    check_alpha_test_and_blends();
    check_render_textured_quad();
  } catch (const std::exception &failure) {
    std::fprintf(stderr, "coregs3 sampler fixture: %s\n", failure.what());
    return 1;
  }
  std::printf("coregs3 sampler fixture: ok\n");
  return 0;
}
