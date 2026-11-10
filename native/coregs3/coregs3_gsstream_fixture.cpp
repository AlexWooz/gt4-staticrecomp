// CTest native_coregs3_gsstream: TASK-090 CoreGS 3.0 phase S1, host only, hand-authored data (no game data).
//
// It pins the `.gt4gs` version 1 writer and reader together:
//   * the round trip: header, record order, sequence, join fields (vblank, pass, last job, guest site, signals), pass
//     register words, frame totals and the writer counters against the parsed file;
//   * the bounds: a frame over its byte cap drops the rest of that frame and keeps the next one (kind 8 + refused
//     frame_end), a file over its byte or record cap stops recording cleanly, a packet outside the VBlank window and a
//     VBlank after GT4_COREGS3_GS_MAX_FRAMES are skipped (not refused), and a malformed packet is refused;
//   * the reader refusals: bad magic, unknown version, nonzero header/record reserved words, reserved kinds, unknown
//     kinds, a record over the end, a GIF qwc mismatch, a bad pass id and a trailing partial record.
//
// The writer source is compiled into this fixture directly (gt4_mtvu_runtime also carries it for the game, but that
// library pulls the donor worker runtime, which a host control must not link).
#include "coregs3_gsstream.hpp"
#include "coregs3_gsstream_file.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace gt4::coregs3;

void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

std::vector<std::uint8_t> packet(std::uint32_t qwc, std::uint8_t seed) {
  std::vector<std::uint8_t> out(static_cast<std::size_t>(qwc) * 16u);
  for (std::size_t index = 0; index < out.size(); ++index)
    out[index] = static_cast<std::uint8_t>((seed + index * 7u) & 0xFFu);
  return out;
}

std::string path_in(const std::filesystem::path &folder, const char *name) {
  return (folder / name).string();
}

bool configure(const std::filesystem::path &folder, const char *name, std::uint64_t max_bytes,
               std::uint64_t max_frame_bytes, std::uint64_t max_records, std::uint64_t max_frames, bool range,
               std::uint64_t first, std::uint64_t last) {
  GsStreamConfig config;
  config.path = path_in(folder, name);
  config.max_bytes = max_bytes;
  config.max_frame_bytes = max_frame_bytes;
  config.max_records = max_records;
  config.max_frames = max_frames;
  config.range = range;
  config.first_vblank = first;
  config.last_vblank = last;
  std::string error;
  const bool ok = configure_gsstream(config, error);
  if (!ok) std::fprintf(stderr, "configure: %s\n", error.c_str());
  return ok;
}

std::vector<std::uint8_t> read_all(const std::string &path) {
  std::FILE *file = std::fopen(path.c_str(), "rb");
  require(file != nullptr, "open written file");
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  require(size >= 0, "size written file");
  std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
  require(data.empty() || std::fread(data.data(), 1, data.size(), file) == data.size(), "read written file");
  std::fclose(file);
  return data;
}

void write_all(const std::string &path, const std::vector<std::uint8_t> &data) {
  std::FILE *file = std::fopen(path.c_str(), "wb");
  require(file != nullptr, "open corrupted file");
  require(data.empty() || std::fwrite(data.data(), 1, data.size(), file) == data.size(), "write corrupted file");
  std::fclose(file);
}

// Expects the reader to refuse `data` with a non-empty reason.
void expect_refused(const std::vector<std::uint8_t> &data, const char *label) {
  const std::string path = std::filesystem::temp_directory_path().string() + "/gt4gs_refused.bin";
  write_all(path, data);
  std::vector<std::uint8_t> image;
  GsStreamFileHeader header;
  std::vector<GsStreamFileRecord> records;
  std::string error;
  const bool ok = read_gs_stream_file(path, image, header, records, error);
  std::remove(path.c_str());
  if (ok || error.empty()) throw std::runtime_error(std::string("reader accepted a bad file: ") + label);
}

