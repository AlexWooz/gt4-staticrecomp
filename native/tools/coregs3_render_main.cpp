// TASK-090 CoreGS 3.0: the offline capture tools (docs/COREGS3-PLAN.md §4 step 1c).
//
//   gt4_coregs3_render <capture.gt4scene> --list
//       prints the frames of a capture with their VBlank, draw/record/packet counts and the channel scan of one frame.
//       This is the instrument that turns a capture into evidence about the shape layout.
//
//   gt4_coregs3_render <capture.gt4scene> --frame N --out <file.png> [--width W --height H] [--background <png>]
//                         [--no-depth] [--position-scale S] [--no-flip-y] [--report <file.txt>] [--no-programs]
//       renders one captured car pass natively and writes a PNG plus a short report. --no-programs is the evidence A/B:
//       it invalidates every item's run pair, so the renderer falls back to the base colours and the difference from the
//       normal render is exactly what the measured per-item VU1 programs add.
//
//   gt4_coregs3_render <capture.gt4scene> --frame N --out <file.png> --vram <capture.gt4vram> [--tap <capture.gt4tap>]
//                         [--no-textured] [--texture-body] ...
//       the first textured car frame (TASK-090 texture milestone): decodes the items' texture records from the GS
//       local-memory sidecar (once per Format G key), takes each item's TEST/ALPHA/FRAME/ZBUF state from the tap when
//       one is given, and samples the measured subset (coregs3_sampler.hpp). The body/paint family stays on the lit
//       colour (its CT32 environment map is sampled with VU1 texgen ST, a later task). --no-textured is the untextured
//       A/B: the same frame with no texture table, byte-for-byte the pre-texture renderer.
//
//   gt4_coregs3_render <capture.gt4scene> --frame N --out <file.png> --vram <capture.gt4vram> --tap <capture.gt4tap>
//                         --world [--no-sky] [--no-underlay] [--no-course] [--no-wet-reflection] [--no-morph]
//                         [--mask <pgm>] [--width W --height H]
//       TASK-090 track M1: the offline sky + road picture. Selects the sky (PassId 2), far-underlay (3), main-view
//       course (5) and since the wet-reflection round the captured reflection pass's main-view items (7), counts every
//       exclusion, and renders them with the captured 0xC1 matrix (packet or carried), the tapped state and the level-0
//       mip/filter fallbacks. --no-sky/--no-underlay/--no-course/--no-wet-reflection are the evidence A/Bs;
//       --world-no-alpha is a diagnostic that ignores the tapped ATE (never an acceptance mode).
//
//   gt4_coregs3_render <capture.gt4scene> --frame N --shade-dump <file.txt>
//       shades every item with its own program and writes one line per mesh vertex (draw, item, vertex, RGBA, program,
//       status) for the tap validation scripts.
//
//   gt4_coregs3_render <capture.gt4scene> --frame N --out <file.png> --world --gs <capture.gt4gs> --tap <capture.gt4tap>
//                         --vram <capture.gt4vram> --hud [--hud-element speed] [--hud-mask <pgm>]
//                         [--hud-font-atlas <gt4f>] [--hud-texture-dump <dir>] --width 640 --height 448
//       TASK-090 S3: the first native HUD layer. Draws the requested element (the measured B-Spec-style speed plate and
//       its digits) over the world picture with the captured GS state (FRAME/SCISSOR/XYOFFSET/TEST/ALPHA/PRIM/TEX0/TEX1/
//       CLAMP), the captured T4/T8 textures with their CT32 CSM1 palettes and the captured placement. --no-hud is the
//       A/B: the world picture with no panel layer, byte for byte the pre-HUD renderer. The layer is native 640x448.
//
// A capture is derived from the user's own disc and is never committed: both the input and the PNG output live under
// private/ or build/evidence/.
#include "coregs3_gsdecode.hpp"
#include "coregs3_gsstream_file.hpp"
#include "coregs3_png.hpp"
#include "coregs3_render.hpp"
#include "coregs3_wet_composite.hpp"
#include "coregs3_scene.hpp"
#include "coregs3_shape.hpp"
#include "coregs3_world.hpp"
#ifdef GT4_COREGS3_TEXTURES
#include "coregs3_hud.hpp"
#include "coregs3_tap_file.hpp"
#include "coregs3_texture.hpp"
#include "coregs3_texture_frame.hpp"
#include "coregs3_vram.hpp"
#include "hw_gs_registers.hpp"
#endif

#include <cmath>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace {

using namespace gt4::coregs3;

// Maps a PGL context byte offset to its slot in CapturedDraw::pgl (the sparse spans of coregs3_scene.hpp).
bool pgl_word(const CapturedDraw &draw, std::uint32_t offset, std::uint32_t &index) {
  std::uint32_t base = 0;
  for (const PglSpan &span : kPglSpans) {
    const std::uint32_t bytes = span.words * 4u;
    if (offset >= span.offset && offset < span.offset + bytes) {
      index = base + (offset - span.offset) / 4u;
      return true;
    }
    base += span.words;
  }
  return false;
}

float as_float(std::uint32_t bits) {
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof value);
  return value;
}

bool read_file(const char *path, std::vector<std::uint8_t> &data) {
  std::FILE *file = std::fopen(path, "rb");
  if (file == nullptr) return false;
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (size < 0) {
    std::fclose(file);
    return false;
  }
  data.resize(static_cast<std::size_t>(size));
  const bool ok = std::fread(data.data(), 1, data.size(), file) == data.size();
  std::fclose(file);
  return ok;
}

// Reads a capture's header and every frame block, skipping nothing: the tool reports what the file actually contains.
bool load_capture(const char *path, SceneHeader &header, std::vector<FrameSnapshot> &frames, std::string &error) {
  std::vector<std::uint8_t> data;
  if (!read_file(path, data)) {
    error = "cannot open " + std::string(path);
    return false;
  }
  std::size_t at = 0;
  if (!parse_scene_header(data.data(), data.size(), header, error)) return false;
  at = 16;
  while (at < data.size()) {
    if (data.size() - at < 8) {
      error = "trailing bytes at the end of the capture";
      return false;
    }
    FrameSnapshot frame;
    const std::size_t consumed = parse_frame_block(data.data() + at, data.size() - at, header.version, frame, error);
    if (consumed == 0) return false;
    frames.push_back(std::move(frame));
    at += consumed;
  }
  return true;
}

void print_ndc_stats(const FrameSnapshot &frame) {
  MeshOptions mesh_options;
  std::uint32_t shown = 0;
  for (const CapturedDraw &draw : frame.draws) {
    if (shown++ >= 6) break;
    Mesh mesh;
    std::string error;
    if (!build_mesh(frame.packets.data(), frame.packets.size(), draw, mesh_options, mesh, error)) {
      std::printf("draw %u: %s\n", draw.sequence, error.c_str());
      continue;
    }
    const DrawMatrix matrix = draw_matrix(draw);
    float w_min = 1e30f, w_max = -1e30f;
    float x_min = 1e30f, x_max = -1e30f, y_min = 1e30f, y_max = -1e30f;
    std::uint32_t front = 0;
    for (const MeshVertex &vertex : mesh.vertices) {
      // The metre transform of the vertex's own position encoding; its row-major form is the measured 0xC1 map for
      // that vertex (coregs3_vertex.hpp).
      const VertexColumns &columns =
          mesh_position_encoding(vertex) == PositionEncoding::integer_16 ? matrix.integer_positions
                                                                        : matrix.float_positions;
      const VertexRows rows = vertex_rows(columns);
      const float x = vertex.position[0], y = vertex.position[1], z = vertex.position[2];
      const float cx = rows[0][0] * x + rows[0][1] * y + rows[0][2] * z + rows[0][3];
      const float cy = rows[1][0] * x + rows[1][1] * y + rows[1][2] * z + rows[1][3];
      const float cw = rows[3][0] * x + rows[3][1] * y + rows[3][2] * z + rows[3][3];
      if (cw > 1e-6f) {
        ++front;
        const float nx = cx / cw, ny = cy / cw;
        if (nx < x_min) x_min = nx;
        if (nx > x_max) x_max = nx;
        if (ny < y_min) y_min = ny;
        if (ny > y_max) y_max = ny;
      }
      if (cw < w_min) w_min = cw;
      if (cw > w_max) w_max = cw;
    }
    std::printf("draw %u: %zu vertices, front %u, w %.3f..%.3f, ndc x %.3f..%.3f y %.3f..%.3f\n", draw.sequence,
                mesh.vertices.size(), front, w_min, w_max, x_min, x_max, y_min, y_max);
  }
}

void print_channel_scan(const FrameSnapshot &frame) {
  std::printf("frame %u vblank %llu draws %u textures %u materials %u packets %zu\n", frame.index,
              static_cast<unsigned long long>(frame.vblank), frame.draw_count, frame.texture_count,
              frame.material_count, frame.packets.size());
  std::printf("camera valid 0x%x camera 0x%08X\n", frame.camera.valid, frame.camera.camera);
  std::uint32_t shown = 0;
  for (const CapturedDraw &draw : frame.draws) {
    if (shown++ >= 4) break;
    const DrawMatrix matrix = draw_matrix(draw);
    std::printf("draw %u pass %s call %s caller %08X shape 0x%08X items %u value %08X\n", draw.sequence,
                pass_name(draw.pass), call_site_name(draw.call_site), draw.caller_ra, draw.shape, draw.item_count,
                draw.cmd_value);
    std::printf("  program mscal %08X sel %u entry 0x%04X | itop %08X dispatch 0x%04X | count %u valid %u; "
                "light block %u qwords cmd %08X\n",
                draw.program_tag, draw.program_selector, draw.program_entry, draw.program_itop, draw.program_dispatch,
                draw.program_count, draw.program_valid, draw.light_block_qwords, draw.light_block_command);
    std::printf("  chain tags %u valid %u uniform %u\n", draw.chain_tags, draw.chain_valid, draw.program_uniform);
    for (std::uint32_t index = 0; index < draw.item_count; ++index) {
      const CapturedItem &item = draw.items[index];
      std::printf("    item %u source %u run %08X %08X valid %u | light %u qwords cmd %08X src %08X\n", index,
                  item.light_source, item.run_itop, item.run_mscal, item.run_valid, item.light_block_qwords,
                  item.light_block_command, item.light_block_source);
    }
    std::printf("  vf24 %08X %08X %08X %08X\n", draw.vf[0][0], draw.vf[0][1], draw.vf[0][2], draw.vf[0][3]);
    std::printf("  vf25 %08X %08X %08X %08X\n", draw.vf[1][0], draw.vf[1][1], draw.vf[1][2], draw.vf[1][3]);
    std::printf("  vf26 %08X %08X %08X %08X\n", draw.vf[2][0], draw.vf[2][1], draw.vf[2][2], draw.vf[2][3]);
    std::printf("  vf27 %08X %08X %08X %08X\n", draw.vf[3][0], draw.vf[3][1], draw.vf[3][2], draw.vf[3][3]);
    for (std::uint32_t row = 0; row < 4; ++row)
      std::printf("  m%d %10.4f %10.4f %10.4f %10.4f\n", row, matrix.row[row][0], matrix.row[row][1],
                  matrix.row[row][2], matrix.row[row][3]);
    for (std::uint32_t row = 0; row < 4; ++row) {
      std::printf("  proj%d", row);
      for (std::uint32_t lane = 0; lane < 4; ++lane) {
        std::uint32_t index = 0;
        pgl_word(draw, 0xFA0u + row * 16u + lane * 4u, index);
        std::printf(" %10.4f", as_float(draw.pgl[index]));
      }
      std::printf("\n");
    }
    std::printf("  matrix_packet valid %u\n", draw.matrix_packet_valid);
    for (std::uint32_t row = 0; row < 8; ++row)
      std::printf("    q%u %10.4f %10.4f %10.4f %10.4f\n", row, as_float(draw.matrix_packet[row][0]),
                  as_float(draw.matrix_packet[row][1]), as_float(draw.matrix_packet[row][2]),
                  as_float(draw.matrix_packet[row][3]));
    std::printf("  count %u x %u y %u mask %08X bits %08X%08X\n",
                draw.pgl[5], draw.pgl[6], draw.pgl[7], draw.pgl[16], draw.pgl[19], draw.pgl[17]);
    const ChannelScan scan = scan_channels(frame.packets.data(), frame.packets.size(), draw);
    std::printf("  packets %u decoded %u channels %zu\n", scan.packets, scan.decoded, scan.channels.size());
    for (const ShapeChannel &channel : scan.channels) {
      std::printf("    item %u cmd %u %s q%03X %-6s x%u %s\n", channel.item, channel.command,
                  channel.tops ? "tops" : "abs ", channel.destination, channel_format_name(channel.format),
                  channel.vectors, channel_role_name(channel.role));
    }
    for (const std::string &item_error : scan.errors) std::printf("    error: %s\n", item_error.c_str());
    Mesh mesh;
    std::string mesh_error;
    MeshOptions options;
    if (build_mesh(frame.packets.data(), frame.packets.size(), draw, options, mesh, mesh_error)) {
      std::printf("  mesh: %zu vertices, %u triangles, %zu strips, colours %d uv %d table %d\n", mesh.vertices.size(),
                  mesh.triangles(), mesh.strip_starts.size(), mesh.has_colors ? 1 : 0, mesh.has_uv ? 1 : 0,
                  mesh.has_strip_table ? 1 : 0);
      std::printf("  strips length:");
      for (std::uint32_t length : mesh.strip_lengths) std::printf(" %u", length);
      std::printf("\n  strips start:");
      for (std::uint32_t start : mesh.strip_starts) std::printf(" %u", start);
      std::printf("\n");
    } else {
      std::printf("  mesh refused: %s\n", mesh_error.c_str());
    }
  }
}

}  // namespace

