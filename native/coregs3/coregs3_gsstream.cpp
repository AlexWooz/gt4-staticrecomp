// TASK-090 CoreGS 3.0, phase S1: the `.gt4gs` writer. See coregs3_gsstream.hpp for the format and the threading rules.
//
// The guest path (packet, pass marker, frame boundary) only appends records to a preallocated ring; the file is opened
// at configuration time and written once, header first, at shutdown. That split is the measured lesson of the v5 capture
// (docs/COREGS3-HOOKS.md §7): I/O on the EE timeline changes guest state at 3x, a memory copy does not.
#include "coregs3_gsstream.hpp"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace gt4::coregs3 {
namespace {

struct GsStreamState {
  GsStreamConfig config;
  bool configured = false;
  std::atomic<bool> enabled{false};
  std::FILE *file = nullptr;
  std::vector<std::uint8_t> ring;  // preallocated at configure(); the guest path only writes into it
  std::size_t capacity = 0;        // record bytes the ring holds (max_bytes minus the file header)
  std::size_t used = 0;
  bool file_full = false;
  std::uint32_t file_full_cause = 0;  // GsStreamCause the file bound last hit
  std::atomic<std::uint64_t> vblank{0};
  std::atomic<std::uint32_t> pass{kPassUnknown};
  std::uint32_t sequence = 0;
  bool frame_open = false;
  bool have_first_vblank = false;
  std::uint32_t frame_index = 0;
  std::uint64_t frame_vblank = 0;
  std::uint64_t frame_path2_bytes = 0;
  std::uint64_t frame_path3_bytes = 0;
  std::uint64_t frame_refused_bytes = 0;
  std::uint64_t frame_refused_records = 0;
  std::uint32_t frame_causes = 0;
  bool frame_refused = false;
  GsStreamCounters counters;
};

GsStreamState g;

std::uint8_t *put_u16(std::uint8_t *at, std::uint16_t value) {
  at[0] = static_cast<std::uint8_t>(value & 0xFFu);
  at[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
  return at + 2;
}

std::uint8_t *put_u32(std::uint8_t *at, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) *at++ = static_cast<std::uint8_t>((value >> shift) & 0xFFu);
  return at;
}

std::uint8_t *put_u64(std::uint8_t *at, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) *at++ = static_cast<std::uint8_t>((value >> shift) & 0xFFu);
  return at;
}

void encode_header(std::uint8_t *at, std::uint64_t first_vblank) {
  std::uint8_t *cursor = put_u32(at, kGsStreamMagic);
  cursor = put_u32(cursor, kGsStreamVersion);
  cursor = put_u32(cursor, kCaptureTargetV200);
  cursor = put_u32(cursor, 0u);  // flags: no exact GS state markers in version 1
  cursor = put_u64(cursor, first_vblank);
  put_u64(cursor, 0u);
}

void encode_record_header(std::uint8_t *at, std::uint32_t bytes, std::uint16_t kind, std::uint8_t path,
                          std::uint8_t flags, std::uint32_t pass, std::uint32_t sequence, std::uint64_t vblank,
                          std::uint64_t last_job, std::uint32_t source_pc, std::uint32_t source_ra,
                          std::uint32_t qwc) {
  std::uint8_t *cursor = put_u32(at, bytes);
  cursor = put_u16(cursor, kind);
  *cursor++ = path;
  *cursor++ = flags;
  cursor = put_u32(cursor, pass);
  cursor = put_u32(cursor, sequence);
  cursor = put_u64(cursor, vblank);
  cursor = put_u64(cursor, last_job);
  cursor = put_u32(cursor, source_pc);
  cursor = put_u32(cursor, source_ra);
  cursor = put_u32(cursor, qwc);
  put_u32(cursor, 0u);
}

