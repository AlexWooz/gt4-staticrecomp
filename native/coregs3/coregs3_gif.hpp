// TASK-090 CoreGS 3.0: a bounded GIF parser for the VU1 tap.
//
// The tap holds the bytes XGKICK sent to the GS. This parser turns them into the register kicks the GS front end would
// see (PRIM, RGBAQ, ST, UV, XYZ2/XYZF2), which is what the per-vertex comparison and the topology analysis need. Field
// layouts follow CoreGS 2.0's own front end (native/hw_gs/frontend/hw_gif_parser.cpp): GIFtag NLOOP 0-14, EOP 15,
// PRE 46, PRIM 47-57, FLG 58-59, NREG 60-63 with the register list in the high qword; a PACKED XYZ has x in bits 0-15
// and y in bits 32-47 of the low qword, z in the high qword, and the ADC bit at high bit 47.
//
// The parser is offline-only: it never runs in the game and refuses anything it does not understand instead of guessing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gt4::coregs3 {

enum class GifFormat : std::uint8_t { packed = 0, reglist = 1, image = 2, disabled = 3 };

struct GifVertex {
  std::uint16_t x = 0;  // 12.4 fixed point, as the GS receives it
  std::uint16_t y = 0;
  std::uint32_t z = 0;  // 24-bit for XYZF2/XYZ2
  bool adc = false;     // address disable: the vertex is not drawn
  std::uint8_t r = 0, g = 0, b = 0, a = 0;
  float q = 1.0f;
  float s = 0.0f, t = 0.0f;
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
  std::uint32_t primitives_started = 0;  // PRIM kicks seen
  bool ok = false;
  std::string error;
};

// Parses one GIF transfer (a tap record). `bytes` must be a multiple of 16.
GifPacket parse_gif(const std::uint8_t *bytes, std::size_t length);

}  // namespace gt4::coregs3