#ifdef GT4_COREGS3_TEXTURES
namespace {

// TASK-090 world families: the census bucket of a measured course world program.
int world_family_index(std::uint32_t dispatch) {
  switch (dispatch) {
  case 0x0080u: return 0;
  case 0x0B08u: return 1;
  case 0x00A0u: return 2;
  case 0x1278u: return 3;
  default: return 4;
  }
}

const char *world_family_label(int index) {
  switch (index) {
  case 0: return "itop_0x80";
  case 1: return "itop_0xB08";
  case 2: return "itop_0xA0";
  case 3: return "itop_0x1278";
  default: return "other";
  }
}

// TASK-090 track M1: the selected world passes and every exclusion, counted. The frame copy keeps the capture's packets
// and records; excluded items are disabled by clearing their packet range (build_mesh skips an item with no packet), so
// the original item indices, the texture table keys and the strip tables stay valid.
struct WorldSelection {
  FrameSnapshot frame;
  std::uint32_t sky_draws = 0, sky_items = 0, sky_vertices = 0, sky_no_position = 0;
  std::uint32_t underlay_draws = 0, underlay_items = 0, underlay_vertices = 0, underlay_no_position = 0;
  std::uint32_t course_draws = 0, course_items = 0, course_vertices = 0;
  std::uint32_t course_itop_other = 0;
  std::uint32_t course_mirror = 0;
  std::uint32_t course_no_state = 0;
  std::uint32_t course_no_position = 0;
  std::uint32_t course_shape_refused = 0;
  std::uint32_t course_uv_unverified = 0;
  std::uint32_t course_morph_uv_substituted = 0;      // 0xA0: first-key UV used (the UV keys differ, no factor)
  std::uint32_t course_morph_colour_substituted = 0;  // 0xA0: first-key colour used (the colour keys differ, no factor)
  // Format version 6: the 0xA0 items whose draw carried the morph factor, and which key pairs were blended with it.
  std::uint32_t course_morph_factor_items = 0;
  std::uint32_t course_morph_blended_uv = 0;
  std::uint32_t course_morph_blended_colour = 0;
  std::uint32_t course_morph_blended_position = 0;
  std::map<std::uint32_t, std::uint32_t> course_morph_factors;  // raw factor word -> selected 0xA0 items
  std::uint32_t course_draws_dropped = 0;
  // TASK-090 wet reflection: the captured reflection pass (PassId 7). Its main-view items are selected (the wet-road
  // reflection-model quads); every other item is a counted exclusion by the same rule as the course.
  std::uint32_t reflection_draws = 0, reflection_items = 0, reflection_vertices = 0;
  std::uint32_t reflection_itop_other = 0;
  std::uint32_t reflection_shape_refused = 0;
  std::uint32_t reflection_no_state = 0;
  std::uint32_t reflection_no_position = 0;
  std::uint32_t reflection_mirror = 0;
  std::uint32_t reflection_draws_dropped = 0;
  // The per-family census of the selected course items (the measured world programs) and the shape refusal reasons.
  std::uint32_t family_items[5] = {};     // indexed by WorldFamilyIndex: 0x80, 0xB08, 0xA0, 0x1278, other
  std::uint32_t family_vertices[5] = {};
  std::map<std::string, std::uint32_t> shape_refusals;
};

bool build_world_selection(const FrameSnapshot &frame, const FrameTextureTable &table, bool keep_sky,
                           bool keep_underlay, bool keep_course, bool keep_reflection, WorldSelection &selection,
                           std::string &error) {
  selection = WorldSelection{};
  selection.frame = frame;
  selection.frame.draws.clear();
  selection.frame.draw_count = 0;
  for (const CapturedDraw &draw : frame.draws) {
    const bool is_sky = draw.pass == kPassSky;
    const bool is_underlay = draw.pass == kPassFarUnderlay;
    const bool is_course = draw.pass == kPassCourse;
    const bool is_reflection = draw.pass == kPassReflection;
    if (!is_sky && !is_underlay && !is_course && !is_reflection) continue;
    if (is_sky && !keep_sky) continue;
    if (is_underlay && !keep_underlay) continue;
    if (is_course && !keep_course) continue;
    if (is_reflection && !keep_reflection) continue;
    CapturedDraw kept = draw;
    const ChannelScan scan = scan_channels(frame.packets.data(), frame.packets.size(), draw, true);
    const std::vector<TextureBinding> *bindings = table.for_draw(draw.sequence);
    std::uint32_t kept_items = 0;
    for (std::uint32_t item = 0; item < draw.item_count; ++item) {
      const CapturedItem &source = draw.items[item];
      WorldItemInput input;
      input.pass = draw.pass;
      input.run_valid = source.run_valid != 0;
      input.dispatch = input.run_valid ? (source.run_itop & 0x3FFu) * 8u : 0u;
      input.has_position = item < scan.items.size() && scan.items[item].has_position && source.packet_bytes != 0;
      // Format version 6: the draw's captured 0x00A0 morph factor turns the key substitutions into blends.
      const MorphBlend morph = morph_blend_of(draw);
      MorphKeyFacts morph_keys;
      input.shape_status = source.packet_bytes != 0
                               ? world_shape_status(frame.packets.data(), frame.packets.size(), source, morph,
                                                    &morph_keys)
                               : WorldShapeStatus::unsupported_program;
      const SampleState *state = bindings != nullptr && item < bindings->size() ? &(*bindings)[item].state : nullptr;
      if (state != nullptr) {
        input.state_known = state->from_tap;
        input.xyoffset_known = state->xyoffset_known;
        input.xyoffset = state->xyoffset;
        input.test_known = state->test_known;
        input.test = state->test;
        input.test_zero = state->world_test_zero;
      }
      const WorldExclusion exclusion = world_item_exclusion(input);
      if (exclusion != WorldExclusion::none) {
        switch (exclusion) {
        case WorldExclusion::itop_other:
          if (is_reflection) selection.reflection_itop_other += 1;
          else selection.course_itop_other += 1;
          break;
        case WorldExclusion::shape_refused:
          if (is_reflection) {
            selection.reflection_shape_refused += 1;
          } else {
            selection.course_shape_refused += 1;
            selection.shape_refusals[world_shape_status_name(input.shape_status)] += 1;
          }
          break;
        case WorldExclusion::mirror:
          if (is_reflection) selection.reflection_mirror += 1;
          else selection.course_mirror += 1;
          break;
        case WorldExclusion::no_state:
          if (is_reflection) selection.reflection_no_state += 1;
          else selection.course_no_state += 1;
          break;
        case WorldExclusion::no_position:
          if (is_reflection) selection.reflection_no_position += 1;
          else if (is_course) selection.course_no_position += 1;
          else if (is_sky) selection.sky_no_position += 1;
          else selection.underlay_no_position += 1;
          break;
        default: break;
        }
        kept.items[item].packet_bytes = 0;
        kept.items[item].qwords = 0;
        continue;
      }
      kept_items += 1;
      const std::uint32_t vertices = item < scan.items.size() ? scan.items[item].descriptor_vertices : 0;
      if (is_course) {
        selection.course_items += 1;
        selection.course_vertices += vertices;
        const int family = world_family_index(input.dispatch);
        selection.family_items[family] += 1;
        selection.family_vertices[family] += vertices;
        if (state != nullptr && state->world_pass_default) selection.course_uv_unverified += 1;
        if (input.dispatch == 0x00A0u) {
          if (morph.valid) {
            selection.course_morph_factor_items += 1;
            selection.course_morph_factors[morph.bits] += 1;
            if (morph_keys.uv_keys_differ) selection.course_morph_blended_uv += 1;
            if (morph_keys.colour_keys_differ) selection.course_morph_blended_colour += 1;
            if (morph_keys.position_keys_differ) selection.course_morph_blended_position += 1;
          } else {
            if (input.shape_status == WorldShapeStatus::morph_uv_first_key)
              selection.course_morph_uv_substituted += 1;
            if (input.shape_status == WorldShapeStatus::morph_colour_first_key)
              selection.course_morph_colour_substituted += 1;
          }
        }
      } else if (is_sky) {
        selection.sky_items += 1;
        selection.sky_vertices += vertices;
      } else if (is_reflection) {
        selection.reflection_items += 1;
        selection.reflection_vertices += vertices;
      } else {
        selection.underlay_items += 1;
        selection.underlay_vertices += vertices;
      }
    }
    if (is_course) {
      if (kept_items == 0) {
        selection.course_draws_dropped += 1;
        continue;
      }
      selection.course_draws += 1;
    } else if (is_sky) {
      if (kept_items == 0) continue;
      selection.sky_draws += 1;
    } else if (is_reflection) {
      if (kept_items == 0) {
        selection.reflection_draws_dropped += 1;
        continue;
      }
      selection.reflection_draws += 1;
    } else {
      if (kept_items == 0) continue;
      selection.underlay_draws += 1;
    }
    selection.frame.draws.push_back(kept);
    selection.frame.draw_count = static_cast<std::uint32_t>(selection.frame.draws.size());
  }
  if (selection.frame.draws.empty()) {
    error = "the world selection is empty";
    return false;
  }
  return true;
}

}  // namespace
#endif

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: gt4_coregs3_render <capture.gt4scene> --list\n"
                 "       gt4_coregs3_render <capture.gt4scene> --frame N --out <file.png> [options]\n");
    return 2;
  }
  SceneHeader header;
  std::vector<FrameSnapshot> frames;
  std::string error;
  if (!load_capture(argv[1], header, frames, error)) {
    std::fprintf(stderr, "gt4_coregs3_render: %s\n", error.c_str());
    return 1;
  }

  bool list = false;
  bool ndc = false;
  bool counts = false;
  bool no_programs = false;
  bool no_morph = false;
  int only_draw = -1;
  float min_y = 0.0f;
  float max_edge = 0.0f;
  int draw_range_last = -1;
  int frame_index = -1;
  const char *out = nullptr;
  const char *report = nullptr;
  const char *shade_dump = nullptr;
  const char *vram_path = nullptr;
  const char *tap_path = nullptr;
  const char *texture_mask_path = nullptr;
  const char *dump_textures_dir = nullptr;
  const char *coverage_mask_path = nullptr;
  bool no_textured = false;
  bool texture_body = false;
  bool no_reflection = false;
  bool world = false;
  bool world_keep_sky = true;
  bool world_keep_underlay = true;
  bool world_keep_course = true;
  bool world_keep_reflection = true;
  int vram_vblank = -1;
  const char *reflection_mask_path = nullptr;
  const char *gs_path = nullptr;
  const char *composite_mask_path = nullptr;
  const char *composite_changed_path = nullptr;
  bool wet_composite = true;
  bool post_chain = true;  // TASK-090: the whole captured PATH2 full-screen chain is on by default with --gs
  const char *chain_dump_dir = nullptr;
  bool composite_world_alpha = false;
  bool hud = false;
  const char *hud_element_text = "speed";
  const char *hud_mask_path = nullptr;
  const char *hud_texture_dump_dir = nullptr;
  const char *hud_font_atlas_path = nullptr;
  const char *hud_vram_mode = "stream";
  const char *hud_vram_file = nullptr;
  const char *hud_dump_page = nullptr;
  bool hud_vram_all_frames = false;
  RenderOptions options;
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--list") list = true;
    else if (argument == "--counts") counts = true;
    else if (argument == "--ndc") ndc = true;
    else if (argument == "--no-programs") no_programs = true;
    else if (argument == "--no-morph") no_morph = true;
    else if (argument == "--vram" && index + 1 < argc) vram_path = argv[++index];
    else if (argument == "--vram-vblank" && index + 1 < argc) vram_vblank = std::atoi(argv[++index]);
    else if (argument == "--tap" && index + 1 < argc) tap_path = argv[++index];
    else if (argument == "--texture-mask" && index + 1 < argc) texture_mask_path = argv[++index];
    else if (argument == "--dump-textures" && index + 1 < argc) dump_textures_dir = argv[++index];
    else if (argument == "--reflection-mask" && index + 1 < argc) reflection_mask_path = argv[++index];
    else if (argument == "--mask" && index + 1 < argc) coverage_mask_path = argv[++index];
    else if (argument == "--no-textured") no_textured = true;
    else if (argument == "--texture-body") texture_body = true;
    else if (argument == "--no-reflection") no_reflection = true;
    else if (argument == "--world") world = true;
    else if (argument == "--no-sky") world_keep_sky = false;
    else if (argument == "--no-underlay") world_keep_underlay = false;
    else if (argument == "--no-course") world_keep_course = false;
    else if (argument == "--no-wet-reflection") world_keep_reflection = false;
    else if (argument == "--world-no-alpha") options.world_ignore_alpha_test = true;
    else if (argument == "--gs" && index + 1 < argc) gs_path = argv[++index];
    else if (argument == "--no-wet-composite") wet_composite = false;
    else if (argument == "--post-chain") post_chain = true;  // compatibility: the chain is the default now
    else if (argument == "--no-post-chain") post_chain = false;
    else if (argument == "--chain-dump" && index + 1 < argc) chain_dump_dir = argv[++index];
    else if (argument == "--composite-world-alpha") composite_world_alpha = true;
    else if (argument == "--composite-mask" && index + 1 < argc) composite_mask_path = argv[++index];
    else if (argument == "--composite-changed" && index + 1 < argc) composite_changed_path = argv[++index];
    else if (argument == "--hud") hud = true;
    else if (argument == "--no-hud") hud = false;
    else if (argument == "--hud-element" && index + 1 < argc) hud_element_text = argv[++index];
    else if (argument == "--hud-mask" && index + 1 < argc) hud_mask_path = argv[++index];
    else if (argument == "--hud-font-atlas" && index + 1 < argc) hud_font_atlas_path = argv[++index];
    else if (argument == "--hud-texture-dump" && index + 1 < argc) hud_texture_dump_dir = argv[++index];
    else if (argument == "--hud-vram" && index + 1 < argc) hud_vram_mode = argv[++index];
    else if (argument == "--hud-vram-file" && index + 1 < argc) hud_vram_file = argv[++index];
    else if (argument == "--hud-dump-page" && index + 1 < argc) hud_dump_page = argv[++index];
    else if (argument == "--hud-vram-all-frames") hud_vram_all_frames = true;
    else if (argument == "--frame" && index + 1 < argc) frame_index = std::atoi(argv[++index]);
    else if (argument == "--out" && index + 1 < argc) out = argv[++index];
    else if (argument == "--report" && index + 1 < argc) report = argv[++index];
    else if (argument == "--shade-dump" && index + 1 < argc) shade_dump = argv[++index];
    else if (argument == "--width" && index + 1 < argc) options.width = static_cast<std::uint32_t>(std::atoi(argv[++index]));
    else if (argument == "--height" && index + 1 < argc) options.height = static_cast<std::uint32_t>(std::atoi(argv[++index]));
    else if (argument == "--position-scale" && index + 1 < argc) options.position_scale = static_cast<float>(std::atof(argv[++index]));
    else if (argument == "--no-depth") options.depth_test = false;
    else if (argument == "--flat") options.flat_shading = true;
    else if (argument == "--object") options.object_space = true;
    else if (argument == "--points") options.points = true;
    else if (argument == "--point-size" && index + 1 < argc) options.point_size = static_cast<std::uint32_t>(std::atoi(argv[++index]));
    else if (argument == "--topology" && index + 1 < argc) {
      const std::string value = argv[++index];
      options.topology = value == "list" ? Topology::list : value == "quad" ? Topology::quad : Topology::strip;
    }
    else if (argument == "--draw" && index + 1 < argc) only_draw = std::atoi(argv[++index]);
    else if (argument == "--min-y" && index + 1 < argc) min_y = static_cast<float>(std::atof(argv[++index]));
    else if (argument == "--first-car" && index + 1 < argc) draw_range_last = std::atoi(argv[++index]) - 1;
    else if (argument == "--max-edge" && index + 1 < argc) max_edge = static_cast<float>(std::atof(argv[++index]));
    else if (argument == "--yaw" && index + 1 < argc) options.object_yaw = static_cast<float>(std::atof(argv[++index]));
    else if (argument == "--pitch" && index + 1 < argc) options.object_pitch = static_cast<float>(std::atof(argv[++index]));
    else if (argument == "--no-flip-y") options.flip_y = false;
    else {
      std::fprintf(stderr, "gt4_coregs3_render: unknown argument %s\n", argument.c_str());
      return 2;
    }
  }

  if (counts) {
    for (const FrameSnapshot &frame : frames) {
      std::printf("frame %u vblank %llu\n", frame.index, static_cast<unsigned long long>(frame.vblank));
      MeshOptions mesh_options;
      for (const CapturedDraw &draw : frame.draws) {
        Mesh mesh;
        std::string ignored;
        const bool ok = build_mesh(frame.packets.data(), frame.packets.size(), draw, mesh_options, mesh, ignored);
        std::uint32_t uploaded = 0, carried = 0, uncertain = 0, none = 0, runs = 0;
        for (std::uint32_t index = 0; index < draw.item_count; ++index) {
          const CapturedItem &item = draw.items[index];
          if (item.light_source == kLightItemUploaded) uploaded += 1;
          else if (item.light_source == kLightItemCarried) carried += 1;
          else if (item.light_source == kLightItemCarriedUncertain) uncertain += 1;
          else none += 1;
          if (item.run_valid != 0) runs += 1;
        }
        std::printf("  draw %u items %u value %08X %s %zu chain %u/%u uniform %u light uploaded %u carried %u "
                    "uncertain %u none %u runs %u\n",
                    draw.sequence, draw.item_count, draw.cmd_value, ok ? "vertices" : "refused", mesh.vertices.size(),
                    draw.chain_tags, draw.chain_valid, draw.program_uniform, uploaded, carried, uncertain, none, runs);
      }
    }
    return 0;
  }

  options.max_edge = max_edge;
  if (ndc) {
    for (const FrameSnapshot &frame : frames) {
      std::printf("frame %u vblank %llu\n", frame.index, static_cast<unsigned long long>(frame.vblank));
      print_ndc_stats(frame);
    }
    return 0;
  }

  if (list) {
    std::printf("capture %s: version %u target %u frames %zu\n", argv[1], header.version, header.target,
                frames.size());
    for (const FrameSnapshot &frame : frames) {
      std::printf("  frame %u vblank %llu draws %u textures %u materials %u packets %zu environment %u journal %zu\n",
                  frame.index, static_cast<unsigned long long>(frame.vblank), frame.draw_count, frame.texture_count,
                  frame.material_count, frame.packets.size(), frame.environment.valid,
                  frame.pass_states.size());
      // Format version 5: the pass table and the per-pass GS state journal of this frame.
      std::uint32_t counts[kPassCount] = {};
      for (const CapturedDraw &draw : frame.draws)
        if (draw.pass < kPassCount) counts[draw.pass] += 1;
      for (std::uint32_t pass = 0; pass < kPassCount; ++pass)
        if (counts[pass] != 0)
          std::printf("    pass %-12s draws %u\n", pass_name(pass), counts[pass]);
      // Format version 6: the 0x00A0 morph factor each draw's chain carried (the value the VU1 pre-pass read).
      std::uint32_t morph_draws = 0, morph_uploads = 0, morph_printed = 0;
      for (const CapturedDraw &draw : frame.draws) {
        if (draw.morph_uploads == 0) continue;
        morph_draws += 1;
        morph_uploads += draw.morph_uploads;
        if (morph_printed < 16) {
          float factor = 0.0f;
          std::memcpy(&factor, &draw.morph_factor, sizeof factor);
          std::printf("    morph draw %u factor 0x%08X (%.6f) pgl 0x%08X flags 0x%X uploads %u\n", draw.sequence,
                      draw.morph_factor, static_cast<double>(factor), draw.morph_factor_pgl,
                      draw.morph_factor_valid, draw.morph_uploads);
          morph_printed += 1;
        }
      }
      if (morph_draws != 0)
        std::printf("    morph draws %u uploads %u\n", morph_draws, morph_uploads);
      for (const CapturedPassState &state : frame.pass_states)
        std::printf("    journal %-12s vblank %llu draws %u items %u first %d | FOGCOL %016llX FRAME %016llX "
                    "XYOFFSET %016llX ZBUF %016llX SCISSOR %016llX ALPHA %016llX TEST %016llX\n",
                    pass_name(state.pass), static_cast<unsigned long long>(state.vblank), state.draw_count,
                    state.item_count, state.first_draw == kNoRecord ? -1 : static_cast<int>(state.first_draw),
                    static_cast<unsigned long long>(state.fogcol),
                    static_cast<unsigned long long>(state.frame),
                    static_cast<unsigned long long>(state.xyoffset),
                    static_cast<unsigned long long>(state.zbuf),
                    static_cast<unsigned long long>(state.scissor),
                    static_cast<unsigned long long>(state.alpha),
                    static_cast<unsigned long long>(state.test));
      if (frame.environment.valid != 0)
        std::printf("    environment sun/fog/lights %08X %08X %08X %08X ... reflection %08X ... lights %08X\n",
                    frame.environment.words[0], frame.environment.words[1], frame.environment.words[2],
                    frame.environment.words[3], frame.environment.words[84], frame.environment.words[98]);
    }
    if (!frames.empty()) print_channel_scan(frames.back());
    return 0;
  }

  if (no_programs && frame_index >= 0 && static_cast<std::size_t>(frame_index) < frames.size()) {
    // Evidence A/B only: invalidate every item's run pair so the renderer keeps the base colours. The difference from
    // the normal render is exactly what the per-item programs add to the picture.
    for (CapturedDraw &draw : frames[frame_index].draws)
      for (std::uint32_t item = 0; item < draw.item_count; ++item) draw.items[item].run_valid = 0;
  }
  if (no_morph && frame_index >= 0 && static_cast<std::size_t>(frame_index) < frames.size()) {
    // Evidence A/B only (format version 6): drop the captured 0x00A0 factor, so the morph items fall back to the
    // first-key substitution the world-families round used. The difference from the normal render is exactly the blend.
    for (CapturedDraw &draw : frames[frame_index].draws) {
      draw.morph_factor = 0;
      draw.morph_factor_pgl = 0;
      draw.morph_factor_valid = 0;
      draw.morph_uploads = 0;
    }
  }

  if (shade_dump != nullptr) {
    // Evidence mode: shade one frame's meshes with the per-item programs and write every vertex's predicted RGBA and
    // outcome. The validation scripts pair these lines with the tapped GIF records of the same capture.
    if (frame_index < 0 || static_cast<std::size_t>(frame_index) >= frames.size()) {
      std::fprintf(stderr, "gt4_coregs3_render: --shade-dump needs a valid --frame\n");
      return 2;
    }
    std::FILE *file = std::fopen(shade_dump, "w");
    if (file == nullptr) {
      std::fprintf(stderr, "gt4_coregs3_render: cannot write %s\n", shade_dump);
      return 1;
    }
    const FrameSnapshot &frame = frames[frame_index];
    MeshOptions mesh_options;
    mesh_options.position_scale = options.position_scale;
    mesh_options.topology = options.topology;
    mesh_options.max_edge = options.max_edge;
    // A --world shade dump decodes the same descriptor-driven layout the world renderer draws, so the validation
    // script compares the per-item programs on the world meshes, not on a car-path misread.
    mesh_options.world_channels = world;
    LightCounters total;
    std::fprintf(file, "# draw item vertex r g b a program status\n");
    for (const CapturedDraw &draw : frame.draws) {
      Mesh mesh;
      std::string mesh_error;
      if (!build_mesh(frame.packets.data(), frame.packets.size(), draw, mesh_options, mesh, mesh_error)) {
        std::fprintf(file, "draw %u refused %s\n", draw.sequence, mesh_error.c_str());
        continue;
      }
      LightCounters counters;
      std::vector<ShadedVertex> shades;
      shade_mesh_items(draw, mesh, counters, &shades);
      total.add(counters);
      for (std::size_t vertex = 0; vertex < mesh.vertices.size(); ++vertex) {
        const ShadedVertex &shade = shades[vertex];
        std::fprintf(file, "%u %u %zu %u %u %u %u %s %s\n", draw.sequence, mesh.vertices[vertex].item, vertex,
                     mesh.vertices[vertex].color[0], mesh.vertices[vertex].color[1], mesh.vertices[vertex].color[2],
                     mesh.vertices[vertex].color[3], light_program_name(shade.program),
                     light_status_name(shade.status));
      }
    }
    std::fclose(file);
    std::printf("shade dump %s: frame %u, %llu vertices lit, %llu fallback, %llu/%llu draws lit\n", shade_dump,
                frame.index, static_cast<unsigned long long>(total.vertices_lit),
                static_cast<unsigned long long>(total.vertices_fallback),
                static_cast<unsigned long long>(total.draws_lit),
                static_cast<unsigned long long>(total.draws_lit + total.draws_fallback));
    return 0;
  }

  if (frame_index < 0 || out == nullptr) {
    std::fprintf(stderr, "gt4_coregs3_render: --frame and --out are required to render\n");
    return 2;
  }
  if (static_cast<std::size_t>(frame_index) >= frames.size()) {
    std::fprintf(stderr, "gt4_coregs3_render: frame %d is not in this capture (%zu frames)\n", frame_index,
                 frames.size());
    return 1;
  }

  FrameSnapshot selected = frames[frame_index];
  if (min_y > 0.0f) {
    // Flat parts (every vertex at y ~= 0) are the car's ground shadows and decals in this model; the body is what has
    // height. Filtering them is a presentation aid for the offline picture, not a claim about the format.
    std::vector<CapturedDraw> kept;
    MeshOptions mesh_options;
    for (const CapturedDraw &draw : selected.draws) {
      Mesh mesh;
      std::string ignored;
      if (!build_mesh(selected.packets.data(), selected.packets.size(), draw, mesh_options, mesh, ignored)) continue;
      bool has_height = false;
      for (const MeshVertex &vertex : mesh.vertices)
        if (std::fabs(vertex.position[1]) >= min_y) {
          has_height = true;
          break;
        }
      if (has_height) kept.push_back(draw);
    }
    selected.draws = std::move(kept);
    selected.draw_count = static_cast<std::uint32_t>(selected.draws.size());
  }
  if (draw_range_last >= 0) {
    // One car of a grid: the draws of one car are contiguous in submission order, and every car's vertices are in its own
    // model space, so rendering all of them at once overlays six models at the origin.
    std::vector<CapturedDraw> kept;
    for (const CapturedDraw &draw : selected.draws)
      if (draw.sequence <= static_cast<std::uint32_t>(draw_range_last)) kept.push_back(draw);
    selected.draws = std::move(kept);
    selected.draw_count = static_cast<std::uint32_t>(selected.draws.size());
  }
  if (only_draw >= 0) {
    std::vector<CapturedDraw> kept;
    for (const CapturedDraw &draw : selected.draws)
      if (draw.sequence == static_cast<std::uint32_t>(only_draw)) kept.push_back(draw);
    if (kept.empty()) {
      std::fprintf(stderr, "gt4_coregs3_render: draw %d is not in frame %d\n", only_draw, frame_index);
      return 1;
    }
    selected.draws = std::move(kept);
    selected.draw_count = static_cast<std::uint32_t>(selected.draws.size());
  }
  bool textured = false;  // the texture table was built and attached
  if (tap_path != nullptr && vram_path == nullptr)
    std::fprintf(stderr, "gt4_coregs3_render: --tap has no effect without --vram\n");
