// TASK-090 CoreGS 3.0: the bounded VIF decoder. See coregs3_vif.hpp for the contract and docs/VIF-FRAMING.md for the
// framing rules this follows (write semantics from the pinned MIT donor's `vif.cpp`, re-derived here).
#include "coregs3_vif.hpp"

#include <cstring>

namespace gt4::coregs3 {
namespace {

constexpr std::uint32_t kUnpack = 0x60u;   // opcodes 0x60..0x7F are UNPACK/UNPACKx
constexpr std::uint32_t kMaxCommands = 65536u;

std::uint32_t read_u32(const std::uint8_t *p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

ChannelFormat format_of(std::uint32_t vn, std::uint32_t vl) {
  if (vn == 3 && vl == 3) return ChannelFormat::v4_5;
  if (vl > 2) return ChannelFormat::unknown;
  static constexpr ChannelFormat table[3][3] = {
      {ChannelFormat::v1_32, ChannelFormat::v1_16, ChannelFormat::v1_8},
      {ChannelFormat::v2_32, ChannelFormat::v2_16, ChannelFormat::v2_8},
      {ChannelFormat::v3_32, ChannelFormat::v3_16, ChannelFormat::v3_8},
  };
  if (vn == 3) {
    switch (vl) {
    case 0: return ChannelFormat::v4_32;
    case 1: return ChannelFormat::v4_16;
    default: return ChannelFormat::v4_8;
    }
  }
  return table[vn][vl];
}

std::uint32_t element_bytes(ChannelFormat format) {
  switch (format) {
  case ChannelFormat::v1_32: return 4;
  case ChannelFormat::v2_32: return 2 * 4;
  case ChannelFormat::v3_32: return 3 * 4;
  case ChannelFormat::v4_32: return 4 * 4;
  case ChannelFormat::v1_16: return 2;
  case ChannelFormat::v2_16: return 2 * 2;
  case ChannelFormat::v3_16: return 3 * 2;
  case ChannelFormat::v4_16: return 4 * 2;
  case ChannelFormat::v1_8: return 1;
  case ChannelFormat::v2_8: return 2;
  case ChannelFormat::v3_8: return 3;
  case ChannelFormat::v4_8: return 4;
  case ChannelFormat::v4_5: return 2;
  default: return 0;
  }
}

std::uint32_t components_of(ChannelFormat format) {
  switch (format) {
  case ChannelFormat::v1_32:
  case ChannelFormat::v1_16:
  case ChannelFormat::v1_8: return 1;
  case ChannelFormat::v2_32:
  case ChannelFormat::v2_16:
  case ChannelFormat::v2_8: return 2;
  case ChannelFormat::v3_32:
  case ChannelFormat::v3_16:
  case ChannelFormat::v3_8: return 3;
  case ChannelFormat::v4_32:
  case ChannelFormat::v4_16:
  case ChannelFormat::v4_8:
  case ChannelFormat::v4_5: return 4;
  default: return 0;
  }
}

// Source vectors actually present in the payload: a fill cycle (WL > CL) omits the filled rows.
std::uint32_t source_vectors(std::uint32_t num, std::uint32_t cl, std::uint32_t wl) {
  if (wl <= cl || wl == 0) return num;
  return (num / wl) * cl + (num % wl < cl ? num % wl : cl);
}

// Bits per source element: 32, 16, 8, or 16 for the packed V4-5 form (all four channels in one word).
std::uint32_t bits_per_component(ChannelFormat format) {
  switch (format) {
  case ChannelFormat::v1_32:
  case ChannelFormat::v2_32:
  case ChannelFormat::v3_32:
  case ChannelFormat::v4_32: return 32;
  case ChannelFormat::v1_16:
  case ChannelFormat::v2_16:
  case ChannelFormat::v3_16:
  case ChannelFormat::v4_16: return 16;
  case ChannelFormat::v1_8:
  case ChannelFormat::v2_8:
  case ChannelFormat::v3_8:
  case ChannelFormat::v4_8: return 8;
  case ChannelFormat::v4_5: return 16;
  default: return 0;
  }
}

}  // namespace

const char *channel_format_name(ChannelFormat format) noexcept {
  switch (format) {
  case ChannelFormat::v1_32: return "V1-32";
  case ChannelFormat::v2_32: return "V2-32";
  case ChannelFormat::v3_32: return "V3-32";
  case ChannelFormat::v4_32: return "V4-32";
  case ChannelFormat::v2_16: return "V2-16";
  case ChannelFormat::v3_16: return "V3-16";
  case ChannelFormat::v4_16: return "V4-16";
  case ChannelFormat::v2_8: return "V2-8";
  case ChannelFormat::v3_8: return "V3-8";
  case ChannelFormat::v4_8: return "V4-8";
  case ChannelFormat::v4_5: return "V4-5";
  case ChannelFormat::v1_16: return "V1-16";
  case ChannelFormat::v1_8: return "V1-8";
  default: return "unknown";
  }
}

VifPacket decode_packet(const std::uint8_t *bytes, std::size_t size) {
  VifPacket packet;
  if (bytes == nullptr || size == 0) {
    packet.error = "empty packet";
    return packet;
  }
  if (size % 4 != 0) {
    packet.error = "packet is not a whole number of VIF words";
    return packet;
  }

  // Register state that changes later writes. STMOD/STMASK/STROW/STCOL/BASE/OFFSET start at the reset values; the
  // packets seen so far do not set BASE/OFFSET, and TOPS is decoded at 0 (see the header).
  std::uint32_t mode = 0;
  std::uint32_t mask = 0;
  std::uint32_t row[4] = {};
  std::uint32_t column[4] = {};
  std::uint32_t cl = 1, wl = 1;
  std::uint32_t tops = 0;
  std::uint32_t base = 0, offset = 0;
  bool double_buffer = false;

  std::size_t at = 0;
  while (at + 4 <= size) {
    if (packet.commands.size() >= kMaxCommands) {
      packet.error = "command count above the cap";
      return packet;
    }
    const std::uint32_t command = read_u32(bytes + at);
    const std::uint32_t opcode = (command >> 24) & 0x7Fu;
    const std::uint32_t immediate = command & 0xFFFFu;
    const std::uint32_t num = (command >> 16) & 0xFFu;

    VifCommand entry;
    entry.offset = static_cast<std::uint32_t>(at);
    entry.header = command;
    entry.opcode = opcode;
    entry.immediate = immediate;
    entry.num = num;

    if ((opcode & kUnpack) == kUnpack) {
      const std::uint32_t vn = (command >> 26) & 3u, vl = (command >> 24) & 3u;
      const ChannelFormat format = format_of(vn, vl);
      if (format == ChannelFormat::unknown) {
        packet.error = "unsupported UNPACK form at offset " + std::to_string(at);
        return packet;
      }
      const std::uint32_t vectors = num == 0 ? 256u : num;
      const std::uint32_t sources = source_vectors(vectors, cl, wl);
      const std::uint32_t words = (sources * element_bytes(format) + 3u) / 4u;
      const std::size_t payload = at + 4;
      if (words > (size - payload) / 4u) {
        packet.error = "UNPACK payload overruns the packet at offset " + std::to_string(at);
        return packet;
      }
      entry.unpack = true;
      entry.tops = (command >> 15) & 1u;
      entry.destination = (command & 0x3FFu) + (entry.tops ? tops : 0u);
      entry.destination %= kVuQwords;
      entry.format = format;
      entry.vectors = vectors;
      entry.source_words = words;
      entry.payload_offset = 0;

      // Write the destination vectors with the STMOD/STMASK/STROW/STCOL semantics of the hardware.
      std::uint8_t *image = entry.tops ? packet.packed.data() : packet.window.data();
      std::uint32_t address = entry.destination;
      std::uint32_t cycle = 0;
      const std::uint8_t *payload_bytes = bytes + payload;
      const std::uint32_t format_bits = bits_per_component(format);
      const std::uint32_t format_components = components_of(format);
      std::uint32_t bit_position = 0;
      auto read_element = [&]() -> std::uint32_t {
        const std::uint32_t word = bit_position / 32u;
        const std::uint32_t shift = bit_position % 32u;
        if (word >= words) return 0;
        std::uint32_t raw = read_u32(payload_bytes + word * 4u) >> shift;
        if (format_bits < 32u) raw &= (1u << format_bits) - 1u;
        bit_position += format_bits;
        return raw;
      };
      for (std::uint32_t n = 0; n < vectors; ++n) {
        const bool fill = wl > cl && cycle >= cl;
        std::uint32_t value[4] = {0, 0, 0, 0};
        if (!fill) {
          if (format == ChannelFormat::v4_5) {
            const std::uint32_t word = read_element();
            value[0] = ((word >> 0) & 0x1Fu) << 3;
            value[1] = ((word >> 5) & 0x1Fu) << 3;
            value[2] = ((word >> 10) & 0x1Fu) << 3;
            value[3] = ((word >> 15) & 1u) << 7;
          } else {
            for (std::uint32_t component = 0; component < format_components; ++component) {
              const std::uint32_t raw = read_element();
              if (format_bits == 16u) {
                value[component] = raw;  // the unpack stores the widened unsigned value; sign is applied by the VU
              } else if (format_bits == 8u) {
                value[component] = raw;
              } else {
                value[component] = raw;
              }
            }
            if (format_components == 2) {
              value[2] = value[0];
              value[3] = value[1];
            }
          }
        }
        std::uint32_t *destination = reinterpret_cast<std::uint32_t *>(image + static_cast<std::size_t>(address) * 16u);
        const std::uint32_t mask_row = cycle < 4 ? cycle : 3;
        for (std::uint32_t component = 0; component < 4; ++component) {
          std::uint32_t write_mode = (mask >> ((mask_row * 4 + component) * 2)) & 3u;
          if (fill && write_mode == 0) write_mode = 1;
          if (write_mode == 3) continue;
          std::uint32_t out = value[component];
          if (write_mode == 1) out = row[component];
          else if (write_mode == 2) out = column[mask_row];
          else if (mode == 1 || mode == 2) {
            out = value[component] + row[component];
            if (mode == 2) row[component] = out;
          }
          std::memcpy(destination + component, &out, sizeof out);
        }
        ++address;
        address %= kVuQwords;
        ++cycle;
        if (cl >= wl) {
          if (cycle >= wl) {
            address = (address + (cl - wl)) % kVuQwords;
            cycle = 0;
          }
        } else if (cycle >= wl) {
          cycle = 0;
        }
      }
      at = payload + words * 4u;
      packet.commands.push_back(entry);
      continue;
    }

    switch (opcode) {
    case 0x00:  // NOP
    case 0x06:  // FLUSHE
    case 0x13:  // FLUSH
    case 0x10:  // MSCAL handled by the caller; the packet may still carry it
      at += 4;
      break;
    case 0x01:  // STCYCL
      cl = immediate & 0xFFu;
      wl = (immediate >> 8) & 0xFFu;
      if (cl == 0) cl = 1;
      if (wl == 0) wl = 1;
      packet.cycle_cl = cl;
      packet.cycle_wl = wl;
      at += 4;
      break;
    case 0x02:  // OFFSET
      offset = immediate & 0x3FFu;
      double_buffer = false;
      tops = (base + (double_buffer ? offset : 0u)) % kVuQwords;
      at += 4;
      break;
    case 0x03:  // BASE
      base = immediate & 0x3FFu;
      tops = (base + (double_buffer ? offset : 0u)) % kVuQwords;
      at += 4;
      break;
    case 0x04:  // ITOPS
      at += 4;
      break;
    case 0x05:  // STMOD
      mode = immediate & 3u;
      at += 4;
      break;
    case 0x07:  // MARK
    case 0x14:  // MSCNT
      at += 4;
      break;
    case 0x20:  // STMASK
      if (size - at < 4 + 4) {
        packet.error = "STMASK without payload";
        return packet;
      }
      mask = read_u32(bytes + at + 4);
      at += 8;
      break;
    case 0x30:  // STROW
      if (size - at < 4 + 16) {
        packet.error = "STROW without payload";
        return packet;
      }
      for (std::uint32_t index = 0; index < 4; ++index) row[index] = read_u32(bytes + at + 4 + index * 4);
      at += 20;
      break;
    case 0x31:  // STCOL
      if (size - at < 4 + 16) {
        packet.error = "STCOL without payload";
        return packet;
      }
      for (std::uint32_t index = 0; index < 4; ++index) column[index] = read_u32(bytes + at + 4 + index * 4);
      at += 20;
      break;
    case 0x50:  // DIRECT
    case 0x51: {  // DIRECTHL
      const std::uint64_t payload = static_cast<std::uint64_t>(immediate == 0 ? 0x10000u : immediate) * 16u;
      if (payload > size - at - 4) {
        packet.error = "DIRECT payload overruns the packet";
        return packet;
      }
      at += 4 + static_cast<std::size_t>(payload);
      break;
    }
    case 0x32:  // not a VIF command; refused instead of guessed (the packets of the car shapes never use it)
    default:
      packet.error = "unhandled VIF opcode " + std::to_string(opcode) + " at offset " + std::to_string(at);
      return packet;
    }
  }
  if (at != size) {
    packet.error = "trailing bytes after the last command";
    return packet;
  }
  packet.ok = true;
  return packet;
}

bool read_vu_vectors(const std::uint8_t *image, std::uint32_t offset, std::uint32_t count,
                     std::vector<VifVector> &out) noexcept {
  if (offset > kVuMemoryBytes || count > (kVuMemoryBytes - offset) / 16u) return false;
  out.resize(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    const std::uint8_t *p = image + offset + static_cast<std::size_t>(index) * 16u;
    for (std::uint32_t lane = 0; lane < 4; ++lane) out[index].v[lane] = read_u32(p + lane * 4);
  }
  return true;
}

}  // namespace gt4::coregs3
