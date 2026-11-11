// TASK-090 CoreGS 3.0, phase 1 step 1d: the VU1 tap (docs/COREGS3-PLAN.md §8.2 item 2).
//
// The tap records what the VU1 microcode actually produced for the frames a capture covers: for every PATH1 (XGKICK)
// emission, the job id, the guest VBlank and the raw GIF bytes. It is observe-only: the bytes are copied after the
// runtime has decided what to do with them, and nothing is written back to the VU1 or the GS.
//
// Why this settles the two open questions of phase 1:
//   * the vertex formula: the tapped XYZ2 values are screen coordinates (12.4 fixed point). A projective fit from the
//     captured object-space vertices to those coordinates recovers the transform the VU1 applied, without needing to
//     read the microcode;
//   * the triangle topology and the ADC rule: the tapped vertices are in emission order, each with its ADC bit, so a
//     strip/list reading can be tested against exactly what the GS received.
//
// The file format is line-free and little endian:
//   header  u32 magic 'G','T','4','T' (0x54345447), u32 version, u32 reserved[2]
//   record  u32 bytes, u32 reserved, u64 job, u64 vblank, u8 data[bytes]   (bytes is a multiple of 16)
// Records are appended in emission order by the worker thread; a capture run reads them next to the `.gt4scene` file.
// The tap is bounded (kTapBytes) and stops with a counter when the bound is reached; it never grows without limit.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace gt4::coregs3 {

inline constexpr std::uint32_t kTapMagic = 0x54345447u;  // "GT4T"
inline constexpr std::uint32_t kTapVersion = 2u;
// Record kinds, in the record's second word. `gif` carries the PATH1 bytes; `source` names the guest address of a VIF
// packet that was posted as a job, which is what links a tapped job to the captured draw that produced it.
inline constexpr std::uint32_t kTapKindGif = 1u;
inline constexpr std::uint32_t kTapKindSource = 2u;
inline constexpr std::size_t kTapBytes = 96u * 1024u * 1024u;

// Reads GT4_COREGS3_TAP (a .gt4tap path). Called next to the capture configuration; does nothing when the switch is off.
void configure_tap_from_environment();
// Called on the worker thread (and on the synchronous path) with one PATH1 emission, after the runtime accepted it.
void tap_path1(std::uint64_t job, const std::uint8_t *data, std::size_t bytes) noexcept;
// Called when the EE posts a VIF packet as job `job`: records the packet's guest address so a tapped record can be tied
// to the shape that produced it (the capture stores the same address per item).
void tap_job_source(std::uint64_t job, std::uint32_t guest_address, std::uint32_t qwc) noexcept;
void tap_at_vblank(std::uint64_t vblank) noexcept;
void tap_shutdown();

struct TapCounters {
  std::uint64_t records = 0;
  std::uint64_t bytes = 0;
  std::uint64_t refused = 0;  // records dropped because the byte bound was reached
  std::uint64_t sources = 0;  // job -> VIF source address records
};
const TapCounters &tap_counters() noexcept;

// TASK-090 live world: the VU1 job -> packet-address pairs the live world needs to bind an item to the pixels its own
// job sampled. The tap file is written only when GT4_COREGS3_TAP is set and it arrives several VBlanks later, so the
// live path takes the same pair at post time through a sink. The sink is installed by the hooks when
// GT4_COREGS3=present and the live world is enabled; with no sink the entry point is one atomic load per posted job.
using LiveJobSourceSink = void (*)(std::uint64_t job, std::uint32_t guest_address) noexcept;
void set_live_job_source_sink(LiveJobSourceSink sink) noexcept;
// Called by the MTVU runtime next to `tap_job_source`, whatever the tap switch says.
void live_job_source(std::uint64_t job, std::uint32_t guest_address) noexcept;

}  // namespace gt4::coregs3
