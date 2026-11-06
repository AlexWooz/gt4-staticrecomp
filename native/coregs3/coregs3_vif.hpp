// TASK-090 CoreGS 3.0: a bounded VIF command decoder for the model packets captured at K1.
//
// The packets are the shape's own data as loaded from the user's disc and sent to VIF1: ordinary UNPACKs of the model's
// channels plus STMOD/STMASK/STROW/STCOL state. They are not a GIF stream and they are not executed by the CPU
// (docs/VIF-FRAMING.md, docs/VIF-STATE-CONTRACT.md). This decoder walks the command list with the resolved
// fill/skip cycle state and writes the unpacked vectors into two bounded 16 KiB images:
//
//   * `window`  the absolute VU1 data-memory writes (a packet address with the TOPS bit clear);
//   * `packed`  the writes relative to VIF1's TOPS window (a packet address with the TOPS bit set), i.e. the data
//               the VU1 program reads and XGKICKs. The packets do not carry BASE/OFFSET, so `packed` is decoded with
//               TOPS = 0; every consumer addresses it by the same relative qword offset the packet declares.
//
// The cycle state (STCYCL) is material: a fill cycle consumes fewer source vectors than destination vectors, and a
// fill row takes STROW/STCOL data instead of payload. `decode` refuses unknown opcodes, packed V4-5 outside the
// supported subset, command-count excess and overruns instead of guessing.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gt4::coregs3 {

inline constexpr std::uint32_t kVuMemoryBytes = 0x4000u;
inline constexpr std::uint32_t kVuQwords = kVuMemoryBytes / 16u;

enum class ChannelFormat : std::uint8_t {
  unknown = 0,
  v2_32 = 1,  // V2-32
  v3_32 = 2,  // V3-32
  v4_32 = 3,  // V4-32
  v2_16 = 4,  // V2-16
  v3_16 = 5,  // V3-16
  v4_16 = 6,  // V4-16
  v2_8 = 7,   // V2-8
  v3_8 = 8,   // V3-8
  v4_8 = 9,   // V4-8
  v4_5 = 10,  // V4-5 packed into one 16-bit word (converted to four 32-bit lanes)
  v1_32 = 11,  // single component, broadcast to all four lanes by the hardware
  v1_16 = 12,
  v1_8 = 13,
};

struct VifCommand {
  std::uint32_t offset = 0;       // byte offset of the command word inside the packet
  std::uint32_t payload_offset = 0;  // byte offset of the first payload word inside the packet
  std::uint32_t header = 0;       // the raw command word
  std::uint32_t opcode = 0;       // bits 24..30
  std::uint32_t immediate = 0;    // bits 0..15
  std::uint32_t num = 0;          // bits 16..23 (UNPACK: 0 means 256)
  bool unpack = false;
  bool tops = false;              // UNPACK imm bit 15: address is relative to the VIF1 double-buffer window
  bool masked = false;            // UNPACK bit 28: the STMASK register applies to this write
  std::uint32_t stmask = 0;       // the STMASK register in force when the command executed
  std::uint32_t destination = 0;  // qword address (0..1023) after the tops bit was removed
  ChannelFormat format = ChannelFormat::unknown;
  std::uint32_t vectors = 0;      // destination vectors written
  std::uint32_t source_words = 0; // 32-bit words consumed from the payload
};

struct VifPacket {
  std::vector<VifCommand> commands;
  std::array<std::uint8_t, kVuMemoryBytes> window{};  // absolute writes
  std::array<std::uint8_t, kVuMemoryBytes> packed{};  // TOPS-relative writes, base 0
  std::uint32_t cycle_cl = 1;
  std::uint32_t cycle_wl = 1;
  bool ok = false;
  std::string error;
};

// Decodes one item packet. `bytes` must be the exact DMA REF range (qwords * 16).
VifPacket decode_packet(const std::uint8_t *bytes, std::size_t size);

// Reads a decoded channel as 4-component vectors. `qword` is the channel's destination qword in `packed` (relative) or
// `window` (absolute, when `relative` is false). The output vector is the number of vectors declared by the command.
struct VifVector {
  std::uint32_t v[4] = {};
};

const char *channel_format_name(ChannelFormat format) noexcept;

// Reads `count` decoded destination vectors (16 bytes each: four 32-bit lanes) from `image` at byte `offset`.
// The UNPACK write loop already applied the source format, sign/zero widening, broadcast and fill-row rules, so a vector
// in the image is what the VU1 program would read.
bool read_vu_vectors(const std::uint8_t *image, std::uint32_t offset, std::uint32_t count,
                     std::vector<VifVector> &out) noexcept;

}  // namespace gt4::coregs3
