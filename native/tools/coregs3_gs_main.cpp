// TASK-090 CoreGS 3.0, phase S1: the `.gt4gs` PATH2/PATH3 capture tools (docs/COREGS3-GSSTREAM-CAPTURE.md §5.4).
//
//   gt4_coregs3_gs --stats <file.gt4gs>
//       totals: records and payload bytes per path, per pass and per frame, the frames opened/closed, the refusal
//       counters the overflow records carry, the largest record and frame, and the first/last VBlank.
//
//   gt4_coregs3_gs --list <file.gt4gs>
//       one line per record: sequence, kind, path, pass, VBlank, last VU1 job, payload bytes and the guest source site.
//
//   gt4_coregs3_gs --dump-frame <index> <file.gt4gs>
//       every record of frame <index> (0-based, the frame_begin's ordinal), with the first 64 payload bytes in hex.
//
//   gt4_coregs3_gs --check-pass <file.gt4gs> <capture.gt4scene>
//       the S1 G-PASS control: for every captured frame, the first `.gt4gs` pass_begin record must equal the first
//       `.gt4scene` v5 journal entry of the same VBlank in pass id and in its seven register words. Exit code 1 on any
//       mismatch, a missing pass record or a missing journal entry.
//
//   gt4_coregs3_gs --census <file.gt4gs> [--json <out.json>]
//       TASK-090 S2: walks every PATH2/PATH3 packet and prints the per-frame census: packet classes, IMAGE uploads, the
//       full-screen batches with their pinned state, and the measured wet-road composite chain (alpha clear, additive
//       composite, post multiplies) attributed to scene frame V from the stream stamped V + 1 (the S1 phase relation).
//       Named refusals are printed; every packet is classified or counted with a reason.
#include "coregs3_gsdecode.hpp"
#include "coregs3_gsstream.hpp"
#include "coregs3_gsstream_file.hpp"
#include "coregs3_scene.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace gt4::coregs3;

// The v5 scene file header is four u32 words (coregs3_scene.cpp write_scene_header).
constexpr std::size_t kSceneHeaderBytes = 16u;

const char *kind_name(std::uint16_t kind) {
  switch (kind) {
    case kGsStreamKindGif: return "gif";
    case kGsStreamKindPassBegin: return "pass_begin";
    case kGsStreamKindPassEnd: return "pass_end";
    case kGsStreamKindFrameBegin: return "frame_begin";
    case kGsStreamKindFrameEnd: return "frame_end";
    case kGsStreamKindGsState: return "gs_state";
    case kGsStreamKindPrivileged: return "privileged";
    case kGsStreamKindOverflow: return "overflow";
    default: return "unknown";
  }
}

const char *path_name(std::uint8_t path) {
  switch (path) {
    case 1: return "path1";
    case 2: return "path2";
    case 3: return "path3";
    default: return "-";
  }
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
  const bool ok = data.empty() || std::fread(data.data(), 1, data.size(), file) == data.size();
  std::fclose(file);
  return ok;
}

bool load_stream(const char *path, std::vector<std::uint8_t> &data, GsStreamFileHeader &header,
                 std::vector<GsStreamFileRecord> &records) {
  std::string error;
  if (!read_gs_stream_file(path, data, header, records, error)) {
    std::fprintf(stderr, "gt4_coregs3_gs: %s\n", error.c_str());
    return false;
  }
  return true;
}

struct FrameRow {
  std::uint32_t index = 0;
  std::uint64_t vblank = 0;
  std::uint64_t path2_bytes = 0;
  std::uint64_t path3_bytes = 0;
  std::uint64_t path2_records = 0;
  std::uint64_t path3_records = 0;
  std::uint64_t gif_records = 0;
  std::uint64_t refused_bytes = 0;
  std::uint64_t refused_records = 0;
  std::uint32_t causes = 0;
  bool closed = false;
};

