// TASK-090 CoreGS 3.0: the VU1 tap tools (plan step 1d).
//
//   gt4_coregs3_tap stats <tap.gt4tap>
//       one line per record: job, VBlank, bytes, GIF tags, vertices, ADC count and the screen bounding box.
//
//   gt4_coregs3_tap fit <tap.gt4tap> <capture.gt4scene> <record> --draw <seq> [--affine]
//       takes one tap record and the matching captured draw, and fits the 3D-to-screen transform the VU1 applied: first
//       an affine fit (w = 1), then a projective one (3x4, 11 degrees of freedom) unless --affine is given. It prints the
//       residual in screen pixels and, for the projective fit, the matrix, so the convention can be identified against
//       the captured 0xC1 block.
//
// A capture and its tap come from the same run; records and draws are both in submission order, so record k of a frame
// belongs to draw k of that frame until proven otherwise (the tool reports the vertex counts it had to match).
#include "coregs3_gif.hpp"
#include "coregs3_render.hpp"
#include "coregs3_scene.hpp"
#include "coregs3_shape.hpp"
#include "coregs3_tap.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace gt4::coregs3;

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

struct TapRecord {
  std::uint64_t job = 0;
  std::uint64_t vblank = 0;
  std::size_t offset = 0;
  std::uint32_t bytes = 0;
};

bool load_tap(const char *path, std::vector<std::uint8_t> &data, std::vector<TapRecord> &records,
              std::string &error) {
  if (!read_file(path, data)) {
    error = "cannot open " + std::string(path);
    return false;
  }
  if (data.size() < 16) {
    error = "tap file is shorter than its header";
    return false;
  }
  auto u32 = [&](std::size_t at) {
    return static_cast<std::uint32_t>(data[at]) | (static_cast<std::uint32_t>(data[at + 1]) << 8) |
           (static_cast<std::uint32_t>(data[at + 2]) << 16) | (static_cast<std::uint32_t>(data[at + 3]) << 24);
  };
  if (u32(0) != kTapMagic) {
    error = "not a .gt4tap file (bad magic)";
    return false;
  }
  if (u32(4) != kTapVersion) {
    error = "unsupported tap version";
    return false;
  }
  std::size_t at = 16;
  while (at + 24 <= data.size()) {
    const std::uint32_t bytes = u32(at);
    TapRecord record;
    record.bytes = bytes;
    record.offset = at + 24;
    for (int index = 0; index < 8; ++index) record.job |= static_cast<std::uint64_t>(data[at + 8 + index]) << (8 * index);
    for (int index = 0; index < 8; ++index)
      record.vblank |= static_cast<std::uint64_t>(data[at + 16 + index]) << (8 * index);
    if (bytes > data.size() - record.offset) {
      error = "record runs past the end of the tap";
      return false;
    }
    records.push_back(record);
    at = record.offset + bytes;
  }
  return true;
}