// Appends one encoded record. Returns false when the file bounds (bytes or record count) would be exceeded; the caller
// counts the refusal. Never allocates, never does I/O.
bool append_record(std::uint16_t kind, std::uint8_t path, std::uint8_t flags, std::uint32_t pass,
                   std::uint64_t vblank, std::uint64_t last_job, std::uint32_t source_pc, std::uint32_t source_ra,
                   std::uint32_t qwc, const std::uint8_t *payload, std::size_t payload_bytes) noexcept {
  const std::size_t total = kGsStreamRecordHeaderBytes + payload_bytes;
  if (g.counters.records >= g.config.max_records) {
    g.file_full = true;
    g.file_full_cause = kGsStreamCauseRecords;
    return false;
  }
  if (g.used > g.capacity || total > g.capacity - g.used) {
    g.file_full = true;
    g.file_full_cause = kGsStreamCauseFileBytes;
    return false;
  }
  std::uint8_t *at = g.ring.data() + g.used;
  encode_record_header(at, static_cast<std::uint32_t>(payload_bytes), kind, path, flags, pass, g.sequence++, vblank,
                       last_job, source_pc, source_ra, qwc);
  if (payload_bytes != 0) std::memcpy(at + kGsStreamRecordHeaderBytes, payload, payload_bytes);
  g.used += total;
  g.counters.records += 1;
  g.counters.bytes += payload_bytes;
  return true;
}

// The first VBlank of the file: the first frame opened, or the range's first VBlank when no frame opened.
std::uint64_t header_first_vblank() noexcept {
  if (g.have_first_vblank) return g.counters.first_vblank;
  if (g.config.range) return g.config.first_vblank;
  return 0;
}

void close_frame() noexcept {
  if (!g.frame_open) return;
  g.frame_open = false;
  if (g.frame_refused) {
    std::uint8_t payload[32] = {};
    put_u64(payload, g.frame_refused_bytes);
    put_u64(payload + 8, g.frame_refused_records);
    put_u32(payload + 16, g.frame_causes);
    append_record(kGsStreamKindOverflow, 0, kGsStreamFlagRefused, 0, g.frame_vblank, 0, 0, 0, 0, payload,
                  sizeof payload);
    g.counters.refused_frames += 1;
  }
  std::uint8_t payload[32] = {};
  put_u32(payload, g.frame_index);
  put_u32(payload + 4, 0u);
  put_u64(payload + 8, g.frame_path2_bytes);
  put_u64(payload + 16, g.frame_path3_bytes);
  const std::uint64_t total = g.frame_path2_bytes + g.frame_path3_bytes;
  if (total > g.counters.largest_frame) g.counters.largest_frame = static_cast<std::uint32_t>(total);
  if (append_record(kGsStreamKindFrameEnd, 0, g.frame_refused ? kGsStreamFlagRefused : 0u, 0, g.frame_vblank, 0, 0, 0,
                    0, payload, sizeof payload)) {
    g.counters.frames_closed += 1;
  } else {
    g.counters.frames_without_end += 1;
  }
}

void reset_state() {
  g.config = GsStreamConfig{};
  g.configured = false;
  g.enabled.store(false, std::memory_order_relaxed);
  g.file = nullptr;
  g.ring.clear();
  g.ring.shrink_to_fit();
  g.capacity = 0;
  g.used = 0;
  g.file_full = false;
  g.file_full_cause = 0;
  g.vblank.store(0, std::memory_order_relaxed);
  g.pass.store(kPassUnknown, std::memory_order_relaxed);
  g.sequence = 0;
  g.frame_open = false;
  g.have_first_vblank = false;
  g.frame_index = 0;
  g.frame_vblank = 0;
  g.frame_path2_bytes = 0;
  g.frame_path3_bytes = 0;
  g.frame_refused_bytes = 0;
  g.frame_refused_records = 0;
  g.frame_causes = 0;
  g.frame_refused = false;
  g.counters = GsStreamCounters{};
}

std::uint64_t parse_bound(const char *text, std::uint64_t fallback, const char *name) {
  if (text == nullptr || *text == '\0') return fallback;
  char *end = nullptr;
  errno = 0;
  const unsigned long long value = std::strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value == 0) {
    std::fprintf(stderr, "[coregs3] %s=%s is not a positive byte count; using %llu\n", name, text,
                 static_cast<unsigned long long>(fallback));
    return fallback;
  }
  return value;
}

}  // namespace