void command_stats(const char *path) {
  std::vector<std::uint8_t> data;
  GsStreamFileHeader header;
  std::vector<GsStreamFileRecord> records;
  if (!load_stream(path, data, header, records)) std::exit(1);
  std::printf("gs %s: version %u target %u flags 0x%x first_vblank %llu, %zu records\n", path, header.version,
              header.target, header.flags, static_cast<unsigned long long>(header.first_vblank), records.size());
  std::uint64_t kinds[9] = {};
  std::uint64_t gif_records[4] = {}, gif_bytes[4] = {};
  std::uint64_t pass_records = 0, signals = 0, unknown_pass = 0;
  std::uint64_t pass_gif_records[kPassCount] = {}, pass_gif_bytes[kPassCount] = {};
  std::uint64_t refused_bytes_total = 0, refused_records_total = 0, refused_frames = 0;
  std::uint32_t causes = 0;
  std::uint32_t largest_record = 0, largest_frame = 0;
  std::uint64_t first_vblank = 0, last_vblank = 0;
  bool have_vblank = false;
  std::vector<FrameRow> frames;
  for (const GsStreamFileRecord &record : records) {
    kinds[record.kind < 9 ? record.kind : 0] += 1;
    if (record.kind == kGsStreamKindGif) {
      gif_records[record.path] += 1;
      gif_bytes[record.path] += record.bytes;
      const std::uint32_t pass = record.pass < kPassCount ? record.pass : kPassUnknown;
      pass_gif_records[pass] += 1;
      pass_gif_bytes[pass] += record.bytes;
      if ((record.flags & kGsStreamFlagSignals) != 0) signals += 1;
      if (record.pass == kPassUnknown) unknown_pass += 1;
      if (record.bytes > largest_record) largest_record = record.bytes;
      if (!frames.empty()) {
        frames.back().gif_records += 1;
        if (record.path == 2) frames.back().path2_records += 1;
        if (record.path == 3) frames.back().path3_records += 1;
      }
    } else if (record.kind == kGsStreamKindPassBegin || record.kind == kGsStreamKindPassEnd) {
      pass_records += 1;
    } else if (record.kind == kGsStreamKindFrameBegin) {
      FrameRow row;
      row.vblank = record.vblank;
      gs_stream_frame_totals(data, record, row.index, row.path2_bytes, row.path3_bytes);
      frames.push_back(row);
    } else if (record.kind == kGsStreamKindFrameEnd) {
      std::uint32_t index = 0;
      std::uint64_t path2 = 0, path3 = 0;
      gs_stream_frame_totals(data, record, index, path2, path3);
      if (!frames.empty() && frames.back().vblank == record.vblank) {
        frames.back().path2_bytes = path2;
        frames.back().path3_bytes = path3;
        frames.back().closed = true;
        const std::uint64_t total = path2 + path3;
        if (total > largest_frame) largest_frame = static_cast<std::uint32_t>(total);
      }
    } else if (record.kind == kGsStreamKindOverflow) {
      std::uint64_t bytes = 0, count = 0;
      std::uint32_t cause = 0;
      gs_stream_overflow_totals(data, record, bytes, count, cause);
      refused_bytes_total += bytes;
      refused_records_total += count;
      causes |= cause;
      if (!frames.empty()) {
        frames.back().refused_bytes += bytes;
        frames.back().refused_records += count;
        frames.back().causes |= cause;
      }
    }
    if (record.kind == kGsStreamKindGif) {
      if (!have_vblank) {
        first_vblank = record.vblank;
        have_vblank = true;
      }
      last_vblank = record.vblank;
    }
  }
  for (const FrameRow &frame : frames) {
    if (frame.refused_bytes != 0 || frame.refused_records != 0) refused_frames += 1;
    (void)frame.closed;
  }
  std::printf("records: gif %llu pass %llu frame %llu overflow %llu; pass_records %llu\n",
              static_cast<unsigned long long>(kinds[kGsStreamKindGif]),
              static_cast<unsigned long long>(kinds[kGsStreamKindPassBegin] + kinds[kGsStreamKindPassEnd]),
              static_cast<unsigned long long>(kinds[kGsStreamKindFrameBegin] + kinds[kGsStreamKindFrameEnd]),
              static_cast<unsigned long long>(kinds[kGsStreamKindOverflow]),
              static_cast<unsigned long long>(pass_records));
  std::printf("bytes: path2 %llu in %llu records, path3 %llu in %llu records\n",
              static_cast<unsigned long long>(gif_bytes[2]), static_cast<unsigned long long>(gif_records[2]),
              static_cast<unsigned long long>(gif_bytes[3]), static_cast<unsigned long long>(gif_records[3]));
  std::printf("frames: opened %zu closed %llu refused %llu; vblanks %llu..%llu\n", frames.size(),
              static_cast<unsigned long long>(std::count_if(frames.begin(), frames.end(),
                                                            [](const FrameRow &frame) { return frame.closed; })),
              static_cast<unsigned long long>(refused_frames),
              static_cast<unsigned long long>(have_vblank ? first_vblank : 0),
              static_cast<unsigned long long>(last_vblank));
  std::printf("refused: bytes %llu records %llu causes 0x%x; signals %llu unknown_pass %llu; largest record %u, "
              "largest frame %u\n",
              static_cast<unsigned long long>(refused_bytes_total),
              static_cast<unsigned long long>(refused_records_total), causes,
              static_cast<unsigned long long>(signals), static_cast<unsigned long long>(unknown_pass),
              largest_record, largest_frame);
  for (std::uint32_t pass = 0; pass < kPassCount; ++pass)
    if (pass_gif_records[pass] != 0 || pass_gif_bytes[pass] != 0)
      std::printf("  pass %-14s records %llu bytes %llu\n", pass_name(pass),
                  static_cast<unsigned long long>(pass_gif_records[pass]),
                  static_cast<unsigned long long>(pass_gif_bytes[pass]));
  for (const FrameRow &frame : frames)
    std::printf("  frame %u vblank %llu path2 %llu in %llu path3 %llu in %llu total %llu refused %llu/%llu%s\n",
                frame.index, static_cast<unsigned long long>(frame.vblank),
                static_cast<unsigned long long>(frame.path2_bytes),
                static_cast<unsigned long long>(frame.path2_records),
                static_cast<unsigned long long>(frame.path3_bytes),
                static_cast<unsigned long long>(frame.path3_records),
                static_cast<unsigned long long>(frame.path2_bytes + frame.path3_bytes),
                static_cast<unsigned long long>(frame.refused_records),
                static_cast<unsigned long long>(frame.refused_bytes), frame.closed ? "" : " (no frame_end)");
}

