// TASK-090 CoreGS 3.0: the bounded GIF parser used by the tap analysis. See coregs3_gif.hpp.
#include "coregs3_gif.hpp"

#include <cstring>

namespace gt4::coregs3 {
namespace {

std::uint64_t read_u64(const std::uint8_t *p) {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) value |= static_cast<std::uint64_t>(p[index]) << (8 * index);
  return value;
}

std::uint32_t bits(std::uint64_t word, unsigned low, unsigned high) {
  const std::uint64_t mask = high - low == 63 ? ~0ull : ((1ull << (high - low + 1)) - 1ull);
  return static_cast<std::uint32_t>((word >> low) & mask);
}

float as_float(std::uint32_t value) {
  float out = 0.0f;
  std::memcpy(&out, &value, sizeof out);
  return out;
}

struct Qword {
  std::uint64_t lo = 0;
  std::uint64_t hi = 0;
};

// PACKED XYZ2/XYZF2 (hw_gif_parser.cpp DecodePackedXyz).
void decode_packed_xyz(const Qword &q, bool with_fog, GifVertex &vertex) {
  vertex.x = static_cast<std::uint16_t>(bits(q.lo, 0, 15));
  vertex.y = static_cast<std::uint16_t>(bits(q.lo, 32, 47));
  if (with_fog) {
    vertex.z = bits(q.hi, 4, 27);
    vertex.fog = static_cast<std::uint8_t>(bits(q.hi, 36, 43));
    vertex.has_fog = true;
  } else {
    vertex.z = bits(q.hi, 0, 31);
  }
  vertex.adc = bits(q.hi, 47, 47) != 0;
}

}  // namespace

GifPacket parse_gif(const std::uint8_t *bytes, std::size_t length) {
  GifPacket packet;
  if (bytes == nullptr || length == 0 || length % 16 != 0) {
    packet.error = "not a whole number of GIF qwords";
    return packet;
  }
  std::size_t at = 0;
  bool prim_known = false;
  std::uint32_t prim = 0;
  GifVertex current;  // register state carried between tags
  while (at + 16 <= length) {
    const Qword tag_qword{read_u64(bytes + at), read_u64(bytes + at + 8)};
    at += 16;
    GifTagSummary summary;
    summary.nloop = bits(tag_qword.lo, 0, 14);
    summary.prim = bits(tag_qword.lo, 47, 57);
    summary.format = static_cast<GifFormat>(bits(tag_qword.lo, 58, 59));
    summary.nreg = bits(tag_qword.lo, 60, 63);
    if (summary.nreg == 0) summary.nreg = 16;
    summary.registers = tag_qword.hi;
    packet.tags.push_back(summary);

    auto read_register = [&](std::uint32_t register_id, const Qword &q) {
      switch (register_id) {
      case 0x0:  // PRIM
        prim = bits(q.lo, 0, 10);
        prim_known = true;
        ++packet.primitives_started;
        break;
      case 0x1: {  // RGBAQ
        GifVertex &vertex = current;
        vertex.r = static_cast<std::uint8_t>(bits(q.lo, 0, 7));
        vertex.g = static_cast<std::uint8_t>(bits(q.lo, 8, 15));
        vertex.b = static_cast<std::uint8_t>(bits(q.lo, 16, 23));
        vertex.a = static_cast<std::uint8_t>(bits(q.lo, 24, 31));
        vertex.q = as_float(bits(q.hi, 0, 31));
        vertex.has_rgba = true;
        break;
      }
      case 0x2:  // ST
        current.s = as_float(bits(q.lo, 0, 31));
        current.t = as_float(bits(q.hi, 32, 63));
        current.has_st = true;
        break;
      case 0x3:  // UV
        current.s = static_cast<float>(bits(q.lo, 0, 13));
        current.t = static_cast<float>(bits(q.lo, 16, 29));
        current.has_st = true;
        break;
      case 0x4:   // XYZF2 (with fog, always draws)
      case 0x5:   // XYZ2 (ADC applies)
      case 0xC:   // XYZF3 (no draw)
      case 0xD: { // XYZ3 (no draw)
        GifVertex vertex = current;
        const bool with_fog = register_id == 0x4 || register_id == 0xC;
        decode_packed_xyz(q, with_fog, vertex);
        vertex.prim = prim_known ? prim : 0;
        // A3=A+D (0xC/0xD) never draws; XYZ3's ADC bit is not a draw flag.
        if (register_id == 0xC || register_id == 0xD) vertex.adc = true;
        packet.vertices.push_back(vertex);
        break;
      }
      case 0xE:  // ADC
        if (!packet.vertices.empty() && bits(q.lo, 0, 0) != 0) packet.vertices.back().adc = true;
        break;
      default:  // TEX0/CLAMP/FOG/FOGCOL/... carry no vertex position: ignored by this parser
        break;
      }
    };

    if (summary.format == GifFormat::packed) {
      for (std::uint32_t loop = 0; loop < summary.nloop; ++loop) {
        for (std::uint32_t index = 0; index < summary.nreg; ++index) {
          if (at + 16 > length) {
            packet.error = "PACKED tag runs past the transfer";
            return packet;
          }
          const Qword q{read_u64(bytes + at), read_u64(bytes + at + 8)};
          at += 16;
          read_register(bits(summary.registers, index * 4, index * 4 + 3), q);
        }
      }
    } else if (summary.format == GifFormat::reglist) {
      // REGLIST: NLOOP is the number of register writes, all to descriptor 0 (one qword each), not NLOOP x NREG.
      for (std::uint32_t loop = 0; loop < summary.nloop; ++loop) {
        if (at + 16 > length) {
          packet.error = "REGLIST tag runs past the transfer";
          return packet;
        }
        const Qword q{read_u64(bytes + at), read_u64(bytes + at + 8)};
        at += 16;
        read_register(bits(summary.registers, 0, 3), q);
      }
    } else if (summary.format == GifFormat::image) {
      const std::size_t data = static_cast<std::size_t>(summary.nloop) * 16u;
      if (data > length - at) {
        packet.error = "IMAGE tag runs past the transfer";
        return packet;
      }
      at += data;
    }  // disabled: no data
    if (bits(tag_qword.lo, 15, 15) != 0) break;  // EOP
  }
  packet.ok = true;
  return packet;
}

}  // namespace gt4::coregs3