void command_stats(const char *path) {
  std::vector<std::uint8_t> data;
  std::vector<TapRecord> records;
  std::string error;
  if (!load_tap(path, data, records, error)) {
    std::fprintf(stderr, "gt4_coregs3_tap: %s\n", error.c_str());
    std::exit(1);
  }
  std::printf("tap %s: %zu records\n", path, records.size());
  std::uint64_t vertices_total = 0, adc_total = 0, drawn_total = 0;
  for (std::size_t index = 0; index < records.size(); ++index) {
    const TapRecord &record = records[index];
    const GifPacket packet = parse_gif(data.data() + record.offset, record.bytes);
    if (!packet.ok) {
      std::printf("  %3zu job %llu vblank %llu bytes %u: %s\n", index,
                  static_cast<unsigned long long>(record.job), static_cast<unsigned long long>(record.vblank),
                  record.bytes, packet.error.c_str());
      continue;
    }
    unsigned low_x = 0xFFFF, low_y = 0xFFFF, high_x = 0, high_y = 0;
    std::uint32_t adc = 0, drawn = 0;
    for (const GifVertex &vertex : packet.vertices) {
      if (vertex.adc) {
        ++adc;
        continue;
      }
      ++drawn;
      low_x = std::min<unsigned>(low_x, vertex.x);
      low_y = std::min<unsigned>(low_y, vertex.y);
      high_x = std::max<unsigned>(high_x, vertex.x);
      high_y = std::max<unsigned>(high_y, vertex.y);
    }
    vertices_total += packet.vertices.size();
    adc_total += adc;
    drawn_total += drawn;
    std::printf("  %3zu job %llu vblank %llu bytes %u tags %zu vertices %zu drawn %u adc %u prim 0x%X box %u..%u x "
                "%u..%u\n",
                index, static_cast<unsigned long long>(record.job), static_cast<unsigned long long>(record.vblank),
                record.bytes, packet.tags.size(), packet.vertices.size(), drawn, adc,
                packet.tags.empty() ? 0u : packet.tags[0].prim, low_x, high_x, low_y, high_y);
  }
  // Group by job: one VIF job can emit several XGKICKs (one GIF packet each), so a draw may span records.
  std::vector<std::pair<std::uint64_t, std::uint32_t>> jobs;  // job, vertices
  for (const TapRecord &record : records) {
    const GifPacket packet = parse_gif(data.data() + record.offset, record.bytes);
    if (!packet.ok) continue;
    const auto count = static_cast<std::uint32_t>(packet.vertices.size());
    if (!jobs.empty() && jobs.back().first == record.job) jobs.back().second += count;
    else jobs.push_back({record.job, count});
  }
  std::printf("jobs %zu:", jobs.size());
  for (const auto &job : jobs) std::printf(" %llu:%u", static_cast<unsigned long long>(job.first), job.second);
  std::printf("\n");
  std::printf("vertices %llu, drawn %llu, adc %llu\n", static_cast<unsigned long long>(vertices_total),
              static_cast<unsigned long long>(drawn_total), static_cast<unsigned long long>(adc_total));
}

// Solves the normal equations of an over-determined linear system by Gaussian elimination with partial pivoting.
bool solve(std::vector<double> &matrix, std::vector<double> &rhs, std::vector<double> &solution) {
  const std::size_t n = rhs.size();
  for (std::size_t column = 0; column < n; ++column) {
    std::size_t pivot = column;
    for (std::size_t row = column + 1; row < n; ++row)
      if (std::fabs(matrix[row * n + column]) > std::fabs(matrix[pivot * n + column])) pivot = row;
    if (std::fabs(matrix[pivot * n + column]) < 1e-12) return false;
    if (pivot != column) {
      for (std::size_t k = 0; k < n; ++k) std::swap(matrix[column * n + k], matrix[pivot * n + k]);
      std::swap(rhs[column], rhs[pivot]);
    }
    for (std::size_t row = column + 1; row < n; ++row) {
      const double factor = matrix[row * n + column] / matrix[column * n + column];
      if (factor == 0.0) continue;
      for (std::size_t k = column; k < n; ++k) matrix[row * n + k] -= factor * matrix[column * n + k];
      rhs[row] -= factor * rhs[column];
    }
  }
  solution.assign(n, 0.0);
  for (std::size_t row = n; row-- > 0;) {
    double sum = rhs[row];
    for (std::size_t k = row + 1; k < n; ++k) sum -= matrix[row * n + k] * solution[k];
    solution[row] = sum / matrix[row * n + row];
  }
  return true;
}