void command_list(const char *path) {
  std::vector<std::uint8_t> data;
  GsStreamFileHeader header;
  std::vector<GsStreamFileRecord> records;
  if (!load_stream(path, data, header, records)) std::exit(1);
  std::printf("gs %s: version %u first_vblank %llu, %zu records\n", path, header.version,
              static_cast<unsigned long long>(header.first_vblank), records.size());
  for (const GsStreamFileRecord &record : records) {
    std::printf("%6u %-11s %-5s pass %-14s vblank %llu job %llu bytes %u qwc %u pc %08X ra %08X%s%s\n",
                record.sequence, kind_name(record.kind), path_name(record.path),
                record.kind == kGsStreamKindGif || record.kind == kGsStreamKindPassBegin ||
                        record.kind == kGsStreamKindPassEnd
                    ? pass_name(record.pass)
                    : "-",
                static_cast<unsigned long long>(record.vblank), static_cast<unsigned long long>(record.last_job),
                record.bytes, record.qwc, record.source_pc, record.source_ra,
                (record.flags & kGsStreamFlagSignals) != 0 ? " signals" : "",
                (record.flags & kGsStreamFlagRefused) != 0 ? " refused" : "");
  }
}

void command_dump_frame(const char *path, std::uint32_t frame_index) {
  std::vector<std::uint8_t> data;
  GsStreamFileHeader header;
  std::vector<GsStreamFileRecord> records;
  if (!load_stream(path, data, header, records)) std::exit(1);
  bool in_frame = false;
  bool found = false;
  for (const GsStreamFileRecord &record : records) {
    if (record.kind == kGsStreamKindFrameBegin) {
      std::uint32_t index = 0;
      std::uint64_t path2 = 0, path3 = 0;
      gs_stream_frame_totals(data, record, index, path2, path3);
      if (in_frame) break;
      in_frame = index == frame_index;
      if (in_frame) {
        found = true;
        std::printf("frame %u vblank %llu\n", index, static_cast<unsigned long long>(record.vblank));
      }
    }
    if (!in_frame) continue;
    std::printf("%6u %-11s %-5s pass %-14s vblank %llu job %llu bytes %u qwc %u pc %08X ra %08X\n", record.sequence,
                kind_name(record.kind), path_name(record.path),
                record.kind == kGsStreamKindGif || record.kind == kGsStreamKindPassBegin ||
                        record.kind == kGsStreamKindPassEnd
                    ? pass_name(record.pass)
                    : "-",
                static_cast<unsigned long long>(record.vblank), static_cast<unsigned long long>(record.last_job),
                record.bytes, record.qwc, record.source_pc, record.source_ra);
    if (record.kind == kGsStreamKindGif) {
      const std::size_t shown = record.bytes < 64u ? record.bytes : 64u;
      std::printf("      payload:");
      for (std::size_t index = 0; index < shown; ++index)
        std::printf(" %02x", data[record.offset + index]);
      std::printf("%s\n", record.bytes > shown ? " ..." : "");
    } else if (record.kind == kGsStreamKindPassBegin || record.kind == kGsStreamKindPassEnd) {
      std::uint64_t words[9] = {};
      gs_stream_pass_words(data, record, words);
      std::printf("      registers fogcol %016llx frame %016llx xyoffset %016llx zbuf %016llx scissor %016llx "
                  "alpha %016llx test %016llx clamp %016llx tex1 %016llx\n",
                  static_cast<unsigned long long>(words[0]), static_cast<unsigned long long>(words[1]),
                  static_cast<unsigned long long>(words[2]), static_cast<unsigned long long>(words[3]),
                  static_cast<unsigned long long>(words[4]), static_cast<unsigned long long>(words[5]),
                  static_cast<unsigned long long>(words[6]), static_cast<unsigned long long>(words[7]),
                  static_cast<unsigned long long>(words[8]));
    } else if (record.kind == kGsStreamKindFrameEnd || record.kind == kGsStreamKindFrameBegin) {
      std::uint32_t index = 0;
      std::uint64_t path2 = 0, path3 = 0;
      gs_stream_frame_totals(data, record, index, path2, path3);
      std::printf("      frame_index %u path2 %llu path3 %llu\n", index,
                  static_cast<unsigned long long>(path2), static_cast<unsigned long long>(path3));
    } else if (record.kind == kGsStreamKindOverflow) {
      std::uint64_t bytes = 0, count = 0;
      std::uint32_t causes = 0;
      gs_stream_overflow_totals(data, record, bytes, count, causes);
      std::printf("      refused_bytes %llu refused_records %llu causes 0x%x\n",
                  static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(count), causes);
    }
    if (record.kind == kGsStreamKindFrameEnd) break;
  }
  if (!found) {
    std::fprintf(stderr, "gt4_coregs3_gs: frame %u is not in the file\n", frame_index);
    std::exit(1);
  }
}