void round_trip(const std::filesystem::path &folder) {
  gsstream_reset_for_test();
  require(configure(folder, "round.gt4gs", 1u << 20, 1u << 18, 4096, 4, true, 7398, 7399), "configure round trip");
  std::uint64_t registers_begin[9] = {0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8};
  std::uint64_t registers_end[9] = {0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8};
  std::uint64_t registers_course[9] = {0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8};
  std::uint64_t registers_second[9] = {0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8};

  const std::vector<std::uint8_t> first_packet = packet(2, 0x10);
  const std::vector<std::uint8_t> second_packet = packet(1, 0x20);
  const std::vector<std::uint8_t> third_packet = packet(4, 0x30);
  const std::vector<std::uint8_t> fourth_packet = packet(3, 0x40);

  gsstream_frame(7398);
  gsstream_pass_marker(kPassSky, true, registers_begin, 10, 0x00100000u, 0x00100004u);
  gsstream_packet(3, first_packet.data(), first_packet.size(), 2, true, 10, 0x00100008u, 0x0010000Cu);
  gsstream_packet(2, second_packet.data(), second_packet.size(), 1, false, 11, 0x00100010u, 0x00100014u);
  gsstream_pass_marker(kPassSky, false, registers_end, 11, 0x00100018u, 0x0010001Cu);
  gsstream_pass_marker(kPassCourse, true, registers_course, 12, 0x00100020u, 0x00100024u);
  gsstream_packet(3, third_packet.data(), third_packet.size(), 4, false, 12, 0x00100028u, 0x0010002Cu);
  gsstream_frame(7399);  // closes 7398 and opens 7399
  gsstream_pass_marker(kPassCourse, true, registers_second, 13, 0x00100030u, 0x00100034u);
  gsstream_packet(3, fourth_packet.data(), fourth_packet.size(), 3, false, 13, 0x00100038u, 0x0010003Cu);
  gsstream_pass_marker(kPassCourse, false, registers_second, 13, 0x00100040u, 0x00100044u);
  gsstream_frame(7400);  // closes 7399; 7400 is outside the window, so no frame opens
  const GsStreamCounters before = gsstream_counters();
  require(before.records == 13, "round trip record count");
  require(before.gif_records[2] == 1 && before.gif_bytes[2] == 16, "round trip path2 counters");
  require(before.gif_records[3] == 3 && before.gif_bytes[3] == 144, "round trip path3 counters");
  require(before.pass_records == 5, "round trip pass records");
  require(before.frames == 2 && before.frames_closed == 2, "round trip frames");
  require(before.pass_gif_records[kPassSky] == 2 && before.pass_gif_bytes[kPassSky] == 48, "round trip sky pass");
  require(before.pass_gif_records[kPassCourse] == 2 && before.pass_gif_bytes[kPassCourse] == 112,
          "round trip course pass");
  require(before.largest_record == 64 && before.largest_frame == 112, "round trip largest record/frame");
  require(before.skipped_outside_window == 0 && before.skipped_frames == 0, "round trip no skips");
  gsstream_shutdown();
  require(before.file_bytes == 0, "the file bytes are only known after shutdown");
  const GsStreamCounters written = gsstream_counters();
  require(written.file_bytes > kGsStreamHeaderBytes, "round trip file written");

  std::vector<std::uint8_t> data;
  GsStreamFileHeader header;
  std::vector<GsStreamFileRecord> records;
  std::string error;
  require(read_gs_stream_file(path_in(folder, "round.gt4gs"), data, header, records, error), error.c_str());
  require(header.version == kGsStreamVersion && header.target == kCaptureTargetV200 && header.flags == 0,
          "round trip header");
  require(header.first_vblank == 7398, "round trip first vblank");
  require(data.size() == written.file_bytes, "round trip file size");
  require(records.size() == 13, "round trip parsed records");
  const std::uint16_t expected_kinds[13] = {
      kGsStreamKindFrameBegin, kGsStreamKindPassBegin,  kGsStreamKindGif,      kGsStreamKindGif,
      kGsStreamKindPassEnd,    kGsStreamKindPassBegin,  kGsStreamKindGif,      kGsStreamKindFrameEnd,
      kGsStreamKindFrameBegin, kGsStreamKindPassBegin,  kGsStreamKindGif,      kGsStreamKindPassEnd,
      kGsStreamKindFrameEnd};
  for (std::size_t index = 0; index < records.size(); ++index) {
    require(records[index].kind == expected_kinds[index], "round trip record order");
    require(records[index].sequence == index, "round trip sequence");
  }
  require(records[2].path == 3 && records[2].bytes == 32 && records[2].qwc == 2, "round trip first gif header");
  require(records[2].vblank == 7398 && records[2].last_job == 10, "round trip first gif join fields");
  require(records[2].pass == kPassSky, "round trip first gif pass");
  require((records[2].flags & kGsStreamFlagSignals) != 0, "round trip signal flag");
  require(records[2].source_pc == 0x00100008u && records[2].source_ra == 0x0010000Cu, "round trip source site");
  require(std::memcmp(data.data() + records[2].offset, first_packet.data(), first_packet.size()) == 0,
          "round trip first gif payload");
  require(records[3].path == 2 && records[3].bytes == 16 && records[3].pass == kPassSky, "round trip path2 gif");
  require((records[3].flags & kGsStreamFlagSignals) == 0, "round trip path2 has no signals");
  std::uint64_t words[9] = {};
  require(gs_stream_pass_words(data, records[1], words), "round trip pass words read");
  for (std::size_t index = 0; index < 9; ++index) require(words[index] == registers_begin[index], "pass begin words");
  require(records[1].pass == kPassSky && records[1].vblank == 7398, "round trip pass begin fields");
  require(gs_stream_pass_words(data, records[4], words), "round trip pass end words read");
  for (std::size_t index = 0; index < 9; ++index) require(words[index] == registers_end[index], "pass end words");
  std::uint32_t frame_index = 0;
  std::uint64_t path2 = 0, path3 = 0;
  require(gs_stream_frame_totals(data, records[7], frame_index, path2, path3), "round trip frame totals read");
  require(frame_index == 0 && path2 == 16 && path3 == 96, "round trip frame 0 totals");
  require(gs_stream_frame_totals(data, records[12], frame_index, path2, path3), "round trip second frame totals");
  require(frame_index == 1 && path2 == 0 && path3 == 48, "round trip frame 1 totals");
}

