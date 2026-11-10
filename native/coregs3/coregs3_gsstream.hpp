// TASK-090 CoreGS 3.0, phase S1: the bounded `.gt4gs` PATH2/PATH3 capture (docs/COREGS3-GSSTREAM-CAPTURE.md §3, §5.4).
//
// Every PATH2/PATH3 byte the game sends to the GS passes exactly one EE-thread function with MTVU on:
// `gt4::mtvu::gif_transfer` (PATH3, and PATH2 when VIF1 is not shadowed) or `gt4::mtvu::shadow_direct` (PATH2 with the
// shadowed VIF1). The recorder copies those packets, with the pass and VBlank they belong to, into a preallocated ring;
// the ring is written to the file once, at shutdown. There is no file I/O, no allocation and no lock on the guest path,
// and every entry point is one predictable branch while the switch is off.
//
// The file is little endian:
//
//   header (32 bytes)
//     u32 magic 'G','T','4','G' in file order (0x47345447)   -- "GT4G"
//     u32 version 1
//     u32 target 0 (kCaptureTargetV200)
//     u32 flags bit0: exact GS state markers present (reserved, 0 in version 1)
//     u64 first_vblank
//     u64 reserved 0; a nonzero value is refused by the reader, not guessed
//
//   records, concatenated, each 16-byte aligned (every payload is a multiple of 16 bytes)
//     u32 bytes       payload byte count
//     u16 kind        1 gif, 2 pass_begin, 3 pass_end, 4 frame_begin, 5 frame_end, 6 gs_state, 7 privileged, 8 overflow
//     u8  path        GIF path 1..3 for kind 1, 0 otherwise
//     u8  flags       bit0 payload carries SIGNAL/FINISH/LABEL, bit1 record is a refusal/partial marker
//     u32 pass        PassId (0 = unknown)
//     u32 sequence    per-file ordinal in acceptance order
//     u64 vblank      coregs3 guest VBlank at acceptance
//     u64 last_job    last VU1 worker job posted at acceptance
//     u32 source_pc   ps2::g_cpu.pc at acceptance
//     u32 source_ra   ps2::g_cpu.r[31] at acceptance
//     u32 qwc         payload quadwords for kind 1, 0 otherwise
//     u32 reserved    0
//     payload:
//       kind 1  the GIF bytes (qwc * 16)
//       kind 2/3  u64 registers[9]: FOGCOL, FRAME, XYOFFSET, ZBUF, SCISSOR, ALPHA, TEST, CLAMP, TEX1, u64 reserved
//       kind 4/5  u32 frame_index, u32 reserved, u64 total_bytes_path2, u64 total_bytes_path3, u64 reserved2
//       kind 8    u64 bytes_refused, u64 records_refused, u32 cause_bits, u32 reserved, u64 reserved2
//
// Kind 6 (exact GS state) and kind 7 (privileged) are reserved for later versions; version 1 never writes them and the
// reader refuses them.
//
// Bounds are capacities, not formats: a packet that would exceed a frame's byte cap is refused and the frame marked,
// never truncated; a file that reaches its byte or record cap stops recording and counts every dropped packet. A packet
// outside the VBlank window (or after the frame cap) is skipped, not refused: the capture simply does not cover it.
//
// Threading: `gsstream_packet`, `gsstream_pass_marker`, `gsstream_frame` and `gsstream_shutdown` all run on the EE thread
// (the same thread as the hooks); the switch fields are atomics so the VBlank stamp and the pass can be read without a
// lock. The single writer is the EE, exactly like the VU1 tap (`coregs3_tap.hpp`).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "coregs3_scene.hpp"