int command_fit(const char *tap_path, const char *capture_path, std::size_t record_index, std::uint32_t draw_sequence,
                bool affine_only) {
  std::vector<std::uint8_t> tap;
  std::vector<TapRecord> records;
  std::string error;
  if (!load_tap(tap_path, tap, records, error)) {
    std::fprintf(stderr, "gt4_coregs3_tap: %s\n", error.c_str());
    return 1;
  }
  if (record_index >= records.size()) {
    std::fprintf(stderr, "gt4_coregs3_tap: record %zu is not in the tap (%zu records)\n", record_index,
                 records.size());
    return 1;
  }
  std::vector<std::uint8_t> capture_data;
  if (!read_file(capture_path, capture_data)) {
    std::fprintf(stderr, "gt4_coregs3_tap: cannot open %s\n", capture_path);
    return 1;
  }
  SceneHeader header;
  std::size_t at = 0;
  if (!parse_scene_header(capture_data.data(), capture_data.size(), header, error)) {
    std::fprintf(stderr, "gt4_coregs3_tap: %s\n", error.c_str());
    return 1;
  }
  at = 16;
  FrameSnapshot frame;
  bool found = false;
  while (at < capture_data.size()) {
    FrameSnapshot candidate;
    const std::size_t consumed =
        parse_frame_block(capture_data.data() + at, capture_data.size() - at, header.version, candidate, error);
    if (consumed == 0) break;
    for (const CapturedDraw &draw : candidate.draws)
      if (draw.sequence == draw_sequence) {
        frame = candidate;
        found = true;
        break;
      }
    if (found) break;
    at += consumed;
  }
  if (!found) {
    std::fprintf(stderr, "gt4_coregs3_tap: draw %u is not in the capture\n", draw_sequence);
    return 1;
  }
  const CapturedDraw *draw = nullptr;
  for (const CapturedDraw &candidate : frame.draws)
    if (candidate.sequence == draw_sequence) draw = &candidate;
  if (draw == nullptr) return 1;

  MeshOptions mesh_options;
  Mesh mesh;
  if (!build_mesh(frame.packets.data(), frame.packets.size(), *draw, mesh_options, mesh, error)) {
    std::fprintf(stderr, "gt4_coregs3_tap: the draw does not decode: %s\n", error.c_str());
    return 1;
  }
  const TapRecord &record = records[record_index];
  // One VIF job can emit several GIF packets: gather every record of the same job, in order.
  std::vector<GifVertex> packet_vertices;
  for (std::size_t index = record_index; index < records.size() && records[index].job == record.job; ++index) {
    const GifPacket packet = parse_gif(tap.data() + records[index].offset, records[index].bytes);
    if (!packet.ok) {
      std::fprintf(stderr, "gt4_coregs3_tap: record %zu does not parse: %s\n", index, packet.error.c_str());
      return 1;
    }
    packet_vertices.insert(packet_vertices.end(), packet.vertices.begin(), packet.vertices.end());
  }
  // Every emitted vertex carries a position; ADC only says the GS must not draw it, so the fit uses them all.
  std::vector<GifVertex> drawn;
  for (const GifVertex &vertex : packet_vertices) drawn.push_back(vertex);
  std::printf("draw %u: %zu vertices; record %zu (job %llu, vblank %llu): %zu vertices, %zu drawn\n",
              draw_sequence, mesh.vertices.size(), record_index, static_cast<unsigned long long>(record.job),
              static_cast<unsigned long long>(record.vblank), packet_vertices.size(), drawn.size());
  if (drawn.size() != mesh.vertices.size()) {
    std::printf("  vertex counts differ; the fit uses %zu pairs\n", std::min(drawn.size(), mesh.vertices.size()));
  }
  const std::size_t pairs = std::min(drawn.size(), mesh.vertices.size());
  if (pairs < 6) {
    std::fprintf(stderr, "gt4_coregs3_tap: too few pairs to fit\n");
    return 1;
  }
  for (std::size_t index = 0; index < 3 && index < pairs; ++index) {
    const GifVertex &vertex = drawn[index];
    std::printf("  pair %zu: object (%.4f %.4f %.4f) screen (%d %d) adc %d\n", index,
                mesh.vertices[index].position[0], mesh.vertices[index].position[1],
                mesh.vertices[index].position[2], static_cast<std::int16_t>(vertex.x),
                static_cast<std::int16_t>(vertex.y), vertex.adc ? 1 : 0);
  }

  // Screen coordinates: XYZ is 12.4 fixed point; the frame is 640x448 at 1x.
  // XYZ is signed 12.4 fixed point: negative coordinates are legal (the guard band) and must keep their sign.
  auto screen = [](const GifVertex &vertex) {
    return std::pair<double, double>(static_cast<std::int16_t>(vertex.x) / 16.0,
                                     static_cast<std::int16_t>(vertex.y) / 16.0);
  };

  // Affine fit: screen = A * (x, y, z, 1), one 4-unknown solve per output row.
  {
    std::vector<double> normal(16, 0.0), rhs_x(4, 0.0), rhs_y(4, 0.0);
    for (std::size_t index = 0; index < pairs; ++index) {
      const double p[4] = {mesh.vertices[index].position[0], mesh.vertices[index].position[1],
                           mesh.vertices[index].position[2], 1.0};
      const auto s = screen(drawn[index]);
      for (std::size_t a = 0; a < 4; ++a) {
        for (std::size_t b = 0; b < 4; ++b) normal[a * 4 + b] += p[a] * p[b];
        rhs_x[a] += p[a] * s.first;
        rhs_y[a] += p[a] * s.second;
      }
    }
    std::vector<double> matrix_x = normal, matrix_y = normal, row_x, row_y;
    if (solve(matrix_x, rhs_x, row_x) && solve(matrix_y, rhs_y, row_y)) {
      double worst = 0.0, sum = 0.0;
      for (std::size_t index = 0; index < pairs; ++index) {
        const double p[4] = {mesh.vertices[index].position[0], mesh.vertices[index].position[1],
                             mesh.vertices[index].position[2], 1.0};
        const auto s = screen(drawn[index]);
        const double fx = row_x[0] * p[0] + row_x[1] * p[1] + row_x[2] * p[2] + row_x[3];
        const double fy = row_y[0] * p[0] + row_y[1] * p[1] + row_y[2] * p[2] + row_y[3];
        const double dx = fx - s.first, dy = fy - s.second;
        worst = std::max(worst, std::sqrt(dx * dx + dy * dy));
        sum += dx * dx + dy * dy;
      }
      std::printf("  affine: rms %.4f px, worst %.4f px\n", std::sqrt(sum / pairs), worst);
      std::printf("    x = %.6g*X + %.6g*Y + %.6g*Z + %.6g\n", row_x[0], row_x[1], row_x[2], row_x[3]);
      std::printf("    y = %.6g*X + %.6g*Y + %.6g*Z + %.6g\n", row_y[0], row_y[1], row_y[2], row_y[3]);
    } else {
      std::printf("  affine: singular system\n");
    }
    if (affine_only) return 0;
  }

  // Projective fit: screen = (P0..P3) . (x,y,z,1) / (P8..P11) . (x,y,z,1), P11 = 1 gauge.
  {
    constexpr std::size_t kUnknowns = 11;
    std::vector<double> normal(kUnknowns * kUnknowns, 0.0), rhs(kUnknowns, 0.0);
    for (std::size_t index = 0; index < pairs; ++index) {
      const double x = mesh.vertices[index].position[0], y = mesh.vertices[index].position[1],
                   z = mesh.vertices[index].position[2];
      const auto s = screen(drawn[index]);
      // Unknowns: P00,P01,P02,P03,P10,P11,P12,P13,P20,P21,P22 (P23 fixed to 1).
      const double row_x[11] = {x, y, z, 1, 0, 0, 0, 0, -s.first * x, -s.first * y, -s.first * z};
      const double row_y[11] = {0, 0, 0, 0, x, y, z, 1, -s.second * x, -s.second * y, -s.second * z};
      const double rhs_x = s.first, rhs_y = s.second;
      for (std::size_t a = 0; a < kUnknowns; ++a) {
        for (std::size_t b = 0; b < kUnknowns; ++b)
          normal[a * kUnknowns + b] += row_x[a] * row_x[b] + row_y[a] * row_y[b];
        rhs[a] += row_x[a] * rhs_x + row_y[a] * rhs_y;
      }
    }
    std::vector<double> matrix = normal, solution;
    if (!solve(matrix, rhs, solution)) {
      std::printf("  projective: singular system\n");
      return 0;
    }
    double worst = 0.0, sum = 0.0;
    for (std::size_t index = 0; index < pairs; ++index) {
      const double x = mesh.vertices[index].position[0], y = mesh.vertices[index].position[1],
                   z = mesh.vertices[index].position[2];
      const auto s = screen(drawn[index]);
      const double w = solution[8] * x + solution[9] * y + solution[10] * z + 1.0;
      if (std::fabs(w) < 1e-9) continue;
      const double fx = (solution[0] * x + solution[1] * y + solution[2] * z + solution[3]) / w;
      const double fy = (solution[4] * x + solution[5] * y + solution[6] * z + solution[7]) / w;
      const double dx = fx - s.first, dy = fy - s.second;
      worst = std::max(worst, std::sqrt(dx * dx + dy * dy));
      sum += dx * dx + dy * dy;
    }
    std::printf("  projective: rms %.4f px, worst %.4f px\n", std::sqrt(sum / pairs), worst);
    std::printf("    row0 %.6g %.6g %.6g %.6g\n", solution[0], solution[1], solution[2], solution[3]);
    std::printf("    row1 %.6g %.6g %.6g %.6g\n", solution[4], solution[5], solution[6], solution[7]);
    std::printf("    row2 %.6g %.6g %.6g 1\n", solution[8], solution[9], solution[10]);
  }
  return 0;
}