void frame_cap(const std::filesystem::path &folder) {
  gsstream_reset_for_test();
  require(configure(folder, "frame-cap.gt4gs", 1u << 20, 32u, 4096, 8, true, 100, 101), "configure frame cap");
  const std::vector<std::uint8_t> small = packet(1, 1);
  const std::vector<std::uint8_t> too_big = packet(3, 2);  // 48 > the 32-byte frame cap
  std::uint64_t registers[9] = {};
  gsstream_frame(100);
  gsstream_packet(3, small.data(), small.size(), 1, false, 0, 0, 0);
  gsstream_packet(3, too_big.data(), too_big.size(), 3, false, 0, 0, 0);
  gsstream_frame(101);  // close 100 (refused), open 101
  gsstream_packet(3, small.data(), small.size(), 1, false, 0, 0, 0);
  gsstream_frame(102);  // close 101 (clean), 102 outside the range
  gsstream_pass_marker(kPassSky, true, registers, 0, 0, 0);  // outside any open frame: skipped
  const GsStreamCounters counters = gsstream_counters();
  require(counters.refused_records == 1 && counters.refused_bytes == 48, "frame cap refused counters");
  require(counters.refused_frames == 1, "frame cap refused frame counter");
  require((counters.causes & kGsStreamCauseFrameBytes) != 0, "frame cap cause");
  require(counters.frames == 2 && counters.frames_closed == 2, "frame cap keeps later frames");
  require(counters.skipped_outside_frame == 1, "marker outside a frame is skipped");
  gsstream_shutdown();

  std::vector<std::uint8_t> data;
  GsStreamFileHeader header;
  std::vector<GsStreamFileRecord> records;
  std::string error;
  require(read_gs_stream_file(path_in(folder, "frame-cap.gt4gs"), data, header, records, error), error.c_str());
  bool overflow = false;
  for (const GsStreamFileRecord &record : records)
    if (record.kind == kGsStreamKindOverflow) {
      std::uint64_t bytes = 0, count = 0;
      std::uint32_t causes = 0;
      require(gs_stream_overflow_totals(data, record, bytes, count, causes), "overflow record parse");
      require(bytes == 48 && count == 1 && causes == kGsStreamCauseFrameBytes, "overflow record contents");
      require((record.flags & kGsStreamFlagRefused) != 0, "overflow record refused flag");
      overflow = true;
    }
  require(overflow, "frame cap writes an overflow record");
  // The refused frame's end record carries the refusal flag; the later frame's end does not.
  std::vector<const GsStreamFileRecord *> frame_ends;
  for (const GsStreamFileRecord &record : records)
    if (record.kind == kGsStreamKindFrameEnd) frame_ends.push_back(&record);
  require(frame_ends.size() == 2, "frame cap frame ends");
  require((frame_ends[0]->flags & kGsStreamFlagRefused) != 0, "refused frame_end flag");
  require((frame_ends[1]->flags & kGsStreamFlagRefused) == 0, "clean frame_end flag");
}

