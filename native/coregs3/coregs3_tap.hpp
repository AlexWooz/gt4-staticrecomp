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
inline constexpr std::uint32_t kTapVersion = 1u;
inline constexpr std::size_t kTapBytes = 96u * 1024u * 1024u;

// Reads GT4_COREGS3_TAP (a .gt4tap path). Called next to the capture configuration; does nothing when the switch is off.
void configure_tap_from_environment();
// Called on the worker thread (and on the synchronous path) with one PATH1 emission, after the runtime accepted it.
void tap_path1(std::uint64_t job, const std::uint8_t *data, std::size_t bytes) noexcept;
void tap_at_vblank(std::uint64_t vblank) noexcept;
void tap_shutdown();

struct TapCounters {
  std::uint64_t records = 0;
  std::uint64_t bytes = 0;
  std::uint64_t refused = 0;  // records dropped because the byte bound was reached
};
const TapCounters &tap_counters() noexcept;

}  // namespace gt4::coregs3
