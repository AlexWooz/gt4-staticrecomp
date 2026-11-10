// CTest native_coregs3_reflection: TASK-090 body/paint reflection milestone. Host only, no game data: every vertex,
// block, texel and packet here is hand-authored, which is what makes this fixture publishable.
//
//   - the pinned texgen formula (coregs3_reflection.hpp): three hand-authored vertices through a hand-authored
//     six-qword body block, pinned against the real AOT fragment's output (the harness ran the fragment 0x10A0 on
//     exactly these inputs; the expected S,T below are its two TOPS+0x40 floats). The fixture also pins the study's
//     agreement bound: our float32 formula differs from the EFU rsqrt's output by less than 5e-4;
//   - every formula refusal (block form, position encoding, normal channel, non-finite, degenerate);
//   - the measured fog mix and the binding subset (additive ALPHA, ABE bit 6, AFAIL KEEP, ZTST GEQUAL, texture state);
//   - a render integration test with two hand-authored draws: a textured base draw whose MODULATE alpha lands in the
//     framebuffer (the destination alpha the reflection reads), then a body/paint reflection draw sampled from a
//     hand-authored CT32 environment map. The test pins the destination alpha test (DATE/DATM), the additive blend,
//     ZBUF's no-write, FBMSK and the fog.
#include "coregs3_reflection.hpp"
#include "coregs3_render.hpp"
#include "coregs3_sampler.hpp"
#include "coregs3_scene.hpp"
#include "coregs3_shape.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
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

// ---- the pinned formula against the AOT harness ----------------------------------------------------------------

// The hand-authored body block: a 30-degree rotation about z as the texgen columns C9..CB, a light position LP, and
// the round-18 lit constants C3/C4 (unused by the texgen formula, present because the fragment reads them).
const float kBlock[6][4] = {
    {0.8660254f, 0.5f, 0.0f, 0.0f},
    {-0.5f, 0.8660254f, 0.0f, 0.0f},
    {0.0f, 0.0f, 1.0f, 0.0f},
    {100.0f, 200.0f, 300.0f, 1.0f},
    {1.0f, 1.0f, 1.0f, 1.0f},
    {0.5f, 0.5f, 0.5f, 0.0f},
};

CapturedItem body_item() {
  CapturedItem item;
  for (std::uint32_t qword = 0; qword < 6; ++qword)
    for (std::uint32_t lane = 0; lane < 4; ++lane) item.light_block[qword][lane] = float_bits(kBlock[qword][lane]);
  item.light_block_qwords = 6;
  item.light_block_command = kBodyLightBlockCommand;
  item.light_source = kLightItemUploaded;
  return item;
}

MeshVertex make_vertex(std::int32_t x, std::int32_t y, std::int32_t z, std::int8_t nx, std::int8_t ny,
                       std::int8_t nz) {
  MeshVertex vertex;
  vertex.position = {static_cast<float>(x) / 4096.0f, static_cast<float>(y) / 4096.0f, static_cast<float>(z) / 4096.0f};
  vertex.raw_position = {x, y, z};
  vertex.has_raw_position = true;
  vertex.normal = {nx, ny, nz};
  vertex.has_normal = true;
  return vertex;
}

void check_formula_harness() {
  // These are the exact inputs the AOT harness ran (the fragment's TOPS+0x40 floats are the expected values). The
  // fragment uses the EFU rsqrt; this module uses the standard sqrt, so the study's agreement bound applies: the
  // harness measured rms 3.4e-4 / max 1.9e-3 over 4,010 captured vertices.
  const CapturedItem item = body_item();
  struct Case {
    MeshVertex vertex;
    float s;
    float t;
  };
  const Case cases[] = {
      {make_vertex(150, 250, 350, 0, 0, 127), 0.614925206f, 0.0710932612f},
      {make_vertex(50, 150, 400, 76, 0, 102), 0.310672939f, 0.764279068f},
      {make_vertex(200, 100, 250, 0, 76, 102), 0.6371454f, 0.38020727f},
  };
  for (const Case &entry : cases) {
    ReflectionCoordinate out;
    require(reflection_st(item, entry.vertex, out) == ReflectionStatus::ok, "the hand-authored vertex has a coordinate");
    require(std::fabs(out.s - entry.s) < 5.0e-4f, "S matches the AOT harness within the study's bound");
    require(std::fabs(out.t - entry.t) < 5.0e-4f, "T matches the AOT harness within the study's bound");
  }
}

