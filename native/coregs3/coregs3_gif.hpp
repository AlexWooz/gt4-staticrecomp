// TASK-090 CoreGS 3.0: a bounded GIF parser for the VU1 tap.
//
// The tap holds the bytes XGKICK sent to the GS. This parser turns them into the register kicks the GS front end would
// see (PRIM, RGBAQ, ST, UV, A+D, XYZ2/XYZF2), which is what the per-vertex comparison and the topology analysis need.
// Field layouts follow CoreGS 2.0's own front end (native/hw_gs/frontend/hw_gif_parser.cpp): GIFtag NLOOP 0-14, EOP 15,
// PRE 46, PRIM 47-57, FLG 58-59, NREG 60-63 with the register list in the high qword; a PACKED XYZ has x in bits 0-15
// and y in bits 32-47 of the low qword, z in the high qword, and the ADC bit at high bit 47.
//
// The PACKED register qwords are GT4's real layout (measured against the r13 tap and CoreGS 2.0's front end):
//   * RGBAQ: R = low qword bits 0-7, G = low qword bits 32-39, B = high qword bits 0-7, A = high qword bits 32-39.
//     The qword carries no Q: the colour pairs with the Q the ST write latched, and a tag with NLOOP > 0 resets that
//     latch to 1.0.
//   * ST: S = f32(low qword bits 0-31), T = f32(low qword bits 32-63), Q = f32(high qword bits 0-31) into the latch.
//   * UV: U = low qword bits 0-13, V = low qword bits 32-45 (a 14-bit integer pair, not a float pair).
//   * A+D (descriptor 0xE): the address is the high qword's low 7 bits, the data is the whole low qword in the
//     canonical 64-bit register layout. It is a register write (TEX0/TEX1/TEX2/CLAMP/TEST/ALPHA/FRAME/ZBUF), never an
//     ADC flag.
//
// The parser is offline-only: it never runs in the game and refuses anything it does not understand instead of guessing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gt4::coregs3 {

enum class GifFormat : std::uint8_t { packed = 0, reglist = 1, image = 2, disabled = 3 };

// One A+D register write (descriptor 0xE): the canonical 64-bit register value the GS would receive.
struct GifRegisterWrite {
  std::uint32_t address = 0;  // GS register address, high qword bits 0-6 (7 bits)
  std::uint64_t data = 0;     // low qword, the 64-bit register layout
};

struct GifVertex {
  std::uint16_t x = 0;  // 12.4 fixed point, as the GS receives it
  std::uint16_t y = 0;
  std::uint32_t z = 0;  // 24-bit for XYZF2/XYZ2
  bool adc = false;     // address disable: the vertex is not drawn
  std::uint8_t r = 0, g = 0, b = 0, a = 0;
  float q = 1.0f;       // the ST latch in force (the GS's Q)
  float s = 0.0f, t = 0.0f;  // ST floats, or the integer UV pair widened to float
  std::uint8_t fog = 0;
  bool has_fog = false;
  bool has_st = false;
  bool has_rgba = false;
  std::uint32_t prim = 0;  // PRIM register in force when the vertex was kicked
};

struct GifTagSummary {
  std::uint32_t prim = 0;
  std::uint32_t nloop = 0;
  std::uint32_t nreg = 0;
  GifFormat format = GifFormat::packed;
  std::uint64_t registers = 0;
};

struct GifPacket {
  std::vector<GifTagSummary> tags;
  std::vector<GifVertex> vertices;
  std::vector<GifRegisterWrite> ad_writes;  // descriptor 0xE writes, in transfer order (state changes only)
  // TASK-090 track M1: the number of A+D writes that precede the first vertex in transfer order. The state a draw runs
  // under is the one after those writes; a write that follows the vertices applies to the next draw. The tap-based
  // per-item state uses this prefix, not the whole record.
  std::uint32_t ad_writes_at_first_vertex = 0;
  std::uint32_t primitives_started = 0;     // PRIM kicks seen
  bool ok = false;
  std::string error;
};

// Parses one GIF transfer (a tap record). `bytes` must be a multiple of 16.
GifPacket parse_gif(const std::uint8_t *bytes, std::size_t length);

// Symbolic name of a GS register address (the 7-bit A+D address), for the tap tools; "unknown" when not named.
const char *gif_register_name(std::uint32_t address) noexcept;

}  // namespace gt4::coregs3
