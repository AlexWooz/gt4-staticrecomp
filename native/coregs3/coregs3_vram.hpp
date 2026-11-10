// TASK-090 CoreGS 3.0: the GS local memory (SLM) sidecar of the captured frames.
//
// The `.gt4scene` capture holds the draw state (TEX0/TEX1/CLAMP of every texture record) but no texture pixels: the
// textures live in GS local memory, which the EE hooks do not see. This sidecar is written by the GS worker
// (coregs2/core2_renderer.cpp, Renderer::Flush and Renderer::Coregs3Snapshot) next to a capture so the offline tools can
// decode the real textures.
//
// File layout (little endian), one or more records concatenated. Each record is self-describing, so a reader can skip
// one it does not understand.
//
// Version 1 record (32-byte header, the historical frame-end format):
//
//   header  u32 magic 'GT4V' (0x56345447), u32 version 1, u64 vblank,
//           u32 first_block, u32 block_count, u32 byte_count, u32 reserved (0)
//   bytes   block_count * 256 bytes
//
// Version 2 record (48-byte header, TASK-090 pass textures):
//
//   header  u32 magic, u32 version 2, u64 vblank,
//           u32 first_block, u32 block_count, u32 byte_count, u32 record_kind,
//           u32 pass, u32 label, u32 window_count, u32 reserved (0)
//   windows window_count * { u32 first_block, u32 block_count }
//   bytes   block_count * 256 bytes, the windows concatenated in table order
//
// `first_block` is the lowest window start, `block_count` the sum of the window block counts and `byte_count` exactly
// `block_count * 256`. Record kinds:
//
//   * `kRecordVblank` (0): the GS worker's frame-end Flush of a VBlank. Its `vblank` is the worker's counter at the
//     flush, one ahead of the capture frame (`frame.vblank == record.vblank - 1`): the historical v1 semantic. One
//     window covering the configured block range. `pass` and `label` are 0.
//   * `kRecordPassBegin` (1): a snapshot taken in GS stream order at a K7 pass-begin marker. `vblank` is the capture
//     frame's own VBlank and `pass` its PassId; one window covering the configured block range; `label` is 0. This was
//     the first candidate design: it proved that the race frame streams its world textures *during* the course pass, so
//     every pass-begin snapshot still holds the previous content.
//   * `kRecordDrawTexture` (2): one snapshot per captured draw, taken in GS stream order at the draw's K1 pre-call
//     marker on the EE. It proved the frame uploads its world textures *after* the K1 marker (the marker still holds
//     the previous content), so it is kept as evidence and as a secondary fallback.
//   * `kRecordJobTexture` (3): TASK-090's fix. One snapshot per VU1 job, taken on the GS worker after the job's records
//     have been buffered and before they are executed: at that point every GS command posted before the job (the
//     texture uploads included) has executed and nothing of the job has. `label` is the job id (the tap's source records
//     carry the same id with the guest address of each item's packet, which is how the offline tool maps an item to its
//     job); `vblank` is the GS worker's VBlank counter during the job. The window table lists the texture and palette
//     blocks the job's TEX0 writes bind, copied from the shadow memory.
//
// Why the per-job windows (docs/COREGS3-WORLD-MATERIALS.md and journal 2026-11-10): the frame-end v1 record and every
// pass-begin record carry the last upload of the frame (page 0x36D0 is byte-identical in all of them), and the K1 draw
// marker also predates the upload. Only the per-job snapshot, taken on the GS worker before the job executes, holds the
// content the draw samples.
//
// Blocks are GS block addresses in 0..16383 and a record never wraps the end of the 4 MiB image.
//
// The dump is diagnostic only. It reads the CPU shadow memory from the GS worker (never guest state), copies bounded
// blocks, and writes the file at shutdown, off the guest's timeline (the capture's measured rule,
// docs/COREGS3-HOOKS.md §7). The magic is the file's first byte order: 'G', 'T', '4', 'V'.
//
// This header is dependency-free so both the GS worker (CoreGS 2.0) and the Vulkan-free offline decoder can include it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace gt4::coregs3::vram {