void file_cap(const std::filesystem::path &folder) {
  gsstream_reset_for_test();
  // Frame begin (48 + 32) + two packets (48 + 16 each) = 208 record bytes; the 32-byte file header brings the file to
  // 240. A third packet (48 + 16) and the frame_end (48 + 32) do not fit.
  require(configure(folder, "file-cap.gt4gs", 240u, 1u << 10, 4096, 8, true, 100, 110), "configure file cap");
  const std::vector<std::uint8_t> small = packet(1, 3);
  gsstream_frame(100);
  gsstream_packet(3, small.data(), small.size(), 1, false, 0, 0, 0);
  gsstream_packet(3, small.data(), small.size(), 1, false, 0, 0, 0);
  gsstream_packet(3, small.data(), small.size(), 1, false, 0, 0, 0);
  gsstream_shutdown();
  const GsStreamCounters counters = gsstream_counters();
  require(counters.refused_records == 1 && counters.refused_bytes == 16, "file cap refused counters");
  require((counters.causes & kGsStreamCauseFileBytes) != 0, "file cap cause");
  require(counters.frames_without_end == 1, "file cap leaves the frame without an end record");
  require(counters.file_bytes <= 240u, "file cap keeps the file within its bound");
  require(counters.file_bytes > kGsStreamHeaderBytes, "file cap still writes the captured prefix");

  std::vector<std::uint8_t> data;
  GsStreamFileHeader header;
  std::vector<GsStreamFileRecord> records;
  std::string error;
  require(read_gs_stream_file(path_in(folder, "file-cap.gt4gs"), data, header, records, error), error.c_str());
  require(records.size() == 3, "file cap writes the frame begin and two packets");
}

void record_cap(const std::filesystem::path &folder) {
  gsstream_reset_for_test();
  require(configure(folder, "record-cap.gt4gs", 1u << 20, 1u << 10, 3u, 8, true, 100, 110), "configure record cap");
  const std::vector<std::uint8_t> small = packet(1, 4);
  std::uint64_t registers[9] = {};
  gsstream_frame(100);                                     // record 1
  gsstream_packet(3, small.data(), small.size(), 1, false, 0, 0, 0);  // record 2
  gsstream_pass_marker(kPassSky, true, registers, 0, 0, 0);           // record 3
  gsstream_packet(3, small.data(), small.size(), 1, false, 0, 0, 0);  // refused
  gsstream_shutdown();
  const GsStreamCounters counters = gsstream_counters();
  require(counters.refused_records == 1, "record cap refused counter");
  require((counters.causes & kGsStreamCauseRecords) != 0, "record cap cause");
}