void check_formula_refusals() {
  const MeshVertex good = make_vertex(150, 250, 350, 0, 0, 127);
  ReflectionCoordinate out;
  {
    CapturedItem item = body_item();
    item.light_block_qwords = 5;
    require(reflection_st(item, good, out) == ReflectionStatus::refused_block, "five block qwords are refused");
  }
  {
    CapturedItem item = body_item();
    item.light_block_command = 0x6C09C0C9u;
    require(reflection_st(item, good, out) == ReflectionStatus::refused_block, "the nine-qword form is refused");
  }
  {
    const CapturedItem item = body_item();
    MeshVertex vertex = good;
    vertex.has_raw_position = false;
    vertex.position = {0.0f, 0.0f, 0.0f};
    require(reflection_st(item, vertex, out) == ReflectionStatus::refused_position, "a V3-32 vertex is refused");
  }
  {
    const CapturedItem item = body_item();
    MeshVertex vertex = good;
    vertex.has_normal = false;
    require(reflection_st(item, vertex, out) == ReflectionStatus::refused_normal, "a vertex without the normal refuses");
  }
  {
    CapturedItem item = body_item();
    item.light_block[0][0] = float_bits(std::numeric_limits<float>::quiet_NaN());
    require(reflection_st(item, good, out) == ReflectionStatus::refused_non_finite, "a NaN block refuses");
  }
  {
    // A block whose texgen columns send the reflected vector exactly along -z makes W = V/|V| + (0,0,1) zero, where
    // the study's formula has no coordinate: the fixture must refuse it instead of dividing by zero.
    CapturedItem item = body_item();
    for (std::uint32_t lane = 0; lane < 4; ++lane) {
      item.light_block[0][lane] = float_bits(lane == 2 ? -1.0f : 0.0f);
      item.light_block[1][lane] = float_bits(0.0f);
      item.light_block[2][lane] = float_bits(0.0f);
    }
    // P - LP = (50, 50, 50); N = (0,0,1) gives c = 0, so R = D and V = (0, 0, -50): |W| = 0.
    const MeshVertex vertex = make_vertex(150, 250, 350, 0, 0, 127);
    require(reflection_st(item, vertex, out) == ReflectionStatus::refused_degenerate,
            "a zero W length is refused, never divided");
  }
}

void check_fog() {
  const std::array<std::uint8_t, 4> colour{100, 200, 40, 128};
  // The GS fog mix always goes through the >> 8 product: F = 255 is the near-identity (255*c)>>8 = c - 1, exactly the
  // hw_gs fixture's rule (native/hw_gs/pixel/hw_gs_pixel.cpp).
  require(apply_vertex_fog(colour, 255, 0x000000u) == std::array<std::uint8_t, 4>{99, 199, 39, 128},
          "fog 255 is the GS truncating near-identity");
  require(apply_vertex_fog(colour, 0, 0x000000u) == std::array<std::uint8_t, 4>{0, 0, 0, 128},
          "fog 0 is FOGCOL black and keeps the alpha");
  require(apply_vertex_fog(colour, 128, 0x000000u) == std::array<std::uint8_t, 4>{50, 100, 20, 128},
          "fog 128 halves toward black in the GS integer form");
  require(apply_vertex_fog(colour, 0, 0x00FF00u) == std::array<std::uint8_t, 4>{0, 254, 0, 128},
          "fog 0 is the FOGCOL colour, truncated by the >> 8 product");
}

