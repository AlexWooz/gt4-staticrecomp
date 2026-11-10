// TASK-090 CoreGS 3.0, phase S1: a bounded reader for the `.gt4gs` PATH2/PATH3 capture (coregs3_gsstream.hpp is the
// writer's contract).
//
// The tool, the fixture and the later offline layers need the same parse, so it lives here in gt4_coregs3 next to the tap
// reader. The reader validates the header, every record length, the kind-specific payload size and the file bounds; it
// never trusts a length word. A file that claims a record kind or version this reader does not know is refused with the
// reason, never skipped silently.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gt4::coregs3 {

struct GsStreamFileHeader {
  std::uint32_t version = 0;
  std::uint32_t target = 0;
  std::uint32_t flags = 0;
  std::uint64_t first_vblank = 0;
};

struct GsStreamFileRecord {
  std::uint16_t kind = 0;   // GsStreamKind
  std::uint8_t path = 0;    // 1..3 for a GIF record, 0 otherwise
  std::uint8_t flags = 0;   // kGsStreamFlagSignals / kGsStreamFlagRefused
  std::uint32_t pass = 0;   // PassId of a GIF or pass record
  std::uint32_t sequence = 0;
  std::uint64_t vblank = 0;
  std::uint64_t last_job = 0;
  std::uint32_t source_pc = 0;
  std::uint32_t source_ra = 0;
  std::uint32_t qwc = 0;
  std::uint32_t bytes = 0;   // payload bytes
  std::size_t offset = 0;    // payload offset in the file image
};

// Reads the whole file and its record table. Returns false with `error` for a bad magic, an unsupported version, a
// nonzero reserved word, an unknown kind, a malformed payload size or a record that runs past the end.
bool read_gs_stream_file(const std::string &path, std::vector<std::uint8_t> &data, GsStreamFileHeader &header,
                         std::vector<GsStreamFileRecord> &records, std::string &error);

// Kind 2/3 payload: the nine GS register words in format order (FOGCOL, FRAME, XYOFFSET, ZBUF, SCISSOR, ALPHA, TEST,
// CLAMP, TEX1). False when `record` is not a pass record or its payload is short.
bool gs_stream_pass_words(const std::vector<std::uint8_t> &data, const GsStreamFileRecord &record,
                          std::uint64_t (&words)[9]) noexcept;

// Kind 4/5 payload: the frame ordinal and its PATH2/PATH3 totals in bytes. False when `record` is not a frame record.
bool gs_stream_frame_totals(const std::vector<std::uint8_t> &data, const GsStreamFileRecord &record,
                            std::uint32_t &frame_index, std::uint64_t &path2_bytes,
                            std::uint64_t &path3_bytes) noexcept;

// Kind 8 payload: the refused byte/record counts and the cause bits. False when `record` is not an overflow record.
bool gs_stream_overflow_totals(const std::vector<std::uint8_t> &data, const GsStreamFileRecord &record,
                               std::uint64_t &refused_bytes, std::uint64_t &refused_records,
                               std::uint32_t &cause_bits) noexcept;

}  // namespace gt4::coregs3