#ifdef GT4_COREGS3_TEXTURES
  // The texture milestone: join the capture with its VRAM sidecar and (optionally) the VU1 tap into the per-item
  // texture table. The sidecar record whose `vblank` is V covers the capture frame with `frame.vblank == V - 1`.
  FrameTextureTable texture_table;
  TextureFrameCounters texture_counters;
  std::string world_report;  // the TASK-090 track M1 selection/exclusion/fallback lines, printed and written to --report
  // TASK-090 S2: the captured wet-road composite chain, its sphere-mapper source page and the applied counters.
  GsPostChainInfo chain_info;              // TASK-090 post chain: every full-screen batch of the attributed stream
  std::vector<GsDecodeBatch> wet_batches;  // keeps the batches `chain_info` points into alive through the render
  std::vector<std::uint8_t> wet_source;    // the CT32 page, 128 x 128 RGBA8
  std::uint32_t wet_source_label = 0;
  bool chain_ready = false;
  std::uint64_t vblank_record_count = 0;  // the sidecar's record census, for the world report
  std::uint64_t pass_record_count = 0;
  std::uint64_t draw_record_count = 0;
  std::uint64_t job_record_count = 0;
  if (vram_path != nullptr && !no_textured) {
    std::vector<VramRecord> vram_records;
    if (!read_vram_sidecar(vram_path, vram_records, error)) {
      std::fprintf(stderr, "gt4_coregs3_render: %s\n", error.c_str());
      return 1;
    }
    // TASK-090 pass textures: a version 2 sidecar carries vblank records (kind 0, the version 1 semantic: record.vblank
    // is the VSYNC that ended the capture frame, i.e. frame.vblank + 1) and pass-begin records (kind 1: record.vblank is
    // the capture frame's own VBlank and record.pass its PassId). Version 1 files hold vblank records only and keep the
    // old selection exactly.
    for (const VramRecord &candidate : vram_records) {
      if (candidate.kind == vram::kRecordPassBegin) ++pass_record_count;
      else if (candidate.kind == vram::kRecordJobTexture) ++job_record_count;
      else if (candidate.kind == vram::kRecordDrawTexture) ++draw_record_count;
      else ++vblank_record_count;
    }
    const VramRecord *vram_record = nullptr;
    if (vram_vblank >= 0) {
      for (const VramRecord &candidate : vram_records)
        if (candidate.kind == vram::kRecordVblank && candidate.vblank == static_cast<std::uint64_t>(vram_vblank)) {
          vram_record = &candidate;
          break;
        }
      if (vram_record == nullptr) {
        std::fprintf(stderr, "gt4_coregs3_render: no vblank VRAM record at VBlank %d (the file has %llu vblank and %llu "
                             "pass-begin records)\n",
                     vram_vblank, static_cast<unsigned long long>(vblank_record_count),
                     static_cast<unsigned long long>(pass_record_count));
        return 1;
      }
    } else {
      for (const VramRecord &candidate : vram_records)
        if (candidate.kind == vram::kRecordVblank && candidate.vblank == selected.vblank + 1u) {
          vram_record = &candidate;
          break;
        }
      if (vram_record == nullptr && pass_record_count != 0) {
        // A file that holds pass-begin records only: the frame's first pass image becomes the default image, printed and
        // counted, never silent. The world path then still prefers each pass's own image.
        for (const VramRecord &candidate : vram_records)
          if (candidate.kind == vram::kRecordPassBegin && candidate.vblank == selected.vblank) {
            vram_record = &candidate;
            break;
          }
        if (vram_record != nullptr)
          std::fprintf(stderr,
                       "gt4_coregs3_render: no vblank VRAM record for frame vblank %llu; using its pass %u pass-begin "
                       "record as the default image (counted)\n",
                       static_cast<unsigned long long>(selected.vblank), vram_record->pass);
      }
      if (vram_record == nullptr) {
        std::fprintf(stderr, "gt4_coregs3_render: no VRAM record at VBlank %llu for frame vblank %llu\n",
                     static_cast<unsigned long long>(selected.vblank + 1u),
                     static_cast<unsigned long long>(selected.vblank));
        return 1;
      }
    }
    SlmImage slm;
    if (!build_slm_image(*vram_record, slm, error)) {
      std::fprintf(stderr, "gt4_coregs3_render: %s\n", error.c_str());
      return 1;
    }
    // The world path's per-pass images: one SLM image per pass-begin record of the captured frame. `std::map` keeps the
    // node addresses stable, so the PassSlmImages pointers stay valid after every insertion.
    std::map<std::uint32_t, SlmImage> pass_image_store;
    PassSlmImages pass_images;
    std::uint64_t pass_images_out_of_range = 0, pass_images_refused = 0, pass_images_duplicate = 0;
    // TASK-090 pass textures: the per-draw texture-window records of the captured frame. They take precedence over the
    // pass images: the measured race frame uploads its world textures during the course pass, after its begin marker.
    std::map<std::uint32_t, const VramRecord *> draw_records;
    DrawTextureImages draw_images;
    if (world) {
      for (const VramRecord &candidate : vram_records) {
        if (candidate.kind != vram::kRecordDrawTexture || candidate.vblank != selected.vblank) continue;
        if (!draw_records.emplace(candidate.label, &candidate).second) {
          std::fprintf(stderr, "gt4_coregs3_render: duplicate draw VRAM record for sequence %u; the first is kept\n",
                       candidate.label);
        }
      }
      draw_images.records = &draw_records;
      if (!draw_records.empty())
        std::fprintf(stderr, "gt4_coregs3_render: frame vblank %llu per-draw texture records: %zu\n",
                     static_cast<unsigned long long>(selected.vblank), draw_records.size());
    }
    // TASK-090 pass textures: the per-VU1-job records, keyed by job id (unique across the run). The tap's source records
    // map every item's packet address to its job, so an item decodes against the pixels its own job sampled.
    std::map<std::uint32_t, const VramRecord *> job_records;
    JobTextureImages job_images;
    if (world) {
      for (const VramRecord &candidate : vram_records) {
        if (candidate.kind != vram::kRecordJobTexture) continue;
        if (!job_records.emplace(candidate.label, &candidate).second) {
          std::fprintf(stderr, "gt4_coregs3_render: duplicate job VRAM record for job %u; the first is kept\n",
                       candidate.label);
        }
      }
      job_images.records = &job_records;
      if (!job_records.empty())
        std::fprintf(stderr, "gt4_coregs3_render: per-job texture records in the sidecar: %zu\n", job_records.size());
    }
    if (world) {
      for (const VramRecord &candidate : vram_records) {
        if (candidate.kind != vram::kRecordPassBegin || candidate.vblank != selected.vblank) continue;
        if (candidate.pass >= kPassCount) {
          ++pass_images_out_of_range;
          continue;
        }
        SlmImage image;
        if (!build_slm_image(candidate, image, error)) {
          ++pass_images_refused;
          std::fprintf(stderr, "gt4_coregs3_render: pass %u VRAM record refused: %s\n", candidate.pass,
                       error.c_str());
          continue;
        }
        if (!pass_image_store.emplace(candidate.pass, std::move(image)).second) ++pass_images_duplicate;
      }
      for (auto &entry : pass_image_store) pass_images.images[entry.first] = &entry.second;
      if (!pass_image_store.empty() || pass_record_count != 0) {
        std::fprintf(stderr, "gt4_coregs3_render: frame vblank %llu pass-begin VRAM records:",
                     static_cast<unsigned long long>(selected.vblank));
        for (const auto &entry : pass_image_store)
          std::fprintf(stderr, " pass %u", entry.first);
        std::fprintf(stderr, " (%llu refused, %llu out of range, %llu duplicate records)\n",
                     static_cast<unsigned long long>(pass_images_refused),
                     static_cast<unsigned long long>(pass_images_out_of_range),
                     static_cast<unsigned long long>(pass_images_duplicate));
      }
    }
    std::vector<std::uint8_t> tap_data;
    std::vector<TapFileRecord> tap_records;
    if (tap_path != nullptr && !read_tap_file(tap_path, tap_data, tap_records, error)) {
      std::fprintf(stderr, "gt4_coregs3_render: %s\n", error.c_str());
      return 1;
    }
    TextureFrameOptions texture_options;
    texture_options.use_tap = tap_path != nullptr;
    texture_options.texture_body = texture_body;
    texture_options.reflections = !no_reflection;
    texture_options.world_channels = world;
    if (world && !pass_image_store.empty()) texture_options.pass_slms = &pass_images;
    if (world && !draw_records.empty()) texture_options.draw_textures = &draw_images;
    if (world && !job_records.empty()) texture_options.job_textures = &job_images;
    if (!build_frame_texture_table(selected, slm, tap_path != nullptr ? tap_data.data() : nullptr, tap_records,
                                   texture_options, texture_table, texture_counters, error)) {
      std::fprintf(stderr, "gt4_coregs3_render: %s\n", error.c_str());
      return 1;
    }
    options.textures = &texture_table;
    options.reflections = !no_reflection;
    textured = true;
    // TASK-090 pass textures evidence: every bound sampler's decoded RGBA, one PNG per sampler, so the native decode can
    // be compared with the CoreGS 2.0 reference dump of the same state.
    if (dump_textures_dir != nullptr) {
      std::uint32_t index = 0;
      for (const SampledTexture &sampler : texture_table.textures) {
        char name[512];
        std::snprintf(name, sizeof name, "%s\\bound_%03u_tex0_%016llX_psm%02X_%ux%u.png", dump_textures_dir, index,
                      static_cast<unsigned long long>(sampler.source_tex0), static_cast<unsigned>(sampler.psm),
                      sampler.width, sampler.height);
        write_png_rgba(name, sampler.width, sampler.height, sampler.rgba);
        ++index;
      }
      std::fprintf(stderr, "gt4_coregs3_render: wrote %u bound textures to %s\n", index, dump_textures_dir);
    }
  }
