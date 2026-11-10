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
  float q_latch = 1.0f;  // the GS Q latch; a tag with NLOOP > 0 resets it to 1.0
  GifVertex current;     // register state carried between tags
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
    if (summary.nloop != 0) q_latch = 1.0f;  // chapter 02.1: a tag that carries data resets Q

    auto read_register = [&](std::uint32_t register_id, const Qword &q) {
      switch (register_id) {
      case 0x0:  // PRIM
        prim = bits(q.lo, 0, 10);
        prim_known = true;
        ++packet.primitives_started;
        break;
      case 0x1: {  // RGBAQ: one byte per 32-bit lane; Q is the ST latch, never a field of this qword
        GifVertex &vertex = current;
        vertex.r = static_cast<std::uint8_t>(bits(q.lo, 0, 7));
        vertex.g = static_cast<std::uint8_t>(bits(q.lo, 32, 39));
        vertex.b = static_cast<std::uint8_t>(bits(q.hi, 0, 7));
        vertex.a = static_cast<std::uint8_t>(bits(q.hi, 32, 39));
        vertex.q = q_latch;
        vertex.has_rgba = true;
        break;
      }
      case 0x2:  // STQ: S, T from the low qword; Q from the high qword and into the latch
        current.s = as_float(bits(q.lo, 0, 31));
        current.t = as_float(bits(q.lo, 32, 63));
        q_latch = as_float(bits(q.hi, 0, 31));
        current.q = q_latch;
        current.has_st = true;
        break;
      case 0x3:  // UV: U 0-13 and V 32-45 of the low qword, widened to the ST s/t fields
        current.s = static_cast<float>(bits(q.lo, 0, 13));
        current.t = static_cast<float>(bits(q.lo, 32, 45));
        current.has_st = true;
        break;
      case 0x4:   // XYZF2 (with fog, always draws)
      case 0x5:   // XYZ2 (ADC applies)
      case 0xC:   // XYZF3 (no draw)
      case 0xD: { // XYZ3 (no draw)
        GifVertex vertex = current;
        const bool with_fog = register_id == 0x4 || register_id == 0xC;
        decode_packed_xyz(q, with_fog, vertex);
        vertex.q = q_latch;  // the GS Q is the latch the last ST (or a tag reset) left in force
        vertex.prim = prim_known ? prim : 0;
        // A3=A+D (0xC/0xD) never draws; XYZ3's ADC bit is not a draw flag.
        if (register_id == 0xC || register_id == 0xD) vertex.adc = true;
        if (packet.vertices.empty())
          packet.ad_writes_at_first_vertex = static_cast<std::uint32_t>(packet.ad_writes.size());
        packet.vertices.push_back(vertex);
        break;
      }
      case 0xE: {  // A+D: a register write, not an ADC flag
        GifRegisterWrite write;
        write.address = bits(q.hi, 0, 6);
        write.data = q.lo;
        packet.ad_writes.push_back(write);
        break;
      }
      default:
        // A direct TEX0/CLAMP/FOG descriptor carries no vertex and the tap analysis does not need it; GT4 sends these
        // registers through A+D (case 0xE), which is recorded. Unknown descriptors are skipped, not guessed.
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

const char *gif_register_name(std::uint32_t address) noexcept {
  switch (address) {
  case 0x00: return "PRIM";
  case 0x01: return "RGBAQ";
  case 0x02: return "ST";
  case 0x03: return "UV";
  case 0x04: return "XYZF2";
  case 0x05: return "XYZ2";
  case 0x06: return "TEX0_1";
  case 0x07: return "TEX0_2";
  case 0x08: return "CLAMP_1";
  case 0x09: return "CLAMP_2";
  case 0x0A: return "FOG";
  case 0x0C: return "XYZF3";
  case 0x0D: return "XYZ3";
  case 0x0E: return "A+D";
  case 0x0F: return "NOP";
  case 0x14: return "TEX1_1";
  case 0x15: return "TEX1_2";
  case 0x16: return "TEX2_1";
  case 0x17: return "TEX2_2";
  case 0x18: return "XYOFFSET_1";
  case 0x19: return "XYOFFSET_2";
  case 0x1A: return "PRMODECONT";
  case 0x1B: return "PRMODE";
  case 0x1C: return "TEXCLUT";
  case 0x22: return "SCANMSK";
  case 0x34: return "MIPTBP1_1";
  case 0x35: return "MIPTBP1_2";
  case 0x36: return "MIPTBP2_1";
  case 0x37: return "MIPTBP2_2";
  case 0x3B: return "TEXA";
  case 0x3D: return "FOGCOL";
  case 0x3F: return "TEXFLUSH";
  case 0x40: return "SCISSOR_1";
  case 0x41: return "SCISSOR_2";
  case 0x42: return "ALPHA_1";
  case 0x43: return "ALPHA_2";
  case 0x44: return "DIMX";
  case 0x45: return "DTHE";
  case 0x46: return "COLCLAMP";
  case 0x47: return "TEST_1";
  case 0x48: return "TEST_2";
  case 0x49: return "PABE";
  case 0x4A: return "FBA_1";
  case 0x4B: return "FBA_2";
  case 0x4C: return "FRAME_1";
  case 0x4D: return "FRAME_2";
  case 0x4E: return "ZBUF_1";
  case 0x4F: return "ZBUF_2";
  case 0x50: return "BITBLTBUF";
  case 0x51: return "TRXPOS";
  case 0x52: return "TRXREG";
  case 0x53: return "TRXDIR";
  case 0x54: return "HWREG";
  case 0x60: return "SIGNAL";
  case 0x61: return "FINISH";
  case 0x62: return "LABEL";
  case 0x7F: return "NOP";
  default: return "unknown";
  }
}

}  // namespace gt4::coregs3