inline constexpr std::uint32_t kMagic = 0x56345447u;  // "GT4V" in file order
inline constexpr std::uint32_t kVersion = 2u;         // what the writer emits
inline constexpr std::uint32_t kLegacyVersion = 1u;   // still parsed: the frame-end-only files keep working
inline constexpr std::size_t kHeaderBytes = 48u;      // version 2
inline constexpr std::size_t kLegacyHeaderBytes = 32u;  // version 1
inline constexpr std::size_t kWindowBytes = 8u;
// A record can name at most 64 windows; a captured draw uses at most kMaxCaptureWindows (16).
inline constexpr std::uint32_t kMaxWindows = 64u;
inline constexpr std::uint32_t kMaxCaptureWindows = 16u;

// Record kinds. A version 1 record is a vblank record.
enum RecordKind : std::uint32_t {
  kRecordVblank = 0,       // the GS worker's frame-end Flush of VBlank `vblank` (the v1 semantic)
  kRecordPassBegin = 1,    // a K7 pass-begin snapshot of frame `vblank`, `pass` naming the marker
  kRecordDrawTexture = 2,  // a K1 draw snapshot of frame `vblank`, `label` naming CapturedDraw::sequence
  kRecordJobTexture = 3    // the GS worker's snapshot at a VU1 job, `label` the job id (low 32 bits)
};
// The pass field is a PassId (0..16 measured); the bound keeps a malformed file from inventing passes.
inline constexpr std::uint32_t kMaxPassId = 63u;

// The measured car texture range of the night race: blocks 0x2BC0..0x3900 (0x2BC0..0x38FF inclusive), palettes
// included (the texture research places the car set at 0x2BC0..0x3848). 0x0D40 blocks = 868,352 bytes.
inline constexpr std::uint32_t kCarFirstBlock = 0x2BC0u;
inline constexpr std::uint32_t kCarBlockCount = 0x0D40u;
inline constexpr std::uint32_t kBlockBytes = 256u;
inline constexpr std::uint32_t kBlocksTotal = 16384u;

// One contiguous run of GS blocks in a record.
struct Window {
  std::uint32_t first_block = 0;
  std::uint32_t block_count = 0;

  bool operator==(const Window &) const = default;
};

struct Header {
  std::uint32_t version = kVersion;
  std::uint64_t vblank = 0;   // the GS worker's VBlank at a vblank record; the capture frame's VBlank otherwise
  std::uint32_t first_block = 0;
  std::uint32_t block_count = 0;
  std::uint32_t kind = kRecordVblank;
  std::uint32_t pass = 0;     // PassId for kRecordPassBegin, 0 otherwise
  std::uint32_t label = 0;    // CapturedDraw::sequence for kRecordDrawTexture, 0 otherwise
  std::uint32_t window_count = 1;
  std::uint32_t reserved = 0;  // 0 in every version; a nonzero value is refused, not guessed
  // Set by decode_header: how many header bytes this record has (32 for version 1, 48 for version 2). Ignored by
  // encode_header, which always writes the current version.
  std::size_t header_bytes = kHeaderBytes;
};

// Serializes a version 2 header. `block_count` must be the sum of the windows' block counts; `window_count` at least 1.
inline void encode_header(std::uint8_t out[kHeaderBytes], const Header &header) {
  const auto put_u32 = [out](std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index)
      out[offset + index] = static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFu);
  };
  put_u32(0, kMagic);
  put_u32(4, kVersion);
  for (std::size_t index = 0; index < 8; ++index)
    out[8 + index] = static_cast<std::uint8_t>((header.vblank >> (8 * index)) & 0xFFu);
  put_u32(16, header.first_block);
  put_u32(20, header.block_count);
  put_u32(24, header.block_count * kBlockBytes);
  put_u32(28, header.kind);
  put_u32(32, header.pass);
  put_u32(36, header.label);
  put_u32(40, header.window_count);
  put_u32(44, header.reserved);
}