// G-PASS: the first pass record of each captured frame against the first `.gt4scene` v5 journal entry of the same VBlank.
void command_check_pass(const char *gs_path, const char *scene_path) {
  std::vector<std::uint8_t> gs_data;
  GsStreamFileHeader header;
  std::vector<GsStreamFileRecord> records;
  if (!load_stream(gs_path, gs_data, header, records)) std::exit(1);
  std::vector<std::uint8_t> scene_data;
  if (!read_file(scene_path, scene_data)) {
    std::fprintf(stderr, "gt4_coregs3_gs: cannot read %s\n", scene_path);
    std::exit(1);
  }
  SceneHeader scene_header;
  std::string error;
  if (!parse_scene_header(scene_data.data(), scene_data.size(), scene_header, error)) {
    std::fprintf(stderr, "gt4_coregs3_gs: %s\n", error.c_str());
    std::exit(1);
  }
  // The first pass_begin record of every VBlank, in file order.
  std::vector<const GsStreamFileRecord *> first_pass;
  for (const GsStreamFileRecord &record : records) {
    if (record.kind != kGsStreamKindPassBegin) continue;
    bool seen = false;
    for (const GsStreamFileRecord *existing : first_pass) seen = seen || existing->vblank == record.vblank;
    if (!seen) first_pass.push_back(&record);
  }
  std::uint64_t checked = 0, matched = 0, mismatched = 0, missing = 0, empty_journal = 0;
  std::size_t at = kSceneHeaderBytes;
  while (at + 4u <= scene_data.size()) {
    FrameSnapshot frame;
    const std::size_t consumed = parse_frame_block(scene_data.data() + at, scene_data.size() - at, scene_header.version,
                                                   frame, error);
    if (consumed == 0) {
      std::fprintf(stderr, "gt4_coregs3_gs: %s\n", error.c_str());
      std::exit(1);
    }
    at += consumed;
    if (frame.pass_states.empty()) {
      empty_journal += 1;
      continue;
    }
    const CapturedPassState &journal = frame.pass_states.front();
    const GsStreamFileRecord *record = nullptr;
    for (const GsStreamFileRecord *candidate : first_pass)
      if (candidate->vblank == frame.vblank) record = candidate;
    checked += 1;
    if (record == nullptr) {
      std::printf("frame %u vblank %llu: MISSING pass_begin (journal %s)\n", frame.index,
                  static_cast<unsigned long long>(frame.vblank), pass_name(journal.pass));
      missing += 1;
      continue;
    }
    std::uint64_t words[9] = {};
    gs_stream_pass_words(gs_data, *record, words);
    std::uint64_t register_matches = 0;
    if (words[0] == journal.fogcol) register_matches += 1;
    if (words[1] == journal.frame) register_matches += 1;
    if (words[2] == journal.xyoffset) register_matches += 1;
    if (words[3] == journal.zbuf) register_matches += 1;
    if (words[4] == journal.scissor) register_matches += 1;
    if (words[5] == journal.alpha) register_matches += 1;
    if (words[6] == journal.test) register_matches += 1;
    const bool pass_matches = record->pass == journal.pass;
    const bool same = pass_matches && register_matches == 7;
    if (same) matched += 1;
    else mismatched += 1;
    std::printf("frame %u vblank %llu: journal %s gs %s regs %llu/7 clamp %016llx tex1 %016llx%s\n", frame.index,
                static_cast<unsigned long long>(frame.vblank), pass_name(journal.pass), pass_name(record->pass),
                static_cast<unsigned long long>(register_matches),
                static_cast<unsigned long long>(words[7]), static_cast<unsigned long long>(words[8]),
                same ? " MATCH" : " MISMATCH");
  }
  std::printf("pass control: frames checked %llu matched %llu mismatched %llu missing %llu empty journal %llu\n",
              static_cast<unsigned long long>(checked), static_cast<unsigned long long>(matched),
              static_cast<unsigned long long>(mismatched), static_cast<unsigned long long>(missing),
              static_cast<unsigned long long>(empty_journal));
  if (checked == 0 || mismatched != 0 || missing != 0 || empty_journal != 0) std::exit(1);
}