#else
  if (vram_path != nullptr || world) {
    std::fprintf(stderr, "gt4_coregs3_render: this build has no texture decoder (configure with GT4_HW_GS=ON)\n");
    return 1;
  }
#endif
#ifdef GT4_COREGS3_TEXTURES
  if (world) {
    // TASK-090 track M1: the offline sky+road picture. The selected frame keeps only the sky, far-underlay and main-view
    // course draws; every exclusion is counted, and the tap is required because the item state (XYOFFSET, TEST, TEX0)
    // is part of the selection and of the measured GS state.
    if (!textured || tap_path == nullptr) {
      std::fprintf(stderr, "gt4_coregs3_render: --world needs --vram and --tap\n");
      return 1;
    }
    WorldSelection selection;
    if (!build_world_selection(selected, texture_table, world_keep_sky, world_keep_underlay, world_keep_course,
                               world_keep_reflection, selection, error)) {
      std::fprintf(stderr, "gt4_coregs3_render: %s\n", error.c_str());
      return 1;
    }
    selected = std::move(selection.frame);
    options.world_pass = true;
    options.reflections = false;
    if (gs_path != nullptr && post_chain) {
      // TASK-090 post chain: the measured phase relation. The scene frame V (selected.vblank) built the GS stream
      // stamped V + 1; the chain recogniser classifies EVERY full-screen batch of that stream and pins the five draws
      // the renderer replays (pre-reflection alpha clear, wet composite, FIX 127 and FIX 255 multiplies, CT16 present).
      std::vector<std::uint8_t> gs_data;
      GsStreamFileHeader gs_header;
      std::vector<GsStreamFileRecord> gs_records;
      std::string gs_error;
      if (!read_gs_stream_file(gs_path, gs_data, gs_header, gs_records, gs_error)) {
        std::fprintf(stderr, "gt4_coregs3_render: %s\n", gs_error.c_str());
        return 1;
      }
      GsStreamCensus gs_census;
      std::vector<GsDecodeImageUpload> gs_uploads;
      if (!census_gs_stream(gs_data, gs_header, gs_records, gs_census, wet_batches, gs_uploads, gs_error)) {
        std::fprintf(stderr, "gt4_coregs3_render: %s\n", gs_error.c_str());
        return 1;
      }
      if (!find_post_chain(wet_batches, selected.vblank, chain_info)) {
        std::fprintf(stderr, "gt4_coregs3_render: post chain refused: %s\n", chain_info.refusal.c_str());
        return 1;
      }
      chain_ready = true;
      if (wet_composite) {
        std::vector<VramRecord> wet_records;
        if (!read_vram_sidecar(vram_path, wet_records, gs_error)) {
          std::fprintf(stderr, "gt4_coregs3_render: %s\n", gs_error.c_str());
          return 1;
        }
        // The sphere-mapper source page: the unique VU1-job snapshot at the stream's vblank (worker counter V + 1)
        // whose window table covers blocks 0x3140..0x323F. It is captured before the job executes, exactly the content
        // the composite draw samples (the job is posted after the composite in the same stream).
        const VramRecord *source_record = nullptr;
        std::uint32_t source_candidates = 0;
        for (const VramRecord &record : wet_records) {
          if (record.kind != vram::kRecordJobTexture || record.vblank != selected.vblank + 1) continue;
          bool covers = false;
          for (const vram::Window &window : record.windows)
            if (window.first_block <= 0x3140u && 0x3140u + 256u <= window.first_block + window.block_count) covers = true;
          if (!covers) continue;
          source_candidates += 1;
          source_record = &record;
        }
        if (source_candidates != 1) {
          std::fprintf(stderr,
                       "gt4_coregs3_render: wet composite refused: expected one sphere-mapper job record covering "
                       "blocks 0x3140..0x323F at vblank %llu, found %u\n",
                       static_cast<unsigned long long>(selected.vblank + 1), source_candidates);
          return 1;
        }
        std::size_t offset = 0;
        bool copied = false;
        for (const vram::Window &window : source_record->windows) {
          const std::size_t bytes = static_cast<std::size_t>(window.block_count) * vram::kBlockBytes;
          if (window.first_block <= 0x3140u && 0x3140u + 256u <= window.first_block + window.block_count) {
            const std::size_t at = offset + static_cast<std::size_t>(0x3140u - window.first_block) * vram::kBlockBytes;
            if (at + 65536u > source_record->bytes.size()) break;
            wet_source.assign(source_record->bytes.begin() + static_cast<std::ptrdiff_t>(at),
                              source_record->bytes.begin() + static_cast<std::ptrdiff_t>(at + 65536u));
            copied = true;
            break;
          }
          offset += bytes;
        }
        if (!copied) {
          std::fprintf(stderr, "gt4_coregs3_render: wet composite refused: the sphere-mapper page is truncated\n");
          return 1;
        }
        wet_source_label = source_record->label;
        options.clear_alpha_before_wet_reflection = !composite_world_alpha;
      }
      char line[640];
      std::snprintf(line, sizeof line,
                    "post chain scene %llu (stream %llu): %llu full-screen batches, unrecognised %llu; alpha_clear seq "
                    "%u, composite seq %u, FIX 127 seq %u, FIX 255 seq %u, present seq %u; source job %u (vblank %llu, "
                    "blocks 0x3140+256)%s",
                    static_cast<unsigned long long>(chain_info.scene_vblank),
                    static_cast<unsigned long long>(chain_info.stream_vblank),
                    static_cast<unsigned long long>(chain_info.full_screen_batches),
                    static_cast<unsigned long long>(chain_info.unrecognised_full_screen),
                    chain_info.alpha_clear->sequence, chain_info.composite->sequence,
                    chain_info.post_darken->sequence, chain_info.post_brighten->sequence,
                    chain_info.present->sequence, wet_source_label,
                    static_cast<unsigned long long>(selected.vblank + 1),
                    composite_world_alpha ? "; DIAGNOSTIC world alpha (the captured clear is skipped)" : "");
      world_report += line;
      world_report += "\n";
      for (const GsChainEntry &entry : chain_info.entries) {
        std::snprintf(line, sizeof line, "  chain seq %u %-22s %s: %s", entry.batch->sequence,
                      gs_chain_op_name(entry.op), entry.after_world ? "after-world" : "before-world",
                      entry.note.c_str());
        world_report += line;
        world_report += "\n";
      }
    } else if (gs_path != nullptr) {
      world_report += "post chain disabled (--no-post-chain A/B): the world picture is the baseline\n";
    }
    // Count the selected items' texture bindings by the same static check the renderer will run, so the fallback counts
    // in the report name the outcome per selected item (the renderer's own counters are per rendered draw as well).
    std::uint32_t probe_textured = 0, probe_no_texture = 0, probe_no_record = 0, probe_refused_filter = 0;
    std::uint32_t probe_refused_afail = 0, probe_refused_other = 0;
    std::string probe_filter_detail;
    std::map<std::uint64_t, std::uint32_t> probe_test_hist;
    // TASK-090 track textures: the binding table, one row per (pass, sampler state), and the selected-item census of the
    // record-vs-tap differences. The sampler state the row prints is exactly what the renderer samples.
    struct BindingRow {
      std::uint32_t pass = 0;
      const SampledTexture *texture = nullptr;
      std::uint32_t items = 0;
    };
    std::vector<BindingRow> binding_rows;
    const auto binding_row = [&](std::uint32_t pass, const SampledTexture *texture) -> BindingRow & {
      for (BindingRow &row : binding_rows)
        if (row.pass == pass && row.texture == texture) return row;
      binding_rows.push_back(BindingRow{pass, texture, 0u});
      return binding_rows.back();
    };
    std::uint32_t probe_state_bound = 0, probe_record_bound = 0;
    std::uint32_t probe_tex0_differs = 0, probe_tex0_tbp = 0, probe_tex0_format = 0, probe_tex0_palette = 0;
    std::uint32_t probe_tex1_differs = 0, probe_clamp_differs = 0, probe_state_record_absent = 0;
    std::string probe_tex0_detail;
    for (const CapturedDraw &draw : selected.draws) {
      const std::vector<TextureBinding> *bindings = texture_table.for_draw(draw.sequence);
      for (std::uint32_t item = 0; item < draw.item_count; ++item) {
        if (draw.items[item].packet_bytes == 0) continue;
        const TextureBinding *binding = bindings != nullptr && item < bindings->size() ? &(*bindings)[item] : nullptr;
        if (binding == nullptr) {
          probe_no_texture += 1;
          binding_row(draw.pass, nullptr).items += 1;
          continue;
        }
        const SampledTexture *sampler = binding->texture;
        binding_row(draw.pass, sampler).items += 1;
        if (sampler != nullptr) {
          if (sampler->from_state) {
            probe_state_bound += 1;
            const std::uint32_t record = draw.items[item].texture_record;
            if (record == kNoRecord || record >= selected.textures.size()) {
              probe_state_record_absent += 1;
            } else {
              TextureRecord record_fields;
              std::string parse_error;
              if (parse_texture_record(selected.textures[record].bytes, sizeof selected.textures[record].bytes,
                                       record_fields, parse_error)) {
                const bool tbp_differs = hwgs::tex0::Tbp0(sampler->source_tex0) != record_fields.tbp0;
                const bool format_differs =
                    hwgs::tex0::Tbw(sampler->source_tex0) != record_fields.tbw ||
                    hwgs::tex0::Psm(sampler->source_tex0) != record_fields.psm ||
                    hwgs::tex0::Tw(sampler->source_tex0) != record_fields.tw ||
                    hwgs::tex0::Th(sampler->source_tex0) != record_fields.th ||
                    hwgs::tex0::Tcc(sampler->source_tex0) != record_fields.tcc ||
                    hwgs::tex0::Tfx(sampler->source_tex0) != record_fields.tfx;
                const bool palette_differs =
                    hwgs::tex0::Cbp(sampler->source_tex0) != record_fields.cbp ||
                    hwgs::tex0::Cpsm(sampler->source_tex0) != record_fields.cpsm ||
                    hwgs::tex0::Csm(sampler->source_tex0) != record_fields.csm ||
                    hwgs::tex0::Csa(sampler->source_tex0) != record_fields.csa;
                if (tbp_differs || format_differs || palette_differs) {
                  probe_tex0_differs += 1;
                  if (tbp_differs) probe_tex0_tbp += 1;
                  if (format_differs) probe_tex0_format += 1;
                  if (palette_differs) probe_tex0_palette += 1;
                  if (probe_tex0_detail.size() < 1600) {
                    char detail[220];
                    std::snprintf(detail, sizeof detail,
                                  "draw %u item %u: record TEX0 %016llX tap TEX0 %016llX (%s%s%s); ",
                                  draw.sequence, item,
                                  static_cast<unsigned long long>(record_fields.tex0),
                                  static_cast<unsigned long long>(sampler->source_tex0),
                                  tbp_differs ? "tbp " : "", format_differs ? "format " : "",
                                  palette_differs ? "palette" : "");
                    probe_tex0_detail += detail;
                  }
                }
                if (sampler->source_tex1 != record_fields.tex1) probe_tex1_differs += 1;
                if (sampler->source_clamp != record_fields.clamp) probe_clamp_differs += 1;
              }
            }
          } else {
            probe_record_bound += 1;
          }
        }
        probe_test_hist[binding->state.test & 0xFFFFFFFFu] += 1;
        switch (check_world_binding(*binding)) {
        case BindingStatus::ok: probe_textured += 1; break;
        case BindingStatus::no_texture:
          probe_no_texture += 1;
          if (draw.items[item].texture_record == kNoRecord) probe_no_record += 1;
          break;
        case BindingStatus::refused_filter:
          probe_refused_filter += 1;
          if (probe_filter_detail.size() < 300 && binding->texture != nullptr) {
            char detail[120];
            std::snprintf(detail, sizeof detail, "draw %u item %u record %u MMAG %u MMIN %u MXL %u; ", draw.sequence,
                          item, binding->texture_record, binding->texture->mmag, binding->texture->mmin,
                          binding->texture->mxl);
            probe_filter_detail += detail;
          }
          break;
        case BindingStatus::refused_alpha_fail: probe_refused_afail += 1; break;
        default: probe_refused_other += 1; break;
        }
      }
    }
    char line[512];
    std::snprintf(line, sizeof line,
                  "world selection: sky %u draws/%u items/%u vertices; far_underlay %u/%u/%u; course %u draws/%u "
                  "items/%u vertices; wet_reflection %u draws/%u items/%u vertices%s",
                  selection.sky_draws, selection.sky_items, selection.sky_vertices, selection.underlay_draws,
                  selection.underlay_items, selection.underlay_vertices, selection.course_draws,
                  selection.course_items, selection.course_vertices, selection.reflection_draws,
                  selection.reflection_items, selection.reflection_vertices,
                  world_keep_reflection ? "" : " (pass off, A/B)");
    world_report += line;
    world_report += "\n";
    std::snprintf(line, sizeof line,
                  "world wet-reflection exclusions: other-itop %u, shape-refused %u, mirror-view %u, no-state %u, "
                  "no-position %u, empty-draws %u",
                  selection.reflection_itop_other, selection.reflection_shape_refused, selection.reflection_mirror,
                  selection.reflection_no_state, selection.reflection_no_position, selection.reflection_draws_dropped);
    world_report += line;
    world_report += "\n";
    world_report += "world course families:";
    for (int family = 0; family < 4; ++family) {
      std::snprintf(line, sizeof line, " %s %u/%u", world_family_label(family), selection.family_items[family],
                    selection.family_vertices[family]);
      world_report += line;
    }
    world_report += " (items/vertices)\n";
    std::snprintf(line, sizeof line,
                  "world 0xA0 morph: items with a captured factor %u, blended UV %u, blended colour %u, blended "
                  "position %u; first-key substitutions without a factor: UV %u, colour %u; factors:",
                  selection.course_morph_factor_items, selection.course_morph_blended_uv,
                  selection.course_morph_blended_colour, selection.course_morph_blended_position,
                  selection.course_morph_uv_substituted, selection.course_morph_colour_substituted);
    world_report += line;
    if (selection.course_morph_factors.empty()) {
      world_report += " none\n";
    } else {
      std::uint32_t printed = 0;
      for (const auto &[bits, count] : selection.course_morph_factors) {
        if (printed == 8) {
          world_report += " ...";
          break;
        }
        float factor = 0.0f;
        std::memcpy(&factor, &bits, sizeof factor);
        std::snprintf(line, sizeof line, " 0x%08X(%.6f)x%u", bits, static_cast<double>(factor), count);
        world_report += line;
        printed += 1;
      }
      world_report += "\n";
    }
    std::snprintf(line, sizeof line,
                  "world course exclusions: other-itop %u, shape-refused %u, mirror %u, no-state %u, no-position %u, "
                  "empty-draws %u; sky no-position %u; far_underlay no-position %u; selected with a neighbour/pass "
                  "default state %u",
                  selection.course_itop_other, selection.course_shape_refused, selection.course_mirror,
                  selection.course_no_state, selection.course_no_position, selection.course_draws_dropped,
                  selection.sky_no_position, selection.underlay_no_position, selection.course_uv_unverified);
    world_report += line;
    world_report += "\n";
    if (!selection.shape_refusals.empty()) {
      world_report += "world course shape refusals:";
      for (const auto &[reason, count] : selection.shape_refusals) {
        world_report += " " + reason + " " + std::to_string(count);
      }
      world_report += "\n";
    }
    std::snprintf(line, sizeof line,
                  "world selected texture bindings: textured %u, no-record %u, no-texture %u, refused-filter %u, "
                  "refused-afail %u, refused-other %u",
                  probe_textured, probe_no_record, probe_no_texture, probe_refused_filter, probe_refused_afail,
                  probe_refused_other);
    world_report += line;
    world_report += "\n";
    std::snprintf(line, sizeof line,
                  "world texture binding source: tapped-state %u, record-fallback %u; record-vs-tap TEX0 differs %u "
                  "(tbp %u, format %u, palette %u), TEX1 differs %u, CLAMP differs %u, tapped state with no record %u; "
                  "frame differing items with an ambiguous UV pairing %llu",
                  probe_state_bound, probe_record_bound, probe_tex0_differs, probe_tex0_tbp, probe_tex0_format,
                  probe_tex0_palette, probe_tex1_differs, probe_clamp_differs, probe_state_record_absent,
                  static_cast<unsigned long long>(texture_counters.world_tap_diff_ambiguous));
    world_report += line;
    world_report += "\n";
    if (!probe_tex0_detail.empty()) {
      world_report += "world record-vs-tap TEX0 differences: ";
      world_report += probe_tex0_detail;
      world_report += "\n";
    }
    std::snprintf(line, sizeof line,
                  "world tapped-state decode counters (frame world passes): decoded %llu, refused %llu, record "
                  "fallback %llu, palette via TEX2 %llu, level-0 mip fallbacks %llu, filter fallbacks %llu; texture "
                  "pairing tie-breaks %llu, ambiguous ties %llu, no stream %llu",
                  static_cast<unsigned long long>(texture_counters.world_state_decoded),
                  static_cast<unsigned long long>(texture_counters.world_state_refused),
                  static_cast<unsigned long long>(texture_counters.world_record_fallback),
                  static_cast<unsigned long long>(texture_counters.world_state_palette_tex2),
                  static_cast<unsigned long long>(texture_counters.records_level0_fallback),
                  static_cast<unsigned long long>(texture_counters.records_filter_fallback),
                  static_cast<unsigned long long>(texture_counters.world_texture_pair_tiebreak),
                  static_cast<unsigned long long>(texture_counters.world_texture_pair_ambiguous),
                  static_cast<unsigned long long>(texture_counters.world_texture_pair_none));
    world_report += line;
    world_report += "\n";
    // TASK-090 pass textures: where each world item's pixels came from. A pass image is the SLM snapshot taken at that
    // pass's K7 begin marker; the fallback is the frame-end (vblank) image the version 1 sidecar always had.
    std::snprintf(line, sizeof line,
                  "world pass SLM images: %llu supplied (scene vblank %llu, file records: vblank %llu, pass-begin %llu, "
                  "draw %llu, job %llu); items bound on their pass image %llu, items whose pass had no image (frame-end "
                  "fallback) %llu",
                  static_cast<unsigned long long>(texture_counters.world_pass_slm_images),
                  static_cast<unsigned long long>(selected.vblank),
                  static_cast<unsigned long long>(vblank_record_count),
                  static_cast<unsigned long long>(pass_record_count),
                  static_cast<unsigned long long>(draw_record_count),
                  static_cast<unsigned long long>(job_record_count),
                  static_cast<unsigned long long>(texture_counters.world_pass_slm),
                  static_cast<unsigned long long>(texture_counters.world_pass_slm_missing));
    world_report += line;
    world_report += "\n";
    std::snprintf(line, sizeof line,
                  "world per-job SLM overlays: %llu records supplied, %llu refused; items bound on their own VU1 job's "
                  "window snapshot %llu (this takes precedence over the draw record and the pass image)",
                  static_cast<unsigned long long>(texture_counters.world_job_slm_records),
                  static_cast<unsigned long long>(texture_counters.world_job_slm_refused),
                  static_cast<unsigned long long>(texture_counters.world_job_slm));
    world_report += line;
    world_report += "\n";
    std::snprintf(line, sizeof line,
                  "world per-draw SLM overlays: %llu records supplied, %llu draws applied, %llu refused; items bound on "
                  "their own draw's window snapshot %llu",
                  static_cast<unsigned long long>(texture_counters.world_draw_slm_records),
                  static_cast<unsigned long long>(texture_counters.world_draw_slm_draws),
                  static_cast<unsigned long long>(texture_counters.world_draw_slm_refused),
                  static_cast<unsigned long long>(texture_counters.world_draw_slm));
    world_report += line;
    world_report += "\n";
    for (const std::string &refusal : texture_counters.state_refusals)
      world_report += "  tapped state refused: " + refusal + "\n";
    // The binding table: the state each selected item samples, grouped by pass and sampler state.
    world_report += "world binding table:\n";
    for (const BindingRow &row : binding_rows) {
      if (row.texture == nullptr) {
        std::snprintf(line, sizeof line, "  %-12s untextured items %u\n", pass_name(row.pass), row.items);
        world_report += line;
        continue;
      }
      const SampledTexture &sampler = *row.texture;
      std::snprintf(line, sizeof line,
                    "  %-12s %-6s TEX0 %016llX TEX1 %016llX CLAMP %016llX psm %02X %ux%u%s items %u\n",
                    pass_name(row.pass), sampler.from_state ? "state" : "record",
                    static_cast<unsigned long long>(sampler.source_tex0),
                    static_cast<unsigned long long>(sampler.source_tex1),
                    static_cast<unsigned long long>(sampler.source_clamp), sampler.psm, sampler.width,
                    sampler.height, sampler.palette_via_tex2 ? " palette-TEX2" : "", row.items);
      world_report += line;
    }
    if (!probe_filter_detail.empty()) {
      world_report += "world refused-filter records: " + probe_filter_detail;
      world_report += "\n";
    }
    for (const auto &[test, count] : probe_test_hist) {
      std::snprintf(line, sizeof line, "world selected TEST %08llX: %u items (ATE %llu, DATE %llu); ",
                    static_cast<unsigned long long>(test), count,
                    static_cast<unsigned long long>(test & 1ull),
                    static_cast<unsigned long long>((test >> 14) & 1ull));
      world_report += line;
    }
    world_report += "\n";
  }
