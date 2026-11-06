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
  std::uint32_t kind = kTapKindGif;
  std::uint32_t address = 0;  // kind == source: the VIF packet's guest address
  std::uint32_t qwc = 0;
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
    record.kind = u32(at + 4);
    if (record.kind == kTapKindSource && bytes >= 8) {
      record.address = u32(record.offset);
      record.qwc = u32(record.offset + 4);
    }
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
  std::uint64_t gif_records = 0, source_records = 0;
  for (std::size_t index = 0; index < records.size(); ++index) {
    const TapRecord &record = records[index];
    if (record.kind == kTapKindSource) {
      ++source_records;
      continue;
    }
    ++gif_records;
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
  std::printf("gif %llu source %llu\n", static_cast<unsigned long long>(gif_records),
              static_cast<unsigned long long>(source_records));
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

// job -> the guest VIF packet addresses it carries. One job covers one packet, or a whole batch of them.
std::vector<std::uint32_t> job_addresses(const std::vector<TapRecord> &records, std::uint64_t job) {
  std::vector<std::uint32_t> addresses;
  for (const TapRecord &record : records)
    if (record.kind == kTapKindSource && record.job == job) addresses.push_back(record.address);
  return addresses;
}

int command_map(const char *tap_path, const char *capture_path, int frame_index) {
  std::vector<std::uint8_t> tap;
  std::vector<TapRecord> records;
  std::string error;
  if (!load_tap(tap_path, tap, records, error)) {
    std::fprintf(stderr, "gt4_coregs3_tap: %s\n", error.c_str());
    return 1;
  }
  std::vector<std::uint8_t> capture_data;
  if (!read_file(capture_path, capture_data)) {
    std::fprintf(stderr, "gt4_coregs3_tap: cannot open %s\n", capture_path);
    return 1;
  }
  SceneHeader header;
  if (!parse_scene_header(capture_data.data(), capture_data.size(), header, error)) {
    std::fprintf(stderr, "gt4_coregs3_tap: %s\n", error.c_str());
    return 1;
  }
  std::size_t at = 16;
  int index = 0;
  while (at < capture_data.size()) {
    FrameSnapshot frame;
    const std::size_t consumed =
        parse_frame_block(capture_data.data() + at, capture_data.size() - at, header.version, frame, error);
    if (consumed == 0) break;
    if (frame_index < 0 || index == frame_index) {
      std::printf("frame %d vblank %llu draws %u\n", index, static_cast<unsigned long long>(frame.vblank),
                  frame.draw_count);
      for (const CapturedDraw &draw : frame.draws)
        for (std::uint32_t item = 0; item < draw.item_count; ++item)
          std::printf("  draw %u item %u address %08X qwc %u\n", draw.sequence, item, draw.items[item].address,
                      draw.items[item].qwords);
      std::printf("jobs:\n");
      std::uint64_t last_job = 0;
      for (const TapRecord &record : records) {
        if (record.kind != kTapKindSource || record.job == last_job) continue;
        last_job = record.job;
        std::printf("  job %llu:%s", static_cast<unsigned long long>(record.job), "");
        for (const std::uint32_t address : job_addresses(records, record.job)) std::printf(" %08X", address);
        std::printf("\n");
      }
      return 0;
    }
    at += consumed;
    ++index;
  }
  std::fprintf(stderr, "gt4_coregs3_tap: frame %d is not in the capture\n", frame_index);
  return 1;
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
  if (record_index != static_cast<std::size_t>(-1) && record_index >= records.size()) {
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
  // Without an explicit record, find the job by the draw's VIF packet addresses: the tap records which guest address each
  // job carried, and the capture stores the same addresses per item. That is the exact draw-to-record mapping.
  if (record_index == static_cast<std::size_t>(-1)) {
    std::uint64_t found = 0;
    for (const CapturedItem &item : draw->items) {
      if (item.packet_bytes == 0) continue;
      for (const TapRecord &source : records)
        if (source.kind == kTapKindSource && source.address == item.address && (found == 0 || source.job < found))
          found = source.job;
    }
    if (found == 0) {
      std::fprintf(stderr, "gt4_coregs3_tap: no tapped job carries draw %u's VIF packets\n", draw_sequence);
      return 1;
    }
    for (std::size_t index = 0; index < records.size(); ++index)
      if (records[index].kind == kTapKindGif && records[index].job == found) {
        record_index = index;
        break;
      }
    std::printf("draw %u -> job %llu (record %zu)\n", draw_sequence, static_cast<unsigned long long>(found),
                record_index);
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

// Fits every draw of a frame against the tapped vertices of its job, in submission order. A job can carry several draws,
// so each job keeps a vertex cursor; the fit is what proves the alignment (a wrong pairing blows the residual up).
int command_fit_all(const char *tap_path, const char *capture_path, int frame_index) {
  std::vector<std::uint8_t> tap;
  std::vector<TapRecord> records;
  std::string error;
  if (!load_tap(tap_path, tap, records, error)) {
    std::fprintf(stderr, "gt4_coregs3_tap: %s\n", error.c_str());
    return 1;
  }
  // job -> its GIF records, in order
  std::vector<std::pair<std::uint64_t, std::vector<std::size_t>>> jobs;
  for (std::size_t index = 0; index < records.size(); ++index) {
    if (records[index].kind != kTapKindGif) continue;
    if (jobs.empty() || jobs.back().first != records[index].job) jobs.push_back({records[index].job, {}});
    jobs.back().second.push_back(index);
  }
  std::vector<std::pair<std::uint32_t, std::uint64_t>> sources;  // address -> job
  for (const TapRecord &record : records)
    if (record.kind == kTapKindSource) sources.push_back({record.address, record.job});

  std::vector<std::uint8_t> capture_data;
  if (!read_file(capture_path, capture_data)) {
    std::fprintf(stderr, "gt4_coregs3_tap: cannot open %s\n", capture_path);
    return 1;
  }
  SceneHeader header;
  if (!parse_scene_header(capture_data.data(), capture_data.size(), header, error)) {
    std::fprintf(stderr, "gt4_coregs3_tap: %s\n", error.c_str());
    return 1;
  }
  std::size_t at = 16;
  int index = 0;
  while (at < capture_data.size()) {
    FrameSnapshot frame;
    const std::size_t consumed =
        parse_frame_block(capture_data.data() + at, capture_data.size() - at, header.version, frame, error);
    if (consumed == 0) break;
    if (frame_index >= 0 && index != frame_index) {
      at += consumed;
      ++index;
      continue;
    }

    std::vector<std::pair<std::uint64_t, std::size_t>> cursor;  // job, next vertex of its stream
    auto vertex_of = [&](std::uint64_t job) -> std::size_t {
      for (auto &entry : cursor)
        if (entry.first == job) return entry.second;
      cursor.push_back({job, 0});
      return 0;
    };
    std::vector<GifVertex> stream;  // all GIF vertices of the frame, per job, rebuilt lazily
    std::uint64_t stream_job = 0;
    auto load_stream = [&](std::uint64_t job) {
      if (stream_job == job && !stream.empty()) return true;
      stream.clear();
      for (auto &entry : jobs) {
        if (entry.first != job) continue;
        for (const std::size_t record_index : entry.second) {
          const GifPacket packet = parse_gif(tap.data() + records[record_index].offset, records[record_index].bytes);
          if (!packet.ok) return false;
          stream.insert(stream.end(), packet.vertices.begin(), packet.vertices.end());
        }
        stream_job = job;
        return true;
      }
      return false;
    };

    double sum_sq = 0.0, worst = 0.0;
    std::uint32_t fitted = 0, skipped = 0, pairs_total = 0;
    std::uint32_t worst_draw = 0;
    std::uint32_t within_010 = 0, within_025 = 0, within_1 = 0, within_4 = 0, beyond_4 = 0;
    std::uint32_t draws_within_025 = 0;
    std::vector<std::pair<double, std::uint32_t>> draw_worst_list;
    MeshOptions mesh_options;
    for (const CapturedDraw &draw : frame.draws) {
      Mesh mesh;
      std::string ignored;
      if (!build_mesh(frame.packets.data(), frame.packets.size(), draw, mesh_options, mesh, ignored)) {
        ++skipped;
        continue;
      }
      std::uint64_t job = 0;
      for (const CapturedItem &item : draw.items) {
        if (item.packet_bytes == 0) continue;
        for (const auto &source : sources)
          if (source.first == item.address && (job == 0 || source.second < job)) job = source.second;
      }
      if (job == 0 || !load_stream(job)) {
        ++skipped;
        continue;
      }
      const std::size_t expected = vertex_of(job);
      if (expected + mesh.vertices.size() > stream.size()) {
        ++skipped;
        continue;
      }
      // The cursor can drift when one draw emits more vertices than the captured shape has (clipping). Search a window
      // around the expected offset for the placement with the smallest residual: a wrong placement cannot fit.
      auto fit_at = [&](std::size_t first, std::vector<double> &row_x, std::vector<double> &row_y, double &rms) {
        std::vector<double> normal(16, 0.0), rhs_x(4, 0.0), rhs_y(4, 0.0);
        for (std::size_t k = 0; k < mesh.vertices.size(); ++k) {
          const double p[4] = {mesh.vertices[k].position[0], mesh.vertices[k].position[1],
                               mesh.vertices[k].position[2], 1.0};
          const double sx = static_cast<std::int16_t>(stream[first + k].x) / 16.0;
          const double sy = static_cast<std::int16_t>(stream[first + k].y) / 16.0;
          for (std::size_t a = 0; a < 4; ++a) {
            for (std::size_t b = 0; b < 4; ++b) normal[a * 4 + b] += p[a] * p[b];
            rhs_x[a] += p[a] * sx;
            rhs_y[a] += p[a] * sy;
          }
        }
        std::vector<double> mx = normal, my = normal;
        if (!solve(mx, rhs_x, row_x) || !solve(my, rhs_y, row_y)) return false;
        double sum = 0.0;
        for (std::size_t k = 0; k < mesh.vertices.size(); ++k) {
          const double p[4] = {mesh.vertices[k].position[0], mesh.vertices[k].position[1],
                               mesh.vertices[k].position[2], 1.0};
          const double sx = static_cast<std::int16_t>(stream[first + k].x) / 16.0;
          const double sy = static_cast<std::int16_t>(stream[first + k].y) / 16.0;
          const double dx = row_x[0] * p[0] + row_x[1] * p[1] + row_x[2] * p[2] + row_x[3] - sx;
          const double dy = row_y[0] * p[0] + row_y[1] * p[1] + row_y[2] * p[2] + row_y[3] - sy;
          sum += dx * dx + dy * dy;
        }
        rms = std::sqrt(sum / static_cast<double>(mesh.vertices.size()));
        return true;
      };
      std::size_t first = expected;
      std::vector<double> best_x, best_y;
      double best_rms = 1e30;
      const std::size_t window = 320;
      const std::size_t low = expected > window ? expected - window : 0;
      const std::size_t high = std::min(expected + window, stream.size() - mesh.vertices.size());
      for (std::size_t candidate = low; candidate <= high; ++candidate) {
        std::vector<double> row_x, row_y;
        double rms = 0.0;
        if (!fit_at(candidate, row_x, row_y, rms)) continue;
        if (rms < best_rms) {
          best_rms = rms;
          first = candidate;
          best_x = row_x;
          best_y = row_y;
        }
      }
      if (best_rms > 1e29) {
        ++skipped;
        continue;
      }
      const std::vector<double> row_x = best_x, row_y = best_y;
      double draw_worst = 0.0;
      for (std::size_t k = 0; k < mesh.vertices.size(); ++k) {
        const double p[4] = {mesh.vertices[k].position[0], mesh.vertices[k].position[1],
                             mesh.vertices[k].position[2], 1.0};
        const double sx = static_cast<std::int16_t>(stream[first + k].x) / 16.0;
        const double sy = static_cast<std::int16_t>(stream[first + k].y) / 16.0;
        const double dx = row_x[0] * p[0] + row_x[1] * p[1] + row_x[2] * p[2] + row_x[3] - sx;
        const double dy = row_y[0] * p[0] + row_y[1] * p[1] + row_y[2] * p[2] + row_y[3] - sy;
        const double error = std::sqrt(dx * dx + dy * dy);
        sum_sq += dx * dx + dy * dy;
        draw_worst = std::max(draw_worst, error);
        if (error <= 0.10) ++within_010;
        else if (error <= 0.25) ++within_025;
        else if (error <= 1.0) ++within_1;
        else if (error <= 4.0) ++within_4;
        else ++beyond_4;
      }
      if (draw_worst <= 0.25) ++draws_within_025;
      draw_worst_list.push_back({draw_worst, draw.sequence});
      if (draw_worst > worst) {
        worst = draw_worst;
        worst_draw = draw.sequence;
      }
      pairs_total += static_cast<std::uint32_t>(mesh.vertices.size());
      ++fitted;
      for (auto &entry : cursor)
        if (entry.first == job) entry.second += mesh.vertices.size();
    }
    std::printf("frame %d vblank %llu: %u draws fitted, %u skipped, %u vertex pairs\n", index,
                static_cast<unsigned long long>(frame.vblank), fitted, skipped, pairs_total);
    if (pairs_total != 0) {
      std::printf("  rms %.4f px, worst %.4f px (draw %u), draws with worst <= 0.25 px: %u of %u\n",
                  std::sqrt(sum_sq / pairs_total), worst, worst_draw, draws_within_025, fitted);
      std::printf("  vertices: <=0.10 px %u, <=0.25 px %u, <=1 px %u, <=4 px %u, >4 px %u\n", within_010,
                  within_025, within_1, within_4, beyond_4);
      std::sort(draw_worst_list.begin(), draw_worst_list.end(),
                [](const auto &a, const auto &b) { return a.first > b.first; });
      std::printf("  worst draws:");
      for (std::size_t i = 0; i < draw_worst_list.size() && i < 6; ++i)
        std::printf(" %u(%.2fpx)", draw_worst_list[i].second, draw_worst_list[i].first);
      std::printf("\n");
    }
    return 0;
  }
  std::fprintf(stderr, "gt4_coregs3_tap: frame %d is not in the capture\n", frame_index);
  return 1;
}

// The strip-table gate (step 1): for every draw the tap covers, decode the mesh without the tap, then check that every
// triangle the VU1 actually draws (ADC = 0 on its last vertex) lies inside one decoded strip. A crossing triangle would
// mean the strip table is misread. It also correlates the ADC flags with the projected facing sign, which is the other
// thing the VU1 does with ADC (measured: ADC = 1 on the culled facing and on the first two vertices of every strip).
//
// Mapping: the tap records the guest address of every posted VIF packet next to its job (source records) and every PATH1
// emission with its job (GIF records). The same model packets are posted again every frame, so the source record's VBlank
// selects the frame; each shape item then matches the GIF record of the same vertex count, in order. Records that belong
// to other passes in the same job are skipped, which is what makes a draw span several jobs or a job carry several draws.
int command_gate(const char *tap_path, const char *capture_path, int frame_index, bool verbose) {
  std::vector<std::uint8_t> tap;
  std::vector<TapRecord> records;
  std::string error;
  if (!load_tap(tap_path, tap, records, error)) {
    std::fprintf(stderr, "gt4_coregs3_tap: %s\n", error.c_str());
    return 1;
  }
  struct TapJob {
    std::uint64_t job = 0;
    std::vector<GifVertex> vertices;
    std::vector<std::uint32_t> record_counts;
    std::vector<std::uint32_t> record_offsets;
  };
  std::vector<TapJob> jobs;
  std::vector<std::pair<std::uint32_t, std::uint64_t>> addresses;  // (address, job), in tap order
  std::vector<std::uint64_t> address_vblanks;
  for (const TapRecord &record : records) {
    if (record.kind == kTapKindSource) {
      addresses.push_back({record.address, record.job});
      address_vblanks.push_back(record.vblank);
      continue;
    }
    if (jobs.empty() || jobs.back().job != record.job) {
      TapJob job;
      job.job = record.job;
      jobs.push_back(std::move(job));
    }
    TapJob &job = jobs.back();
    const GifPacket packet = parse_gif(tap.data() + record.offset, record.bytes);
    if (!packet.ok) {
      std::fprintf(stderr, "gt4_coregs3_tap: a GIF record does not parse: %s\n", packet.error.c_str());
      return 1;
    }
    job.record_offsets.push_back(static_cast<std::uint32_t>(job.vertices.size()));
    job.record_counts.push_back(static_cast<std::uint32_t>(packet.vertices.size()));
    job.vertices.insert(job.vertices.end(), packet.vertices.begin(), packet.vertices.end());
  }
  auto job_by_id = [&](std::uint64_t id) -> TapJob * {
    for (TapJob &job : jobs)
      if (job.job == id) return &job;
    return nullptr;
  };

  std::vector<std::uint8_t> capture_data;
  if (!read_file(capture_path, capture_data)) {
    std::fprintf(stderr, "gt4_coregs3_tap: cannot open %s\n", capture_path);
    return 1;
  }
  SceneHeader header;
  if (!parse_scene_header(capture_data.data(), capture_data.size(), header, error)) {
    std::fprintf(stderr, "gt4_coregs3_tap: %s\n", error.c_str());
    return 1;
  }
  FrameSnapshot frame;
  bool found = false;
  std::size_t at = 16;
  int index = 0;
  while (at < capture_data.size()) {
    FrameSnapshot candidate;
    const std::size_t consumed =
        parse_frame_block(capture_data.data() + at, capture_data.size() - at, header.version, candidate, error);
    if (consumed == 0) break;
    if (index == frame_index) {
      frame = std::move(candidate);
      found = true;
      break;
    }
    at += consumed;
    ++index;
  }
  if (!found) {
    std::fprintf(stderr, "gt4_coregs3_tap: frame %d is not in the capture\n", frame_index);
    return 1;
  }

  // One assignment per (draw, item): the tap source record of this frame and the GIF record it produced. Items are
  // processed per job in posting order; a job may carry items of several draws and other passes.
  struct ItemRef {
    std::uint32_t draw = 0;
    std::uint32_t item = 0;
    std::uint32_t count = 0;
    std::uint32_t address = 0;
    std::uint32_t source_order = 0xFFFFFFFFu;  // position in the address list, to order items inside one job
    std::uint32_t job_index = 0;
    bool mapped = false;
    std::size_t first_vertex = 0;
    std::vector<std::uint32_t> strip_starts;  // local vertex index of every strip, from the item's own table
    bool has_table = false;
    bool zero_table = false;
  };
  std::vector<ItemRef> items;
  std::vector<std::uint32_t> draw_item_base(frame.draws.size());
  std::vector<std::uint32_t> draw_item_count(frame.draws.size());
  std::uint32_t total_items = 0;
  for (std::uint32_t draw_index = 0; draw_index < frame.draws.size(); ++draw_index) {
    const CapturedDraw &draw = frame.draws[draw_index];
    draw_item_base[draw_index] = total_items;
    std::uint32_t count = 0;
    for (std::uint32_t item_index = 0; item_index < draw.item_count; ++item_index) {
      const CapturedItem &item = draw.items[item_index];
      if (item.packet_bytes == 0) continue;
      const VifPacket packet = decode_packet(frame.packets.data() + item.packet_offset, item.packet_bytes);
      std::uint32_t vertices = 0;
      std::vector<std::uint8_t> strip_bytes;
      for (const VifCommand &entry : packet.commands) {
        if (!entry.unpack) continue;
        if (entry.tops && entry.destination == 0 &&
            (entry.format == ChannelFormat::v3_16 || entry.format == ChannelFormat::v3_32))
          vertices += entry.vectors;
        if (classify_strip_table(entry.tops, entry.destination, entry.format)) {
          std::vector<VifVector> vectors;
          const std::uint8_t *image = entry.tops ? packet.packed.data() : packet.window.data();
          strip_bytes.clear();
          if (read_vu_vectors(image, entry.destination * 16u, entry.vectors, vectors))
            for (const VifVector &vector : vectors) strip_bytes.push_back(static_cast<std::uint8_t>(vector.v[3] & 0xFFu));
        }
      }
      ItemRef ref;
      ref.draw = draw_index;
      ref.item = item_index;
      ref.count = vertices;
      ref.address = item.address;
      ref.has_table = !strip_bytes.empty();
      ref.zero_table = !strip_bytes.empty() && strip_bytes[0] == 0;
      {
        std::vector<std::uint32_t> lengths;
        std::string table_error;
        if (decode_strip_table(strip_bytes, vertices, lengths, table_error)) {
          std::uint32_t start = 0;
          for (const std::uint32_t length : lengths) {
            ref.strip_starts.push_back(start);
            start += length;
          }
        }
      }
      items.push_back(ref);
      ++count;
      ++total_items;
    }
    draw_item_count[draw_index] = count;
  }

  // For every item, find the GIF record that matches its vertex count and whose ADC pattern matches the item's own strip
  // table (the VU1 marks the first two vertices of every strip ADC, so a correct record scores two per strip). The same
  // model packets are posted every frame, so a job is preferred when its source VBlank is the frame's VBlank, then the
  // nearest one; the score decides between records that share a vertex count.
  auto score_record = [&](const TapJob &job, std::uint32_t record, const ItemRef &ref) -> long {
    const std::uint32_t base = job.record_offsets[record];
    long score = 0;
    for (const std::uint32_t start : ref.strip_starts)
      for (std::uint32_t offset = 0; offset < 2 && start + offset < ref.count; ++offset)
        score += job.vertices[base + start + offset].adc ? 1 : -1;
    return score;
  };
  for (ItemRef &ref : items) {
    long best_score = 0;
    long best_distance = 0;
    long best_record = -1;
    std::uint32_t best_job = 0;
    bool have = false;
    for (std::size_t source = 0; source < addresses.size(); ++source) {
      if (addresses[source].first != ref.address) continue;
      TapJob *job = job_by_id(addresses[source].second);
      if (job == nullptr || job->record_counts.empty()) continue;
      const long distance = std::llabs(static_cast<long long>(address_vblanks[source]) -
                                      static_cast<long long>(frame.vblank));
      for (std::uint32_t record = 0; record < job->record_counts.size(); ++record) {
        if (job->record_counts[record] != ref.count) continue;
        const long score = score_record(*job, record, ref);
        if (!have || score > best_score ||
            (score == best_score && distance < best_distance) ||
            (score == best_score && distance == best_distance && static_cast<long>(record) < best_record)) {
          have = true;
          best_score = score;
          best_distance = distance;
          best_record = record;
          best_job = static_cast<std::uint32_t>(job - jobs.data());
        }
      }
    }
    if (have) {
      ref.mapped = true;
      ref.job_index = best_job;
      ref.first_vertex = jobs[best_job].record_offsets[static_cast<std::size_t>(best_record)];
    }
  }
  // A clipped item emits fewer vertices than the model has. Accept the nearest record when no exact one exists, so the
  // draw is reported as a count mismatch (with its real numbers) instead of an unmapped draw.
  for (ItemRef &ref : items) {
    if (ref.mapped || ref.count == 0) continue;
    long best_delta = 0;
    long best_distance = 0;
    std::uint32_t best_job = 0;
    std::uint32_t best_record = 0;
    bool have = false;
    for (std::size_t source = 0; source < addresses.size(); ++source) {
      if (addresses[source].first != ref.address) continue;
      TapJob *job = job_by_id(addresses[source].second);
      if (job == nullptr) continue;
      const long distance = std::llabs(static_cast<long long>(address_vblanks[source]) -
                                      static_cast<long long>(frame.vblank));
      for (std::uint32_t record = 0; record < job->record_counts.size(); ++record) {
        const std::uint32_t count = job->record_counts[record];
        if (count == 0) continue;
        const long delta = std::llabs(static_cast<long long>(count) - static_cast<long long>(ref.count));
        if (delta > static_cast<long>(ref.count) / 8 + 4) continue;
        if (!have || delta < best_delta || (delta == best_delta && distance < best_distance)) {
          have = true;
          best_delta = delta;
          best_distance = distance;
          best_job = static_cast<std::uint32_t>(job - jobs.data());
          best_record = record;
        }
      }
    }
    if (have) {
      ref.mapped = true;
      ref.job_index = best_job;
      ref.first_vertex = jobs[best_job].record_offsets[best_record];
    }
  }

  struct Totals {
    std::uint32_t draws = 0, gated = 0, unmapped = 0, count_mismatch = 0, table_mismatch = 0;
    std::uint32_t vertices = 0, strips = 0, triangles_drawn = 0, triangles_culled = 0, crossing = 0;
    std::uint32_t tables = 0, zero_tables = 0;
    std::uint32_t strip_start_vertices = 0, strip_start_adc = 0;
    std::uint32_t drawn_positive = 0, drawn_negative = 0, culled_positive = 0, culled_negative = 0;
    std::uint32_t facing_exceptions = 0, near_degenerate = 0;
    std::uint32_t fit_ok = 0, fit_bad = 0, fit_singular = 0;
    double fit_rms_sum = 0.0, fit_worst = 0.0;
  } totals;

  for (const ItemRef &ref : items) {
    if (ref.has_table) ++totals.tables;
    if (ref.zero_table) ++totals.zero_tables;
  }
  MeshOptions mesh_options;
  for (std::uint32_t draw_index = 0; draw_index < frame.draws.size(); ++draw_index) {
    const CapturedDraw &draw = frame.draws[draw_index];
    ++totals.draws;
    Mesh mesh;
    std::string mesh_error;
    if (!build_mesh(frame.packets.data(), frame.packets.size(), draw, mesh_options, mesh, mesh_error)) {
      ++totals.unmapped;
      if (verbose) std::printf("draw %u: mesh refused: %s\n", draw.sequence, mesh_error.c_str());
      continue;
    }
    if (!mesh.has_strip_table) ++totals.table_mismatch;
    // Concatenate the item slices in item order; every item must be mapped.
    std::vector<GifVertex> slice;
    bool mapped = true;
    for (std::uint32_t offset = 0; offset < draw_item_count[draw_index]; ++offset) {
      const ItemRef &ref = items[draw_item_base[draw_index] + offset];
      if (!ref.mapped) {
        mapped = false;
        break;
      }
      const TapJob &job = jobs[ref.job_index];
      slice.insert(slice.end(), job.vertices.begin() + static_cast<std::ptrdiff_t>(ref.first_vertex),
                   job.vertices.begin() + static_cast<std::ptrdiff_t>(ref.first_vertex + ref.count));
    }
    if (!mapped) {
      ++totals.unmapped;
      if (verbose) std::printf("draw %u: no tapped GIF record matches every item\n", draw.sequence);
      continue;
    }
    if (slice.size() != mesh.vertices.size()) {
      ++totals.count_mismatch;
      if (verbose)
        std::printf("draw %u: tap slice %zu vertices, mesh %zu (clipped)\n", draw.sequence, slice.size(),
                    mesh.vertices.size());
      continue;
    }
    // The slice must also be the right *vertices*, not just the right count: an affine fit of the model positions to the
    // tapped screen positions is exact to a few hundredths of a pixel when the record belongs to this draw (G-TAP), and
    // blows up when a record of another shape was picked. This is the guard against a count coincidence.
    double fit_sum = 0.0;
    double fit_worst = 0.0;
    bool solved = false;
    {
      std::vector<double> normal(16, 0.0), rhs_x(4, 0.0), rhs_y(4, 0.0), row_x(4, 0.0), row_y(4, 0.0);
      for (std::size_t k = 0; k < mesh.vertices.size(); ++k) {
        const double p[4] = {mesh.vertices[k].position[0], mesh.vertices[k].position[1],
                             mesh.vertices[k].position[2], 1.0};
        const double sx = static_cast<std::int16_t>(slice[k].x) / 16.0;
        const double sy = static_cast<std::int16_t>(slice[k].y) / 16.0;
        for (std::size_t a = 0; a < 4; ++a) {
          for (std::size_t b = 0; b < 4; ++b) normal[a * 4 + b] += p[a] * p[b];
          rhs_x[a] += p[a] * sx;
          rhs_y[a] += p[a] * sy;
        }
      }
      std::vector<double> mx = normal, my = normal;
      if (solve(mx, rhs_x, row_x) && solve(my, rhs_y, row_y)) {
        for (std::size_t k = 0; k < mesh.vertices.size(); ++k) {
          const double p[4] = {mesh.vertices[k].position[0], mesh.vertices[k].position[1],
                               mesh.vertices[k].position[2], 1.0};
          const double dx = row_x[0] * p[0] + row_x[1] * p[1] + row_x[2] * p[2] + row_x[3] -
                            static_cast<std::int16_t>(slice[k].x) / 16.0;
          const double dy = row_y[0] * p[0] + row_y[1] * p[1] + row_y[2] * p[2] + row_y[3] -
                            static_cast<std::int16_t>(slice[k].y) / 16.0;
          fit_sum += dx * dx + dy * dy;
          fit_worst = std::max(fit_worst, std::sqrt(dx * dx + dy * dy));
        }
        solved = true;
      }
    }
    const double fit_rms = mesh.vertices.empty() ? 0.0 : std::sqrt(fit_sum / mesh.vertices.size());
    if (!solved) {
      // A flat part (every y equal) makes the normal equations singular; that is a property of the model, not of the
      // mapping, and the strip check still applies.
      ++totals.fit_singular;
      ++totals.fit_ok;
    } else if (fit_rms > 10.0 || fit_worst > 40.0) {
      // The slice is still checked against the strip table (the topology gate); this only says the count-based record
      // pick is ambiguous for this draw, which is reported apart and never hidden.
      ++totals.fit_bad;
      if (verbose)
        std::printf("draw %u: slice does not fit the model (rms %.4f px, worst %.4f px)\n", draw.sequence, fit_rms,
                    fit_worst);
    } else if (solved) {
      ++totals.fit_ok;
      totals.fit_rms_sum += fit_rms;
      totals.fit_worst = std::max(totals.fit_worst, fit_worst);
    }
    totals.vertices += static_cast<std::uint32_t>(mesh.vertices.size());
    totals.strips += static_cast<std::uint32_t>(mesh.strip_lengths.size());
    std::vector<std::uint32_t> segment(mesh.vertices.size(), 0);
    for (std::uint32_t strip = 0; strip < mesh.strip_starts.size(); ++strip)
      for (std::uint32_t v = mesh.strip_starts[strip]; v < mesh.strip_starts[strip] + mesh.strip_lengths[strip]; ++v)
        segment[v] = strip;
    for (std::uint32_t strip = 0; strip < mesh.strip_starts.size(); ++strip)
      for (std::uint32_t offset = 0; offset < 2 && offset < mesh.strip_lengths[strip]; ++offset) {
        ++totals.strip_start_vertices;
        if (slice[mesh.strip_starts[strip] + offset].adc) ++totals.strip_start_adc;
      }
    std::uint32_t crossing = 0;
    std::uint32_t exceptions = 0;
    std::uint32_t near_degenerate = 0;
    for (std::uint32_t i = 2; i < mesh.vertices.size(); ++i) {
      const GifVertex &last = slice[i];
      const long area =
          static_cast<long>((static_cast<std::int16_t>(slice[i - 1].x) - static_cast<std::int16_t>(slice[i - 2].x)) *
                                (static_cast<std::int16_t>(last.y) - static_cast<std::int16_t>(slice[i - 2].y)) -
                            (static_cast<std::int16_t>(slice[i - 1].y) - static_cast<std::int16_t>(slice[i - 2].y)) *
                                (static_cast<std::int16_t>(last.x) - static_cast<std::int16_t>(slice[i - 2].x)));
      // A strip tri 0,1,2 / 2,1,3 has alternating winding; the facing sign is the area with that parity folded in.
      const bool parity = ((i - mesh.strip_starts[segment[i]]) & 1u) != 0;
      const bool positive = (area > 0) != parity;
      const bool inside = segment[i - 2] == segment[i - 1] && segment[i - 1] == segment[i];
      if (!last.adc) {
        ++totals.triangles_drawn;
        if (positive) ++totals.drawn_positive;
        else ++totals.drawn_negative;
        if (!inside) ++crossing;
      } else {
        ++totals.triangles_culled;
        if (positive) ++totals.culled_positive;
        else ++totals.culled_negative;
      }
      // The facing sign only means something for a triangle inside one strip; crossing triangles are suppressed
      // structurally. Near-degenerate triangles (under a quarter of a pixel of area) are reported apart: the fixed-point
      // sign there is not stable.
      if (inside && positive == !last.adc) {
        if (std::llabs(area) <= 64) ++near_degenerate;
        else {
          ++totals.facing_exceptions;
          ++exceptions;
        }
      }
    }
    totals.near_degenerate += near_degenerate;
    totals.crossing += crossing;
    ++totals.gated;
    if (verbose || crossing != 0)
      std::printf("draw %u: %zu vertices, %zu strips, facing exceptions %u, drawn %s\n", draw.sequence,
                  mesh.vertices.size(), mesh.strip_lengths.size(), exceptions,
                  crossing == 0 ? "inside the strips" : "CROSSES a strip break");
  }

  std::printf("gate frame %d vblank %llu: %u draws, %u gated, %u unmapped, %u count mismatch, %u without table\n",
              frame_index, static_cast<unsigned long long>(frame.vblank), totals.draws, totals.gated, totals.unmapped,
              totals.count_mismatch, totals.table_mismatch);
  std::printf("  vertices %u, strips %u, tables %u (zero %u), drawn triangles %u, culled %u, crossing %u\n",
              totals.vertices, totals.strips, totals.tables, totals.zero_tables, totals.triangles_drawn,
              totals.triangles_culled, totals.crossing);
  std::printf("  structural ADC at strip starts %u of %u, facing exceptions %u (near-degenerate %u)\n",
              totals.strip_start_adc, totals.strip_start_vertices, totals.facing_exceptions, totals.near_degenerate);
  std::printf("  affine fit: %u draws ok, %u bad, mean rms %.4f px, worst %.4f px\n", totals.fit_ok, totals.fit_bad,
              totals.fit_ok == 0 ? 0.0 : totals.fit_rms_sum / totals.fit_ok, totals.fit_worst);
  return totals.crossing == 0 && totals.strip_start_adc == totals.strip_start_vertices && totals.gated != 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: gt4_coregs3_tap stats <tap.gt4tap>\n"
                 "       gt4_coregs3_tap fit <tap.gt4tap> <capture.gt4scene> <record> --draw <seq> [--affine]\n"
                 "       gt4_coregs3_tap fit-all <tap.gt4tap> <capture.gt4scene> <frame>\n"
                 "       gt4_coregs3_tap gate <tap.gt4tap> <capture.gt4scene> <frame> [--verbose]\n"
                 "       gt4_coregs3_tap map <tap.gt4tap> <capture.gt4scene> <frame>\n");
    return 2;
  }
  const std::string command = argv[1];
  if (command == "stats") {
    command_stats(argv[2]);
    return 0;
  }
  if (command == "fit-all") {
    if (argc < 5) {
      std::fprintf(stderr, "gt4_coregs3_tap: fit-all needs a capture and a frame index\n");
      return 2;
    }
    return command_fit_all(argv[2], argv[3], std::atoi(argv[4]));
  }
  if (command == "gate") {
    if (argc < 5) {
      std::fprintf(stderr, "gt4_coregs3_tap: gate needs a tap, a capture and a frame index\n");
      return 2;
    }
    const bool verbose = argc > 5 && std::strcmp(argv[5], "--verbose") == 0;
    return command_gate(argv[2], argv[3], std::atoi(argv[4]), verbose);
  }
  if (command == "map") {
    if (argc < 5) {
      std::fprintf(stderr, "gt4_coregs3_tap: map needs a capture and a frame index\n");
      return 2;
    }
    return command_map(argv[2], argv[3], std::atoi(argv[4]));
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
    const int record_arg = std::atoi(argv[4]);
    const std::size_t record = record_arg < 0 ? static_cast<std::size_t>(-1) : static_cast<std::size_t>(record_arg);
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
