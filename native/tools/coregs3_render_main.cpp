// TASK-090 CoreGS 3.0: the offline capture tools (docs/COREGS3-PLAN.md §4 step 1c).
//
//   gt4_coregs3_render <capture.gt4scene> --list
//       prints the frames of a capture with their VBlank, draw/record/packet counts and the channel scan of one frame.
//       This is the instrument that turns a capture into evidence about the shape layout.
//
//   gt4_coregs3_render <capture.gt4scene> --frame N --out <file.png> [--width W --height H] [--background <png>]
//                         [--no-depth] [--position-scale S] [--no-flip-y] [--report <file.txt>]
//       renders one captured car pass natively and writes a PNG plus a short report.
//
// A capture is derived from the user's own disc and is never committed: both the input and the PNG output live under
// private/ or build/evidence/.
#include "coregs3_png.hpp"
#include "coregs3_render.hpp"
#include "coregs3_scene.hpp"
#include "coregs3_shape.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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
    std::printf("draw %u shape 0x%08X items %u value %08X\n", draw.sequence, draw.shape, draw.item_count,
                draw.cmd_value);
    std::printf("  program mscal %08X sel %u entry 0x%04X | itop %08X dispatch 0x%04X | count %u valid %u; "
                "light block %u qwords cmd %08X\n",
                draw.program_tag, draw.program_selector, draw.program_entry, draw.program_itop, draw.program_dispatch,
                draw.program_count, draw.program_valid, draw.light_block_qwords, draw.light_block_command);
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
  int only_draw = -1;
  float min_y = 0.0f;
  float max_edge = 0.0f;
  int draw_range_last = -1;
  int frame_index = -1;
  const char *out = nullptr;
  const char *report = nullptr;
  RenderOptions options;
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--list") list = true;
    else if (argument == "--counts") counts = true;
    else if (argument == "--ndc") ndc = true;
    else if (argument == "--frame" && index + 1 < argc) frame_index = std::atoi(argv[++index]);
    else if (argument == "--out" && index + 1 < argc) out = argv[++index];
    else if (argument == "--report" && index + 1 < argc) report = argv[++index];
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
        std::printf("  draw %u items %u value %08X %s %zu\n", draw.sequence, draw.item_count, draw.cmd_value,
                    ok ? "vertices" : "refused", mesh.vertices.size());
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
    for (const FrameSnapshot &frame : frames)
      std::printf("  frame %u vblank %llu draws %u textures %u materials %u packets %zu\n", frame.index,
                  static_cast<unsigned long long>(frame.vblank), frame.draw_count, frame.texture_count,
                  frame.material_count, frame.packets.size());
    if (!frames.empty()) print_channel_scan(frames.back());
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
  std::vector<std::uint8_t> rgba, mask;
  RenderStats stats;
  if (!render_frame(selected, options, rgba, mask, stats, error)) {
    std::fprintf(stderr, "gt4_coregs3_render: %s\n", error.c_str());
    return 1;
  }
  if (!write_png_rgba(out, options.width, options.height, rgba.data())) {
    std::fprintf(stderr, "gt4_coregs3_render: cannot write %s\n", out);
    return 1;
  }

  const FrameSnapshot &frame = selected;
  std::printf("frame %u vblank %llu: %u draws (%u drawn, %u refused), %u vertices, %u/%u triangles, %llu pixels\n",
              frame.index, static_cast<unsigned long long>(frame.vblank), stats.draws, stats.shapes_drawn,
              stats.shapes_refused, stats.vertices, stats.triangles_drawn, stats.triangles,
              static_cast<unsigned long long>(stats.covered_pixels));
  std::printf("lighting: %u draws lit, %u fallback; %u vertices lit, %u fallback\n", stats.draws_lit,
              stats.draws_fallback, stats.vertices_lit, stats.vertices_fallback);
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
      for (const std::string &refusal : stats.refusals) std::fprintf(file, "refusal %s\n", refusal.c_str());
      std::fclose(file);
    }
  }
  return 0;
}