bool configure_gsstream(const GsStreamConfig &config, std::string &error) {
  if (g.configured) {
    error = "the .gt4gs recorder is already configured";
    return false;
  }
  if (config.path.empty()) {
    error = "empty .gt4gs path";
    return false;
  }
  if (config.max_bytes < kGsStreamHeaderBytes + kGsStreamRecordHeaderBytes || config.max_frame_bytes == 0 ||
      config.max_records == 0 || config.max_frames == 0) {
    error = "inconsistent .gt4gs bounds";
    return false;
  }
  std::FILE *file = std::fopen(config.path.c_str(), "wb");
  if (file == nullptr) {
    error = "cannot open " + config.path + ": " + std::strerror(errno);
    return false;
  }
  g.config = config;
  g.capacity = static_cast<std::size_t>(config.max_bytes - kGsStreamHeaderBytes);
  g.ring.resize(g.capacity);
  g.used = 0;
  g.file_full = false;
  g.file = file;
  g.configured = true;
  g.counters.active = true;
  g.enabled.store(true, std::memory_order_relaxed);
  std::fprintf(stderr, "[coregs3] GS stream %s (bound %llu MB, %llu B/frame, %llu records, %llu frames)\n",
               config.path.c_str(), static_cast<unsigned long long>(config.max_bytes >> 20),
               static_cast<unsigned long long>(config.max_frame_bytes),
               static_cast<unsigned long long>(config.max_records),
               static_cast<unsigned long long>(config.max_frames));
  return true;
}

void configure_gsstream_from_environment() {
  if (g.configured) return;
  const char *path = std::getenv("GT4_COREGS3_GS");
  if (path == nullptr || *path == '\0') return;
  GsStreamConfig config;
  config.path = path;
  const char *range = std::getenv("GT4_COREGS3_GS_VBLANKS");
  if (range == nullptr || *range == '\0') range = std::getenv("GT4_COREGS3_CAPTURE_VBLANKS");
  if (range != nullptr && *range != '\0') {
    char *end = nullptr;
    const long long first = std::strtoll(range, &end, 10);
    if (end != nullptr && *end == '-') {
      const long long last = std::strtoll(end + 1, nullptr, 10);
      if (first >= 0 && last >= first) {
        config.first_vblank = static_cast<std::uint64_t>(first);
        config.last_vblank = static_cast<std::uint64_t>(last);
        config.range = true;
      }
    }
    if (!config.range)
      std::fprintf(stderr, "[coregs3] GT4_COREGS3_GS_VBLANKS=%s is not a-b; capturing the first frames\n", range);
  }
  config.max_bytes = parse_bound(std::getenv("GT4_COREGS3_GS_MAX_BYTES"), kGsStreamDefaultMaxBytes,
                                 "GT4_COREGS3_GS_MAX_BYTES");
  config.max_frame_bytes = parse_bound(std::getenv("GT4_COREGS3_GS_MAX_FRAME_BYTES"), kGsStreamDefaultMaxFrameBytes,
                                       "GT4_COREGS3_GS_MAX_FRAME_BYTES");
  config.max_records = parse_bound(std::getenv("GT4_COREGS3_GS_MAX_RECORDS"), kGsStreamDefaultMaxRecords,
                                   "GT4_COREGS3_GS_MAX_RECORDS");
  config.max_frames = parse_bound(std::getenv("GT4_COREGS3_GS_MAX_FRAMES"), kGsStreamDefaultMaxFrames,
                                  "GT4_COREGS3_GS_MAX_FRAMES");
  std::string error;
  if (!configure_gsstream(config, error)) {
    std::fprintf(stderr, "[coregs3] %s\n", error.c_str());
    std::exit(2);
  }
}

bool gsstream_active() noexcept { return g.enabled.load(std::memory_order_relaxed); }