void check_binding_subset() {
  std::vector<std::uint8_t> rgba(4u * 4u, 128u);
  SampledTexture texture;
  texture.psm = 0x00u;
  texture.tw = 1;
  texture.th = 1;
  texture.tcc = 1;
  texture.tfx = 0;
  texture.mxl = 0;
  texture.mmag = 1;
  texture.mmin = 1;
  texture.wms = 2;
  texture.wmt = 2;
  texture.minu = 0;
  texture.maxu = 1;
  texture.minv = 0;
  texture.maxv = 1;
  texture.width = 2;
  texture.height = 2;
  texture.rgba = rgba.data();

  TextureBinding binding;
  binding.texture = &texture;
  binding.reflection = true;
  binding.state = measured_sample_state();
  binding.state.prim = 0x7Cu;  // ABE on (bit 6)
  binding.state.alpha = 0ull | (2ull << 2) | (1ull << 4) | (1ull << 6);  // A Cs, B 0, C Ad, D Cd
  binding.state.test = 1ull | (6ull << 1) | (32ull << 4) | (1ull << 14) | (0ull << 15) | (1ull << 16) | (2ull << 17);
  require(check_reflection_binding(binding) == ReflectionBindingStatus::ok, "the measured reflection state is accepted");

  TextureBinding missing = binding;
  missing.texture = nullptr;
  require(check_reflection_binding(missing) == ReflectionBindingStatus::no_texture, "no environment map is counted");

  TextureBinding normal = binding;
  normal.state.alpha = 0ull | (1ull << 2) | (0ull << 4) | (1ull << 6);
  require(check_reflection_binding(normal) == ReflectionBindingStatus::refused_blend, "the normal blend is refused");

  TextureBinding no_abe = binding;
  no_abe.state.prim = 0x3Cu;
  require(check_reflection_binding(no_abe) == ReflectionBindingStatus::refused_blend, "ABE off is refused");

  TextureBinding fail_mode = binding;
  fail_mode.state.test = 1ull | (6ull << 1) | (32ull << 4) | (1ull << 12);
  require(check_reflection_binding(fail_mode) == ReflectionBindingStatus::refused_alpha_fail,
          "an AFAIL other than KEEP is refused");

  TextureBinding ztst = binding;
  ztst.state.test = 1ull | (6ull << 1) | (32ull << 4) | (1ull << 16) | (1ull << 17);
  require(check_reflection_binding(ztst) == ReflectionBindingStatus::refused_depth, "ZTST other than GEQUAL is refused");

  TextureBinding mip = binding;
  mip.texture = nullptr;
  SampledTexture mipped = texture;
  mipped.mxl = 1;
  mip.texture = &mipped;
  require(check_reflection_binding(mip) == ReflectionBindingStatus::refused_mip, "a mipmapped map is refused");
}

// ---- render integration: a textured base draw then the reflection draw -----------------------------------------

// A 2x2 nearest/linear CT32 texture. The row order is the decoder's: the top row first.
std::vector<std::uint8_t> quad_texture(std::uint8_t alpha00, std::uint8_t alpha10, std::uint8_t alpha01,
                                       std::uint8_t alpha11, const std::array<std::uint8_t, 3> &rgb) {
  return {rgb[0], rgb[1], rgb[2], alpha00, rgb[0], rgb[1], rgb[2], alpha10,
          rgb[0], rgb[1], rgb[2], alpha01, rgb[0], rgb[1], rgb[2], alpha11};
}

// The base draw's packet: V3-16 positions at TOPS 0, V2-16 UVs at TOPS 0x40, V4-8 colour 0x80 = 1.0 at TOPS 0x80.
std::vector<std::uint8_t> make_base_packet() {
  std::vector<std::uint8_t> packet;
  auto pad = [&packet] {
    while (packet.size() % 4 != 0) packet.push_back(0);
  };
  put_u32(packet, unpack_word(2, 1, 4, 0x00, true));
  const std::int16_t positions[4][3] = {{-4096, -4096, 0}, {4096, -4096, 0}, {-4096, 4096, 0}, {4096, 4096, 0}};
  for (const auto &position : positions)
    for (std::int16_t component : position) put_u16(packet, static_cast<std::uint16_t>(component));
  pad();
  put_u32(packet, unpack_word(1, 1, 4, 0x40, true));
  const std::int16_t uvs[4][2] = {{0, 0}, {4096, 0}, {0, 4096}, {4096, 4096}};
  for (const auto &uv : uvs)
    for (std::int16_t component : uv) put_u16(packet, static_cast<std::uint16_t>(component));
  pad();
  put_u32(packet, unpack_word(3, 2, 4, 0x80, true));
  for (int vertex = 0; vertex < 4; ++vertex)
    for (int lane = 0; lane < 4; ++lane) packet.push_back(0x80);
  pad();
  return packet;
}