#endif
  std::vector<std::uint8_t> rgba, mask;
  std::vector<std::uint8_t> textured_mask;
  std::vector<std::uint8_t> reflection_mask;
  RenderStats stats;
  if (!render_frame(selected, options, rgba, mask, stats, error,
                    texture_mask_path != nullptr ? &textured_mask : nullptr,
                    reflection_mask_path != nullptr ? &reflection_mask : nullptr)) {
    std::fprintf(stderr, "gt4_coregs3_render: %s\n", error.c_str());
    return 1;
  }
#ifdef GT4_COREGS3_TEXTURES
  if (world && chain_ready) {
    // TASK-090 post chain: apply every captured full-screen batch in stream order over the world image.
    //   * the pre-world clears and the frame sample execute before the frame's PATH1 world draws: the world image is the
    //     result of the cleared frame plus those draws, so they are accounted for, not re-applied (the frame sample has
    //     FBMSK 0xFFFFFFFF and ZBUF.ZMSK 1 and writes nothing: it is run to count its samples and the would-be change);
    //   * the pre-reflection alpha clear is applied inside the world render at its measured position (the PassId 7
    //     draws), so it is accounted for, not re-applied over the finished image (its alpha writes are the mask the
    //     composite reads);
    //   * the composite, the two multiplies and the CT16 present are applied over the world image in sequence order.
    if (chain_dump_dir != nullptr) {
      std::error_code ec;
      std::filesystem::create_directories(chain_dump_dir, ec);
    }
    std::vector<std::uint8_t> before = rgba;
    std::vector<std::uint8_t> covered, masked;
    std::vector<std::uint8_t> chain_display;
    bool have_display = false;
    char line[1024];
    std::uint32_t stage = 0;
    const auto dump_stage = [&](const char *name, const std::vector<std::uint8_t> &image) {
      if (chain_dump_dir == nullptr) return;
      char path[1024];
      std::snprintf(path, sizeof path, "%s\\stage-%02u-%s.png", chain_dump_dir, stage, name);
      if (!write_png_rgba(path, options.width, options.height, image.data())) {
        std::fprintf(stderr, "gt4_coregs3_render: cannot write %s\n", path);
      }
    };
    for (const GsChainEntry &entry : chain_info.entries) {
      const GsDecodeBatch &batch = *entry.batch;
      WetCompositeStats stage_stats;
      switch (entry.op) {
      case GsChainOp::pre_world_clear_rgb:
      case GsChainOp::pre_world_clear_alpha:
      case GsChainOp::pre_world_clear_test:
        std::snprintf(line, sizeof line,
                      "chain stage seq %u %s: applied inside the world render (the frame starts black and alpha 0, the "
                      "cleared state the world image is drawn into); 0 pixels changed here",
                      batch.sequence, gs_chain_op_name(entry.op));
        world_report += line;
        world_report += "\n";
        break;
      case GsChainOp::frame_sample:
        if (!apply_frame_sample(batch, rgba, options.width, options.height, 640, 448, stage_stats, error)) {
          std::fprintf(stderr, "gt4_coregs3_render: frame sample refused: %s\n", error.c_str());
          return 1;
        }
        std::snprintf(line, sizeof line,
                      "chain stage seq %u frame-sample/depth-clear: sprites %llu sampled %llu colour-written %llu "
                      "changed %llu added luma %.3f (FBMSK 0xFFFFFFFF masks every colour lane; ZBUF.ZMSK 0 writes the "
                      "frame's initial depth, which the world render owns)",
                      batch.sequence, static_cast<unsigned long long>(stage_stats.sprites),
                      static_cast<unsigned long long>(stage_stats.sampled_pixels),
                      static_cast<unsigned long long>(stage_stats.written_pixels),
                      static_cast<unsigned long long>(stage_stats.changed_pixels), stage_stats.added_luma);
        world_report += line;
        world_report += "\n";
        dump_stage("frame-sample", rgba);
        break;
      case GsChainOp::reflection_alpha_clear:
        std::snprintf(line, sizeof line,
                      "chain stage seq %u reflection-alpha-clear: applied inside the world render at its stream "
                      "position (immediately before the PassId 7 draws)%s; the world image's alpha is the reflection "
                      "mask the composite reads",
                      batch.sequence, composite_world_alpha ? " (SKIPPED: the world-alpha diagnostic is on)" : "");
        world_report += line;
        world_report += "\n";
        break;
      case GsChainOp::wet_composite:
        if (!wet_composite) {
          std::snprintf(line, sizeof line,
                        "chain stage seq %u wet-composite: SKIPPED by --no-wet-composite (A/B)", batch.sequence);
          world_report += line;
          world_report += "\n";
          break;
        }
        if (!apply_wet_composite(batch, wet_source.data(), 128, 128, rgba, options.width, options.height, 640, 448,
                                 stage_stats, error, &covered, &masked)) {
          std::fprintf(stderr, "gt4_coregs3_render: wet composite refused: %s\n", error.c_str());
          return 1;
        }
        std::snprintf(line, sizeof line,
                      "chain stage seq %u wet-composite: sprites %llu pixels %llu masked (Ad != 0) %llu changed %llu "
                      "sampled %llu refused %llu added luma %.3f",
                      batch.sequence, static_cast<unsigned long long>(stage_stats.sprites),
                      static_cast<unsigned long long>(stage_stats.pixels),
                      static_cast<unsigned long long>(stage_stats.masked_pixels),
                      static_cast<unsigned long long>(stage_stats.changed_pixels),
                      static_cast<unsigned long long>(stage_stats.sampled_pixels),
                      static_cast<unsigned long long>(stage_stats.refused_pixels), stage_stats.added_luma);
        world_report += line;
        world_report += "\n";
        dump_stage("wet-composite", rgba);
        break;
      case GsChainOp::post_multiply:
        if (!apply_full_screen_multiply(batch, rgba, options.width, options.height, 640, 448, stage_stats, error)) {
          std::fprintf(stderr, "gt4_coregs3_render: post multiply refused: %s\n", error.c_str());
          return 1;
        }
        std::snprintf(line, sizeof line,
                      "chain stage seq %u post-multiply FIX %u: sprites %llu pixels %llu written %llu",
                      batch.sequence, batch.state.alpha_fix(), static_cast<unsigned long long>(stage_stats.sprites),
                      static_cast<unsigned long long>(stage_stats.pixels),
                      static_cast<unsigned long long>(stage_stats.written_pixels));
        world_report += line;
        world_report += "\n";
        {
          char name[48];
          std::snprintf(name, sizeof name, "post-multiply-%u", batch.state.alpha_fix());
          dump_stage(name, rgba);
        }
        break;
      case GsChainOp::present_ct16:
        if (!apply_present_ct16(batch, rgba, options.width, options.height, 640, 448, chain_display, stage_stats,
                                error)) {
          std::fprintf(stderr, "gt4_coregs3_render: present refused: %s\n", error.c_str());
          return 1;
        }
        have_display = true;
        std::snprintf(line, sizeof line,
                      "chain stage seq %u present-ct16: sprites %llu pixels %llu written %llu changed %llu; DTHE %llu "
                      "DIMX %016llX COLCLAMP %llu",
                      batch.sequence, static_cast<unsigned long long>(stage_stats.sprites),
                      static_cast<unsigned long long>(stage_stats.pixels),
                      static_cast<unsigned long long>(stage_stats.written_pixels),
                      static_cast<unsigned long long>(stage_stats.changed_pixels),
                      static_cast<unsigned long long>(batch.state.dthe & 1u),
                      static_cast<unsigned long long>(batch.state.dimx),
                      static_cast<unsigned long long>(batch.state.colclamp & 1u));
        world_report += line;
        world_report += "\n";
        dump_stage("present-ct16", chain_display);
        break;
      case GsChainOp::display_alpha_clear:
        std::snprintf(line, sizeof line,
                      "chain stage seq %u display-alpha-clear: alpha-only write to the CT16 display buffer after the "
                      "present (FBMSK 0x00FFFFFF); 0 RGB pixels changed, the scanout image is unaffected",
                      batch.sequence);
        world_report += line;
        world_report += "\n";
        break;
      case GsChainOp::unknown:
        std::snprintf(line, sizeof line,
                      "chain stage seq %u unknown: EXCLUDED (counted); the state is outside every measured chain "
                      "signature", batch.sequence);
        world_report += line;
        world_report += "\n";
        break;
      default:
        break;
      }
      stage += 1;
    }
    if (composite_mask_path != nullptr && !masked.empty()) {
      std::FILE *mask_file = std::fopen(composite_mask_path, "wb");
      if (mask_file == nullptr) {
        std::fprintf(stderr, "gt4_coregs3_render: cannot write %s\n", composite_mask_path);
        return 1;
      }
      std::fprintf(mask_file, "P5\n%u %u\n255\n", options.width, options.height);
      std::fwrite(masked.data(), 1, masked.size(), mask_file);
      std::fclose(mask_file);
      std::size_t count = 0;
      for (std::uint8_t value : masked) count += value != 0 ? 1u : 0u;
      std::printf("composite mask %s: %zu pixels (covered with Ad != 0)\n", composite_mask_path, count);
    }
    if (composite_changed_path != nullptr) {
      std::vector<std::uint8_t> changed(static_cast<std::size_t>(options.width) * options.height, 0);
      std::size_t count = 0;
      for (std::size_t pixel = 0; pixel < changed.size(); ++pixel) {
        if (before[pixel * 4 + 0] != rgba[pixel * 4 + 0] || before[pixel * 4 + 1] != rgba[pixel * 4 + 1] ||
            before[pixel * 4 + 2] != rgba[pixel * 4 + 2]) {
          changed[pixel] = 255;
          count += 1;
        }
      }
      std::FILE *mask_file = std::fopen(composite_changed_path, "wb");
      if (mask_file == nullptr) {
        std::fprintf(stderr, "gt4_coregs3_render: cannot write %s\n", composite_changed_path);
        return 1;
      }
      std::fprintf(mask_file, "P5\n%u %u\n255\n", options.width, options.height);
      std::fwrite(changed.data(), 1, changed.size(), mask_file);
      std::fclose(mask_file);
      std::printf("composite changed mask %s: %zu pixels (RGB changed by the chain)\n", composite_changed_path,
                  count);
    }
    if (have_display) {
      // The whole chain's output is the CT16 display buffer (the scanout image the reference dump holds). The world
      // image after the multiplies stays available through --chain-dump (the post-multiply-255 stage).
      std::snprintf(line, sizeof line, "post chain output: the CT16 display buffer (scanout image, %u x %u)", options.width,
                    options.height);
      world_report += line;
      world_report += "\n";
      rgba.swap(chain_display);
    }
  }