void gsstream_packet(int path, const std::uint8_t *data, std::size_t bytes, std::uint32_t qwc, bool signals,
                     std::uint64_t last_job, std::uint32_t source_pc, std::uint32_t source_ra) noexcept {
  if (!g.enabled.load(std::memory_order_relaxed)) return;
  // PATH1 belongs to the VU1 tap; anything outside 2..3 is not this capture's traffic.
  if (path != 2 && path != 3) return;
  if (data == nullptr || bytes == 0 || bytes % 16u != 0 || static_cast<std::size_t>(qwc) * 16u != bytes) {
    g.counters.refused_records += 1;
    g.counters.refused_bytes += bytes;
    g.counters.causes |= kGsStreamCauseMalformed;
    if (g.frame_open) {
      g.frame_refused = true;
      g.frame_causes |= kGsStreamCauseMalformed;
      g.frame_refused_bytes += bytes;
      g.frame_refused_records += 1;
    }
    return;
  }
  const std::uint64_t vblank = g.vblank.load(std::memory_order_relaxed);
  if (g.config.range && (vblank < g.config.first_vblank || vblank > g.config.last_vblank)) {
    g.counters.skipped_outside_window += 1;
    return;
  }
  if (!g.frame_open) {
    g.counters.skipped_outside_frame += 1;
    return;
  }
  if (g.file_full) {
    const std::uint32_t cause = g.file_full_cause != 0 ? g.file_full_cause : kGsStreamCauseFileBytes;
    g.counters.refused_records += 1;
    g.counters.refused_bytes += bytes;
    g.counters.causes |= cause;
    g.frame_refused = true;
    g.frame_causes |= cause;
    g.frame_refused_bytes += bytes;
    g.frame_refused_records += 1;
    return;
  }
  if (g.frame_path2_bytes + g.frame_path3_bytes + bytes > g.config.max_frame_bytes) {
    g.counters.refused_records += 1;
    g.counters.refused_bytes += bytes;
    g.counters.causes |= kGsStreamCauseFrameBytes;
    g.frame_refused = true;
    g.frame_causes |= kGsStreamCauseFrameBytes;
    g.frame_refused_bytes += bytes;
    g.frame_refused_records += 1;
    return;
  }
  const std::uint32_t pass = g.pass.load(std::memory_order_relaxed);
  const std::uint8_t flags = signals ? kGsStreamFlagSignals : 0u;
  if (!append_record(kGsStreamKindGif, static_cast<std::uint8_t>(path), flags, pass, vblank, last_job, source_pc,
                     source_ra, qwc, data, bytes)) {
    const std::uint32_t cause = g.file_full_cause != 0 ? g.file_full_cause : kGsStreamCauseFileBytes;
    g.counters.refused_records += 1;
    g.counters.refused_bytes += bytes;
    g.counters.causes |= cause;
    g.frame_refused = true;
    g.frame_causes |= cause;
    g.frame_refused_bytes += bytes;
    g.frame_refused_records += 1;
    return;
  }
  g.counters.gif_records[path] += 1;
  g.counters.gif_bytes[path] += bytes;
  const std::uint32_t pass_index = pass < kPassCount ? pass : kPassUnknown;
  g.counters.pass_gif_records[pass_index] += 1;
  g.counters.pass_gif_bytes[pass_index] += bytes;
  if (signals) g.counters.signal_records += 1;
  if (pass == kPassUnknown) g.counters.unknown_pass_records += 1;
  if (bytes > g.counters.largest_record) g.counters.largest_record = static_cast<std::uint32_t>(bytes);
  if (path == 2) g.frame_path2_bytes += bytes;
  else g.frame_path3_bytes += bytes;
}

void gsstream_pass_marker(std::uint32_t pass, bool begin,
                          const std::uint64_t (&registers)[kGsStreamPassRegisters], std::uint64_t last_job,
                          std::uint32_t source_pc, std::uint32_t source_ra) noexcept {
  if (!g.enabled.load(std::memory_order_relaxed)) return;
  if (pass >= kPassCount) {
    pass = kPassUnknown;
    g.counters.bad_pass_markers += 1;
  }
  if (!begin) {
    const std::uint32_t open = g.pass.load(std::memory_order_relaxed);
    if (open == kPassUnknown || open != pass) {
      g.counters.mismatched_pass_ends += 1;
      return;  // never close a record a different marker opened
    }
  }
  g.pass.store(begin ? pass : kPassUnknown, std::memory_order_relaxed);
  if (!g.frame_open) {
    g.counters.skipped_outside_frame += 1;
    return;
  }
  std::uint8_t payload[kGsStreamPassPayloadBytes] = {};
  for (std::size_t index = 0; index < kGsStreamPassRegisters; ++index)
    put_u64(payload + index * 8u, registers[index]);
  if (append_record(begin ? kGsStreamKindPassBegin : kGsStreamKindPassEnd, 0, 0, pass,
                    g.vblank.load(std::memory_order_relaxed), last_job, source_pc, source_ra, 0, payload,
                    sizeof payload)) {
    g.counters.pass_records += 1;
  }
}

