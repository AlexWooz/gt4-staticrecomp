// TASK-090 CoreGS 3.0: the GS local memory (SLM) sidecar of the captured car-pass frames.
//
// The `.gt4scene` capture holds the draw state (TEX0/TEX1/CLAMP of every texture record) but no texture pixels: the
// textures live in GS local memory, which the EE hooks do not see. This sidecar is written by the GS worker
// (coregs2/core2_renderer.cpp, Renderer::Flush) next to a capture so the offline tools can decode the real textures.
//
// File layout (little endian):
//
//   header  u32 magic 'GT4V' (0x56345447), u32 version, u64 vblank,
//           u32 first_block, u32 block_count, u32 byte_count, u32 reserved
//   bytes   block_count * 256 bytes, block `first_block` at offset 0 (a block is the GS 256-byte unit)
//
// A file holds one or more records concatenated; each record is self-describing, so a reader can skip one it does not
// understand. Blocks are GS block addresses in 0..16383 and a record never wraps the end of the 4 MiB image.
//
// The dump is diagnostic only. It reads the CPU shadow memory from the GS worker (never guest state), copies a bounded
// block range once per captured VBlank and writes the file at shutdown, off the guest's timeline (the capture's
// measured rule, docs/COREGS3-HOOKS.md §7). The magic is the file's first byte order: 'G', 'T', '4', 'V'.
//
// This header is dependency-free so both the GS worker (CoreGS 2.0) and the Vulkan-free offline decoder can include it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace gt4::coregs3::vram {

inline constexpr std::uint32_t kMagic = 0x56345447u;  // "GT4V" in file order
inline constexpr std::uint32_t kVersion = 1u;
inline constexpr std::size_t kHeaderBytes = 32u;

// The measured car texture range of the night race: blocks 0x2BC0..0x3900 (0x2BC0..0x38FF inclusive), palettes
// included (the texture research places the car set at 0x2BC0..0x3848). 0x0D40 blocks = 868,352 bytes.
inline constexpr std::uint32_t kCarFirstBlock = 0x2BC0u;
inline constexpr std::uint32_t kCarBlockCount = 0x0D40u;
inline constexpr std::uint32_t kBlockBytes = 256u;
inline constexpr std::uint32_t kBlocksTotal = 16384u;

struct Header {
  std::uint32_t version = kVersion;
  std::uint64_t vblank = 0;         // the GS worker's guest VBlank counter at the flush
  std::uint32_t first_block = 0;    // first GS block in the record
  std::uint32_t block_count = 0;    // blocks stored after the header
  std::uint32_t reserved = 0;       // 0 in version 1; a nonzero value is refused, not guessed
};

// Serializes a header. `block_count` must describe the bytes the writer appends.
inline void encode_header(std::uint8_t out[kHeaderBytes], const Header &header) {
  const auto put_u32 = [out](std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index)
      out[offset + index] = static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFu);
  };
  put_u32(0, kMagic);
  put_u32(4, header.version);
  for (std::size_t index = 0; index < 8; ++index)
    out[8 + index] = static_cast<std::uint8_t>((header.vblank >> (8 * index)) & 0xFFu);
  put_u32(16, header.first_block);
  put_u32(20, header.block_count);
  put_u32(24, header.block_count * kBlockBytes);
  put_u32(28, header.reserved);
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

// Parses one header and checks the invariants a version 1 record must satisfy. `available` is how many bytes of the
// file remain from `data`. The byte count is checked against `available` too.
inline bool decode_header(const std::uint8_t *data, std::size_t available, Header &header, std::string &error) {
  if (available < kHeaderBytes) {
    error = "truncated VRAM record header";
    return false;
  }
  if (read_u32(data, 0) != kMagic) {
    error = "not a GT4V sidecar (bad magic)";
    return false;
  }
  header.version = read_u32(data, 4);
  if (header.version != kVersion) {
    error = "unsupported GT4V version " + std::to_string(header.version);
    return false;
  }
  header.vblank = read_u64(data, 8);
  header.first_block = read_u32(data, 16);
  header.block_count = read_u32(data, 20);
  const std::uint32_t byte_count = read_u32(data, 24);
  header.reserved = read_u32(data, 28);
  if (header.reserved != 0) {
    error = "GT4V version 1 reserved word is not zero";
    return false;
  }
  if (header.block_count > kBlocksTotal || header.first_block >= kBlocksTotal ||
      header.block_count > kBlocksTotal - header.first_block) {
    error = "GT4V record block range is outside the 4 MiB image";
    return false;
  }
  if (byte_count != header.block_count * kBlockBytes) {
    error = "GT4V record byte count does not match its block range";
    return false;
  }
  if (available - kHeaderBytes < byte_count) {
    error = "truncated GT4V record bytes";
    return false;
  }
  return true;
}

}  // namespace gt4::coregs3::vram