// TASK-090 S2: the packet census. Every packet is classified or counted with a named reason; the measured wet-road
// composite chain is recognised per scene frame with the S1 V -> V-1 phase relation.
void json_string(std::FILE *file, const std::string &text) {
  std::fputc('"', file);
  for (const char ch : text) {
    if (ch == '"' || ch == '\\') std::fprintf(file, "\\%c", ch);
    else if (static_cast<unsigned char>(ch) < 0x20) std::fprintf(file, "\\u%04x", ch);
    else std::fputc(ch, file);
  }
  std::fputc('"', file);
}

void command_census(const char *path, const char *json_path) {
  std::vector<std::uint8_t> data;
  GsStreamFileHeader header;
  std::vector<GsStreamFileRecord> records;
  if (!load_stream(path, data, header, records)) std::exit(1);
  GsStreamCensus census;
  std::vector<GsDecodeBatch> batches;
  std::vector<GsDecodeImageUpload> uploads;
  std::string error;
  if (!census_gs_stream(data, header, records, census, batches, uploads, error)) {
    std::fprintf(stderr, "gt4_coregs3_gs: %s\n", error.c_str());
    std::exit(1);
  }
  std::printf("gs %s: version %u, %llu records, %llu GIF records\n", path, header.version,
              static_cast<unsigned long long>(census.records),
              static_cast<unsigned long long>(census.gif_records[2] + census.gif_records[3]));
  std::printf("gif: path2 %llu records / %llu B, path3 %llu records / %llu B\n",
              static_cast<unsigned long long>(census.gif_records[2]),
              static_cast<unsigned long long>(census.gif_bytes[2]),
              static_cast<unsigned long long>(census.gif_records[3]),
              static_cast<unsigned long long>(census.gif_bytes[3]));
  std::printf("packets: %llu (packed %llu, reglist %llu, image %llu, disabled %llu); register-only %llu, "
              "primitive %llu, empty %llu\n",
              static_cast<unsigned long long>(census.packets),
              static_cast<unsigned long long>(census.packed_tags),
              static_cast<unsigned long long>(census.reglist_tags),
              static_cast<unsigned long long>(census.image_tags),
              static_cast<unsigned long long>(census.disabled_tags),
              static_cast<unsigned long long>(census.register_packets),
              static_cast<unsigned long long>(census.primitive_packets),
              static_cast<unsigned long long>(census.empty_packets));
  std::printf("writes: A+D %llu, register %llu, PRIM kicks %llu; batches %llu, draws %llu, vertices %llu "
              "(drawing %llu), full-screen %llu\n",
              static_cast<unsigned long long>(census.ad_writes),
              static_cast<unsigned long long>(census.register_writes),
              static_cast<unsigned long long>(census.prim_kicks),
              static_cast<unsigned long long>(census.batches),
              static_cast<unsigned long long>(census.draws),
              static_cast<unsigned long long>(census.vertices),
              static_cast<unsigned long long>(census.drawing_vertices),
              static_cast<unsigned long long>(census.full_screen_batches));
  std::printf("images: %llu uploads, %llu B; signals %llu\n",
              static_cast<unsigned long long>(census.image_uploads),
              static_cast<unsigned long long>(census.image_bytes),
              static_cast<unsigned long long>(census.signals));
  std::printf("refusals: truncated %llu, disabled-format %llu, unsupported-reglist-descriptor %llu, "
              "image-without-transfer-state %llu, vertices-without-prim %llu; split packets handled %llu\n",
              static_cast<unsigned long long>(census.truncated_packets),
              static_cast<unsigned long long>(census.unknown_format_packets),
              static_cast<unsigned long long>(census.unsupported_reglist_descriptor),
              static_cast<unsigned long long>(census.image_without_transfer_state),
              static_cast<unsigned long long>(census.vertices_without_prim),
              static_cast<unsigned long long>(census.split_packets));
  // Per-frame rows in capture VBlank order, with the full-screen batches of each.
  for (const auto &entry : census.frames) {
    const GsFrameCensus &row = entry.second;
    std::printf("frame %llu scene %llu: gif p2 %llu/%llu B p3 %llu/%llu B; packets %llu (register %llu, primitive "
                "%llu); images %llu/%llu B; batches %llu draws %llu verts %llu full-screen %llu\n",
                static_cast<unsigned long long>(row.capture_vblank),
                static_cast<unsigned long long>(row.scene_frame),
                static_cast<unsigned long long>(row.gif_records[2]),
                static_cast<unsigned long long>(row.gif_bytes[2]),
                static_cast<unsigned long long>(row.gif_records[3]),
                static_cast<unsigned long long>(row.gif_bytes[3]),
                static_cast<unsigned long long>(row.packets),
                static_cast<unsigned long long>(row.register_packets),
                static_cast<unsigned long long>(row.primitive_packets),
                static_cast<unsigned long long>(row.image_uploads),
                static_cast<unsigned long long>(row.image_bytes),
                static_cast<unsigned long long>(row.batches),
                static_cast<unsigned long long>(row.draws),
                static_cast<unsigned long long>(row.vertices),
                static_cast<unsigned long long>(row.full_screen_batches));
    for (const GsDecodeBatch &batch : batches) {
      if (batch.vblank != row.capture_vblank || !batch.full_screen) continue;
      std::printf("  %-28s %s\n", gs_batch_class_name(batch), gs_batch_state_line(batch).c_str());
    }
  }
  // The measured chain, per attributed scene frame (the stream stamped V + 1 belongs to frame V).
  std::vector<GsWetCompositeInfo> chains;
  for (const auto &entry : census.frames) {
    const std::uint64_t capture_vblank = entry.second.capture_vblank;
    if (capture_vblank == 0) continue;
    GsWetCompositeInfo info;
    const bool found = find_wet_composite(batches, capture_vblank - 1, info);
    chains.push_back(info);
    if (found) {
      std::printf("scene %llu (stream %llu): wet composite FOUND alpha_clear seq %u, composite seq %u, "
                  "post_darken seq %u, post_brighten seq %u; unrecognised full-screen %llu\n",
                  static_cast<unsigned long long>(info.scene_vblank),
                  static_cast<unsigned long long>(info.stream_vblank), info.alpha_clear->sequence,
                  info.composite->sequence, info.post_darken != nullptr ? info.post_darken->sequence : 0,
                  info.post_brighten != nullptr ? info.post_brighten->sequence : 0,
                  static_cast<unsigned long long>(info.unrecognised_full_screen));
    } else {
      std::printf("scene %llu (stream %llu): wet composite REFUSED: %s\n",
                  static_cast<unsigned long long>(info.scene_vblank),
                  static_cast<unsigned long long>(info.stream_vblank), info.refusal.c_str());
    }
  }
  if (json_path == nullptr) return;
  std::FILE *file = std::fopen(json_path, "wb");
  if (file == nullptr) {
    std::fprintf(stderr, "gt4_coregs3_gs: cannot write %s\n", json_path);
    std::exit(1);
  }
  std::fprintf(file, "{\"file\":");
  json_string(file, path);
  std::fprintf(file, ",\"version\":%u,\"records\":%llu,\"gif_records\":[%llu,%llu,%llu,%llu],"
                     "\"gif_bytes\":[%llu,%llu,%llu,%llu],\"packets\":%llu,\"packed_tags\":%llu,"
                     "\"reglist_tags\":%llu,\"image_tags\":%llu,\"disabled_tags\":%llu,"
                     "\"register_packets\":%llu,\"primitive_packets\":%llu,\"empty_packets\":%llu,"
                     "\"ad_writes\":%llu,\"register_writes\":%llu,\"prim_kicks\":%llu,\"batches\":%llu,"
                     "\"draws\":%llu,\"vertices\":%llu,\"drawing_vertices\":%llu,"
                     "\"full_screen_batches\":%llu,\"image_uploads\":%llu,\"image_bytes\":%llu,"
                     "\"signals\":%llu,\"truncated_packets\":%llu,\"unknown_format_packets\":%llu,"
                     "\"unsupported_reglist_descriptor\":%llu,\"image_without_transfer_state\":%llu,"
                     "\"vertices_without_prim\":%llu,\"split_packets\":%llu,\"frames\":[",
              header.version, static_cast<unsigned long long>(census.records),
              static_cast<unsigned long long>(census.gif_records[0]),
              static_cast<unsigned long long>(census.gif_records[1]),
              static_cast<unsigned long long>(census.gif_records[2]),
              static_cast<unsigned long long>(census.gif_records[3]),
              static_cast<unsigned long long>(census.gif_bytes[0]),
              static_cast<unsigned long long>(census.gif_bytes[1]),
              static_cast<unsigned long long>(census.gif_bytes[2]),
              static_cast<unsigned long long>(census.gif_bytes[3]),
              static_cast<unsigned long long>(census.packets),
              static_cast<unsigned long long>(census.packed_tags),
              static_cast<unsigned long long>(census.reglist_tags),
              static_cast<unsigned long long>(census.image_tags),
              static_cast<unsigned long long>(census.disabled_tags),
              static_cast<unsigned long long>(census.register_packets),
              static_cast<unsigned long long>(census.primitive_packets),
              static_cast<unsigned long long>(census.empty_packets),
              static_cast<unsigned long long>(census.ad_writes),
              static_cast<unsigned long long>(census.register_writes),
              static_cast<unsigned long long>(census.prim_kicks),
              static_cast<unsigned long long>(census.batches),
              static_cast<unsigned long long>(census.draws),
              static_cast<unsigned long long>(census.vertices),
              static_cast<unsigned long long>(census.drawing_vertices),
              static_cast<unsigned long long>(census.full_screen_batches),
              static_cast<unsigned long long>(census.image_uploads),
              static_cast<unsigned long long>(census.image_bytes),
              static_cast<unsigned long long>(census.signals),
              static_cast<unsigned long long>(census.truncated_packets),
              static_cast<unsigned long long>(census.unknown_format_packets),
              static_cast<unsigned long long>(census.unsupported_reglist_descriptor),
              static_cast<unsigned long long>(census.image_without_transfer_state),
              static_cast<unsigned long long>(census.vertices_without_prim),
              static_cast<unsigned long long>(census.split_packets));
  bool first = true;
  for (const auto &entry : census.frames) {
    const GsFrameCensus &row = entry.second;
    std::fprintf(file,
                 "%s{\"capture_vblank\":%llu,\"scene_frame\":%llu,\"gif_records\":[%llu,%llu,%llu,%llu],"
                 "\"gif_bytes\":[%llu,%llu,%llu,%llu],\"packets\":%llu,\"register_packets\":%llu,"
                 "\"primitive_packets\":%llu,\"batches\":%llu,\"draws\":%llu,\"vertices\":%llu,"
                 "\"drawing_vertices\":%llu,\"full_screen_batches\":%llu,\"image_uploads\":%llu,"
                 "\"image_bytes\":%llu}",
                 first ? "" : ",", static_cast<unsigned long long>(row.capture_vblank),
                 static_cast<unsigned long long>(row.scene_frame),
                 static_cast<unsigned long long>(row.gif_records[0]),
                 static_cast<unsigned long long>(row.gif_records[1]),
                 static_cast<unsigned long long>(row.gif_records[2]),
                 static_cast<unsigned long long>(row.gif_records[3]),
                 static_cast<unsigned long long>(row.gif_bytes[0]),
                 static_cast<unsigned long long>(row.gif_bytes[1]),
                 static_cast<unsigned long long>(row.gif_bytes[2]),
                 static_cast<unsigned long long>(row.gif_bytes[3]),
                 static_cast<unsigned long long>(row.packets),
                 static_cast<unsigned long long>(row.register_packets),
                 static_cast<unsigned long long>(row.primitive_packets),
                 static_cast<unsigned long long>(row.batches),
                 static_cast<unsigned long long>(row.draws),
                 static_cast<unsigned long long>(row.vertices),
                 static_cast<unsigned long long>(row.drawing_vertices),
                 static_cast<unsigned long long>(row.full_screen_batches),
                 static_cast<unsigned long long>(row.image_uploads),
                 static_cast<unsigned long long>(row.image_bytes));
    first = false;
  }
  std::fprintf(file, "],\"full_screen\":[");
  first = true;
  for (const GsDecodeBatch &batch : batches) {
    if (!batch.full_screen) continue;
    const GsDecodeState &state = batch.state;
    std::fprintf(file, "%s{\"sequence\":%u,\"vblank\":%llu,\"class\":", first ? "" : ",", batch.sequence,
                 static_cast<unsigned long long>(batch.vblank));
    json_string(file, gs_batch_class_name(batch));
    std::fprintf(file,
                 ",\"prim\":%u,\"tex0_tbp0\":%u,\"tex0_psm\":%u,\"frame_fbp\":%u,\"frame_fbw\":%u,"
                 "\"frame_fbmsk\":%u,\"test\":%llu,\"alpha\":%llu,\"zbuf\":%llu,\"clamp\":%llu,"
                 "\"xyoffset\":%llu,\"vertices\":%u,\"drawing_vertices\":%u,"
                 "\"extent\":[%.3f,%.3f,%.3f,%.3f]}",
                 state.prim_value(), state.tex0_tbp0(), state.tex0_psm(), state.frame_fbp(), state.frame_fbw(),
                 state.frame_fbmsk(), static_cast<unsigned long long>(state.test),
                 static_cast<unsigned long long>(state.alpha), static_cast<unsigned long long>(state.zbuf),
                 static_cast<unsigned long long>(state.clamp), static_cast<unsigned long long>(state.xyoffset),
                 batch.vertices, batch.drawing_vertices, batch.min_x, batch.max_x, batch.min_y, batch.max_y);
    first = false;
  }
  std::fprintf(file, "],\"composite\":[");
  first = true;
  for (const GsWetCompositeInfo &info : chains) {
    std::fprintf(file, "%s{\"scene_vblank\":%llu,\"stream_vblank\":%llu,\"found\":%s,\"refusal\":",
                 first ? "" : ",", static_cast<unsigned long long>(info.scene_vblank),
                 static_cast<unsigned long long>(info.stream_vblank), info.found ? "true" : "false");
    json_string(file, info.refusal);
    std::fprintf(file, ",\"alpha_clear_sequence\":%u,\"composite_sequence\":%u,\"post_darken_sequence\":%u,"
                       "\"post_brighten_sequence\":%u,\"unrecognised_full_screen\":%llu}",
                 info.alpha_clear != nullptr ? info.alpha_clear->sequence : 0,
                 info.composite != nullptr ? info.composite->sequence : 0,
                 info.post_darken != nullptr ? info.post_darken->sequence : 0,
                 info.post_brighten != nullptr ? info.post_brighten->sequence : 0,
                 static_cast<unsigned long long>(info.unrecognised_full_screen));
    first = false;
  }
  std::fprintf(file, "]}\n");
  std::fclose(file);
  std::printf("census JSON written to %s\n", json_path);
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: gt4_coregs3_gs --stats <file.gt4gs>\n"
                 "       gt4_coregs3_gs --list <file.gt4gs>\n"
                 "       gt4_coregs3_gs --dump-frame <index> <file.gt4gs>\n"
                 "       gt4_coregs3_gs --check-pass <file.gt4gs> <capture.gt4scene>\n");
    return 2;
  }
  const std::string command = argv[1];
  if (command == "--stats") {
    command_stats(argv[2]);
  } else if (command == "--list") {
    command_list(argv[2]);
  } else if (command == "--dump-frame") {
    if (argc < 4) {
      std::fprintf(stderr, "gt4_coregs3_gs: --dump-frame needs an index and a file\n");
      return 2;
    }
    command_dump_frame(argv[3], static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10)));
  } else if (command == "--census") {
    const char *json_path = nullptr;
    for (int index = 3; index + 1 < argc; ++index)
      if (std::string(argv[index]) == "--json") json_path = argv[index + 1];
    command_census(argv[2], json_path);
  } else if (command == "--check-pass") {
    if (argc < 4) {
      std::fprintf(stderr, "gt4_coregs3_gs: --check-pass needs a .gt4gs and a .gt4scene\n");
      return 2;
    }
    command_check_pass(argv[2], argv[3]);
  } else {
    std::fprintf(stderr, "gt4_coregs3_gs: unknown command %s\n", command.c_str());
    return 2;
  }
  return 0;
}