#endif
#ifdef GT4_COREGS3_TEXTURES
  if (hud) {
    // TASK-090 S3: the first native HUD layer over the world frame. The element is selected from the attributed GS
    // stream (scene frame V -> stream V + 1), the plate from the tap, and both are drawn with the captured state and
    // the captured textures at the native 640x448 scanout.
    if (gs_path == nullptr || tap_path == nullptr) {
      std::fprintf(stderr, "gt4_coregs3_render: --hud needs --gs and --tap\n");
      return 1;
    }
    if (options.width != 640 || options.height != 448) {
      std::fprintf(stderr, "gt4_coregs3_render: --hud renders at the native 640x448 scanout (--width 640 --height 448)\n");
      return 1;
    }
    HudElement element;
    if (!parse_hud_element(hud_element_text, element)) {
      std::fprintf(stderr, "gt4_coregs3_render: unknown --hud-element %s\n", hud_element_text);
      return 1;
    }
    std::vector<std::uint8_t> gs_data;
    GsStreamFileHeader gs_header;
    std::vector<GsStreamFileRecord> gs_records;
    if (!read_gs_stream_file(gs_path, gs_data, gs_header, gs_records, error)) {
      std::fprintf(stderr, "gt4_coregs3_render: %s\n", error.c_str());
      return 1;
    }
    GsStreamCensus hud_census;
    std::vector<GsDecodeBatch> hud_batches;
    std::vector<GsDecodeImageUpload> hud_uploads;
    if (!census_gs_stream(gs_data, gs_header, gs_records, hud_census, hud_batches, hud_uploads, error)) {
      std::fprintf(stderr, "gt4_coregs3_render: %s\n", error.c_str());
      return 1;
    }
    std::vector<std::uint8_t> tap_data;
    std::vector<TapFileRecord> tap_records;
    if (!read_tap_file(tap_path, tap_data, tap_records, error)) {
      std::fprintf(stderr, "gt4_coregs3_render: %s\n", error.c_str());
      return 1;
    }
    HudPanel panel;
    if (!build_hud_panel(hud_batches, tap_data, tap_records, selected.vblank, element, panel, error)) {
      std::fprintf(stderr, "gt4_coregs3_render: HUD panel refused: %s\n", error.c_str());
      return 1;
    }
    SlmImage hud_slm;
    std::string hud_source_line;
    std::string hud_mode(hud_vram_mode);
    const char *hud_sidecar_path = hud_vram_file != nullptr ? hud_vram_file : vram_path;
    if (hud_mode == "merged" && hud_sidecar_path == nullptr) hud_mode = "stream";  // no sidecar: the stream alone
    if (hud_mode == "stream" || hud_mode == "merged") {
      // The preferred source: the stream's own PATH3 IMAGE uploads of the panel's frame. The captured bytes are the
      // pixels the draws sample, with no cross-run assumption. `merged` seeds the image with the run's own sidecar
      // record so a page the stream does not re-upload (the font atlas) keeps its captured content.
      SlmImage sidecar_image;
      bool have_sidecar = false;
      if (hud_mode == "merged") {
        std::vector<VramRecord> hud_vram_records;
        if (!read_vram_sidecar(hud_sidecar_path, hud_vram_records, error)) {
          std::fprintf(stderr, "gt4_coregs3_render: %s\n", error.c_str());
          return 1;
        }
        const VramRecord *base_record = nullptr;
        for (const VramRecord &record : hud_vram_records) {
          if (record.kind != vram::kRecordVblank) continue;
          if (vram_vblank >= 0 && record.vblank != static_cast<std::uint64_t>(vram_vblank)) continue;
          base_record = &record;
          break;
        }
        if (base_record != nullptr) {
          if (!build_slm_image(*base_record, sidecar_image, error)) {
            std::fprintf(stderr, "gt4_coregs3_render: %s\n", error.c_str());
            return 1;
          }
          have_sidecar = true;
        }
      }
      HudUploadCounters upload_counters;
      if (!build_hud_stream_vram(hud_uploads, panel.stream_vblank, hud_vram_all_frames, hud_slm, upload_counters,
                                 error, have_sidecar ? &sidecar_image : nullptr)) {
        std::fprintf(stderr, "gt4_coregs3_render: HUD refused: %s\n", error.c_str());
        return 1;
      }
      char line[512];
      std::snprintf(line, sizeof line,
                    "HUD VRAM source: %s, stream %llu uploads, %llu applied, %llu bytes, %llu refused%s\n",
                    have_sidecar ? "sidecar frame record + stream uploads" : "stream",
                    static_cast<unsigned long long>(panel.stream_vblank),
                    static_cast<unsigned long long>(upload_counters.applied),
                    static_cast<unsigned long long>(upload_counters.bytes),
                    static_cast<unsigned long long>(upload_counters.refused),
                    hud_vram_all_frames ? " (all captured streams up to it)" : "");
      hud_source_line = line;
      for (const auto &entry : upload_counters.reasons) {
        std::snprintf(line, sizeof line, "HUD upload refusal: %s x %llu\n", entry.first.c_str(),
                      static_cast<unsigned long long>(entry.second));
        hud_source_line += line;
      }
    } else if (hud_mode == "sidecar") {
      if (hud_sidecar_path == nullptr) {
        std::fprintf(stderr, "gt4_coregs3_render: --hud-vram sidecar needs --vram or --hud-vram-file\n");
        return 1;
      }
      std::vector<VramRecord> hud_vram_records;
      if (!read_vram_sidecar(hud_sidecar_path, hud_vram_records, error)) {
        std::fprintf(stderr, "gt4_coregs3_render: %s\n", error.c_str());
        return 1;
      }
      // The frame-end (`vblank`) record whose worker counter is the stream's VBlank holds the textures the stream's
      // panel draws sample. A different record is a counted fallback (the HUD pages are static across the run).
      const VramRecord *hud_record = nullptr;
      std::uint64_t fallback_distance = 0;
      for (const VramRecord &record : hud_vram_records) {
        if (record.kind != vram::kRecordVblank) continue;
        if (vram_vblank >= 0) {
          if (record.vblank == static_cast<std::uint64_t>(vram_vblank)) {
            hud_record = &record;
            fallback_distance = 0;
          }
          continue;
        }
        if (record.vblank == panel.stream_vblank) {
          hud_record = &record;
          fallback_distance = 0;
          break;
        }
        const std::uint64_t distance = record.vblank > panel.stream_vblank ? record.vblank - panel.stream_vblank
                                                                           : panel.stream_vblank - record.vblank;
        if (hud_record == nullptr || distance < fallback_distance) {
          hud_record = &record;
          fallback_distance = distance;
        }
      }
      if (hud_record == nullptr) {
        std::fprintf(stderr, "gt4_coregs3_render: HUD refused: the VRAM sidecar has no vblank record\n");
        return 1;
      }
      if (!build_slm_image(*hud_record, hud_slm, error)) {
        std::fprintf(stderr, "gt4_coregs3_render: %s\n", error.c_str());
        return 1;
      }
      char line[512];
      std::snprintf(line, sizeof line, "HUD VRAM source: sidecar vblank record %llu (stream %llu)%s\n",
                    static_cast<unsigned long long>(hud_record->vblank),
                    static_cast<unsigned long long>(panel.stream_vblank),
                    fallback_distance == 0 ? "" : " - counted fallback: no record at the stream's VBlank");
      hud_source_line = line;
    } else {
      std::fprintf(stderr, "gt4_coregs3_render: unknown --hud-vram %s (stream|sidecar)\n", hud_vram_mode);
      return 1;
    }
    // TASK-090 HUD font: the captured stream carries the font page's palette bundle but not the glyph texels (the
    // game uploads the page before the captured window). The atlas sidecar supplies the page's 8192 bytes; the apply
    // preserves the frame's captured 16-entry CSM1 bank at the draw's CBP, so the palette stays the frame's own.
    bool font_atlas_applied = false;
    std::uint32_t applied_font_tbp0 = 0;
    if (hud_font_atlas_path != nullptr) {
      std::vector<std::uint8_t> atlas_data;
      if (!read_file(hud_font_atlas_path, atlas_data)) {
        std::fprintf(stderr, "gt4_coregs3_render: cannot read %s\n", hud_font_atlas_path);
        return 1;
      }
      HudFontAtlas font_atlas;
      if (!parse_hud_font_atlas(atlas_data, font_atlas, error)) {
        std::fprintf(stderr, "gt4_coregs3_render: font atlas refused: %s\n", error.c_str());
        return 1;
      }
      std::uint32_t palette_cbp = 0;
      bool have_palette = false;
      bool tbp_mismatch = false;
      for (const HudDraw &draw : panel.draws) {
        if (draw.state.tex0_psm() != 0x14u) continue;
        if (draw.state.tex0_tbp0() != font_atlas.tbp0) {
          tbp_mismatch = true;
          continue;
        }
        palette_cbp = draw.state.tex0_cbp();
        have_palette = true;
        break;
      }
      char line[512];
      if (tbp_mismatch && !have_palette) {
        std::snprintf(line, sizeof line,
                      "HUD font atlas: %s refused: TBP0 0x%04X does not match the panel's T4 draws\n",
                      hud_font_atlas_path, font_atlas.tbp0);
        hud_source_line += line;
      } else if (!have_palette) {
        std::snprintf(line, sizeof line, "HUD font atlas: %s not applied: the panel has no T4 draw\n",
                      hud_font_atlas_path);
        hud_source_line += line;
      } else if (!apply_hud_font_atlas(font_atlas, hud_slm, palette_cbp, error)) {
        std::fprintf(stderr, "gt4_coregs3_render: font atlas refused: %s\n", error.c_str());
        return 1;
      } else {
        std::snprintf(line, sizeof line,
                      "HUD font atlas: %s, T4 page 0x%04X (TBW %u, 128x64, CLAMP 0x%016llX), 32 blocks applied, "
                      "palette bank 0x%04X preserved (64 bytes)\n",
                      hud_font_atlas_path, font_atlas.tbp0, font_atlas.tbw,
                      static_cast<unsigned long long>(font_atlas.clamp), palette_cbp);
        hud_source_line += line;
        font_atlas_applied = true;
        applied_font_tbp0 = font_atlas.tbp0;
      }
    }
    HudTextureCache hud_textures(hud_slm);
    if (font_atlas_applied) hud_textures.set_font_atlas_page(applied_font_tbp0);
    if (hud_dump_page != nullptr) {
      // Diagnostic: decode one page of the reconstructed local memory through the measured record decoder.
      unsigned tbp = 0, w = 128, h = 64, cbp = 0, psm = 0x14, tw = 7, th = 6, csa = 0;
      if (std::sscanf(hud_dump_page, "%x:%x:%x:%u:%u:%u:%u:%u", &tbp, &psm, &cbp, &w, &h, &tw, &th, &csa) < 5) {
        std::fprintf(stderr, "gt4_coregs3_render: --hud-dump-page wants tbp:psm:cbp:width:height[:tw:th:csa]\n");
        return 1;
      }
      HudDrawState state;
      state.tex0 = tbp | (2ull << 14) | (static_cast<std::uint64_t>(psm) << 20) |
                   (static_cast<std::uint64_t>(tw) << 26) | (static_cast<std::uint64_t>(th) << 30) | (1ull << 34) |
                   (static_cast<std::uint64_t>(cbp) << 37) | (static_cast<std::uint64_t>(csa) << 56);
      state.tex1 = 0x60;
      state.clamp = 2ull | (2ull << 2) | (static_cast<std::uint64_t>(w - 1) << 14) |
                    (static_cast<std::uint64_t>(h - 1) << 34);
      std::string dump_error;
      const HudTexture &texture = hud_textures.get(state, dump_error);
      if (texture.status == HudTextureStatus::ok) {
        write_png_rgba("hud-page.png", texture.width, texture.height, texture.rgba.data());
        std::printf("hud page %s: %ux%u at (%u,%u) -> hud-page.png\n", hud_dump_page, texture.width, texture.height,
                    texture.x0, texture.y0);
      } else {
        std::printf("hud page %s refused: %s\n", hud_dump_page, dump_error.c_str());
      }
    }
    HudDrawOptions hud_options;
    HudDrawCounters hud_counters;
    std::vector<std::uint8_t> hud_mask;
    if (!draw_hud_panel(panel, hud_textures, rgba, options.width, options.height, hud_options,
                        hud_mask_path != nullptr ? &hud_mask : nullptr, hud_counters, error)) {
      std::fprintf(stderr, "gt4_coregs3_render: HUD raster refused: %s\n", error.c_str());
      return 1;
    }
    world_report += hud_source_line;
    world_report += hud_panel_report(panel, hud_counters, hud_textures);
    if (hud_mask_path != nullptr) {
      std::FILE *mask_file = std::fopen(hud_mask_path, "wb");
      if (mask_file == nullptr) {
        std::fprintf(stderr, "gt4_coregs3_render: cannot write %s\n", hud_mask_path);
        return 1;
      }
      std::fprintf(mask_file, "P5\n%u %u\n255\n", options.width, options.height);
      std::fwrite(hud_mask.data(), 1, hud_mask.size(), mask_file);
      std::fclose(mask_file);
      std::printf("hud mask %s: %zu pixels (the element's written fragments)\n", hud_mask_path,
                  static_cast<std::size_t>(hud_counters.masked_pixels));
    }
    if (hud_texture_dump_dir != nullptr) {      std::uint32_t texture_index = 0;
      std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>, std::string> written;
      for (const HudDraw &draw : panel.draws) {
        std::string texture_error;
        const HudTexture &texture = hud_textures.get(draw.state, texture_error);
        if (texture.status != HudTextureStatus::ok) continue;
        const auto key = std::make_tuple(texture.tex0, texture.tex1, texture.clamp);
        if (written.find(key) != written.end()) continue;
        char name[512];
        std::snprintf(name, sizeof name, "%s/hud-texture-%02u-tbp%04X-%ux%u.png", hud_texture_dump_dir, texture_index++,
                      static_cast<unsigned>(texture.tex0 & 0x3FFFu), texture.width, texture.height);
        write_png_rgba(name, texture.width, texture.height, texture.rgba.data());
        written.emplace(key, name);
        std::printf("hud texture %s: TEX0 %016llX TEX1 %016llX CLAMP %016llX, %ux%u at (%u,%u)\n", name,
                    static_cast<unsigned long long>(texture.tex0),
                    static_cast<unsigned long long>(texture.tex1),
                    static_cast<unsigned long long>(texture.clamp), texture.width, texture.height, texture.x0,
                    texture.y0);
      }
    }
    if (!world) std::printf("%s", world_report.c_str());
  }