// Serializes the version 1 header the first captures used. The fixture keeps it to prove that v1 files still parse.
inline void encode_legacy_header(std::uint8_t out[kLegacyHeaderBytes], const Header &header) {
  const auto put_u32 = [out](std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index)
      out[offset + index] = static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFu);
  };
  put_u32(0, kMagic);
  put_u32(4, kLegacyVersion);
  for (std::size_t index = 0; index < 8; ++index)
    out[8 + index] = static_cast<std::uint8_t>((header.vblank >> (8 * index)) & 0xFFu);
  put_u32(16, header.first_block);
  put_u32(20, header.block_count);
  put_u32(24, header.block_count * kBlockBytes);
  put_u32(28, 0u);
}

inline std::uint32_t read_u32(const std::uint8_t *data, std::size_t offset) {
  return static_cast<std::uint32_t>(data[offset]) | (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

inline std::uint64_t read_u64(const std::uint8_t *data, std::size_t offset) {
  return static_cast<std::uint64_t>(read_u32(data, offset)) |
         (static_cast<std::uint64_t>(read_u32(data, offset + 4)) << 32);
}

inline void encode_window(std::uint8_t *out, const Window &window) {
  for (std::size_t index = 0; index < 4; ++index) {
    out[index] = static_cast<std::uint8_t>((window.first_block >> (8 * index)) & 0xFFu);
    out[4 + index] = static_cast<std::uint8_t>((window.block_count >> (8 * index)) & 0xFFu);
  }
}

inline Window decode_window(const std::uint8_t *data) {
  Window window;
  window.first_block = read_u32(data, 0);
  window.block_count = read_u32(data, 4);
  return window;
}

// Parses one header and checks its version's invariants. `available` is how many bytes of the file remain from `data`;
// the window table and the bytes are checked against it too. `windows` receives the window table (1 entry for v1).
inline bool decode_header(const std::uint8_t *data, std::size_t available, Header &header, Window *windows,
                          std::string &error) {
  if (available < 8) {
    error = "truncated VRAM record header";
    return false;
  }
  if (read_u32(data, 0) != kMagic) {
    error = "not a GT4V sidecar (bad magic)";
    return false;
  }
  header.version = read_u32(data, 4);
  const std::size_t header_bytes =
      header.version == kLegacyVersion ? kLegacyHeaderBytes : header.version == kVersion ? kHeaderBytes : 0;
  if (header_bytes == 0) {
    error = "unsupported GT4V version " + std::to_string(header.version);
    return false;
  }
  if (available < header_bytes) {
    error = "truncated VRAM record header";
    return false;
  }
  header.header_bytes = header_bytes;
  header.vblank = read_u64(data, 8);
  header.first_block = read_u32(data, 16);
  header.block_count = read_u32(data, 20);
  const std::uint32_t byte_count = read_u32(data, 24);
  if (header.version == kLegacyVersion) {
    header.kind = kRecordVblank;
    header.pass = 0;
    header.label = 0;
    header.window_count = 1;
    header.reserved = read_u32(data, 28);
    if (header.reserved != 0) {
      error = "GT4V version 1 reserved word is not zero";
      return false;
    }
  } else {
    header.kind = read_u32(data, 28);
    header.pass = read_u32(data, 32);
    header.label = read_u32(data, 36);
    header.window_count = read_u32(data, 40);
    header.reserved = read_u32(data, 44);
    if (header.kind != kRecordVblank && header.kind != kRecordPassBegin && header.kind != kRecordDrawTexture &&
        header.kind != kRecordJobTexture) {
      error = "GT4V version 2 record kind " + std::to_string(header.kind) + " is not vblank, pass-begin, draw or job";
      return false;
    }
    if (header.kind == kRecordPassBegin && header.pass > kMaxPassId) {
      error = "GT4V version 2 pass id " + std::to_string(header.pass) + " is outside 0..63";
      return false;
    }
    if (header.kind != kRecordDrawTexture && header.kind != kRecordJobTexture && header.label != 0) {
      error = "GT4V version 2 label is not zero on a vblank or pass-begin record";
      return false;
    }
    if (header.window_count == 0 || header.window_count > kMaxWindows) {
      error = "GT4V version 2 window count " + std::to_string(header.window_count) + " is outside 1..64";
      return false;
    }
    if (header.reserved != 0) {
      error = "GT4V version 2 reserved word is not zero";
      return false;
    }
  }
  if (header.block_count > kBlocksTotal || byte_count != header.block_count * kBlockBytes) {
    error = "GT4V record byte count does not match its block range";
    return false;
  }
  const std::size_t table_bytes = header.version == kLegacyVersion ? 0 : header.window_count * kWindowBytes;
  if (available < header_bytes + table_bytes) {
    error = "truncated GT4V window table";
    return false;
  }
  std::uint32_t sum = 0;
  std::uint32_t minimum = kBlocksTotal;
  for (std::uint32_t index = 0; index < header.window_count; ++index) {
    const Window window = header.version == kLegacyVersion
                              ? Window{header.first_block, header.block_count}
                              : decode_window(data + header_bytes + index * kWindowBytes);
    if (window.block_count == 0 || window.first_block >= kBlocksTotal ||
        window.block_count > kBlocksTotal - window.first_block) {
      error = "GT4V record block range is outside the 4 MiB image";
      return false;
    }
    sum += window.block_count;
    if (window.first_block < minimum) minimum = window.first_block;
    if (windows != nullptr) windows[index] = window;
  }
  if (sum != header.block_count) {
    error = "GT4V window table does not sum to the record's block count";
    return false;
  }
  if (header.block_count != 0 && header.first_block != minimum) {
    error = "GT4V record first block is not the lowest window start";
    return false;
  }
  if (available < header_bytes + table_bytes + static_cast<std::size_t>(header.block_count) * kBlockBytes) {
    error = "truncated GT4V record bytes";
    return false;
  }
  return true;
}

// ---- The snapshot channel (TASK-090 pass textures) ----
//
// The K7 pass markers and the K1 draw markers run on the EE thread; the GS shadow memory belongs to the GS stream (the
// worker thread, or the EE thread when GT4_HW_GS_THREAD=0). The EE enqueues a small request into the same stream that
// carries the GIF commands, so the copy lands exactly at the marker's stream position, never from a concurrent thread.
// Until a sink is registered the entry points are one atomic load; an unconfigured capture never registers one.
struct SnapshotRequest {
  std::uint32_t kind = kRecordPassBegin;  // kRecordPassBegin or kRecordDrawTexture
  std::uint32_t pass = 0;                 // PassId for a pass-begin request
  std::uint32_t label = 0;                // CapturedDraw::sequence for a draw request
  std::uint64_t vblank = 0;               // the capture frame's VBlank
  std::uint32_t window_count = 0;         // draw requests: the windows to copy; 0 = the configured full range
  Window windows[kMaxCaptureWindows] = {};
};
using SnapshotSink = void (*)(const SnapshotRequest &request) noexcept;

// Installs (or clears, with nullptr) the sink. Called once at GS backend start on the EE thread.
void set_snapshot_sink(SnapshotSink sink) noexcept;
bool snapshot_sink_installed() noexcept;

// Called from the K7 pass-begin hook (EE thread). Never throws, never allocates, never takes a lock.
void note_pass_begin(std::uint32_t pass, std::uint64_t vblank) noexcept;
// Called from the K1 pre-call hook (EE thread) with the texture/palette blocks the draw's items name.
void note_draw_textures(std::uint32_t sequence, std::uint64_t vblank, const Window *windows,
                        std::uint32_t window_count) noexcept;

// The capture's filters, mirrored on the EE side so a marker the writer would drop is never enqueued (the measured
// run pushes ~1.5M draw and ~0.4M pass markers; the ring should carry only the captured frames').
void set_pass_mark_mask(std::uint64_t pass_mask, bool filter) noexcept;  // filter=false: every pass
void set_draw_marks_enabled(bool enabled) noexcept;
bool draw_marks_enabled() noexcept;

struct SnapshotCounters {
  std::uint64_t requests = 0;  // note_* calls that reached an installed sink
  std::uint64_t delivered = 0; // requests the sink accepted
};
SnapshotCounters snapshot_counters() noexcept;
void reset_snapshot_counters() noexcept;

}  // namespace gt4::coregs3::vram