namespace gt4::coregs3 {

inline constexpr std::uint32_t kGsStreamMagic = 0x47345447u;  // "GT4G" in file order
inline constexpr std::uint32_t kGsStreamVersion = 1u;
inline constexpr std::size_t kGsStreamHeaderBytes = 32u;
inline constexpr std::size_t kGsStreamRecordHeaderBytes = 48u;

// Record kinds, in the record header's `kind` word.
enum GsStreamKind : std::uint16_t {
  kGsStreamKindGif = 1,
  kGsStreamKindPassBegin = 2,
  kGsStreamKindPassEnd = 3,
  kGsStreamKindFrameBegin = 4,
  kGsStreamKindFrameEnd = 5,
  kGsStreamKindGsState = 6,   // reserved for a later version (the exact GS state marker)
  kGsStreamKindPrivileged = 7,  // reserved for a later version
  kGsStreamKindOverflow = 8,
};

inline constexpr std::uint8_t kGsStreamFlagSignals = 1u << 0;  // payload carries SIGNAL/FINISH/LABEL
inline constexpr std::uint8_t kGsStreamFlagRefused = 1u << 1;  // the record is a refusal/partial marker

// Cause bits of a kind 8 overflow record.
enum GsStreamCause : std::uint32_t {
  kGsStreamCauseFrameBytes = 1u << 0,  // GT4_COREGS3_GS_MAX_FRAME_BYTES reached: the rest of the frame was dropped
  kGsStreamCauseRecords = 1u << 1,     // GT4_COREGS3_GS_MAX_RECORDS reached: recording stopped
  kGsStreamCauseFileBytes = 1u << 2,   // GT4_COREGS3_GS_MAX_BYTES reached: recording stopped
  kGsStreamCauseMalformed = 1u << 3,   // a caller passed a packet that is not whole 16-byte GIF qwords
};

// The nine GS register mirrors a pass record carries: the seven of the v5 journal (coregs3_scene.hpp) plus CLAMP 0x1A8
// and TEX1 0x1B8. The last u64 of the payload is reserved.
inline constexpr std::size_t kGsStreamPassRegisters = 9u;
inline constexpr std::size_t kGsStreamPassPayloadBytes = (kGsStreamPassRegisters + 1u) * 8u;

// Bounds. They are capacities; see the header comment for the refusal rules.
inline constexpr std::uint64_t kGsStreamDefaultMaxBytes = 64u * 1024u * 1024u;
inline constexpr std::uint64_t kGsStreamDefaultMaxFrameBytes = 8u * 1024u * 1024u;
inline constexpr std::uint64_t kGsStreamDefaultMaxRecords = 65536u;
inline constexpr std::uint64_t kGsStreamDefaultMaxFrames = 8u;

struct GsStreamConfig {
  std::string path;            // the `.gt4gs` file; an empty path leaves the recorder off
  bool range = false;          // when set, only VBlanks first_vblank..last_vblank are recorded
  std::uint64_t first_vblank = 0;
  std::uint64_t last_vblank = 0;
  std::uint64_t max_bytes = kGsStreamDefaultMaxBytes;
  std::uint64_t max_frame_bytes = kGsStreamDefaultMaxFrameBytes;
  std::uint64_t max_records = kGsStreamDefaultMaxRecords;
  std::uint64_t max_frames = kGsStreamDefaultMaxFrames;
};

// Turns the recorder on with an explicit configuration (host controls and the fixture). Returns false with `error` for a
// path that cannot be opened or an inconsistent bound. Opening the file here is before the guest starts; nothing is
// written until `gsstream_shutdown()`.
bool configure_gsstream(const GsStreamConfig &config, std::string &error);

// Reads GT4_COREGS3_GS (a `.gt4gs` path), GT4_COREGS3_GS_VBLANKS (a-b, default GT4_COREGS3_CAPTURE_VBLANKS),
// GT4_COREGS3_GS_MAX_BYTES, _MAX_FRAME_BYTES, _MAX_RECORDS and _MAX_FRAMES. Does nothing when the path is unset.
void configure_gsstream_from_environment();

// One branch (an atomic load) while the switch is off. Called from gif_transfer/shadow_direct on the EE thread.
bool gsstream_active() noexcept;

// Records one accepted PATH2/PATH3 GIF transfer. `bytes` must be qwc * 16. `signals` is true when the packet carries a
// SIGNAL/FINISH/LABEL write. `last_job` is gt4::mtvu::last_posted_job() at acceptance; `source_pc`/`source_ra` are the
// guest site that kicked the transfer (evidence only). PATH1 is ignored: the VU1 tap owns it.
void gsstream_packet(int path, const std::uint8_t *data, std::size_t bytes, std::uint32_t qwc, bool signals,
                     std::uint64_t last_job, std::uint32_t source_pc, std::uint32_t source_ra) noexcept;

// Records a pass begin/end with its nine GS register words, at the K7 marker. `registers` is in the format order above.
void gsstream_pass_marker(std::uint32_t pass, bool begin, const std::uint64_t (&registers)[kGsStreamPassRegisters],
                          std::uint64_t last_job, std::uint32_t source_pc, std::uint32_t source_ra) noexcept;

// Called at the start of every guest VBlank: closes the previous frame, stores the VBlank stamp and opens the frame when
// it is inside the capture window and under the frame cap.
void gsstream_frame(std::uint64_t vblank) noexcept;

// Closes the open frame and writes the header and the ring to the file. Safe to call more than once; no-op when the
// recorder is off.
void gsstream_shutdown();

// Host controls only (the native fixture): closes the file without writing it and clears the configuration, so several
// scenarios can run in one process. Never called by the game.
void gsstream_reset_for_test();

struct GsStreamCounters {
  bool active = false;
  std::uint64_t records = 0;      // every record in the ring (all kinds)
  std::uint64_t bytes = 0;        // every payload byte in the ring
  std::uint64_t gif_records[4] = {};
  std::uint64_t gif_bytes[4] = {};
  std::uint64_t pass_records = 0;  // kind 2 + kind 3
  std::uint64_t pass_gif_records[kPassCount] = {};
  std::uint64_t pass_gif_bytes[kPassCount] = {};
  std::uint64_t frames = 0;        // frames opened
  std::uint64_t frames_closed = 0;
  std::uint64_t frames_without_end = 0;  // the file cap hit before a frame_end could be written
  std::uint64_t refused_records = 0;     // packets dropped by a frame or file bound
  std::uint64_t refused_bytes = 0;
  std::uint64_t refused_frames = 0;      // frames closed with a refusal
  std::uint64_t skipped_outside_window = 0;    // packets whose VBlank is outside the capture window
  std::uint64_t skipped_outside_frame = 0;     // packets and pass markers accepted while no capture frame is open
  std::uint64_t skipped_frames = 0;            // in-range VBlanks not opened because MAX_FRAMES was reached
  std::uint64_t signal_records = 0;            // GIF records flagged SIGNAL/FINISH/LABEL
  std::uint64_t unknown_pass_records = 0;      // GIF records accepted outside every pass marker
  std::uint64_t bad_pass_markers = 0;          // marker calls with a PassId outside kPassCount
  std::uint64_t mismatched_pass_ends = 0;      // pass_end without the matching open pass (never closes a record)
  std::uint32_t causes = 0;                    // OR of GsStreamCause seen, ever
  std::uint32_t largest_record = 0;            // largest GIF payload in bytes
  std::uint32_t largest_frame = 0;             // largest frame total (PATH2 + PATH3) in bytes
  std::uint64_t file_bytes = 0;                // header + records written at shutdown
  std::uint64_t first_vblank = 0;              // the first frame opened (0 when none)
};

const GsStreamCounters &gsstream_counters() noexcept;

// One `[coregs3] gsstream ...` report line (docs/COREGS3-GSSTREAM-CAPTURE.md §3.3). No-op when the switch is off.
void gsstream_print_report(std::FILE *out) noexcept;

}  // namespace gt4::coregs3
