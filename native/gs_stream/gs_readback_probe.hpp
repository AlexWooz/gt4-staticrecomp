// SPDX-License-Identifier: MIT
// TASK-116 step 0 (P0 gate): the READ_FIFO probe of the night-race flare readback.
//
// The game asks the GS process for one 2 KiB readback per frame in a night race (docs/GS-READBACK-STUDY.md). Before the
// per-frame wait can be removed, we have to know what the GS process actually answers in each HWDownloadMode: real
// pixels, zeros, or the previous frame's reply. GT4_GS_READBACK_LOG=1 makes the game print one line per READ_FIFO
// (frame, size, guest destination, status, wait, hash of the reply) and a per-destination summary at exit, so two runs
// (Accurate, Performance mode) answer that from the log alone.
//
// Nothing here is game logic: the probe only reads the reply after the client copied it, and it is off unless the switch
// is set. The fixture (gs_readback_probe_fixture.cpp) pins the INI parsing and the aggregation.
#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace gt4::gs_stream {

// HWDownloadMode of the gt4-gs.ini at `path` (section [EmuCore/GS], key HWDownloadMode; the launcher's Performance mode
// writes 2 = NoReadbacks there, 0 = Accurate otherwise). -1 when the path is empty or unreadable, the key is missing,
// or the value is not a whole number in 0..3. Pure: no globals, no environment.
int download_mode_from_ini(const char *path);
// The same parse over an INI text (the fixture's entry point; download_mode_from_ini reads the file and calls it).
int download_mode_from_text(const std::string &text);

// FNV-1a (64-bit) over `size` bytes; the same function the stream hash uses (TASK-041), so a reply hash here can be
// compared with any other FNV of the same bytes.
std::uint64_t fnv1a64(const void *data, std::size_t size);

// One READ_FIFO reply as the client saw it.
struct ReadbackObservation {
  std::uint64_t frame = 0;        // VSYNC frame the EE was in
  std::uint32_t qwords = 0;       // requested size (a 64x8 PSMCT32 flare is 128)
  std::uint32_t destination = 0;  // guest (EE RAM) address, or 0 when the reply did not land in RAM
  std::uint32_t status = 0;       // protocol readback_status (0 ok, 1 zero filled, 2 failed)
  std::uint64_t wait_us = 0;      // EE wait for this part of the reply
  const std::uint8_t *bytes = nullptr;  // the reply, already copied into guest memory (qwords * 16 bytes)
};

class ReadbackProbe {
 public:
  explicit ReadbackProbe(bool enabled = false) : enabled_(enabled) {}

  bool enabled() const { return enabled_; }
  // GT4_GS_READBACK_LOG: "1" (or any non-zero number) turns the probe on; unset, "0" or empty keeps it off.
  static bool enabled_from_environment();

  // Prints one line and folds the reply into the per-destination summary. Does nothing when disabled.
  void record(const ReadbackObservation &observation, std::FILE *output);
  // One line per destination: calls, distinct replies, how often a reply repeated the previous one, zero replies, the
  // wait distribution. Prints nothing when the probe is disabled or saw nothing.
  void print_summary(std::FILE *output) const;

  std::uint64_t calls() const { return calls_; }

 private:
  struct Destination {
    std::uint64_t calls = 0;
    std::uint64_t zero_replies = 0;
    std::uint64_t same_as_previous = 0;
    std::uint64_t wait_us_total = 0;
    std::uint64_t wait_us_max = 0;
    std::uint32_t nonzero_min = UINT32_MAX;
    std::uint32_t nonzero_max = 0;
    std::uint32_t statuses = 0;  // bit per status value seen
    bool has_previous = false;
    std::uint64_t previous_hash = 0;
    std::uint64_t first_hash = 0;
    std::uint64_t last_hash = 0;
    std::set<std::uint64_t> distinct;
  };

  bool enabled_ = false;
  std::uint64_t calls_ = 0;
  std::map<std::pair<std::uint32_t, std::uint32_t>, Destination> by_destination_;  // (destination, qwords)
};

}  // namespace gt4::gs_stream