void window_and_frames(const std::filesystem::path &folder) {
  gsstream_reset_for_test();
  require(configure(folder, "window.gt4gs", 1u << 20, 1u << 10, 4096, 2, true, 100, 110), "configure window");
  const std::vector<std::uint8_t> small = packet(1, 5);
  gsstream_frame(99);  // outside the range: no frame opens
  gsstream_packet(3, small.data(), small.size(), 1, false, 0, 0, 0);
  gsstream_frame(100);
  gsstream_packet(3, small.data(), small.size(), 1, false, 0, 0, 0);
  gsstream_frame(101);
  gsstream_packet(3, small.data(), small.size(), 1, false, 0, 0, 0);
  gsstream_frame(102);  // max frames reached: 102..110 are in range but not opened
  gsstream_frame(103);
  gsstream_packet(3, small.data(), small.size(), 1, false, 0, 0, 0);
  gsstream_shutdown();
  const GsStreamCounters counters = gsstream_counters();
  require(counters.skipped_outside_window == 1, "window skips packets outside the VBlank range");
  require(counters.skipped_outside_frame == 1, "window skips a packet after the frame cap");
  require(counters.frames == 2 && counters.frames_closed == 2, "window frame cap");
  require(counters.skipped_frames == 2, "window frame cap rows");
  require(counters.refused_records == 0, "window skips are not refusals");
  require(counters.first_vblank == 100, "window first vblank");
}

void malformed_packet(const std::filesystem::path &folder) {
  gsstream_reset_for_test();
  require(configure(folder, "malformed.gt4gs", 1u << 20, 1u << 10, 4096, 8, true, 100, 110), "configure malformed");
  const std::vector<std::uint8_t> small = packet(1, 6);
  gsstream_frame(100);
  gsstream_packet(3, small.data(), small.size(), 2, false, 0, 0, 0);  // qwc does not match the bytes
  gsstream_shutdown();
  const GsStreamCounters counters = gsstream_counters();
  require(counters.refused_records == 1 && (counters.causes & kGsStreamCauseMalformed) != 0, "malformed packet refused");
}