#endif
  if (!write_png_rgba(out, options.width, options.height, rgba.data())) {
    std::fprintf(stderr, "gt4_coregs3_render: cannot write %s\n", out);
    return 1;
  }
  if (texture_mask_path != nullptr) {
    // The textured draws' geometric coverage, as a binary PGM the metrics script reads.
    std::FILE *mask_file = std::fopen(texture_mask_path, "wb");
    if (mask_file == nullptr) {
      std::fprintf(stderr, "gt4_coregs3_render: cannot write %s\n", texture_mask_path);
      return 1;
    }
    std::fprintf(mask_file, "P5\n%u %u\n255\n", options.width, options.height);
    std::fwrite(textured_mask.data(), 1, textured_mask.size(), mask_file);
    std::fclose(mask_file);
  }
  if (coverage_mask_path != nullptr) {
    // The whole car pass's written coverage (the renderer's own mask), for the wider comparison.
    std::FILE *mask_file = std::fopen(coverage_mask_path, "wb");
    if (mask_file == nullptr) {
      std::fprintf(stderr, "gt4_coregs3_render: cannot write %s\n", coverage_mask_path);
      return 1;
    }
    std::fprintf(mask_file, "P5\n%u %u\n255\n", options.width, options.height);
    std::fwrite(mask.data(), 1, mask.size(), mask_file);
    std::fclose(mask_file);
  }

  const FrameSnapshot &frame = selected;
  std::printf("frame %u vblank %llu: %u draws (%u drawn, %u refused), %u vertices, %u/%u triangles, %llu pixels\n",
              frame.index, static_cast<unsigned long long>(frame.vblank), stats.draws, stats.shapes_drawn,
              stats.shapes_refused, stats.vertices, stats.triangles_drawn, stats.triangles,
              static_cast<unsigned long long>(stats.covered_pixels));
  std::printf("lighting: %u draws lit, %u fallback; %u vertices lit, %u fallback\n", stats.draws_lit,
              stats.draws_fallback, stats.vertices_lit, stats.vertices_fallback);
