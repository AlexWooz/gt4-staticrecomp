// TASK-090 CoreGS 3.0: a bounded reader for the `.gt4tap` VU1 tap file (coregs3_tap.hpp is the writer's contract).
//
// The offline tools (the tap analysis and the texture-state extraction) need the same parse, so it lives here in
// gt4_coregs3 instead of in one tool. The reader validates the header, every record length and the file bounds; it
// never trusts a length word.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gt4::coregs3 {

inline constexpr std::size_t kTapFileHeaderBytes = 16u;

struct TapFileRecord {
  std::uint32_t kind = 0;      // kTapKindGif or kTapKindSource
  std::uint64_t job = 0;
  std::uint64_t vblank = 0;    // the tap's VBlank counter at the emission/post
  std::uint32_t bytes = 0;     // payload bytes
  std::size_t offset = 0;      // payload offset in the file image
  std::uint32_t address = 0;   // kind == source: the VIF packet's guest address
  std::uint32_t qwc = 0;       // kind == source: the REF QWC word as the tap recorded it
};

// Reads the whole file and its record table. Returns false with `error` for a bad magic, an unsupported version, a
// truncated header or a record that runs past the end.
bool read_tap_file(const std::string &path, std::vector<std::uint8_t> &data, std::vector<TapFileRecord> &records,
                   std::string &error);

}  // namespace gt4::coregs3