// The body/paint reflection draw's packet: V3-16 positions, V3-8 normals at TOPS 0x40, V4-8 colour at TOPS 0x80.
std::vector<std::uint8_t> make_body_packet() {
  std::vector<std::uint8_t> packet;
  auto pad = [&packet] {
    while (packet.size() % 4 != 0) packet.push_back(0);
  };
  put_u32(packet, unpack_word(2, 1, 4, 0x00, true));
  const std::int16_t positions[4][3] = {{-4096, -4096, 0}, {4096, -4096, 0}, {-4096, 4096, 0}, {4096, 4096, 0}};
  for (const auto &position : positions)
    for (std::int16_t component : position) put_u16(packet, static_cast<std::uint16_t>(component));
  pad();
  put_u32(packet, unpack_word(2, 2, 4, 0x40, true));  // V3-8 normals, 4 vectors of one qword
  for (int vertex = 0; vertex < 4; ++vertex) {
    packet.push_back(0);    // nx
    packet.push_back(0);    // ny
    packet.push_back(127);  // nz = 1.0 after the fragment's ITOF0 * 1/127
    packet.push_back(0);
  }
  pad();
  put_u32(packet, unpack_word(3, 2, 4, 0x80, true));  // V4-8 colour: 0x80 = 1.0
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

CapturedDraw make_draw(std::uint32_t sequence, std::uint32_t packet_bytes) {
  CapturedDraw draw;
  draw.sequence = sequence;
  draw.shape = 0x00400000u;
  draw.item_count = 1;
  draw.items[0].address = 0x00100000u + sequence;
  draw.items[0].qwords = static_cast<std::uint16_t>(packet_bytes / 16);
  draw.items[0].packet_offset = 0;
  draw.items[0].packet_bytes = packet_bytes;
  set_identity_columns(draw);
  return draw;
}

struct RenderFixture {
  FrameSnapshot frame;
  FrameTextureTable table;
  std::vector<std::uint8_t> rgba, mask;
  std::vector<std::uint8_t> reflection_mask;
  RenderStats stats;
  std::string error;
  // Textures owned by the fixture so the bindings' pointers stay valid.
  std::vector<std::uint8_t> base_payload;
  std::vector<std::uint8_t> env_payload;
  SampledTexture base_texture;
  SampledTexture env_texture;

  RenderFixture(std::uint8_t base00, std::uint8_t base10, std::uint8_t base01, std::uint8_t base11,
                const std::array<std::uint8_t, 3> &base_rgb, const std::array<std::uint8_t, 3> &env_rgb,
                std::uint8_t fog) {
    // The base draw's packet is the sampler fixture's textured quad; its MODULATE alpha is the destination alpha the
    // reflection's DATE reads. The body draw's packet carries the reflection item's inputs.
    std::vector<std::uint8_t> base_packet = make_base_packet();
    std::vector<std::uint8_t> body_packet = make_body_packet();
    CapturedDraw base = make_draw(0, static_cast<std::uint32_t>(base_packet.size()));
    CapturedDraw body = make_draw(1, static_cast<std::uint32_t>(body_packet.size()));
    // The body item's block and command are the measured six-qword form; the item's own colour falls back to the V4-8
    // channel (0x80) because the fixture does not author a run pair.
    body.items[0] = body_item();
    body.items[0].address = 0x00300001u;
    body.items[0].qwords = static_cast<std::uint16_t>(body_packet.size() / 16);
    body.items[0].packet_offset = 0;
    body.items[0].packet_bytes = static_cast<std::uint32_t>(body_packet.size());
    body.items[0].texture_record = 1;
    base.items[0].texture_record = 0;
    body.items[0].run_valid = 0;
    body.items[0].light_source = kLightItemUploaded;

    {
      Mesh mesh;
      MeshOptions mesh_options;
      std::string mesh_error;
      require(build_mesh(body_packet.data(), body_packet.size(), body, mesh_options, mesh, mesh_error),
              "the body packet builds a mesh");
      require(mesh.vertices.size() == 4 && mesh.has_normals && mesh.vertices[0].has_normal,
              "the body mesh carries the V3-8 normals");
      require(!mesh.has_uv, "the body packet has no model UV (the reflection uses the texgen coordinate)");
    }
    frame.draws.push_back(base);
    frame.draws.push_back(body);
    frame.draw_count = 2;
    frame.packets = base_packet;
    frame.packets.insert(frame.packets.end(), body_packet.begin(), body_packet.end());
    frame.draws[1].items[0].packet_offset = static_cast<std::uint32_t>(base_packet.size());

    // The texture table: both textures are decoded RGBA8 the fixture authors directly (CT32 in the sampler's terms).
    base_payload = quad_texture(base00, base10, base01, base11, base_rgb);
    // Every environment texel has the same RGB and alpha 200, so whichever texel the texgen coordinate lands on the
    // MODULATE result (and therefore the expected pixel) is the same.
    env_payload = quad_texture(200, 200, 200, 200, env_rgb);
    const auto texture = [](std::uint32_t tw, std::uint32_t th, const std::vector<std::uint8_t> &rgba) {
      SampledTexture value;
      value.psm = 0x00u;  // CT32
      value.tw = tw;
      value.th = th;
      value.tcc = 1;
      value.tfx = 0;
      value.mxl = 0;
      value.mmag = 0;  // nearest: the fixture's ST lands exactly on texel centres
      value.mmin = 0;
      value.wms = 2;
      value.wmt = 2;
      value.minu = 0;
      value.maxu = 1;
      value.minv = 0;
      value.maxv = 1;
      value.width = 1u << tw;
      value.height = 1u << th;
      value.rgba = rgba.data();
      return value;
    };
    base_texture = texture(1, 1, base_payload);
    env_texture = texture(1, 1, env_payload);

    table.sequence = {0, 1};
    table.payloads.push_back(base_payload);
    table.payloads.push_back(env_payload);
    table.textures.push_back(base_texture);
    table.textures.push_back(env_texture);
    std::vector<TextureBinding> base_bindings(1);
    base_bindings[0].texture = &table.textures.front();
    base_bindings[0].state = measured_sample_state();
    base_bindings[0].state.test = 0;  // ATE off: the alpha-0 texels must write alpha 0, not fail the test
    base_bindings[0].state.prim = 0x3Cu;  // ABE off, the measured base draw
    std::vector<TextureBinding> body_bindings(1);
    body_bindings[0].texture = &table.textures.back();
    body_bindings[0].reflection = true;
    body_bindings[0].state = measured_sample_state();
    body_bindings[0].state.prim = 0x7Cu;  // ABE on (bit 6)
    body_bindings[0].state.alpha = 0ull | (2ull << 2) | (1ull << 4) | (1ull << 6);  // additive
    // TEST: ATE off, DATE 1, DATM 0, ZTE 1, ZTST GEQUAL (2).
    body_bindings[0].state.test = (1ull << 14) | (1ull << 16) | (2ull << 17);
    body_bindings[0].state.zmsk = 1;  // the measured pass writes no depth
    body_bindings[0].vertex_fog.assign(4, fog);
    table.draws.push_back(base_bindings);
    table.draws.push_back(body_bindings);
  }

  void render(bool reflections) {
    RenderOptions options;
    options.width = 64;
    options.height = 48;
    options.textures = &table;
    options.reflections = reflections;
    require(render_frame(frame, options, rgba, mask, stats, error, nullptr, &reflection_mask),
            "the two draws render");
  }

  std::array<std::uint8_t, 4> pixel(int x, int y) const {
    const std::size_t at = (static_cast<std::size_t>(y) * 64u + static_cast<std::size_t>(x)) * 4u;
    return {rgba[at + 0], rgba[at + 1], rgba[at + 2], rgba[at + 3]};
  }
};

void check_render_reflection() {
  // The base quad's texture is a checkerboard of MODULATE alphas 64 (top-left/bottom-left) and 128 (right column).
  // The reflection's ST lands each corner on the environment map's texel centre; DATE DATM=0 draws only where the
  // destination alpha's MSB is clear, i.e. the alpha-64 texels, and the additive blend uses Ad=64.
  {
    RenderFixture fixture(64, 128, 64, 128, {0, 0, 0}, {200, 100, 50}, 255);
    fixture.render(true);
    require(fixture.stats.items_reflected == 1, "the body item is drawn as the reflection pass");
    require(fixture.stats.items_reflection_refused == 0, "the measured state is accepted");
    require(fixture.stats.reflection_pixels > 0, "the reflection writes fragments");
    require(fixture.stats.reflection_date_failed > 0, "the destination alpha test masks the alpha-128 texels");
    require(fixture.stats.reflection_blended_pixels == fixture.stats.reflection_pixels,
            "every written reflection fragment is blended");
    require(fixture.stats.reflection_depth_masked_pixels == fixture.stats.reflection_pixels,
            "the measured pass writes no depth");
    // The quad's four UV quadrants map to the environment map's four texels. The top-left quadrant (u<0.5, v<0.5) is
    // env texel (0,0) = (200,100,50) with the lit colour 0x80 (MODULATE passes it), fog 255 (the GS truncating
    // near-identity: 199/99/49) and Ad = 64:
    //   ((199*64)>>7, (99*64)>>7, (49*64)>>7) + 0 = (99, 49, 24), alpha As = 200.
    require(fixture.pixel(16, 12) == std::array<std::uint8_t, 4>{99, 49, 24, 200},
            "the DATE-passing quadrant is the additive reflection over the base");
    // The top-right quadrant (u>=0.5) is base alpha 128: DATE masks the fragment, so the pixel stays the base draw's
    // black with alpha 128.
    require(fixture.pixel(48, 12) == std::array<std::uint8_t, 4>{0, 0, 0, 128},
            "the DATE-failing quadrant keeps the base draw");
    require(fixture.reflection_mask[12 * 64 + 16] == 1 && fixture.reflection_mask[12 * 64 + 48] == 0,
            "the reflection mask marks the written fragments only");
  }
  // The A/B: with reflections off the body item keeps the lit colour and the destination alpha is untouched.
  {
    RenderFixture fixture(64, 128, 64, 128, {0, 0, 0}, {200, 100, 50}, 255);
    fixture.render(false);
    require(fixture.stats.reflection_pixels == 0, "the A/B writes no reflection");
    require(fixture.pixel(16, 12) == std::array<std::uint8_t, 4>{128, 128, 128, 255},
            "the A/B keeps the lit body colour");
  }
  // Fog 128 toward FOGCOL black halves the sampled colour before the blend: (200/2 * 64)>>7 = 50, (100/2*64)>>7=25,
  // (50/2*64)>>7=12. The alpha lane is unchanged by fog (As = 200).
  {
    RenderFixture fixture(64, 128, 64, 128, {0, 0, 0}, {200, 100, 50}, 128);
    fixture.render(true);
    require(fixture.pixel(16, 12) == std::array<std::uint8_t, 4>{50, 25, 12, 200},
            "the vertex fog halves the reflection toward FOGCOL black");
  }
  // FBMSK keeps the destination's red lane: 0x000000FF is the R lane in the sampler's byte order.
  {
    RenderFixture fixture(64, 128, 64, 128, {0, 0, 0}, {200, 100, 50}, 255);
    fixture.table.draws[1][0].state.fbmsk = 0x000000FFu;
    fixture.render(true);
    require(fixture.pixel(16, 12) == std::array<std::uint8_t, 4>{0, 49, 24, 200},
            "FBMSK keeps the destination red lane");
    require(fixture.stats.reflection_fbmsk_pixels == fixture.stats.reflection_pixels, "the FBMSK merges are counted");
  }
  // ZBUF's no-write: a third draw at the same depth must still pass its GEQUAL test after the reflection wrote no
  // depth. The fixture checks the counter here; the depth buffer itself is internal to render_frame.
  {
    RenderFixture fixture(64, 128, 64, 128, {0, 0, 0}, {200, 100, 50}, 255);
    fixture.render(true);
    require(fixture.stats.reflection_depth_masked_pixels > 0, "ZBUF.ZMSK is reproduced");
  }
}

}  // namespace

int main() {
  try {
    check_formula_harness();
    check_formula_refusals();
    check_fog();
    check_binding_subset();
    check_render_reflection();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "coregs3 reflection fixture: %s\n", error.what());
    return 1;
  }
  std::printf("coregs3 reflection fixture: ok\n");
  return 0;
}