void command_dump(const char *path, std::size_t record_index) {
  std::vector<std::uint8_t> data;
  std::vector<TapRecord> records;
  std::string error;
  if (!load_tap(path, data, records, error)) {
    std::fprintf(stderr, "gt4_coregs3_tap: %s\n", error.c_str());
    std::exit(1);
  }
  if (record_index >= records.size()) {
    std::fprintf(stderr, "gt4_coregs3_tap: record %zu is not in the tap\n", record_index);
    std::exit(1);
  }
  const TapRecord &record = records[record_index];
  std::printf("record %zu job %llu vblank %llu bytes %u\n", record_index,
              static_cast<unsigned long long>(record.job), static_cast<unsigned long long>(record.vblank),
              record.bytes);
  const GifPacket packet = parse_gif(data.data() + record.offset, record.bytes);
  std::printf("tags %zu vertices %zu ok %d %s\n", packet.tags.size(), packet.vertices.size(), packet.ok ? 1 : 0,
              packet.error.c_str());
  for (const GifTagSummary &tag : packet.tags)
    std::printf("  tag prim 0x%X nloop %u nreg %u format %u regs %016llx\n", tag.prim, tag.nloop, tag.nreg,
                static_cast<unsigned>(tag.format), static_cast<unsigned long long>(tag.registers));
  for (std::size_t index = 0; index < packet.vertices.size() && index < 12; ++index) {
    const GifVertex &vertex = packet.vertices[index];
    std::printf("  v%zu x %5d y %5d z %7u adc %d rgba %u,%u,%u,%u q %.3f st %.3f,%.3f\n", index,
                static_cast<std::int16_t>(vertex.x), static_cast<std::int16_t>(vertex.y), vertex.z,
                vertex.adc ? 1 : 0, vertex.r, vertex.g, vertex.b, vertex.a, vertex.q, vertex.s, vertex.t);
  }
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: gt4_coregs3_tap stats <tap.gt4tap>\n"
                 "       gt4_coregs3_tap fit <tap.gt4tap> <capture.gt4scene> <record> --draw <seq> [--affine]\n");
    return 2;
  }
  const std::string command = argv[1];
  if (command == "stats") {
    command_stats(argv[2]);
    return 0;
  }
  if (command == "dump") {
    if (argc < 4) {
      std::fprintf(stderr, "gt4_coregs3_tap: dump needs a record index\n");
      return 2;
    }
    command_dump(argv[2], static_cast<std::size_t>(std::atoi(argv[3])));
    return 0;
  }
  if (command == "fit") {
    if (argc < 6) {
      std::fprintf(stderr, "gt4_coregs3_tap: fit needs a record index and --draw\n");
      return 2;
    }
    const std::size_t record = static_cast<std::size_t>(std::atoi(argv[4]));
    std::uint32_t draw = 0;
    bool affine_only = false;
    for (int index = 5; index < argc; ++index) {
      const std::string argument = argv[index];
      if (argument == "--draw" && index + 1 < argc) draw = static_cast<std::uint32_t>(std::atoi(argv[++index]));
      else if (argument == "--affine") affine_only = true;
    }
    return command_fit(argv[2], argv[3], record, draw, affine_only);
  }
  std::fprintf(stderr, "gt4_coregs3_tap: unknown command %s\n", command.c_str());
  return 2;
}