void reader_refusals(const std::filesystem::path &folder) {
  gsstream_reset_for_test();
  require(configure(folder, "refusals.gt4gs", 1u << 20, 1u << 10, 4096, 8, true, 100, 110), "configure refusals");
  const std::vector<std::uint8_t> small = packet(1, 7);
  std::uint64_t registers[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  gsstream_frame(100);
  gsstream_pass_marker(kPassSky, true, registers, 0, 0, 0);
  gsstream_packet(3, small.data(), small.size(), 1, false, 0, 0, 0);
  gsstream_shutdown();
  const std::vector<std::uint8_t> valid = read_all(path_in(folder, "refusals.gt4gs"));
  require(valid.size() > 100, "refusal base file");

  std::vector<std::uint8_t> bad = valid;
  bad[0] = 0;  // bad magic
  expect_refused(bad, "bad magic");
  bad = valid;
  bad[4] = 2;  // version 2
  expect_refused(bad, "unknown version");
  bad = valid;
  bad[12] = 1;  // header flags
  expect_refused(bad, "header flags");
  bad = valid;
  bad[24] = 1;  // header reserved word (first_vblank is 16..23)
  expect_refused(bad, "header reserved");
  bad = valid;
  bad[32 + 4] = static_cast<std::uint8_t>(kGsStreamKindGsState);  // reserved kind 6
  expect_refused(bad, "reserved kind 6");
  bad = valid;
  bad[32 + 4] = 9;  // unknown kind
  expect_refused(bad, "unknown kind");
  bad = valid;
  bad[32 + 44] = 1;  // record reserved
  expect_refused(bad, "record reserved");
  bad = valid;
  bad[32] = 17;  // first record payload not a multiple of 16
  expect_refused(bad, "unaligned payload");
  // A record whose length runs past the end: the last record's length is the frame_end at the tail.
  bad = valid;
  const std::size_t tail = bad.size() - kGsStreamRecordHeaderBytes - 32u;
  bad[tail + 0] = 0xFF;
  bad[tail + 1] = 0xFF;
  expect_refused(bad, "record over the end");
  // A GIF qwc mismatch and a bad pass id, both on the GIF record in the middle of the file.
  std::size_t gif_at = 0;
  for (std::size_t at = kGsStreamHeaderBytes; at + kGsStreamRecordHeaderBytes <= valid.size();) {
    const std::uint16_t kind = static_cast<std::uint16_t>(valid[at + 4] | (valid[at + 5] << 8));
    const std::uint32_t bytes = static_cast<std::uint32_t>(valid[at]) |
                                (static_cast<std::uint32_t>(valid[at + 1]) << 8) |
                                (static_cast<std::uint32_t>(valid[at + 2]) << 16) |
                                (static_cast<std::uint32_t>(valid[at + 3]) << 24);
    if (kind == kGsStreamKindGif) gif_at = at;
    at += kGsStreamRecordHeaderBytes + bytes;
  }
  require(gif_at != 0, "refusal base file has a GIF record");
  bad = valid;
  bad[gif_at + 40] = 2;  // qwc 2 for a 16-byte payload
  expect_refused(bad, "GIF qwc mismatch");
  bad = valid;
  bad[gif_at + 8] = 99;  // pass outside the PassId table
  expect_refused(bad, "bad GIF pass");
  bad = valid;
  bad.resize(valid.size() - 40);  // the last record header is no longer complete
  expect_refused(bad, "trailing partial record");
}

void bad_pass_marker(const std::filesystem::path &folder) {
  gsstream_reset_for_test();
  require(configure(folder, "bad-pass.gt4gs", 1u << 20, 1u << 10, 4096, 8, true, 100, 110), "configure bad pass");
  const std::vector<std::uint8_t> small = packet(1, 8);
  std::uint64_t registers[9] = {};
  gsstream_frame(100);
  gsstream_pass_marker(9999, true, registers, 0, 0, 0);  // out of the PassId table: recorded as unknown, counted
  gsstream_packet(3, small.data(), small.size(), 1, false, 0, 0, 0);
  gsstream_shutdown();
  const GsStreamCounters counters = gsstream_counters();
  require(counters.bad_pass_markers == 1, "bad pass marker counted");
  require(counters.unknown_pass_records == 1, "bad pass marker records the packet as unknown");
  std::vector<std::uint8_t> data;
  GsStreamFileHeader header;
  std::vector<GsStreamFileRecord> records;
  std::string error;
  require(read_gs_stream_file(path_in(folder, "bad-pass.gt4gs"), data, header, records, error), error.c_str());
  require(records[1].kind == kGsStreamKindPassBegin && records[1].pass == kPassUnknown, "bad marker stored as unknown");
}

}  // namespace

int main(int argc, char **argv) {
  try {
    const std::filesystem::path folder =
        argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::temp_directory_path();
    std::filesystem::create_directories(folder);
    round_trip(folder);
    frame_cap(folder);
    file_cap(folder);
    record_cap(folder);
    window_and_frames(folder);
    malformed_packet(folder);
    reader_refusals(folder);
    bad_pass_marker(folder);
    gsstream_reset_for_test();
    std::printf("coregs3_gsstream: all checks passed\n");
  } catch (const std::exception &error) {
    std::fprintf(stderr, "coregs3_gsstream: %s\n", error.what());
    return 1;
  }
  return 0;
}