void gsstream_frame(std::uint64_t vblank) noexcept {
  if (!g.enabled.load(std::memory_order_relaxed)) return;
  close_frame();
  // The hooks reset their pass at every VBlank (reset_frame sets g.current_pass to unknown), so a pass that never ended
  // before the boundary does not leak into the next frame's packets here either.
  g.pass.store(kPassUnknown, std::memory_order_relaxed);
  g.vblank.store(vblank, std::memory_order_relaxed);
  if (g.file_full) return;
  if (g.config.range && (vblank < g.config.first_vblank || vblank > g.config.last_vblank)) return;
  if (g.counters.frames >= g.config.max_frames) {
    g.counters.skipped_frames += 1;
    return;
  }
  std::uint8_t payload[32] = {};
  put_u32(payload, g.counters.frames);
  if (!append_record(kGsStreamKindFrameBegin, 0, 0, 0, vblank, 0, 0, 0, 0, payload, sizeof payload)) return;
  g.frame_open = true;
  g.frame_index = static_cast<std::uint32_t>(g.counters.frames);
  g.frame_vblank = vblank;
  g.frame_path2_bytes = 0;
  g.frame_path3_bytes = 0;
  g.frame_refused_bytes = 0;
  g.frame_refused_records = 0;
  g.frame_causes = 0;
  g.frame_refused = false;
  if (!g.have_first_vblank) {
    g.have_first_vblank = true;
    g.counters.first_vblank = vblank;
  }
  g.counters.frames += 1;
}

void gsstream_shutdown() {
  if (!g.configured) return;
  close_frame();
  g.enabled.store(false, std::memory_order_relaxed);
  if (g.file != nullptr) {
    std::uint8_t header[kGsStreamHeaderBytes] = {};
    encode_header(header, header_first_vblank());
    bool ok = std::fwrite(header, 1, sizeof header, g.file) == sizeof header;
    if (g.used != 0 && ok) ok = std::fwrite(g.ring.data(), 1, g.used, g.file) == g.used;
    if (!ok) std::fprintf(stderr, "[coregs3] the .gt4gs file could not be written\n");
    std::fflush(g.file);
    std::fclose(g.file);
    g.file = nullptr;
    g.counters.file_bytes = sizeof header + g.used;
    std::fprintf(stderr, "[coregs3] GS stream written %llu B (%llu records, %llu gif records, refused %llu)\n",
                 static_cast<unsigned long long>(g.counters.file_bytes),
                 static_cast<unsigned long long>(g.counters.records),
                 static_cast<unsigned long long>(g.counters.gif_records[2] + g.counters.gif_records[3]),
                 static_cast<unsigned long long>(g.counters.refused_records));
  }
  g.configured = false;
}

void gsstream_reset_for_test() {
  if (g.file != nullptr) {
    std::fclose(g.file);
    g.file = nullptr;
  }
  reset_state();
}

const GsStreamCounters &gsstream_counters() noexcept { return g.counters; }

void gsstream_print_report(std::FILE *out) noexcept {
  if (out == nullptr || !g.counters.active) return;
  const GsStreamCounters &c = g.counters;
  std::fprintf(out,
               "[coregs3] gsstream records %llu bytes %llu path2 %llu/%llu path3 %llu/%llu pass_records %llu "
               "frames %llu closed %llu no_end %llu refused_records %llu refused_bytes %llu refused_frames %llu "
               "skipped_window %llu skipped_frame %llu skipped_frames %llu signals %llu unknown_pass %llu "
               "bad_pass %llu mismatched_ends %llu largest_record %u largest_frame %u causes 0x%x file_bytes %llu\n",
               static_cast<unsigned long long>(c.records), static_cast<unsigned long long>(c.bytes),
               static_cast<unsigned long long>(c.gif_records[2]), static_cast<unsigned long long>(c.gif_bytes[2]),
               static_cast<unsigned long long>(c.gif_records[3]), static_cast<unsigned long long>(c.gif_bytes[3]),
               static_cast<unsigned long long>(c.pass_records), static_cast<unsigned long long>(c.frames),
               static_cast<unsigned long long>(c.frames_closed),
               static_cast<unsigned long long>(c.frames_without_end),
               static_cast<unsigned long long>(c.refused_records),
               static_cast<unsigned long long>(c.refused_bytes),
               static_cast<unsigned long long>(c.refused_frames),
               static_cast<unsigned long long>(c.skipped_outside_window),
               static_cast<unsigned long long>(c.skipped_outside_frame),
               static_cast<unsigned long long>(c.skipped_frames),
               static_cast<unsigned long long>(c.signal_records),
               static_cast<unsigned long long>(c.unknown_pass_records),
               static_cast<unsigned long long>(c.bad_pass_markers),
               static_cast<unsigned long long>(c.mismatched_pass_ends), c.largest_record, c.largest_frame, c.causes,
               static_cast<unsigned long long>(c.file_bytes));
}

}  // namespace gt4::coregs3