#ifdef GT4_COREGS3_TEXTURES
  const auto print_textures = [&textured, &texture_counters, &stats](std::FILE *file) {
    if (!textured) return;
    std::fprintf(file,
                 "textures items %llu textured %llu untextured %llu (texgen %llu, reflected %llu, no-record %llu, "
                 "no-uv %llu, decode-refused %llu, unknown-program %llu)\n",
                 static_cast<unsigned long long>(texture_counters.items_visited),
                 static_cast<unsigned long long>(texture_counters.items_textured),
                 static_cast<unsigned long long>(texture_counters.items_untextured),
                 static_cast<unsigned long long>(texture_counters.items_texgen_deferred),
                 static_cast<unsigned long long>(texture_counters.items_reflected),
                 static_cast<unsigned long long>(texture_counters.items_no_texture),
                 static_cast<unsigned long long>(texture_counters.items_no_uv),
                 static_cast<unsigned long long>(texture_counters.items_decode_refused),
                 static_cast<unsigned long long>(texture_counters.items_program_unknown));
    std::fprintf(file, "textures decode calls %llu run, refused %llu, payload cache-hits %llu\n",
                 static_cast<unsigned long long>(texture_counters.records_decoded),
                 static_cast<unsigned long long>(texture_counters.records_refused),
                 static_cast<unsigned long long>(texture_counters.cache_hits));
    std::fprintf(file, "textures tap paired %llu defaulted %llu ambiguous %llu records %llu state-writes %llu\n",
                 static_cast<unsigned long long>(texture_counters.tap_items_paired),
                 static_cast<unsigned long long>(texture_counters.tap_items_defaulted),
                 static_cast<unsigned long long>(texture_counters.tap_items_ambiguous),
                 static_cast<unsigned long long>(texture_counters.tap_records),
                 static_cast<unsigned long long>(texture_counters.tap_state_writes));
    std::fprintf(file,
                 "textures raster items %u (untextured %u), pixels %llu, alpha-fail %llu, blended %llu (additive %llu), "
                 "fbmsk %llu, depth-masked %llu, sample-refused %llu\n",
                 stats.items_textured, stats.items_untextured, static_cast<unsigned long long>(stats.textured_pixels),
                 static_cast<unsigned long long>(stats.alpha_test_failed),
                 static_cast<unsigned long long>(stats.blended_pixels),
                 static_cast<unsigned long long>(stats.additive_pixels),
                 static_cast<unsigned long long>(stats.fbmsk_pixels),
                 static_cast<unsigned long long>(stats.depth_masked_pixels),
                 static_cast<unsigned long long>(stats.sample_refused_pixels));
    std::fprintf(file,
                 "textures refused-items tfx %u filter %u psm %u mip %u blend %u date %u afail %u\n",
                 stats.items_refused_tfx, stats.items_refused_filter, stats.items_refused_psm, stats.items_refused_mip,
                 stats.items_refused_blend, stats.items_refused_date, stats.items_refused_alpha_fail);
  };
  print_textures(stdout);
  if (texture_mask_path != nullptr) {
    std::size_t masked = 0;
    for (std::uint8_t value : textured_mask) masked += value != 0 ? 1u : 0u;
    std::printf("texture mask %s: %zu pixels (geometric coverage of the textured draws)\n", texture_mask_path, masked);
  }
  const auto print_reflection = [&textured, &texture_counters, &stats](std::FILE *file) {
    if (!textured) return;
    std::fprintf(file,
                 "reflection items reflected %llu, no-tapped-state %llu, refused %u (psm %u tfx %u filter %u mip %u "
                 "afail %u blend %u depth %u), st-refused %u, untextured %u\n",
                 static_cast<unsigned long long>(texture_counters.items_reflected),
                 static_cast<unsigned long long>(texture_counters.items_reflection_no_state),
                 stats.items_reflection_refused, stats.reflection_refused_psm, stats.reflection_refused_tfx,
                 stats.reflection_refused_filter, stats.reflection_refused_mip, stats.reflection_refused_alpha_fail,
                 stats.reflection_refused_blend, stats.reflection_refused_depth, stats.reflection_refused_st,
                 stats.items_reflection_untextured);
    std::fprintf(file,
                 "reflection raster pixels %llu, date-failed %llu, alpha-failed %llu, depth-failed %llu, "
                 "sample-refused %llu, alpha-missing %llu, fog-missing %llu, blended %llu, fbmsk %llu, "
                 "depth-masked %llu\n",
                 static_cast<unsigned long long>(stats.reflection_pixels),
                 static_cast<unsigned long long>(stats.reflection_date_failed),
                 static_cast<unsigned long long>(stats.reflection_alpha_failed),
                 static_cast<unsigned long long>(stats.reflection_depth_failed),
                 static_cast<unsigned long long>(stats.reflection_sample_refused),
                 static_cast<unsigned long long>(stats.reflection_alpha_missing),
                 static_cast<unsigned long long>(stats.reflection_fog_missing),
                 static_cast<unsigned long long>(stats.reflection_blended_pixels),
                 static_cast<unsigned long long>(stats.reflection_fbmsk_pixels),
                 static_cast<unsigned long long>(stats.reflection_depth_masked_pixels));
  };
  print_reflection(stdout);
  if (world) {
    char line[512];
    std::snprintf(line, sizeof line,
                  "world draws %u, matrices packet %u vu0 %u last-packet %u missing %u; vertices int16 %u int32 %u "
                  "float32 %u; untextured items %u",
                  stats.world_draws, stats.world_matrices_packet, stats.world_matrices_vu0,
                  stats.world_matrices_last_packet, stats.world_matrices_missing, stats.world_vertices_integer_16,
                  stats.world_vertices_integer_32, stats.world_vertices_float_32, stats.world_items_untextured);
    world_report += line;
    world_report += "\n";
    std::snprintf(line, sizeof line,
                  "world pixels: fog %llu missing %llu, date-failed %llu date-missing %llu, afail-written %llu "
                  "afail-depth %llu, blended %llu, sky-background-depth %llu; texture decode level-0 mip fallbacks %llu, "
                  "filter fallbacks %llu",
                  static_cast<unsigned long long>(stats.world_fog_pixels),
                  static_cast<unsigned long long>(stats.world_fog_missing),
                  static_cast<unsigned long long>(stats.world_date_failed),
                  static_cast<unsigned long long>(stats.world_date_missing),
                  static_cast<unsigned long long>(stats.world_afail_written),
                  static_cast<unsigned long long>(stats.world_afail_depth),
                  static_cast<unsigned long long>(stats.world_blended_pixels),
                  static_cast<unsigned long long>(stats.world_sky_depth_background),
                  static_cast<unsigned long long>(texture_counters.records_level0_fallback),
                  static_cast<unsigned long long>(texture_counters.records_filter_fallback));
    world_report += line;
    world_report += "\n";
    std::snprintf(line, sizeof line,
                  "world wet-reflection raster: draws %u items %u vertices %u, fragments %llu (alpha-only FBMSK "
                  "0x00FFFFFF %llu, RGB-only FBMSK 0xFF000000 %llu), depth-failed %llu, sample-refused %llu; the "
                  "captured pass's main-view items write destination alpha in this frame (the colour composite is "
                  "PATH3 and not captured)",
                  stats.world_reflection_draws, stats.world_reflection_items, stats.world_reflection_vertices,
                  static_cast<unsigned long long>(stats.world_reflection_pixels),
                  static_cast<unsigned long long>(stats.world_reflection_alpha_only),
                  static_cast<unsigned long long>(stats.world_reflection_rgb_only),
                  static_cast<unsigned long long>(stats.world_reflection_depth_failed),
                  static_cast<unsigned long long>(stats.world_reflection_sample_refused));
    world_report += line;
    world_report += "\n";
    world_report += options.world_ignore_alpha_test ? "world alpha test: ignored (diagnostic A/B)\n"
                                                    : "world alpha test: tapped ATE/ATST/AREF\n";
    std::printf("%s", world_report.c_str());
  }
  if (reflection_mask_path != nullptr) {
    // The reflection pass's written-fragment mask, a binary PGM the metrics script reads.
    if (std::FILE *mask_file = std::fopen(reflection_mask_path, "wb")) {
      std::fprintf(mask_file, "P5\n%u %u\n255\n", options.width, options.height);
      std::fwrite(reflection_mask.data(), 1, reflection_mask.size(), mask_file);
      std::fclose(mask_file);
      std::size_t masked = 0;
      for (std::uint8_t value : reflection_mask) masked += value != 0 ? 1u : 0u;
      std::printf("reflection mask %s: %zu pixels (fragments the reflection pass wrote)\n", reflection_mask_path,
                  masked);
    } else {
      std::fprintf(stderr, "gt4_coregs3_render: cannot write %s\n", reflection_mask_path);
      return 1;
    }
  }
#else
  (void)textured;
#endif
  const auto print_lighting = [&stats](std::FILE *file) {
    for (std::uint32_t program = 0; program < kLightProgramCount; ++program) {
      const LightFamilyCounts &family = stats.lighting.family[program];
      if (family.items == 0 && family.vertices_lit == 0 && family.vertices_fallback == 0) continue;
      std::fprintf(file,
                   "family %-20s items %llu lit %llu fallback %llu vertices_lit %llu vertices_fallback %llu "
                   "draws_lit %llu draws_fallback %llu\n",
                   light_program_name(static_cast<LightProgram>(program)),
                   static_cast<unsigned long long>(family.items), static_cast<unsigned long long>(family.items_lit),
                   static_cast<unsigned long long>(family.items_fallback),
                   static_cast<unsigned long long>(family.vertices_lit),
                   static_cast<unsigned long long>(family.vertices_fallback),
                   static_cast<unsigned long long>(family.draws_lit),
                   static_cast<unsigned long long>(family.draws_fallback));
    }
    for (std::uint32_t status = 0; status < kLightStatusCount; ++status)
      if (stats.lighting.status[status] != 0)
        std::fprintf(file, "status %-20s vertices %llu\n", light_status_name(static_cast<LightStatus>(status)),
                     static_cast<unsigned long long>(stats.lighting.status[status]));
    std::fprintf(file, "light_sources uploaded %llu carried %llu uncertain %llu none %llu\n",
                 static_cast<unsigned long long>(stats.lighting.items_uploaded),
                 static_cast<unsigned long long>(stats.lighting.items_carried),
                 static_cast<unsigned long long>(stats.lighting.items_uncertain),
                 static_cast<unsigned long long>(stats.lighting.items_none));
  };
  print_lighting(stdout);
  for (const std::string &refusal : stats.refusals) std::printf("  refused: %s\n", refusal.c_str());

  if (report != nullptr) {
    if (std::FILE *file = std::fopen(report, "w")) {
      std::fprintf(file, "# CoreGS 3.0 offline render\n");
      std::fprintf(file, "capture %s\nframe %u\nvblank %llu\n", argv[1], frame.index,
                   static_cast<unsigned long long>(frame.vblank));
      std::fprintf(file, "image %ux%u\n", options.width, options.height);
      std::fprintf(file, "draws %u\ndrawn %u\nrefused %u\nvertices %u\ntriangles %u\ntriangles_drawn %u\n"
                         "covered_pixels %llu\ndraws_lit %u\ndraws_fallback %u\nvertices_lit %u\n"
                         "vertices_fallback %u\n",
                   stats.draws, stats.shapes_drawn, stats.shapes_refused, stats.vertices, stats.triangles,
                   stats.triangles_drawn, static_cast<unsigned long long>(stats.covered_pixels), stats.draws_lit,
                   stats.draws_fallback, stats.vertices_lit, stats.vertices_fallback);
      print_lighting(file);
#ifdef GT4_COREGS3_TEXTURES
      print_textures(file);
      print_reflection(file);
      if (!world_report.empty()) std::fprintf(file, "%s", world_report.c_str());
#endif
      for (const std::string &refusal : stats.refusals) std::fprintf(file, "refusal %s\n", refusal.c_str());
      std::fclose(file);
    }
  }
  return 0;
}
